// SPDX-License-Identifier: GPL-2.0-or-later

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "sgi_l1_ioctl.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define SGIL1_REV_LEN 64
#define SGIL1_IO_SIZE 4096
#define SGIL1_IR_DEFAULT_L1_CMD_ADDR 0x00041003U
#define SGIL1_IR_DEFAULT_SRC_ADDR 0x83000101U
#define SGIL1_IR_DISCOVERY_ADDR 0x0ffff00eU
#define SGIL1_IR_DISCOVERY_SRC_ADDR 0x84000101U
#define SGIL1_IR_L1_CMD_TASK 0x03U
#define SGIL1_IR_DEFAULT_CLASS 0U
#define SGIL1_IR_DEFAULT_AUTHORITY 0U
#define SGIL1_IR_DEFAULT_PDATA 0U
#define SGIL1_IR_ARG_BASE 0x10U
#define SGIL1_IR_HEADER_LEN 20U
#define SGIL1_IR_ARG_LEN 8U
#define SGIL1_L1_MAX_COMMAND_TEXT 72U
#define SGIL1_L1_MAX_COMMAND_TRANSFER \
	(SGIL1_IR_HEADER_LEN + (2U * SGIL1_IR_ARG_LEN) + \
	 SGIL1_L1_MAX_COMMAND_TEXT + 1U)
#define SGIL1_IR_MAX_FRAMES 32U
#define SGIL1_PIPE_RECORD_LEN 128U
#define SGIL1_PIPE_HEADER_LEN 9U
#define SGIL1_PIPE_PAYLOAD_MAX 0x76U
#define SGIL1_PIPE_TYPE_BASE 0x3000U
#define SGIL1_PIPE_TYPE_FIRST_NUMBERED 0x3001U
#define SGIL1_PIPE_TYPE_FIRST_UNNUMBERED 0x3002U
#define SGIL1_PIPE_TYPE_CONTINUE 0x3003U
#define SGIL1_DEFAULT_TIME_DRIFT_SEC 60
#define SGIL1_WAIT_POLL_FALLBACK_MS 30000
#define SGIL1_DEFAULT_KEEPALIVE_SEC 0
#define SGIL1_POWER_UP_CONFIRM_TIMEOUT_MS 30000
#define SGIL1_POWER_UP_POLL_MS 2000
#define SGIL1_WAIT_BIND_SETTLE_MS 1000
#define SGIL1_L1CMD_RESPONSE_TIMEOUT 3
#define SGIL1_L1CMD_DISCOVERY_TIMEOUT 4
#define SGIL1_AUTO_DEVICE_MAX 255
#define SGIL1_TZ_MAX 128
#define SGIL1_LOCK_PATH "/var/lock/sgil1ctl.lock"
#define SGIL1_LOCK_FALLBACK_PATH "/tmp/sgil1ctl.lock"
#define SGIL1_DRAIN_QUIET_MS 200

static int sgil1_lock_fd = -1;

static const char *data_candidate_patterns[] = {
	"/dev/sgi-l1/l1-%u",
	"/dev/sgil1_%u",
	"/dev/usb/sgil1_%u",
};

static const char *status_candidates[] = {
	"/dev/sgi-l1/status",
	"/dev/sgil1_cs",
};

struct options {
	const char *device;
	const char *status_device;
	int timeout_ms;
	uint32_t src_addr;
	uint32_t dest_addr;
	uint8_t ir_class;
	uint8_t authority;
	uint8_t pdata;
	bool force;
	bool pipe_records;
	bool no_discover;
	bool dest_overridden;
	bool debug;
};

struct status_options {
	bool set_time;
	const char *timezone;
	char timezone_buf[SGIL1_TZ_MAX];
	int drift_seconds;
};

struct wait_options {
	struct status_options status;
	bool power_up;
	bool power_down;
	bool reset;
	bool force;
	bool background;
	int wait_timeout_seconds;
	int keepalive_seconds;
};

static int l1_text_command_status(const struct options *opts, const char *cmd,
				  bool allow_time_setting, char **text);
static char *l1_text_command(const struct options *opts, const char *cmd,
			     bool allow_time_setting);
static void print_text_block(const char *text);
static int do_power_up_confirmed(const struct options *opts,
				 bool allow_destructive);
static int do_power_down_confirmed(const struct options *opts,
				   bool allow_destructive);
static const char *find_existing_data_device(const struct options *opts);

static void usage(FILE *out, bool full)
{
	fprintf(out,
		"Usage: sgil1ctl [GLOBAL OPTIONS] COMMAND [COMMAND OPTIONS]\n"
		"\n"
			"Global options:\n"
			"  --device PATH         data device path (default: auto)\n"
			"                        auto scans /dev/sgi-l1/l1-*, /dev/sgil1_*, /dev/usb/sgil1_*\n"
			"  --status-device PATH  status device path (default: auto)\n"
			"                        auto tries /dev/sgi-l1/status, /dev/sgil1_cs\n"
			"  --timeout MS          poll timeout for reads (default 3000)\n"
			"  --force               confirm guarded actions or unlisted pass-through\n"
			"  --debug               show IRouter framing diagnostics\n"
			"  -h, --help            show this help\n"
			"  --help-all            show all commands and low-level options\n"
			"                        global options are parsed before COMMAND\n"
		"\n"
		"User commands:\n"
		"  status                print consolidated L1 health/status data\n"
			"  date [OPTIONS]        show or set the L1 clock\n"
				"                        date options: --set-time,\n"
				"                        --timezone TZ (default: host timezone with --set-time),\n"
				"                        --drift-seconds SEC (default 60)\n"
			"  wait [OPTIONS]        wait for an L1 USB device, then run status checks\n"
				"                        wait options: --background,\n"
				"                        --wait-timeout SEC (default: none), --set-time,\n"
				"                        --timezone TZ (default: host timezone with --set-time),\n"
				"                        --drift-seconds SEC (default 60),\n"
				"                        --power-up, --power-down, --reset,\n"
				"                        --keepalive SEC (default 0), --force\n"
		"  version|usb|env|log   send read-only L1 text commands over USB\n"
		"  power [check|vrm]     send read-only L1 power status commands over USB\n"
		"  power up|down         send power commands; requires --force\n"
		"  reset                 send L1 reset command; requires --force\n"
			"  l1cmd <command> [...] send a live-help-listed L1 text command over USB\n"
			"                        add --force to send a command not listed by help\n");

	if (!full)
		return;

	fprintf(out,
		"\n"
		"Protocol options:\n"
			"  --src ADDR            IRouter source address (default 0x83000101)\n"
			"  --dest ADDR           IRouter L1 command destination (default 0x00041003)\n"
			"  --class N             IRouter frame class byte (default 0)\n"
			"  --auth N              IRouter authority, 0..31 (default 0)\n"
			"  --pdata N             IRouter pdata, 0..7 (default 0)\n"
			"  --pipe-records        use legacy SGI pipe records around IRouter frames\n"
			"  --no-discover         skip automatic L1 command destination discovery\n"
		"\n"
		"Diagnostics:\n"
		"  probe                 show status device and first available data device\n"
		"  discover              discover and print the L1 command destination\n"
		"  driver-status         read sgil1_cs status bitmap\n"
		"  read-cfg              read SGIL1_READ_CFG from the data device\n"
		"\n"
		"Low-level developer commands:\n"
		"  reset-read            reset the read URB\n"
		"  reset-write           clear the write endpoint halt\n"
		"  reset-pipes           clear read and write endpoint halts\n"
		"  reset-device          issue a USB device reset\n"
		"  raw-send HEX...       write one raw USB transfer; first two bytes are overwritten by the driver\n"
		"  raw-recv              read one raw USB transfer and print a hexdump\n"
		"  monitor               print raw USB transfers until interrupted\n"
		"  build-l1cmd <command> [...]\n"
		"                        print the IRouter frame for an allowlisted L1 text command\n"
		"\n"
		"Pass-through notes:\n"
		"  l1cmd '*' <command>   SGI broadcast-prefix form; quote '*' to avoid shell expansion\n"
		"                        L1 text commands are limited to 72 bytes on direct USB\n");
}

static int path_exists(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0;
}

static const char *first_existing(const char *const *paths, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++) {
		if (path_exists(paths[i]))
			return paths[i];
	}

	return paths[0];
}

static int format_data_candidate(char *buf, size_t len, const char *pattern,
				 unsigned int index)
{
	int n = snprintf(buf, len, pattern, index);

	return n >= 0 && (size_t)n < len ? 0 : -1;
}

static const char *find_auto_data_device(char *buf, size_t len)
{
	size_t i;
	unsigned int index;

	for (i = 0; i < ARRAY_SIZE(data_candidate_patterns); i++) {
		for (index = 0; index <= SGIL1_AUTO_DEVICE_MAX; index++) {
			if (format_data_candidate(buf, len,
						  data_candidate_patterns[i],
						  index))
				continue;
			if (path_exists(buf))
				return buf;
		}
	}

	return NULL;
}

static int open_lock_file_at(const char *path)
{
	int flags = O_RDWR | O_CREAT | O_CLOEXEC;
	int fd;

#ifdef O_NOFOLLOW
	flags |= O_NOFOLLOW;
#endif

	fd = open(path, flags, 0666);
	if (fd >= 0) {
		if (fchmod(fd, 0666) < 0 && errno != EPERM)
			fprintf(stderr, "warning: failed to chmod lock file %s: %s\n",
				path, strerror(errno));
		return fd;
	}

	if (errno == EACCES || errno == EPERM) {
		flags = O_RDONLY | O_CLOEXEC;
#ifdef O_NOFOLLOW
		flags |= O_NOFOLLOW;
#endif
		fd = open(path, flags);
	}

	return fd;
}

static int open_lock_file(void)
{
	int fd = open_lock_file_at(SGIL1_LOCK_PATH);

	if (fd >= 0)
		return fd;
	if (errno != ENOENT && errno != EACCES && errno != EPERM &&
	    errno != EROFS)
		fprintf(stderr, "warning: failed to open lock file %s: %s\n",
			SGIL1_LOCK_PATH, strerror(errno));

	fd = open_lock_file_at(SGIL1_LOCK_FALLBACK_PATH);
	if (fd >= 0)
		return fd;

	fprintf(stderr, "failed to open lock file %s: %s\n",
		SGIL1_LOCK_FALLBACK_PATH, strerror(errno));
	return -1;
}

static int acquire_sgil1_lock(void)
{
	int fd;

	if (sgil1_lock_fd >= 0)
		return 0;

	fd = open_lock_file();
	if (fd < 0)
		return 1;

	for (;;) {
		if (!flock(fd, LOCK_EX))
			break;
		if (errno == EINTR)
			continue;

		fprintf(stderr, "failed to lock sgil1ctl transaction: %s\n",
			strerror(errno));
		close(fd);
		return 1;
	}

	sgil1_lock_fd = fd;
	return 0;
}

static void release_sgil1_lock(void)
{
	if (sgil1_lock_fd < 0)
		return;

	if (flock(sgil1_lock_fd, LOCK_UN) < 0)
		fprintf(stderr, "warning: failed to unlock sgil1ctl transaction: %s\n",
			strerror(errno));
	close(sgil1_lock_fd);
	sgil1_lock_fd = -1;
}

static bool command_uses_l1_transaction(const char *cmd)
{
	return strcmp(cmd, "build-l1cmd") != 0;
}

static int open_data_device(const struct options *opts, int flags)
{
	char auto_path[PATH_MAX];
	const char *path = opts->device;
	int fd;

	if (!path)
		path = find_auto_data_device(auto_path, sizeof(auto_path));
	if (!path) {
		fprintf(stderr,
			"no SGI L1 data device found; tried /dev/sgi-l1/l1-*, /dev/sgil1_*, /dev/usb/sgil1_*\n");
		return -1;
	}

	fd = open(path, flags);
	if (fd < 0)
		fprintf(stderr, "failed to open data device %s: %s\n", path,
			strerror(errno));

	return fd;
}

static int open_status_device(const struct options *opts, int flags)
{
	const char *path = opts->status_device;
	int fd;

	if (!path)
		path = first_existing(status_candidates,
				      ARRAY_SIZE(status_candidates));

	fd = open(path, flags);
	if (fd < 0)
		fprintf(stderr, "failed to open status device %s: %s\n", path,
			strerror(errno));

	return fd;
}

static void print_cfg(const struct sgil1_cfg *cfg)
{
	int i;

	printf("bus=%u dev=%u level=%u path=", cfg->bus, cfg->dev, cfg->level);
	if (!cfg->level) {
		printf("(root)");
	} else {
		for (i = 0; i < cfg->level && i < SGIL1_MAX_LEVEL; i++)
			printf("%s%u", i ? "." : "", cfg->path[i]);
	}
	printf("\n");
}

static void hexdump(const uint8_t *buf, size_t len)
{
	size_t i;
	size_t j;

	for (i = 0; i < len; i += 16) {
		printf("%04zx  ", i);
		for (j = 0; j < 16; j++) {
			if (i + j < len)
				printf("%02x ", buf[i + j]);
			else
				printf("   ");
			if (j == 7)
				printf(" ");
		}

		printf(" |");
		for (j = 0; j < 16 && i + j < len; j++) {
			uint8_t c = buf[i + j];

			putchar((c >= 32 && c <= 126) ? c : '.');
		}
		printf("|\n");
	}
}

static void put_be16(uint8_t *buf, uint16_t value)
{
	buf[0] = (value >> 8) & 0xff;
	buf[1] = value & 0xff;
}

static void put_be32(uint8_t *buf, uint32_t value)
{
	buf[0] = (value >> 24) & 0xff;
	buf[1] = (value >> 16) & 0xff;
	buf[2] = (value >> 8) & 0xff;
	buf[3] = value & 0xff;
}

static uint16_t get_be16(const uint8_t *buf)
{
	return ((uint16_t)buf[0] << 8) | buf[1];
}

static uint32_t get_be32(const uint8_t *buf)
{
	return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
	       ((uint32_t)buf[2] << 8) | buf[3];
}

static int parse_u32(const char *text, uint32_t *value)
{
	char *end = NULL;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(text, &end, 0);
	if (errno || !end || *end || parsed > UINT32_MAX)
		return -1;

	*value = (uint32_t)parsed;
	return 0;
}

static bool streq_ci(const char *a, const char *b)
{
	while (*a && *b) {
		unsigned char ca = (unsigned char)*a;
		unsigned char cb = (unsigned char)*b;

		if (ca >= 'A' && ca <= 'Z')
			ca = ca - 'A' + 'a';
		if (cb >= 'A' && cb <= 'Z')
			cb = cb - 'A' + 'a';
		if (ca != cb)
			return false;
		a++;
		b++;
	}

	return *a == *b;
}

static bool startswith_ci(const char *text, const char *prefix);

static bool l1_command_is_read_only(const char *cmd)
{
	return streq_ci(cmd, "version") || streq_ci(cmd, "ver") ||
	       streq_ci(cmd, "help") || startswith_ci(cmd, "help ") ||
	       streq_ci(cmd, "hlp") || startswith_ci(cmd, "hlp ") ||
	       streq_ci(cmd, "usb") || streq_ci(cmd, "env") ||
	       streq_ci(cmd, "env check") || streq_ci(cmd, "fan") ||
	       streq_ci(cmd, "date") || streq_ci(cmd, "date tz") ||
	       streq_ci(cmd, "serial") || streq_ci(cmd, "serial all") ||
	       streq_ci(cmd, "log") ||
	       streq_ci(cmd, "power") || streq_ci(cmd, "pwr") ||
	       streq_ci(cmd, "power check") || streq_ci(cmd, "pwr check") ||
	       streq_ci(cmd, "power vrm") || streq_ci(cmd, "pwr vrm");
}

static bool startswith_ci(const char *text, const char *prefix)
{
	while (*prefix) {
		unsigned char ct = (unsigned char)*text;
		unsigned char cp = (unsigned char)*prefix;

		if (!*text)
			return false;

		if (ct >= 'A' && ct <= 'Z')
			ct = ct - 'A' + 'a';
		if (cp >= 'A' && cp <= 'Z')
			cp = cp - 'A' + 'a';
		if (ct != cp)
			return false;
		text++;
		prefix++;
	}

	return true;
}

static bool contains_ci(const char *haystack, const char *needle)
{
	if (!*needle)
		return true;

	for (; *haystack; haystack++) {
		if (startswith_ci(haystack, needle))
			return true;
	}

	return false;
}

static bool is_force_option(const char *arg)
{
	return !strcmp(arg, "--force") || !strcmp(arg, "--yes");
}

static bool l1_command_sets_time(const char *cmd)
{
	return startswith_ci(cmd, "date ") && !streq_ci(cmd, "date tz");
}

static bool l1_command_is_destructive(const char *cmd)
{
	return streq_ci(cmd, "power up") || streq_ci(cmd, "pwr up") ||
	       streq_ci(cmd, "pwr u") || streq_ci(cmd, "power down") ||
	       streq_ci(cmd, "pwr down") || streq_ci(cmd, "pwr d") ||
	       streq_ci(cmd, "reset");
}

static int validate_l1_command_text_len(const char *cmd)
{
	size_t text_len = strlen(cmd);

	if (text_len <= SGIL1_L1_MAX_COMMAND_TEXT)
		return 0;

	fprintf(stderr,
		"refusing L1 command text of %zu bytes; maximum safe L1 USB command text is %u bytes (%u-byte IRouter transfer)\n",
		text_len, SGIL1_L1_MAX_COMMAND_TEXT,
		SGIL1_L1_MAX_COMMAND_TRANSFER);
	return -1;
}

static int build_l1_command_frame(const char *cmd, uint16_t seq, uint32_t dest,
				  uint32_t src, uint8_t ir_class,
				  uint8_t authority, uint8_t pdata, uint8_t *buf,
				  size_t cap, size_t *out_len)
{
	size_t text_len = strlen(cmd);
	size_t cmd_len = text_len + 1;
	size_t payload_off = SGIL1_IR_HEADER_LEN + (2 * SGIL1_IR_ARG_LEN);
	size_t frame_len = payload_off + cmd_len;
	uint8_t *arg;

	if (validate_l1_command_text_len(cmd))
		return -1;

	if (cmd_len > UINT16_MAX || frame_len > UINT16_MAX || frame_len > cap) {
		fprintf(stderr, "L1 command frame is too large\n");
		return -1;
	}

	memset(buf, 0, frame_len);

	put_be16(buf, frame_len);
	buf[2] = 2;              /* IRouter frame version */
	buf[3] = 0;              /* request/error code */
	buf[4] = seq & 0xff;
	buf[5] = 0x81;           /* first frame plus one-frame packet count */
	buf[6] = ir_class;
	buf[7] = ((authority & 0x1f) << 3) | (pdata & 0x07);
	put_be32(buf + 8, dest);
	put_be32(buf + 12, src);
	buf[16] = 2;             /* argc */
	buf[17] = (seq >> 8) & 0xff;
	put_be16(buf + 18, 0);   /* opcode */

	arg = buf + SGIL1_IR_HEADER_LEN + SGIL1_IR_ARG_LEN;
	arg[0] = 0x0a;           /* legacy text argument class */
	arg[1] = 1;              /* frame number */
	put_be16(arg + 2, cmd_len);
	put_be32(arg + 4, SGIL1_IR_ARG_BASE + payload_off);

	memcpy(buf + payload_off, cmd, cmd_len);
	*out_len = frame_len;
	return 0;
}

static int build_l1_discovery_frame(uint16_t seq, uint8_t *buf, size_t cap,
				    size_t *out_len)
{
	size_t frame_len = SGIL1_IR_HEADER_LEN + (3 * SGIL1_IR_ARG_LEN);

	if (frame_len > cap) {
		fprintf(stderr, "L1 discovery frame is too large\n");
		return -1;
	}

	memset(buf, 0, frame_len);

	put_be16(buf, frame_len);
	buf[2] = 2;
	buf[3] = 0;
	buf[4] = seq & 0xff;
	buf[5] = 0x81;
	buf[6] = 0;
	buf[7] = 0;
	put_be32(buf + 8, SGIL1_IR_DISCOVERY_ADDR);
	put_be32(buf + 12, SGIL1_IR_DISCOVERY_SRC_ADDR);
	buf[16] = 3;
	buf[17] = (seq >> 8) & 0xff;
	put_be16(buf + 18, 0);

	put_be32(buf + SGIL1_IR_HEADER_LEN + 4, 6);

	*out_len = frame_len;
	return 0;
}

static int write_all(int fd, const uint8_t *buf, size_t len)
{
	size_t off = 0;

	while (off < len) {
		ssize_t n = write(fd, buf + off, len - off);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "write failed: %s\n", strerror(errno));
			return -1;
		}
		if (n == 0) {
			fprintf(stderr, "write returned 0 bytes\n");
			return -1;
		}
		off += n;
	}

	return 0;
}

static int write_one_transfer(int fd, const uint8_t *buf, size_t len)
{
	ssize_t n;

	do {
		n = write(fd, buf, len);
	} while (n < 0 && errno == EINTR);

	if (n < 0) {
		fprintf(stderr, "write failed: %s\n", strerror(errno));
		return -1;
	}
	if ((size_t)n != len) {
		fprintf(stderr, "short write: %zd/%zu\n", n, len);
		return -1;
	}

	return 0;
}

static int write_pipe_frame(int fd, const uint8_t *buf, size_t len,
			    size_t *out_records)
{
	uint8_t record[SGIL1_PIPE_RECORD_LEN];
	size_t off = 0;
	size_t records = 0;
	bool first = true;

	if (len > UINT16_MAX) {
		fprintf(stderr, "IRouter frame is too large for SGI pipe framing\n");
		return -1;
	}

	while (off < len) {
		size_t chunk = len - off;
		size_t record_len;

		if (chunk > SGIL1_PIPE_PAYLOAD_MAX)
			chunk = SGIL1_PIPE_PAYLOAD_MAX;

		memset(record, 0, sizeof(record));
		record[4] = (SGIL1_PIPE_TYPE_BASE >> 8) & 0xff;
		if (first) {
			if (len >= 6 && (buf[5] & 0x80)) {
				record[5] = SGIL1_PIPE_TYPE_FIRST_NUMBERED &
					    0xff;
				record[6] = buf[5] & 0x7f;
			} else {
				record[5] = SGIL1_PIPE_TYPE_FIRST_UNNUMBERED &
					    0xff;
				record[6] = 0;
			}
			put_be16(record + 7, len);
		} else {
			record[5] = SGIL1_PIPE_TYPE_CONTINUE & 0xff;
		}
		memcpy(record + SGIL1_PIPE_HEADER_LEN, buf + off, chunk);

		record_len = SGIL1_PIPE_HEADER_LEN + chunk;
		if (write_all(fd, record, record_len))
			return -1;

		off += chunk;
		records++;
		first = false;
	}

	if (out_records)
		*out_records = records;
	return 0;
}

static int hex_nibble(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static int parse_hex_args(int argc, char **argv, int start, uint8_t **out,
			  size_t *out_len)
{
	uint8_t *buf = NULL;
	size_t cap = 0;
	size_t len = 0;
	int pending = -1;
	int i;

	if (start >= argc) {
		fprintf(stderr, "raw-send needs hex bytes\n");
		return -1;
	}

	for (i = start; i < argc; i++) {
		const char *p = argv[i];

		while (*p) {
			int nibble;

			if ((p[0] == '0') && (p[1] == 'x' || p[1] == 'X')) {
				p += 2;
				continue;
			}

			nibble = hex_nibble((unsigned char)*p);
			if (nibble < 0) {
				if (*p == ' ' || *p == '\t' || *p == ':' ||
				    *p == ',' || *p == '-' || *p == '_') {
					p++;
					continue;
				}
				fprintf(stderr, "invalid hex character '%c'\n", *p);
				free(buf);
				return -1;
			}

			if (pending < 0) {
				pending = nibble;
			} else {
				if (len == cap) {
					size_t next_cap = cap ? cap * 2 : 64;
					uint8_t *next = realloc(buf, next_cap);

					if (!next) {
						perror("realloc");
						free(buf);
						return -1;
					}
					buf = next;
					cap = next_cap;
				}
				buf[len++] = (pending << 4) | nibble;
				pending = -1;
			}
			p++;
		}
	}

	if (pending >= 0) {
		fprintf(stderr, "odd number of hex digits\n");
		free(buf);
		return -1;
	}
	if (len < 2) {
		fprintf(stderr, "frame must contain at least two length bytes\n");
		free(buf);
		return -1;
	}

	*out = buf;
	*out_len = len;
	return 0;
}

static int do_driver_status(const struct options *opts)
{
	uint8_t status[SGIL1_MAX_DEVICES];
	int fd;
	ssize_t n;
	int i;
	int active = 0;

	fd = open_status_device(opts, O_RDONLY);
	if (fd < 0)
		return 1;

	n = read(fd, status, sizeof(status));
	if (n < 0) {
		fprintf(stderr, "status read failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	for (i = 0; i < n; i++) {
		if (status[i]) {
			printf("sgil1_%d present\n", i);
			active++;
		}
	}

	if (!active)
		printf("no SGI L1 USB devices registered\n");

	close(fd);
	return 0;
}

static int do_read_cfg(const struct options *opts)
{
	struct sgil1_cfg cfg;
	int fd = open_data_device(opts, O_RDWR | O_NONBLOCK);

	if (fd < 0)
		return 1;

	memset(&cfg, 0, sizeof(cfg));
	if (ioctl(fd, SGIL1_READ_CFG, &cfg) < 0) {
		fprintf(stderr, "SGIL1_READ_CFG failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	print_cfg(&cfg);
	close(fd);
	return 0;
}

static int do_probe(const struct options *opts)
{
	char rev[SGIL1_REV_LEN];
	const char *path;
	int fd;
	int ret = 0;

	fd = open_status_device(opts, O_RDONLY);
	if (fd >= 0) {
		memset(rev, 0, sizeof(rev));
		if (ioctl(fd, SGIL1_ST_READ_REV, rev) == 0)
			printf("driver=%s\n", rev);
		close(fd);
	}

	ret = do_driver_status(opts);
	if (ret)
		return ret;

	path = find_existing_data_device(opts);
	if (path)
		printf("data device: %s\n", path);

	return do_read_cfg(opts);
}

static int ioctl_command(const struct options *opts, unsigned long request,
			 const char *name)
{
	int fd = open_data_device(opts, O_RDWR | O_NONBLOCK);

	if (fd < 0)
		return 1;

	if (ioctl(fd, request) < 0) {
		fprintf(stderr, "%s failed: %s\n", name, strerror(errno));
		close(fd);
		return 1;
	}

	printf("%s ok\n", name);
	close(fd);
	return 0;
}

static void recover_l1_pipes_fd(int fd, const char *reason)
{
	if (ioctl(fd, SGIL1_RESET_PIPES) < 0) {
		fprintf(stderr, "reset-pipes after %s failed: %s\n", reason,
			strerror(errno));
		return;
	}

	fprintf(stderr, "reset-pipes ok after %s\n", reason);
}

static int do_raw_send(const struct options *opts, int argc, char **argv,
		       int start)
{
	uint8_t *buf = NULL;
	size_t len = 0;
	ssize_t n;
	int fd;

	if (parse_hex_args(argc, argv, start, &buf, &len))
		return 1;

	fd = open_data_device(opts, O_RDWR);
	if (fd < 0) {
		free(buf);
		return 1;
	}

	n = write(fd, buf, len);
	if (n < 0) {
		fprintf(stderr, "write failed: %s\n", strerror(errno));
		close(fd);
		free(buf);
		return 1;
	}
	if ((size_t)n != len) {
		fprintf(stderr, "short write: %zd/%zu\n", n, len);
		close(fd);
		free(buf);
		return 1;
	}

	printf("wrote %zu bytes\n", len);
	close(fd);
	free(buf);
	return 0;
}

static int read_one_frame(int fd, int timeout_ms)
{
	uint8_t buf[SGIL1_IO_SIZE];
	struct pollfd pfd = {
		.fd = fd,
		.events = POLLIN | POLLHUP,
	};
	int ret;
	ssize_t n;

	ret = poll(&pfd, 1, timeout_ms);
	if (ret < 0) {
		fprintf(stderr, "poll failed: %s\n", strerror(errno));
		return 1;
	}
	if (ret == 0) {
		fprintf(stderr, "timed out waiting for a frame\n");
		return 1;
	}
	if (pfd.revents & POLLHUP) {
		fprintf(stderr, "device disconnected\n");
		return 1;
	}

	n = read(fd, buf, sizeof(buf));
	if (n < 0) {
		fprintf(stderr, "read failed: %s\n", strerror(errno));
		return 1;
	}

	printf("read %zd bytes\n", n);
	hexdump(buf, n);
	return 0;
}

static void print_text_payload(const uint8_t *buf, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		uint8_t c = buf[i];

		if (c == 0)
			continue;
		if (c == '\r')
			continue;
		if (c == '\n' || c == '\t' || (c >= 32 && c <= 126))
			putchar(c);
		else
			putchar('.');
	}
	if (!len || buf[len - 1] != '\n')
		putchar('\n');
}

static bool payload_looks_text(const uint8_t *buf, size_t len)
{
	size_t printable = 0;
	size_t total = 0;
	size_t i;

	for (i = 0; i < len; i++) {
		uint8_t c = buf[i];

		if (c == 0)
			continue;
		total++;
		if (c == '\r' || c == '\n' || c == '\t' ||
		    (c >= 32 && c <= 126))
			printable++;
	}

	return total > 0 && printable * 100 / total >= 85;
}

static void print_irouter_frame(const uint8_t *buf, size_t len)
{
	uint16_t advertised;
	uint16_t opcode;
	uint32_t dest;
	uint32_t src;
	unsigned int argc;
	unsigned int i;

	if (len < SGIL1_IR_HEADER_LEN) {
		printf("short IRouter frame (%zu bytes)\n", len);
		hexdump(buf, len);
		return;
	}

	advertised = get_be16(buf);
	opcode = get_be16(buf + 18);
	dest = get_be32(buf + 8);
	src = get_be32(buf + 12);
	argc = buf[16];

	printf("IRouter frame: got=%zu advertised=%u version=%u err=%u seq=%u "
	       "frame=0x%02x class=%u auth=%u pdata=%u dest=0x%08x "
	       "src=0x%08x opcode=0x%04x argc=%u\n",
	       len, advertised, buf[2], buf[3],
	       ((unsigned int)buf[17] << 8) | buf[4], buf[5], buf[6],
	       buf[7] >> 3, buf[7] & 0x07, dest, src, opcode, argc);

	if (advertised != len)
		printf("warning: advertised frame length differs from read length\n");

	for (i = 0; i < argc; i++) {
		size_t desc_off = SGIL1_IR_HEADER_LEN + i * SGIL1_IR_ARG_LEN;
		unsigned int klass;
		unsigned int frame;
		uint16_t size;
		uint32_t value;

		if (desc_off + SGIL1_IR_ARG_LEN > len) {
			printf("arg%u: descriptor outside frame\n", i);
			break;
		}

		klass = buf[desc_off];
		frame = buf[desc_off + 1];
		size = get_be16(buf + desc_off + 2);
		value = get_be32(buf + desc_off + 4);

		printf("arg%u: class=%u frame=%u size=%u value=0x%08x\n",
		       i, klass, frame, size, value);

		if ((klass & 0x07) && size) {
			size_t data_off;

			if (value < SGIL1_IR_ARG_BASE) {
				printf("arg%u: invalid payload offset\n", i);
				continue;
			}
			data_off = value - SGIL1_IR_ARG_BASE;
			if (data_off + size > len) {
				printf("arg%u: payload outside frame\n", i);
				continue;
			}
			if (payload_looks_text(buf + data_off, size)) {
				printf("arg%u text:\n", i);
				print_text_payload(buf + data_off, size);
			} else {
				printf("arg%u data:\n", i);
				hexdump(buf + data_off, size);
			}
		}
	}
}

struct ir_text_assembly {
	uint8_t *buf;
	size_t expected;
	size_t got;
	unsigned int arg_index;
	uint8_t *extra;
	size_t extra_len;
};

static void free_ir_text_assembly(struct ir_text_assembly *assembly)
{
	free(assembly->buf);
	free(assembly->extra);
	memset(assembly, 0, sizeof(*assembly));
}

static int append_ir_text_extra(struct ir_text_assembly *assembly,
				const uint8_t *buf, size_t len)
{
	uint8_t *extra;

	if (!len)
		return 0;

	extra = realloc(assembly->extra, assembly->extra_len + len);
	if (!extra) {
		perror("realloc");
		return -1;
	}
	memcpy(extra + assembly->extra_len, buf, len);
	assembly->extra = extra;
	assembly->extra_len += len;
	return 0;
}

static int collect_ir_text_frame(struct ir_text_assembly *assembly,
				 const uint8_t *buf, size_t len,
				 bool first_frame)
{
	size_t data_off = SGIL1_IR_HEADER_LEN;
	size_t copy_len;

	if (len < SGIL1_IR_HEADER_LEN)
		return 0;

	if (first_frame) {
		unsigned int argc = buf[16];
		unsigned int i;

		for (i = 0; i < argc; i++) {
			size_t desc_off = SGIL1_IR_HEADER_LEN +
					  i * SGIL1_IR_ARG_LEN;
			size_t arg_data_off;
			unsigned int klass;
			uint16_t size;
			uint32_t value;

			if (desc_off + SGIL1_IR_ARG_LEN > len)
				break;

			klass = buf[desc_off];
			size = get_be16(buf + desc_off + 2);
			value = get_be32(buf + desc_off + 4);
			if (klass != 0x0a || !size)
				continue;
			if (value < SGIL1_IR_ARG_BASE)
				return -1;

			arg_data_off = value - SGIL1_IR_ARG_BASE;
			if (arg_data_off >= len)
				return -1;

			if (!assembly->buf) {
				data_off = arg_data_off;
				assembly->buf = malloc(size);
				if (!assembly->buf) {
					perror("malloc");
					return -1;
				}
				assembly->expected = size;
				assembly->arg_index = i;
				continue;
			}

			if (arg_data_off + size > len)
				continue;
			if (append_ir_text_extra(assembly, buf + arg_data_off,
						 size))
				return -1;
		}
	}

	if (!assembly->buf)
		return 0;
	if (data_off >= len)
		return 0;

	copy_len = len - data_off;
	if (copy_len > assembly->expected - assembly->got)
		copy_len = assembly->expected - assembly->got;

	memcpy(assembly->buf + assembly->got, buf + data_off, copy_len);
	assembly->got += copy_len;
	return 0;
}

static int read_raw_irouter_frame(int fd, int timeout_ms, uint8_t *buf,
				  size_t cap, size_t *out_len)
{
	for (;;) {
		struct pollfd pfd = {
			.fd = fd,
			.events = POLLIN | POLLHUP,
		};
		int ret;
		ssize_t n;

		ret = poll(&pfd, 1, timeout_ms);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "poll failed: %s\n", strerror(errno));
			return -1;
		}
		if (ret == 0)
			return 0;
		if (pfd.revents & POLLHUP) {
			fprintf(stderr, "device disconnected\n");
			return -1;
		}

		n = read(fd, buf, cap);
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			fprintf(stderr, "read failed: %s\n", strerror(errno));
			return -1;
		}
		if ((size_t)n < SGIL1_IR_HEADER_LEN) {
			fprintf(stderr, "short IRouter frame: %zd bytes\n", n);
			hexdump(buf, n > 0 ? (size_t)n : 0);
			return -1;
		}

		*out_len = (size_t)n;
		return 1;
	}
}

static unsigned int drain_raw_irouter_frames_fd(int fd, int quiet_ms)
{
	uint8_t buf[SGIL1_IO_SIZE];
	unsigned int drained;

	for (drained = 0; drained < SGIL1_IR_MAX_FRAMES; drained++) {
		size_t len = 0;
		int ret = read_raw_irouter_frame(fd, quiet_ms, buf, sizeof(buf),
						 &len);

		if (ret <= 0)
			break;
	}

	if (drained == SGIL1_IR_MAX_FRAMES)
		fprintf(stderr,
			"warning: stopped draining after %u stale IRouter frames\n",
			drained);

	return drained;
}

static int discover_l1_command_dest_fd(const struct options *opts, int fd,
				       uint32_t *dest_out, bool verbose)
{
	static const uint16_t seqs[] = { 0, 2 };
	uint8_t tx[SGIL1_IO_SIZE];
	uint8_t rx[SGIL1_IO_SIZE];
	size_t tx_len = 0;
	size_t i;

	if (opts->pipe_records) {
		fprintf(stderr,
			"automatic discovery is not supported with --pipe-records\n");
		return 1;
	}

	drain_raw_irouter_frames_fd(fd, SGIL1_DRAIN_QUIET_MS);

	for (i = 0; i < ARRAY_SIZE(seqs); i++) {
		unsigned int frame;

		if (build_l1_discovery_frame(seqs[i], tx, sizeof(tx), &tx_len))
			return 1;
		if (write_one_transfer(fd, tx, tx_len))
			return 1;

		for (frame = 0; frame < SGIL1_IR_MAX_FRAMES; frame++) {
			size_t rx_len = 0;
			uint32_t dest;
			uint32_t src;
			int timeout = frame ? 250 : opts->timeout_ms;
			int ret;

			ret = read_raw_irouter_frame(fd, timeout, rx, sizeof(rx),
						     &rx_len);
			if (ret < 0) {
				recover_l1_pipes_fd(fd,
						    "L1 discovery read failure");
				return 1;
			}
			if (ret == 0)
				break;

			dest = get_be32(rx + 8);
			src = get_be32(rx + 12);
			if (rx[2] == 2 && rx[3] == 0 &&
			    dest == SGIL1_IR_DISCOVERY_SRC_ADDR && src) {
				*dest_out = (src & 0xfffffff0U) |
					    SGIL1_IR_L1_CMD_TASK;
				if (verbose)
					printf("discovered L1 route src=0x%08x command-dest=0x%08x\n",
					       src, *dest_out);
				return 0;
			}

			if (verbose)
				fprintf(stderr,
					"ignoring non-discovery IRouter frame dest=0x%08x src=0x%08x len=%zu\n",
					dest, src, rx_len);
		}
	}

	fprintf(stderr,
		"timed out waiting for L1 discovery response; use --no-discover or --dest if the command destination is known\n");
	recover_l1_pipes_fd(fd, "L1 discovery timeout");
	return SGIL1_L1CMD_DISCOVERY_TIMEOUT;
}

static int discover_l1_command_dest(const struct options *opts,
				    uint32_t *dest_out, bool verbose)
{
	int fd;
	int ret;

	fd = open_data_device(opts, O_RDWR | O_NONBLOCK);
	if (fd < 0)
		return 1;

	ret = discover_l1_command_dest_fd(opts, fd, dest_out, verbose);
	close(fd);
	return ret;
}

static int read_pipe_irouter_frame(int fd, int timeout_ms, uint8_t *buf,
				   size_t cap, size_t *out_len)
{
	uint8_t record[SGIL1_PIPE_RECORD_LEN];
	size_t got = 0;
	size_t expected = 0;
	bool started = false;

	for (;;) {
		struct pollfd pfd = {
			.fd = fd,
			.events = POLLIN | POLLHUP,
		};
		int poll_timeout = started && timeout_ms >= 0 ? 500 : timeout_ms;
		uint16_t type;
		uint16_t total_len;
		size_t payload_len;
		int ret;
		ssize_t n;

		ret = poll(&pfd, 1, poll_timeout);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "poll failed: %s\n", strerror(errno));
			return -1;
		}
		if (ret == 0) {
			if (started)
				fprintf(stderr,
					"timed out waiting for pipe continuation\n");
			return started ? -1 : 0;
		}
		if (pfd.revents & POLLHUP) {
			fprintf(stderr, "device disconnected\n");
			return -1;
		}

		n = read(fd, record, sizeof(record));
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			fprintf(stderr, "read failed: %s\n", strerror(errno));
			return -1;
		}
		if ((size_t)n < SGIL1_PIPE_HEADER_LEN) {
			fprintf(stderr, "short SGI pipe record: %zd bytes\n", n);
			hexdump(record, n > 0 ? (size_t)n : 0);
			return -1;
		}

		type = get_be16(record + 4);
		total_len = get_be16(record + 7);
		payload_len = (size_t)n - SGIL1_PIPE_HEADER_LEN;

		if (type == SGIL1_PIPE_TYPE_FIRST_NUMBERED ||
		    type == SGIL1_PIPE_TYPE_FIRST_UNNUMBERED) {
			if (started) {
				fprintf(stderr,
					"unexpected new SGI pipe frame before continuation completed\n");
				return -1;
			}
			if (!total_len) {
				fprintf(stderr, "empty SGI pipe frame\n");
				return -1;
			}
			if (total_len > cap) {
				fprintf(stderr,
					"SGI pipe frame is too large: %u bytes\n",
					total_len);
				return -1;
			}
			started = true;
			expected = total_len;
		} else if (type != SGIL1_PIPE_TYPE_CONTINUE) {
			fprintf(stderr, "unexpected SGI pipe record type 0x%04x\n",
				type);
			hexdump(record, (size_t)n);
			return -1;
		} else if (!started) {
			fprintf(stderr, "SGI pipe continuation without first record\n");
			hexdump(record, (size_t)n);
			return -1;
		}

		if (payload_len > expected - got) {
			fprintf(stderr,
				"SGI pipe payload exceeds advertised frame length\n");
			hexdump(record, (size_t)n);
			return -1;
		}

		memcpy(buf + got, record + SGIL1_PIPE_HEADER_LEN, payload_len);
		got += payload_len;
		if (got == expected) {
			*out_len = got;
			return 1;
		}
	}
}

static unsigned int drain_pipe_irouter_frames_fd(int fd, int quiet_ms)
{
	uint8_t buf[SGIL1_IO_SIZE];
	unsigned int drained;

	for (drained = 0; drained < SGIL1_IR_MAX_FRAMES; drained++) {
		size_t len = 0;
		int ret = read_pipe_irouter_frame(fd, quiet_ms, buf, sizeof(buf),
						  &len);

		if (ret <= 0)
			break;
	}

	if (drained == SGIL1_IR_MAX_FRAMES)
		fprintf(stderr,
			"warning: stopped draining after %u stale SGI pipe frames\n",
			drained);

	return drained;
}

static char *text_payload_to_string(const uint8_t *buf, size_t len)
{
	char *text;
	size_t i;
	size_t out = 0;

	text = malloc(len + 1);
	if (!text) {
		perror("malloc");
		return NULL;
	}

	for (i = 0; i < len; i++) {
		uint8_t c = buf[i];

		if (c == 0 || c == '\r')
			continue;
		if (c == '\n' || c == '\t' || (c >= 32 && c <= 126))
			text[out++] = (char)c;
		else
			text[out++] = '.';
	}
	text[out] = '\0';
	return text;
}

static char *empty_string(void)
{
	char *text = malloc(1);

	if (!text) {
		perror("malloc");
		return NULL;
	}
	text[0] = '\0';
	return text;
}

static int run_l1_command_core(const struct options *opts, const char *l1cmd,
			       bool allow_destructive, bool allow_time_setting,
			       bool allow_unlisted,
			       bool response_timeout_is_pending, bool verbose,
			       char **out_text)
{
	uint8_t tx[SGIL1_IO_SIZE];
	uint8_t rx[SGIL1_IO_SIZE];
	size_t tx_len = 0;
	size_t rx_len = 0;
	size_t pipe_records = 0;
	unsigned int expected_frames = 1;
	unsigned int frame;
	struct ir_text_assembly assembly = { 0 };
	uint32_t dest_addr = opts->dest_addr;
	int fd;
	int ret = 1;
	bool read_only = l1_command_is_read_only(l1cmd);
	bool time_setting = l1_command_sets_time(l1cmd);
	bool destructive = l1_command_is_destructive(l1cmd);

	if (out_text)
		*out_text = NULL;

	if (!read_only && !time_setting && !destructive && !allow_unlisted) {
		fprintf(stderr,
			"refusing L1 command '%s': command is not allowlisted; add --force to send anyway\n",
			l1cmd);
		return 2;
	}
	if (time_setting && !allow_time_setting && !allow_unlisted) {
		fprintf(stderr,
			"refusing L1 command '%s': add --force or use --set-time to confirm L1 clock change\n",
			l1cmd);
		return 2;
	}
	if (destructive && !allow_destructive && !allow_unlisted) {
		fprintf(stderr,
			"refusing L1 command '%s': add --force to confirm workstation power/reset action\n",
			l1cmd);
		return 2;
	}
	if (validate_l1_command_text_len(l1cmd))
		return 2;

	fd = open_data_device(opts, O_RDWR | O_NONBLOCK);
	if (fd < 0)
		return 1;

	if (opts->pipe_records)
		drain_pipe_irouter_frames_fd(fd, SGIL1_DRAIN_QUIET_MS);
	else
		drain_raw_irouter_frames_fd(fd, SGIL1_DRAIN_QUIET_MS);

	if (!opts->pipe_records && !opts->no_discover && !opts->dest_overridden) {
		int discover_ret;

		discover_ret = discover_l1_command_dest_fd(opts, fd, &dest_addr,
							   verbose);
		if (discover_ret) {
			ret = discover_ret;
			goto out;
		}
		drain_raw_irouter_frames_fd(fd, SGIL1_DRAIN_QUIET_MS);
	}

	if (build_l1_command_frame(l1cmd, 0, dest_addr, opts->src_addr,
				   opts->ir_class, opts->authority, opts->pdata,
				   tx, sizeof(tx), &tx_len))
		goto out;

	if (opts->pipe_records) {
		if (write_pipe_frame(fd, tx, tx_len, &pipe_records)) {
			recover_l1_pipes_fd(fd, "L1 command write failure");
			goto out;
		}

		if (verbose)
			printf("sent L1 command '%s' in %zu-byte IRouter frame (%zu SGI pipe record%s, dest=0x%08x src=0x%08x class=%u auth=%u pdata=%u)\n",
			       l1cmd, tx_len, pipe_records,
			       pipe_records == 1 ? "" : "s", dest_addr,
			       opts->src_addr, opts->ir_class,
			       opts->authority, opts->pdata);
	} else {
		if (write_one_transfer(fd, tx, tx_len)) {
			recover_l1_pipes_fd(fd, "L1 command write failure");
			goto out;
		}

		if (verbose)
			printf("sent L1 command '%s' in %zu-byte raw IRouter frame (dest=0x%08x src=0x%08x class=%u auth=%u pdata=%u)\n",
			       l1cmd, tx_len, dest_addr, opts->src_addr,
			       opts->ir_class, opts->authority, opts->pdata);
	}

	for (frame = 0; frame < SGIL1_IR_MAX_FRAMES; frame++) {
		int timeout = frame ? 500 : opts->timeout_ms;

		if (opts->pipe_records)
			ret = read_pipe_irouter_frame(fd, timeout, rx,
						      sizeof(rx), &rx_len);
		else
			ret = read_raw_irouter_frame(fd, timeout, rx,
						     sizeof(rx), &rx_len);
		if (ret < 0) {
			recover_l1_pipes_fd(fd, "L1 response read failure");
			ret = 1;
			goto out;
		}
		if (ret == 0) {
			if (!frame) {
				if (response_timeout_is_pending)
					fprintf(stderr,
						"timed out waiting for immediate response; checking command result\n");
				else
					fprintf(stderr,
						"timed out waiting for response\n");
			}
			if (!response_timeout_is_pending)
				recover_l1_pipes_fd(fd, "L1 response timeout");
			ret = frame ? 0 : SGIL1_L1CMD_RESPONSE_TIMEOUT;
			goto out;
		}

		if (verbose)
			printf("read %zu-byte IRouter frame\n", rx_len);

		if (!frame && rx_len >= 6) {
			expected_frames = rx[5] & 0x7f;
			if (!expected_frames)
				expected_frames = 1;
		}

		if (verbose)
			print_irouter_frame(rx, rx_len);

		if (collect_ir_text_frame(&assembly, rx, rx_len, frame == 0)) {
			ret = 1;
			goto out;
		}

		if (frame + 1 >= expected_frames) {
			if (assembly.buf) {
				if (assembly.got == assembly.expected) {
					if (out_text) {
						uint8_t *combined = assembly.buf;
						size_t combined_len = assembly.got;

						if (assembly.extra_len) {
							combined_len +=
								assembly.extra_len;
							combined = malloc(combined_len);
							if (!combined) {
								perror("malloc");
								ret = 1;
								goto out;
							}
							memcpy(combined, assembly.buf,
							       assembly.got);
							memcpy(combined + assembly.got,
							       assembly.extra,
							       assembly.extra_len);
						}
						*out_text = text_payload_to_string(
							combined, combined_len);
						if (combined != assembly.buf)
							free(combined);
						if (!*out_text) {
							ret = 1;
							goto out;
						}
					}
					if (verbose && expected_frames > 1) {
						printf("assembled arg%u text (%zu bytes):\n",
						       assembly.arg_index,
						       assembly.got);
						print_text_payload(assembly.buf,
								   assembly.got);
					}
				} else {
					fprintf(stderr,
						"incomplete assembled arg%u text: %zu/%zu bytes\n",
						assembly.arg_index, assembly.got,
						assembly.expected);
					ret = 1;
					goto out;
				}
			}
			if (out_text && !*out_text) {
				*out_text = empty_string();
				if (!*out_text) {
					ret = 1;
					goto out;
				}
			}
			ret = 0;
			goto out;
		}
	}

	fprintf(stderr, "stopped after %u response frames\n",
		SGIL1_IR_MAX_FRAMES);
	ret = 1;

out:
	free_ir_text_assembly(&assembly);
	close(fd);
	return ret;
}

static int do_l1_command(const struct options *opts, const char *l1cmd,
			 bool allow_destructive)
{
	char *text = NULL;
	int ret;

	ret = run_l1_command_core(opts, l1cmd, allow_destructive, opts->force,
				  false, false, opts->debug, &text);
	if (ret)
		return ret;

	if (!opts->debug)
		print_text_block(text);
	free(text);
	return 0;
}

static int do_raw_recv(const struct options *opts)
{
	int fd = open_data_device(opts, O_RDWR | O_NONBLOCK);
	int ret;

	if (fd < 0)
		return 1;

	ret = read_one_frame(fd, opts->timeout_ms);
	close(fd);
	return ret;
}

static int do_monitor(const struct options *opts)
{
	int fd = open_data_device(opts, O_RDWR | O_NONBLOCK);

	if (fd < 0)
		return 1;

	for (;;) {
		if (read_one_frame(fd, -1)) {
			close(fd);
			return 1;
		}
	}
}

static char *join_command_args_with_force(int argc, char **argv, int start,
					  bool *force)
{
	size_t len = 0;
	size_t parts = 0;
	char *joined;
	char *p;
	int i;

	for (i = start; i < argc; i++) {
		if (is_force_option(argv[i])) {
			*force = true;
			continue;
		}
		len += strlen(argv[i]) + (parts ? 1 : 0);
		parts++;
	}

	if (!parts)
		return NULL;

	joined = malloc(len + 1);
	if (!joined) {
		perror("malloc");
		return NULL;
	}

	p = joined;
	parts = 0;
	for (i = start; i < argc; i++) {
		size_t part_len;

		if (is_force_option(argv[i]))
			continue;

		part_len = strlen(argv[i]);
		if (parts)
			*p++ = ' ';
		memcpy(p, argv[i], part_len);
		p += part_len;
		parts++;
	}
	*p = '\0';

	return joined;
}

static char *first_l1_command_word(const char *cmd)
{
	const char *start;
	const char *end;
	char *word;
	size_t len;

	for (start = cmd; isspace((unsigned char)*start); start++)
		;
	for (end = start; *end && !isspace((unsigned char)*end); end++)
		;

	len = (size_t)(end - start);
	if (!len)
		return NULL;

	word = malloc(len + 1);
	if (!word) {
		perror("malloc");
		return NULL;
	}
	memcpy(word, start, len);
	word[len] = '\0';

	return word;
}

static const char *l1_broadcast_payload(const char *cmd)
{
	while (isspace((unsigned char)*cmd))
		cmd++;
	if (*cmd != '*')
		return NULL;
	cmd++;
	if (*cmd && !isspace((unsigned char)*cmd))
		return NULL;
	while (isspace((unsigned char)*cmd))
		cmd++;

	return cmd;
}

static bool l1_command_is_parent_only(const char *cmd, const char *word)
{
	size_t len;

	if (!cmd || !word)
		return false;

	while (isspace((unsigned char)*cmd))
		cmd++;
	len = strlen(word);
	if (strncasecmp(cmd, word, len))
		return false;
	cmd += len;
	while (isspace((unsigned char)*cmd))
		cmd++;

	return *cmd == '\0';
}

static bool command_word_matches(const char *text, size_t len,
				 const char *word)
{
	size_t i;

	if (strlen(word) != len)
		return false;

	for (i = 0; i < len; i++) {
		unsigned char a = (unsigned char)text[i];
		unsigned char b = (unsigned char)word[i];

		if (a >= 'A' && a <= 'Z')
			a = a - 'A' + 'a';
		if (b >= 'A' && b <= 'Z')
			b = b - 'A' + 'a';
		if (a != b)
			return false;
	}

	return true;
}

static bool is_help_word_char(char c)
{
	unsigned char uc = (unsigned char)c;

	return isalnum(uc) || c == '_' || c == '-';
}

static bool help_line_advertises_word(const char *line, size_t len,
				      const char *word)
{
	const char *p = line;
	const char *end = line + len;

	while (p < end && isspace((unsigned char)*p))
		p++;
	while (p < end && (*p == '*' || *p == '-')) {
		p++;
		while (p < end && isspace((unsigned char)*p))
			p++;
	}

	while (p < end) {
		const char *start;

		while (p < end && (*p == ',' || *p == '/' || *p == '|'))
			p++;
		start = p;
		while (p < end && is_help_word_char(*p))
			p++;
		if (p > start) {
			if (command_word_matches(start, (size_t)(p - start), word))
				return true;
			continue;
		}

		if (isspace((unsigned char)*p) || *p == ':' || *p == '[' ||
		    *p == '(')
			break;
		p++;
	}

	return false;
}

static bool help_line_is_blank(const char *line, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if (!isspace((unsigned char)line[i]))
			return false;
	}

	return true;
}

static bool help_line_contains_word(const char *line, size_t len,
				    const char *word)
{
	const char *p = line;
	const char *end = line + len;

	while (p < end) {
		const char *start;

		while (p < end && !is_help_word_char(*p))
			p++;
		start = p;
		while (p < end && is_help_word_char(*p))
			p++;
		if (p > start &&
		    command_word_matches(start, (size_t)(p - start), word))
			return true;
	}

	return false;
}

static bool l1_help_advertises_command(const char *help_text,
				       const char *cmd)
{
	const char *broadcast_payload = l1_broadcast_payload(cmd);
	char *word = first_l1_command_word(broadcast_payload ?
					   broadcast_payload : cmd);
	const char *line = help_text;
	bool in_command_list = false;
	bool advertised = false;

	if (!word)
		return false;

	while (*line) {
		const char *next = strchr(line, '\n');
		size_t len = next ? (size_t)(next - line) : strlen(line);

		if (contains_ci(line, "Commands are:")) {
			in_command_list = true;
		} else if (in_command_list && help_line_is_blank(line, len)) {
			in_command_list = false;
		} else if ((in_command_list &&
			    help_line_contains_word(line, len, word)) ||
			   (!in_command_list &&
			    help_line_advertises_word(line, len, word))) {
			advertised = true;
			break;
		}

		if (!next)
			break;
		line = next + 1;
	}

	free(word);
	return advertised;
}

static bool l1_text_indicates_failure(const char *text)
{
	return text && (contains_ci(text, "ERROR:") ||
			contains_ci(text, "command not found") ||
			contains_ci(text, "Invalid input string") ||
			contains_ci(text, "Check format"));
}

static bool l1_text_is_command_not_found(const char *text)
{
	static const char expected[] = "ERROR: command not found.";
	size_t len = sizeof(expected) - 1;

	if (!text)
		return false;
	while (isspace((unsigned char)*text))
		text++;
	if (strncasecmp(text, expected, len))
		return false;
	text += len;
	while (*text) {
		if (!isspace((unsigned char)*text))
			return false;
		text++;
	}

	return true;
}

static bool l1_help_text_is_valid(const char *text)
{
	return text && *text && !l1_text_indicates_failure(text);
}

static bool help_line_lists_child_command(const char *line, size_t len,
					  const char *parent)
{
	const char *p = line;
	const char *end = line + len;
	const char *start;

	while (p < end && isspace((unsigned char)*p))
		p++;
	while (p < end && (*p == '*' || *p == '-')) {
		p++;
		while (p < end && isspace((unsigned char)*p))
			p++;
	}

	start = p;
	while (p < end && is_help_word_char(*p))
		p++;
	if (p == start || !command_word_matches(start, (size_t)(p - start),
						parent))
		return false;
	if (p >= end || !isspace((unsigned char)*p))
		return false;
	while (p < end && isspace((unsigned char)*p))
		p++;

	return p < end && is_help_word_char(*p);
}

static bool l1_help_lists_child_command(const char *text, const char *parent)
{
	const char *line = text;

	if (!l1_help_text_is_valid(text) || !parent)
		return false;

	while (*line) {
		const char *next = strchr(line, '\n');
		size_t len = next ? (size_t)(next - line) : strlen(line);

		if (help_line_lists_child_command(line, len, parent))
			return true;
		if (!next)
			break;
		line = next + 1;
	}

	return false;
}

static char *l1_help_for_command(const struct options *opts, const char *cmd)
{
	const char *broadcast_payload = l1_broadcast_payload(cmd);
	char *word;
	char help_cmd[128];
	char *text;

	word = first_l1_command_word(broadcast_payload ? broadcast_payload : cmd);
	if (!word)
		return NULL;
	if (streq_ci(word, "help") || streq_ci(word, "hlp")) {
		free(word);
		return NULL;
	}
	if (snprintf(help_cmd, sizeof(help_cmd), "help %s", word) >=
	    (int)sizeof(help_cmd)) {
		free(word);
		return NULL;
	}

	text = l1_text_command(opts, help_cmd, false);
	free(word);
	return text;
}

static int do_l1_pass_through_command(const struct options *opts,
				      const char *l1cmd, bool force)
{
	const char *broadcast_payload = l1_broadcast_payload(l1cmd);
	const char *lookup_cmd = broadcast_payload ? broadcast_payload : l1cmd;
	char *help_text = NULL;
	char *text = NULL;
	char *command_help = NULL;
	char *command_word = NULL;
	bool l1_failed;
	bool advertised_by_help = false;
	bool suppress_failure_text = false;
	bool parent_only = false;
	int help_ret;
	int ret;

	if (broadcast_payload && !*broadcast_payload) {
		fprintf(stderr,
			"refusing L1 command '*': '*' is a broadcast prefix, not a complete command; use 'l1cmd * <command>'\n");
		return 2;
	}
	command_word = first_l1_command_word(lookup_cmd);
	parent_only = l1_command_is_parent_only(lookup_cmd, command_word);

	if (!force && !streq_ci(l1cmd, "help")) {
		help_ret = l1_text_command_status(opts, "help", false,
						  &help_text);
		if (help_ret) {
			if (help_ret == SGIL1_L1CMD_DISCOVERY_TIMEOUT ||
			    help_ret == SGIL1_L1CMD_RESPONSE_TIMEOUT)
				fprintf(stderr,
					"could not retrieve live L1 help after a USB timeout; command '%s' was not sent\n",
					l1cmd);
			else
				fprintf(stderr,
					"could not retrieve live L1 help; command '%s' was not sent\n",
					l1cmd);
			free(command_word);
			return 1;
		}
		if (!help_text) {
			fprintf(stderr,
				"could not retrieve live L1 help; command '%s' was not sent\n",
				l1cmd);
			free(command_word);
			return 1;
		}
		if (!l1_help_advertises_command(help_text, l1cmd)) {
			fprintf(stderr,
				"refusing L1 command '%s': not advertised by live L1 help; add --force to send anyway\n",
				l1cmd);
			free(help_text);
			free(command_word);
			return 2;
		}
		advertised_by_help = true;
		free(help_text);
	}

	ret = run_l1_command_core(opts, l1cmd, true, true, true, false,
				  opts->debug, &text);
	if (ret) {
		free(command_word);
		return ret;
	}

	l1_failed = l1_text_indicates_failure(text);
	if (l1_failed) {
		if (advertised_by_help && parent_only &&
		    l1_text_is_command_not_found(text)) {
			command_help = l1_help_for_command(opts, l1cmd);
			suppress_failure_text =
				l1_help_lists_child_command(command_help,
							    command_word);
		}
		if (!suppress_failure_text && !opts->debug)
			print_text_block(text);
		if (suppress_failure_text) {
			printf("Help for '%s':\n", l1cmd);
			print_text_block(command_help);
		}
	} else if (!opts->debug) {
		print_text_block(text);
	}
	free(command_help);
	free(text);
	free(command_word);
	return l1_failed ? 2 : 0;
}

static int do_l1_pass_through_args(const struct options *opts, int argc,
				   char **argv, int start)
{
	char *l1cmd;
	bool force = opts->force;
	int ret;

	l1cmd = join_command_args_with_force(argc, argv, start, &force);
	if (!l1cmd) {
		fprintf(stderr, "L1 pass-through command text is empty\n");
		return 2;
	}

	ret = do_l1_pass_through_command(opts, l1cmd, force);
	free(l1cmd);
	return ret;
}

static int do_build_l1cmd_args(const struct options *opts, int argc, char **argv,
			       int start)
{
	uint8_t frame[SGIL1_IO_SIZE];
	size_t frame_len = 0;
	char *l1cmd;
	bool force = opts->force;
	int ret = 0;

	l1cmd = join_command_args_with_force(argc, argv, start, &force);
	if (!l1cmd) {
		fprintf(stderr, "build-l1cmd needs command text\n");
		return 2;
	}

	if (!l1_command_is_read_only(l1cmd) &&
	    !l1_command_sets_time(l1cmd) && !l1_command_is_destructive(l1cmd) &&
	    !force) {
		fprintf(stderr,
			"refusing L1 command '%s': command is not allowlisted; add --force to build it anyway\n",
			l1cmd);
		free(l1cmd);
		return 2;
	}
	if (l1_command_sets_time(l1cmd) && !force) {
		fprintf(stderr,
			"refusing L1 command '%s': add --force to confirm L1 clock change\n",
			l1cmd);
		free(l1cmd);
		return 2;
	}
	if (l1_command_is_destructive(l1cmd) && !force) {
		fprintf(stderr,
			"refusing L1 command '%s': add --force to confirm workstation power/reset action\n",
			l1cmd);
		free(l1cmd);
		return 2;
	}

	if (build_l1_command_frame(l1cmd, 0, opts->dest_addr, opts->src_addr,
				   opts->ir_class, opts->authority, opts->pdata,
				   frame, sizeof(frame), &frame_len)) {
		free(l1cmd);
		return 1;
	}

	printf("L1 command '%s' frame (%zu bytes, dest=0x%08x src=0x%08x class=%u auth=%u pdata=%u):\n",
	       l1cmd, frame_len, opts->dest_addr, opts->src_addr, opts->ir_class,
	       opts->authority, opts->pdata);
	hexdump(frame, frame_len);
	free(l1cmd);
	return ret;
}

static int do_power_command(const struct options *opts, int argc, char **argv,
			    int command_index)
{
	if (command_index + 1 == argc)
		return do_l1_command(opts, "power", false);
	if (command_index + 2 == argc && streq_ci(argv[command_index + 1],
						  "check"))
		return do_l1_command(opts, "power check", false);
	if (command_index + 2 == argc && streq_ci(argv[command_index + 1],
						  "vrm"))
		return do_l1_command(opts, "power vrm", false);
	if (command_index + 2 <= argc &&
	    (streq_ci(argv[command_index + 1], "up") ||
	     streq_ci(argv[command_index + 1], "down"))) {
		bool force = opts->force;
		int i;

		for (i = command_index + 2; i < argc; i++) {
			if (is_force_option(argv[i])) {
				force = true;
			} else {
				fprintf(stderr, "unknown argument for power %s: %s\n",
					argv[command_index + 1], argv[i]);
				return 2;
			}
		}

		if (streq_ci(argv[command_index + 1], "up"))
			return do_power_up_confirmed(opts, force);
		return do_power_down_confirmed(opts, force);
	}

	fprintf(stderr,
		"unknown power subcommand; use 'power check', 'power vrm', 'power up', 'power down', or 'l1cmd power ...'\n");
	return 2;
}

static int trailing_force_only(int argc, char **argv, int command_index,
			       bool *force)
{
	int i;

	for (i = command_index + 1; i < argc; i++) {
		if (is_force_option(argv[i])) {
			*force = true;
		} else {
			fprintf(stderr, "unknown argument for %s: %s\n",
				argv[command_index], argv[i]);
			return -1;
		}
	}

	return 0;
}

static const char *find_existing_data_device(const struct options *opts)
{
	static char auto_path[PATH_MAX];

	if (opts->device)
		return path_exists(opts->device) ? opts->device : NULL;

	return find_auto_data_device(auto_path, sizeof(auto_path));
}

static int parse_int_arg(const char *text, int min, int max, int *value)
{
	char *end = NULL;
	long parsed;

	errno = 0;
	parsed = strtol(text, &end, 0);
	if (errno || !end || *end || parsed < min || parsed > max)
		return -1;

	*value = (int)parsed;
	return 0;
}

struct tz_state {
	int isdst;
	long gmtoff;
	char zone[32];
};

struct tz_transition {
	time_t when;
	struct tz_state before;
	struct tz_state after;
	struct tm before_local;
};

static int save_tz(char **saved, bool *had_tz)
{
	const char *old = getenv("TZ");

	*had_tz = old;
	*saved = old ? strdup(old) : NULL;
	if (old && !*saved) {
		perror("strdup");
		return -1;
	}

	return 0;
}

static void restore_tz(char *saved, bool had_tz)
{
	if (had_tz)
		setenv("TZ", saved, 1);
	else
		unsetenv("TZ");
	tzset();
	free(saved);
}

static int set_tz_for_probe(const char *timezone)
{
	if (timezone) {
		if (setenv("TZ", timezone, 1))
			return -1;
	} else {
		if (unsetenv("TZ"))
			return -1;
	}
	tzset();
	return 0;
}

static time_t utc_year_start(int year)
{
	struct tm tm = { 0 };

	tm.tm_year = year - 1900;
	tm.tm_mon = 0;
	tm.tm_mday = 1;
	tm.tm_isdst = 0;
	return timegm(&tm);
}

static int read_tz_state(time_t when, struct tz_state *state)
{
	struct tm tm;
	const char *zone;

	if (!localtime_r(&when, &tm))
		return -1;

	zone = tm.tm_zone ? tm.tm_zone : "";
	state->isdst = tm.tm_isdst > 0;
	state->gmtoff = tm.tm_gmtoff;
	if (snprintf(state->zone, sizeof(state->zone), "%s", zone) >=
	    (int)sizeof(state->zone))
		return -1;

	return 0;
}

static bool tz_state_same(const struct tz_state *a, const struct tz_state *b)
{
	return a->isdst == b->isdst && a->gmtoff == b->gmtoff &&
	       !strcmp(a->zone, b->zone);
}

static int find_transition_second(time_t lo, time_t hi,
				  const struct tz_state *before,
				  time_t *transition)
{
	while (hi - lo > 1) {
		time_t mid = lo + ((hi - lo) / 2);
		struct tz_state state;

		if (read_tz_state(mid, &state))
			return -1;
		if (tz_state_same(&state, before))
			lo = mid;
		else
			hi = mid;
	}

	*transition = hi;
	return 0;
}

static int days_in_month(int year, int month)
{
	static const int days[] = {
		31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
	};

	if (month == 2 &&
	    ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
		return 29;
	return days[month - 1];
}

static int format_posix_tz_name(const char *name, char *buf, size_t cap)
{
	bool bare = strlen(name) >= 3;
	const char *p;

	for (p = name; *p; p++) {
		if (!isalpha((unsigned char)*p))
			bare = false;
		if (*p == '>')
			return -1;
	}

	if (bare) {
		if (snprintf(buf, cap, "%s", name) >= (int)cap)
			return -1;
	} else {
		if (snprintf(buf, cap, "<%s>", name) >= (int)cap)
			return -1;
	}

	return 0;
}

static int format_posix_tz_offset(long gmtoff, char *buf, size_t cap)
{
	long offset = -gmtoff;
	const char *sign = "";
	long hour;
	long min;
	long sec;

	if (offset < 0) {
		sign = "-";
		offset = -offset;
	}

	hour = offset / 3600;
	min = (offset % 3600) / 60;
	sec = offset % 60;

	if (sec) {
		if (snprintf(buf, cap, "%s%ld:%02ld:%02ld", sign, hour,
			     min, sec) >= (int)cap)
			return -1;
	} else if (min) {
		if (snprintf(buf, cap, "%s%ld:%02ld", sign, hour, min) >=
		    (int)cap)
			return -1;
	} else {
		if (snprintf(buf, cap, "%s%ld", sign, hour) >= (int)cap)
			return -1;
	}

	return 0;
}

static int format_posix_rule_time(int seconds, char *buf, size_t cap)
{
	int hour = seconds / 3600;
	int min = (seconds % 3600) / 60;
	int sec = seconds % 60;

	if (sec) {
		if (snprintf(buf, cap, "%d:%02d:%02d", hour, min, sec) >=
		    (int)cap)
			return -1;
	} else if (min) {
		if (snprintf(buf, cap, "%d:%02d", hour, min) >= (int)cap)
			return -1;
	} else {
		if (snprintf(buf, cap, "%d", hour) >= (int)cap)
			return -1;
	}

	return 0;
}

static int format_posix_transition_rule(const struct tm *local,
					char *buf, size_t cap)
{
	char timebuf[32];
	int year = local->tm_year + 1900;
	int month = local->tm_mon + 1;
	int week = ((local->tm_mday - 1) / 7) + 1;
	int seconds = local->tm_hour * 3600 + local->tm_min * 60 +
		      local->tm_sec;

	if (local->tm_mday + 7 > days_in_month(year, month))
		week = 5;

	if (format_posix_rule_time(seconds, timebuf, sizeof(timebuf)))
		return -1;
	if (snprintf(buf, cap, "M%d.%d.%d/%s", month, week, local->tm_wday,
		     timebuf) >= (int)cap)
		return -1;

	return 0;
}

static int format_derived_posix_tz(const struct tz_state *std,
				   const struct tz_state *dst,
				   const struct tm *dst_start,
				   const struct tm *dst_end,
				   char *buf, size_t cap)
{
	char std_name[40];
	char dst_name[40];
	char std_off[24];
	char dst_off[24];
	char start_rule[40];
	char end_rule[40];
	int ret;

	if (!*std->zone || !*dst->zone)
		return -1;
	if (format_posix_tz_name(std->zone, std_name, sizeof(std_name)) ||
	    format_posix_tz_name(dst->zone, dst_name, sizeof(dst_name)) ||
	    format_posix_tz_offset(std->gmtoff, std_off, sizeof(std_off)) ||
	    format_posix_transition_rule(dst_start, start_rule,
					 sizeof(start_rule)) ||
	    format_posix_transition_rule(dst_end, end_rule, sizeof(end_rule)))
		return -1;

	if (dst->gmtoff == std->gmtoff + 3600) {
		ret = snprintf(buf, cap, "%s%s%s,%s,%s", std_name, std_off,
			       dst_name, start_rule, end_rule);
	} else {
		if (format_posix_tz_offset(dst->gmtoff, dst_off,
					   sizeof(dst_off)))
			return -1;
		ret = snprintf(buf, cap, "%s%s%s%s,%s,%s", std_name,
			       std_off, dst_name, dst_off, start_rule,
			       end_rule);
	}

	return ret >= (int)cap ? -1 : 0;
}

static int derive_posix_timezone(const char *timezone, char *buf, size_t cap)
{
	struct tz_transition transitions[8];
	struct tz_state prev;
	struct tz_state std_state = { 0 };
	struct tz_state dst_state = { 0 };
	struct tm dst_start = { 0 };
	struct tm dst_end = { 0 };
	char *saved = NULL;
	bool had_tz = false;
	bool have_start = false;
	bool have_end = false;
	bool have_std = false;
	bool have_dst = false;
	time_t now;
	time_t start;
	time_t end;
	time_t t;
	int transition_count = 0;
	int year;
	struct tm now_tm;
	int ret = -1;

	now = time(NULL);
	if (now == (time_t)-1 || !localtime_r(&now, &now_tm))
		return -1;
	year = now_tm.tm_year + 1900;

	if (save_tz(&saved, &had_tz))
		return -1;
	if (set_tz_for_probe(timezone))
		goto out;

	start = utc_year_start(year);
	end = utc_year_start(year + 1);
	if (start == (time_t)-1 || end == (time_t)-1 ||
	    read_tz_state(start, &prev))
		goto out;

	for (t = start + 3600; t <= end; t += 3600) {
		struct tz_state state;

		if (read_tz_state(t, &state))
			goto out;
		if (!tz_state_same(&state, &prev)) {
			time_t transition;
			time_t local_before;

			if (transition_count >= (int)ARRAY_SIZE(transitions) ||
			    find_transition_second(t - 3600, t, &prev,
						   &transition))
				goto out;

			transitions[transition_count].when = transition;
			transitions[transition_count].before = prev;
			if (read_tz_state(transition,
					  &transitions[transition_count].after))
				goto out;
			local_before = transition + prev.gmtoff;
			if (!gmtime_r(&local_before,
				      &transitions[transition_count].before_local))
				goto out;
			prev = transitions[transition_count].after;
			transition_count++;
		} else {
			prev = state;
		}
	}

	if (transition_count != 2)
		goto out;

	for (int i = 0; i < transition_count; i++) {
		struct tz_transition *tr = &transitions[i];

		if (!tr->before.isdst && tr->after.isdst) {
			std_state = tr->before;
			dst_state = tr->after;
			dst_start = tr->before_local;
			have_std = true;
			have_dst = true;
			have_start = true;
		} else if (tr->before.isdst && !tr->after.isdst) {
			dst_state = tr->before;
			std_state = tr->after;
			dst_end = tr->before_local;
			have_dst = true;
			have_std = true;
			have_end = true;
		}
	}

	if (!have_std || !have_dst || !have_start || !have_end)
		goto out;

	ret = format_derived_posix_tz(&std_state, &dst_state, &dst_start,
				      &dst_end, buf, cap);

out:
	restore_tz(saved, had_tz);
	return ret;
}

static int derive_fixed_posix_timezone(const char *timezone, char *buf, size_t cap)
{
	struct tz_state state;
	char name[40];
	char offset[24];
	char *saved = NULL;
	bool had_tz = false;
	time_t now;
	int ret = -1;

	now = time(NULL);
	if (now == (time_t)-1)
		return -1;
	if (save_tz(&saved, &had_tz))
		return -1;
	if (set_tz_for_probe(timezone))
		goto out;
	if (read_tz_state(now, &state))
		goto out;
	if (!*state.zone)
		goto out;
	if (format_posix_tz_name(state.zone, name, sizeof(name)) ||
	    format_posix_tz_offset(state.gmtoff, offset, sizeof(offset)))
		goto out;
	if (snprintf(buf, cap, "%s%s", name, offset) >= (int)cap)
		goto out;

	ret = 0;

out:
	restore_tz(saved, had_tz);
	return ret;
}

static bool timezone_looks_like_zoneinfo(const char *timezone)
{
	return timezone[0] == ':' || timezone[0] == '/' ||
	       strchr(timezone, '/') || streq_ci(timezone, "local");
}

static bool expand_timezone_spec(const char *timezone, char *buf, size_t cap)
{
	char local[SGIL1_TZ_MAX];
	const char *comma;
	size_t prefix_len;

	if (strchr(timezone, ','))
		return false;

	if (timezone_looks_like_zoneinfo(timezone) &&
	    !derive_posix_timezone(streq_ci(timezone, "local") ? NULL : timezone,
				   buf, cap))
		return true;

	if (derive_posix_timezone(NULL, local, sizeof(local)))
		return false;

	comma = strchr(local, ',');
	if (!comma)
		return false;
	prefix_len = (size_t)(comma - local);
	if (strlen(timezone) != prefix_len ||
	    strncmp(timezone, local, prefix_len))
		return false;

	if (snprintf(buf, cap, "%s", local) >= (int)cap)
		return false;
	return true;
}

static void set_status_timezone(struct status_options *status,
				const char *timezone)
{
	if (expand_timezone_spec(timezone, status->timezone_buf,
				 sizeof(status->timezone_buf)))
		status->timezone = status->timezone_buf;
	else
		status->timezone = timezone;
}

static void set_status_host_timezone(struct status_options *status)
{
	const char *tz;

	if (status->timezone)
		return;

	tz = getenv("TZ");
	if (tz && *tz) {
		set_status_timezone(status, tz);
		return;
	}

	if (!derive_posix_timezone(NULL, status->timezone_buf,
				   sizeof(status->timezone_buf)) ||
	    !derive_fixed_posix_timezone(NULL, status->timezone_buf,
					 sizeof(status->timezone_buf))) {
		status->timezone = status->timezone_buf;
		return;
	}

	fprintf(stderr,
		"warning: could not derive host timezone; L1 timezone will not be changed\n");
}


static int parse_status_args(int argc, char **argv, int start,
			     struct status_options *status)
{
	int i;

	memset(status, 0, sizeof(*status));
	status->drift_seconds = SGIL1_DEFAULT_TIME_DRIFT_SEC;

	for (i = start; i < argc; i++) {
		if (!strcmp(argv[i], "--set-time")) {
			status->set_time = true;
		} else if (!strcmp(argv[i], "--timezone")) {
			if (++i >= argc) {
				fprintf(stderr, "--timezone needs a POSIX timezone string\n");
				return -1;
			}
			set_status_timezone(status, argv[i]);
		} else if (!strcmp(argv[i], "--time-drift") ||
			   !strcmp(argv[i], "--drift-seconds")) {
			if (++i >= argc) {
				fprintf(stderr, "%s needs seconds\n", argv[i - 1]);
				return -1;
			}
			if (parse_int_arg(argv[i], 0, INT_MAX,
					  &status->drift_seconds)) {
				fprintf(stderr, "invalid %s value\n", argv[i - 1]);
				return -1;
			}
		} else {
			fprintf(stderr, "unknown status option: %s\n", argv[i]);
			return -1;
		}
	}

	if (status->set_time && !status->timezone)
		set_status_host_timezone(status);

	return 0;
}

static int parse_wait_args(int argc, char **argv, int start,
			   struct wait_options *wait)
{
	int i;

	memset(wait, 0, sizeof(*wait));
	wait->status.drift_seconds = SGIL1_DEFAULT_TIME_DRIFT_SEC;
	wait->wait_timeout_seconds = -1;
	wait->keepalive_seconds = SGIL1_DEFAULT_KEEPALIVE_SEC;

	for (i = start; i < argc; i++) {
		if (!strcmp(argv[i], "--set-time")) {
			wait->status.set_time = true;
		} else if (!strcmp(argv[i], "--timezone")) {
			if (++i >= argc) {
				fprintf(stderr, "--timezone needs a POSIX timezone string\n");
				return -1;
			}
			set_status_timezone(&wait->status, argv[i]);
		} else if (!strcmp(argv[i], "--time-drift") ||
			   !strcmp(argv[i], "--drift-seconds")) {
			if (++i >= argc) {
				fprintf(stderr, "%s needs seconds\n", argv[i - 1]);
				return -1;
			}
			if (parse_int_arg(argv[i], 0, INT_MAX,
					  &wait->status.drift_seconds)) {
				fprintf(stderr, "invalid %s value\n", argv[i - 1]);
				return -1;
			}
		} else if (!strcmp(argv[i], "--power-up")) {
			wait->power_up = true;
		} else if (!strcmp(argv[i], "--power-down")) {
			wait->power_down = true;
		} else if (!strcmp(argv[i], "--reset")) {
			wait->reset = true;
		} else if (is_force_option(argv[i])) {
			wait->force = true;
		} else if (!strcmp(argv[i], "--background") ||
			   !strcmp(argv[i], "--defer") ||
			   !strcmp(argv[i], "--deferred") ||
			   !strcmp(argv[i], "--next-bind")) {
			wait->background = true;
		} else if (!strcmp(argv[i], "--wait-timeout")) {
			if (++i >= argc) {
				fprintf(stderr, "--wait-timeout needs seconds\n");
				return -1;
			}
			if (parse_int_arg(argv[i], -1, INT_MAX,
					  &wait->wait_timeout_seconds)) {
				fprintf(stderr, "invalid --wait-timeout value\n");
				return -1;
			}
		} else if (!strcmp(argv[i], "--keepalive") ||
			   !strcmp(argv[i], "--interval")) {
			if (++i >= argc) {
				fprintf(stderr, "%s needs seconds\n", argv[i - 1]);
				return -1;
			}
			if (parse_int_arg(argv[i], 0, INT_MAX,
					  &wait->keepalive_seconds)) {
				fprintf(stderr, "invalid keepalive interval\n");
				return -1;
			}
		} else {
			fprintf(stderr, "unknown wait option: %s\n", argv[i]);
			return -1;
		}
	}

	if ((wait->power_up ? 1 : 0) + (wait->power_down ? 1 : 0) +
		    (wait->reset ? 1 : 0) >
	    1) {
		fprintf(stderr,
			"wait accepts only one power action: --power-up, --power-down, or --reset\n");
		return -1;
	}
	if (wait->status.set_time && !wait->status.timezone)
		set_status_host_timezone(&wait->status);

	return 0;
}

static int l1_text_command_status(const struct options *opts, const char *cmd,
				  bool allow_time_setting, char **text)
{
	return run_l1_command_core(opts, cmd, false, allow_time_setting, false,
				   false, false, text);
}

static char *l1_text_command(const struct options *opts, const char *cmd,
			     bool allow_time_setting)
{
	char *text = NULL;

	if (l1_text_command_status(opts, cmd, allow_time_setting, &text))
		return NULL;

	return text;
}

static void print_text_block(const char *text)
{
	if (text && *text)
		printf("%s", text);
	else
		printf("(no output)\n");
	if (!text || !*text || text[strlen(text) - 1] != '\n')
		putchar('\n');
}

static int parse_l1_time(const char *text, time_t *when)
{
	const char *p;

	for (p = text; *p; p++) {
		unsigned int month;
		unsigned int day;
		unsigned int year;
		unsigned int hour;
		unsigned int minute;
		unsigned int second;
		struct tm tm;

		if (!isdigit((unsigned char)*p))
			continue;

		if (sscanf(p, "%u/%u/%u %u:%u:%u", &month, &day, &year,
			   &hour, &minute, &second) != 6)
			continue;
		if (month < 1 || month > 12 || day < 1 || day > 31 ||
		    hour > 23 || minute > 59 || second > 60 || year < 1970)
			continue;

		memset(&tm, 0, sizeof(tm));
		tm.tm_year = (int)year - 1900;
		tm.tm_mon = (int)month - 1;
		tm.tm_mday = (int)day;
		tm.tm_hour = (int)hour;
		tm.tm_min = (int)minute;
		tm.tm_sec = (int)second;
		tm.tm_isdst = -1;

		*when = mktime(&tm);
		return *when == (time_t)-1 ? -1 : 0;
	}

	return -1;
}

static void format_local_time(time_t when, char *buf, size_t len)
{
	struct tm tm;

	if (!localtime_r(&when, &tm)) {
		snprintf(buf, len, "(unavailable)");
		return;
	}

	if (!strftime(buf, len, "%m/%d/%Y %H:%M:%S %Z", &tm))
		snprintf(buf, len, "(unavailable)");
}

static int make_l1_date_command(char *buf, size_t len)
{
	time_t now = time(NULL);
	struct tm tm;

	if (now == (time_t)-1 || !localtime_r(&now, &tm))
		return -1;

	if (snprintf(buf, len, "date %02d%02d%02d%02d%04d.%02d",
		     tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
		     tm.tm_year + 1900, tm.tm_sec) >= (int)len)
		return -1;

	return 0;
}

static int maybe_set_l1_time(const struct options *opts,
			     const struct status_options *status,
			     const char *date_text)
{
	char command[128];
	time_t l1_time;
	time_t host_time;
	double drift;
	char *response;
	int changed = 0;

	if (!status->set_time)
		return 0;

	if (status->timezone) {
		if (snprintf(command, sizeof(command), "date tz %s",
			     status->timezone) >= (int)sizeof(command)) {
			fprintf(stderr, "timezone command is too long\n");
			return 1;
		}
		response = l1_text_command(opts, command, true);
		if (!response)
			return 1;
		free(response);
		printf("Clock: set L1 timezone to %s\n", status->timezone);
		changed = 2;
	}

	host_time = time(NULL);
	if (host_time == (time_t)-1)
		return 1;

	if (parse_l1_time(date_text, &l1_time)) {
		drift = (double)status->drift_seconds + 1.0;
	} else {
		drift = difftime(host_time, l1_time);
		if (drift < 0)
			drift = -drift;
	}

	if (drift <= status->drift_seconds) {
		printf("Clock: drift %.0f seconds, not setting L1 time\n", drift);
		return changed;
	}

	if (make_l1_date_command(command, sizeof(command))) {
		fprintf(stderr, "failed to format host time for L1\n");
		return 1;
	}

	response = l1_text_command(opts, command, true);
	if (!response)
		return 1;
	free(response);
	printf("Clock: set L1 time from host because drift was %.0f seconds\n",
	       drift);
	return 2;
}

static int parse_mac_from_serial(const char *text, uint8_t mac[6])
{
	const char *p = strstr(text, "SSN:");
	unsigned int values[6];
	int i;

	if (!p)
		return -1;

	p += 4;
	while (*p && isspace((unsigned char)*p))
		p++;

	if (sscanf(p, "%2x:%2x:%2x:%2x:%2x:%2x", &values[0], &values[1],
		   &values[2], &values[3], &values[4], &values[5]) != 6)
		return -1;

	for (i = 0; i < 6; i++) {
		if (values[i] > 0xff)
			return -1;
		mac[i] = (uint8_t)values[i];
	}

	return 0;
}

static void print_mac_status(const char *serial_text)
{
	uint8_t mac[6];

	if (parse_mac_from_serial(serial_text, mac))
		return;

	if (!(mac[0] & 0x03))
		return;

	printf("MAC: %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1],
	       mac[2], mac[3], mac[4], mac[5]);
	printf("MAC multicast bit: %s\n", (mac[0] & 0x01) ? "set" : "clear");
	printf("MAC locally-administered bit: %s\n",
	       (mac[0] & 0x02) ? "set" : "clear");
	printf("MAC classification: %s %s\n",
	       (mac[0] & 0x01) ? "multicast" : "unicast",
	       (mac[0] & 0x02) ? "locally administered" :
				  "globally administered");
}

static int prepare_command_options(const struct options *opts,
				   struct options *cmd_opts)
{
	uint32_t dest_addr;

	*cmd_opts = *opts;
	if (opts->pipe_records || opts->no_discover || opts->dest_overridden)
		return 0;

	if (discover_l1_command_dest(opts, &dest_addr, false))
		return 1;

	cmd_opts->dest_addr = dest_addr;
	cmd_opts->dest_overridden = true;
	return 0;
}

static int print_clock_report(const struct options *cmd_opts,
			      const struct status_options *status)
{
	char host_buf[64];
	char *date = NULL;
	char *timezone = NULL;
	int ret = 0;

	date = l1_text_command(cmd_opts, "date", false);
	timezone = l1_text_command(cmd_opts, "date tz", false);

	printf("Clock\n");
	if (date) {
		time_t l1_time;
		time_t host_time = time(NULL);

		printf("L1: ");
		print_text_block(date);
		if (host_time != (time_t)-1) {
			format_local_time(host_time, host_buf, sizeof(host_buf));
			printf("Host: %s\n", host_buf);
		}
		if (!parse_l1_time(date, &l1_time) &&
		    host_time != (time_t)-1) {
			double drift = difftime(host_time, l1_time);

			if (drift < 0)
				drift = -drift;
			printf("Drift: %.0f seconds\n", drift);
		} else {
			printf("Drift: unavailable\n");
		}
			{
				int time_ret = maybe_set_l1_time(cmd_opts, status, date);

				if (time_ret == 2) {
					char *updated = l1_text_command(cmd_opts,
									"date",
									false);

					if (updated) {
						printf("Updated L1 clock reading:\n");
						print_text_block(updated);
						free(updated);
					}
				} else if (time_ret) {
					ret = 1;
				}
			}
	} else {
		printf("(date unavailable)\n");
		ret = 1;
	}
	printf("Timezone: ");
	if (timezone)
		print_text_block(timezone);
	else {
		printf("unavailable\n");
		ret = 1;
	}

	free(date);
	free(timezone);
	return ret;
}

static int do_clock(const struct options *opts,
		    const struct status_options *status)
{
	struct options cmd_opts;

	if (prepare_command_options(opts, &cmd_opts))
		return 1;

	return print_clock_report(&cmd_opts, status);
}

static int do_consolidated_status(const struct options *opts,
				  const struct status_options *status)
{
	struct options cmd_opts;
	char *version = NULL;
	char *serial = NULL;
	char *usb = NULL;
	char *power_check = NULL;
	char *env = NULL;
	int ret = 0;

	if (prepare_command_options(opts, &cmd_opts))
		return 1;

	version = l1_text_command(&cmd_opts, "version", false);
	serial = l1_text_command(&cmd_opts, "serial", false);
	power_check = l1_text_command(&cmd_opts, "power check", false);
	env = l1_text_command(&cmd_opts, "env", false);
	usb = l1_text_command(&cmd_opts, "usb", false);

	printf("Firmware\n");
	if (version)
		print_text_block(version);
	else
		ret = 1;

	printf("\nIdentity\n");
	if (serial) {
		print_text_block(serial);
		print_mac_status(serial);
	} else {
		printf("(serial unavailable)\n");
		ret = 1;
	}

	printf("\n");
	if (print_clock_report(&cmd_opts, status))
		ret = 1;

	printf("\nPower State\n");
	if (power_check)
		print_text_block(power_check);
	else
		ret = 1;

	printf("\nEnvironment\n");
	if (env)
		print_text_block(env);
	else
		ret = 1;

	printf("\nUSB Transport\n");
	if (usb)
		print_text_block(usb);
	else
		ret = 1;

	free(version);
	free(serial);
	free(usb);
	free(power_check);
	free(env);
	return ret;
}

static bool wait_event_may_remove_l1(const struct inotify_event *event,
				     int wd_dev, int wd_sgil1)
{
	if (!(event->mask & (IN_DELETE | IN_MOVED_FROM | IN_DELETE_SELF |
			     IN_MOVE_SELF)))
		return false;

	if (event->wd == wd_sgil1)
		return true;
	if (event->wd != wd_dev)
		return false;
	if (!event->len)
		return true;

	return !strcmp(event->name, "sgi-l1") ||
	       !strncmp(event->name, "sgil1_", strlen("sgil1_")) ||
	       !strcmp(event->name, "usb");
}

static int add_sgil1_watch(int fd)
{
	return inotify_add_watch(fd, "/dev/sgi-l1",
				 IN_CREATE | IN_MOVED_TO | IN_MOVED_FROM |
					 IN_ATTRIB | IN_DELETE |
					 IN_DELETE_SELF | IN_MOVE_SELF);
}

static int wait_for_data_device(const struct options *opts,
				int timeout_seconds, bool background)
{
	int fd;
	int wd_dev = -1;
	int wd_sgil1 = -1;
	time_t start = time(NULL);
	bool reported = false;
	bool seen_absence = find_existing_data_device(opts) == NULL;

	if (!background && !seen_absence)
		return 0;

	fd = inotify_init1(IN_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "inotify_init1 failed: %s\n", strerror(errno));
		return 1;
	}

	wd_dev = inotify_add_watch(fd, "/dev",
				   IN_CREATE | IN_MOVED_TO | IN_MOVED_FROM |
					   IN_ATTRIB | IN_DELETE |
					   IN_DELETE_SELF | IN_MOVE_SELF);
	if (wd_dev < 0) {
		fprintf(stderr, "failed to watch /dev: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	if (path_exists("/dev/sgi-l1"))
		wd_sgil1 = add_sgil1_watch(fd);

	for (;;) {
		struct pollfd pfd = {
			.fd = fd,
			.events = POLLIN,
		};
		int poll_timeout = -1;
		int pret;
		char events[4096]
			__attribute__((aligned(__alignof__(struct inotify_event))));
		ssize_t len;
		ssize_t off = 0;
		bool present = find_existing_data_device(opts) != NULL;

		if (background) {
			if (seen_absence && present) {
				close(fd);
				return 0;
			}
			if (!seen_absence && !present) {
				printf("SGI L1 USB device disappeared; waiting for next bind\n");
				fflush(stdout);
				seen_absence = true;
				reported = false;
			}
		} else if (present) {
			close(fd);
			return 0;
		}

		if (!reported) {
			const char *path = opts->device ? opts->device :
				"/dev/sgi-l1/l1-* or /dev/sgil1_*";

			if (background && !seen_absence)
				printf("SGI L1 USB device already present at %s; waiting for disconnect before next bind\n",
				       path);
			else
				printf("waiting for SGI L1 USB device at %s\n",
				       path);
			fflush(stdout);
			reported = true;
		}

		if (timeout_seconds >= 0) {
			time_t now = time(NULL);
			int elapsed = now == (time_t)-1 || start == (time_t)-1 ?
				      0 : (int)(now - start);
			int remaining = timeout_seconds - elapsed;

			if (remaining <= 0) {
				fprintf(stderr, "timed out waiting for SGI L1 USB device\n");
				close(fd);
				return 2;
			}
			poll_timeout = remaining * 1000;
		}

		pret = poll(&pfd, 1, poll_timeout);
		if (pret < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "poll failed: %s\n", strerror(errno));
			close(fd);
			return 1;
		}
		if (pret == 0) {
			fprintf(stderr, "timed out waiting for SGI L1 USB device\n");
			close(fd);
			return 2;
		}

		len = read(fd, events, sizeof(events));
		if (len < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			fprintf(stderr, "inotify read failed: %s\n",
				strerror(errno));
			close(fd);
			return 1;
		}

		while (off < len) {
			const struct inotify_event *event =
				(const struct inotify_event *)(events + off);

			if (background && !seen_absence &&
			    wait_event_may_remove_l1(event, wd_dev, wd_sgil1)) {
				seen_absence = true;
				reported = false;
			}
			if (event->wd == wd_sgil1 &&
			    (event->mask & IN_IGNORED))
				wd_sgil1 = -1;

			if (event->wd == wd_dev && event->len &&
			    !strcmp(event->name, "sgi-l1") &&
			    path_exists("/dev/sgi-l1") && wd_sgil1 < 0)
				wd_sgil1 = add_sgil1_watch(fd);

			off += sizeof(*event) + event->len;
		}

		if (wd_sgil1 < 0 && path_exists("/dev/sgi-l1"))
			wd_sgil1 = add_sgil1_watch(fd);

		(void)wd_sgil1;
	}
}

static bool power_text_says_on(const char *text)
{
	return text && (strstr(text, "appears on") || strstr(text, " on") ||
			strstr(text, "\ton"));
}

static bool power_text_says_off(const char *text)
{
	return text && (strstr(text, "appears off") || strstr(text, " off") ||
			strstr(text, "\toff"));
}

static bool power_down_text_requests_second_command(const char *text)
{
	return text && contains_ci(text, "power down") &&
	       contains_ci(text, "again") &&
	       (contains_ci(text, "pwr d") || contains_ci(text, "power down"));
}

static void sleep_milliseconds(int milliseconds)
{
	if (milliseconds > 0)
		usleep((useconds_t)milliseconds * 1000);
}

static int power_confirm_timeout_ms(const struct options *opts)
{
	if (opts->timeout_ms < 0)
		return -1;
	if (opts->timeout_ms > SGIL1_POWER_UP_CONFIRM_TIMEOUT_MS)
		return opts->timeout_ms;
	return SGIL1_POWER_UP_CONFIRM_TIMEOUT_MS;
}

static int wait_for_power_state(const struct options *opts, int timeout_ms,
				bool want_on)
{
	time_t start = time(NULL);
	bool reported_wait = false;

	for (;;) {
		char *power_check = l1_text_command(opts, "power check", false);

		if (power_check) {
			bool matched = want_on ? power_text_says_on(power_check) :
						 power_text_says_off(power_check);

			if (matched) {
				printf("Power-%s: confirmed workstation appears %s\n",
				       want_on ? "up" : "down",
				       want_on ? "on" : "off");
				free(power_check);
				return 0;
			}
			free(power_check);
		}

		if (!reported_wait) {
			if (timeout_ms < 0)
				printf("Power-%s: waiting for power check to report %s\n",
				       want_on ? "up" : "down",
				       want_on ? "on" : "off");
			else
				printf("Power-%s: waiting up to %d seconds for power check to report %s\n",
				       want_on ? "up" : "down",
				       (timeout_ms + 999) / 1000,
				       want_on ? "on" : "off");
			reported_wait = true;
		}

		if (timeout_ms >= 0) {
			time_t now = time(NULL);
			int elapsed_ms;

			if (now == (time_t)-1 || start == (time_t)-1)
				elapsed_ms = timeout_ms;
			else
				elapsed_ms = (int)(now - start) * 1000;
			if (elapsed_ms >= timeout_ms) {
				fprintf(stderr,
					"Power-%s: timed out waiting for workstation to appear %s\n",
					want_on ? "up" : "down",
					want_on ? "on" : "off");
				return 1;
			}
		}

		sleep_milliseconds(SGIL1_POWER_UP_POLL_MS);
	}
}

static int wait_for_power_on(const struct options *opts, int timeout_ms)
{
	return wait_for_power_state(opts, timeout_ms, true);
}

static int wait_for_power_off(const struct options *opts, int timeout_ms)
{
	return wait_for_power_state(opts, timeout_ms, false);
}

static int do_power_up_confirmed(const struct options *opts,
				 bool allow_destructive)
{
	int ret;

	ret = run_l1_command_core(opts, "power up", allow_destructive, false,
				  false, true, opts->debug, NULL);
	if (ret != 0 && ret != SGIL1_L1CMD_RESPONSE_TIMEOUT)
		return ret;

	return wait_for_power_on(opts, power_confirm_timeout_ms(opts));
}

static int do_power_down_confirmed(const struct options *opts,
				   bool allow_destructive)
{
	char *response = NULL;
	int ret;

	ret = run_l1_command_core(opts, "power down", allow_destructive, false,
				  false, false, opts->debug, &response);
	if (ret)
		goto out;

	if (power_down_text_requests_second_command(response)) {
		printf("Power-down: L1 requested a second power down command; issuing it\n");
		free(response);
		response = NULL;
		ret = run_l1_command_core(opts, "power down", allow_destructive,
					  false, false, false, opts->debug, NULL);
		if (ret)
			goto out;
	}

	ret = wait_for_power_off(opts, power_confirm_timeout_ms(opts));

out:
	free(response);
	return ret;
}

static int reset_pipes_after_bind(const struct options *opts)
{
	int fd;
	int ret;

	fd = open_data_device(opts, O_RDWR | O_NONBLOCK);
	if (fd < 0)
		return 1;

	ret = ioctl(fd, SGIL1_RESET_PIPES);
	if (ret < 0) {
		fprintf(stderr, "reset-pipes after bind failed: %s\n",
			strerror(errno));
		ret = 1;
	} else {
		ret = 0;
	}

	close(fd);
	return ret;
}

static int maybe_power_up_from_wait(const struct options *opts)
{
	struct options cmd_opts;
	char *power_check;
	int ret = 0;

	if (prepare_command_options(opts, &cmd_opts))
		return 1;

	power_check = l1_text_command(&cmd_opts, "power check", false);
	if (!power_check)
		return 1;

	printf("\nPower-up check\n");
	print_text_block(power_check);
	if (power_text_says_on(power_check)) {
		printf("Power-up: workstation already appears on; no action taken\n");
	} else if (power_text_says_off(power_check)) {
		printf("Power-up: workstation appears off; issuing power up\n");
		ret = do_power_up_confirmed(opts, true);
	} else {
		fprintf(stderr,
			"Power-up: cannot determine power state; no action taken\n");
		ret = 1;
	}

	free(power_check);
	return ret;
}

static int do_reset_for_wait(const struct options *opts)
{
	int ret;

	printf("\nReset: issuing L1 reset command\n");
	ret = run_l1_command_core(opts, "reset", true, false, false, true,
				  opts->debug, NULL);
	if (ret == SGIL1_L1CMD_RESPONSE_TIMEOUT) {
		printf("Reset: command sent; response timed out, which can happen if USB drops during reset\n");
		return 0;
	}

	return ret;
}

static int do_wait_power_action(const struct options *opts,
				const struct wait_options *wait)
{
	if (wait->power_up)
		return maybe_power_up_from_wait(opts);
	if (wait->power_down) {
		printf("\nPower-down: issuing power down from wait mode\n");
		return do_power_down_confirmed(opts, true);
	}
	if (wait->reset)
		return do_reset_for_wait(opts);

	return 0;
}

static int validate_wait_power_action(const struct options *opts,
				      const struct wait_options *wait)
{
	bool force = wait->force || opts->force;

	if (!wait->power_up && !wait->power_down && !wait->reset)
		return 0;

	if (wait->power_down) {
		fprintf(stderr,
			"WARNING: wait --power-down is armed; when an L1 USB device connects, sgil1ctl will power off the workstation.\n");
	} else if (wait->reset) {
		fprintf(stderr,
			"WARNING: wait --reset is armed; when an L1 USB device connects, sgil1ctl will reset/restart the workstation.\n");
	}

	if (force)
		return 0;

	if (wait->power_up)
		fprintf(stderr,
			"wait --power-up requires --force because it changes workstation power state\n");
	else if (wait->power_down)
		fprintf(stderr,
			"wait --power-down requires --force because it will power off the workstation on connection\n");
	else
		fprintf(stderr,
			"wait --reset requires --force because it will reset/restart the workstation on connection\n");

	return 2;
}

static int do_wait(const struct options *opts, const struct wait_options *wait)
{
	int ret;

	ret = validate_wait_power_action(opts, wait);
	if (ret)
		return ret;

	for (;;) {
		const char *path;

		ret = wait_for_data_device(opts, wait->wait_timeout_seconds,
					   wait->background);
		if (ret)
			return ret;

		path = find_existing_data_device(opts);
		printf("SGI L1 USB device available: %s\n",
		       path ? path : "(unknown)");

		ret = acquire_sgil1_lock();
		if (ret)
			return ret;

		sleep_milliseconds(SGIL1_WAIT_BIND_SETTLE_MS);
		if (reset_pipes_after_bind(opts))
			printf("SGI L1 USB post-bind pipe reset failed; continuing with status probe\n");

		ret = do_consolidated_status(opts, &wait->status);
		if (ret) {
			printf("SGI L1 status probe failed; resetting pipes and retrying once\n");
			sleep_milliseconds(SGIL1_WAIT_BIND_SETTLE_MS);
			if (!reset_pipes_after_bind(opts))
				ret = do_consolidated_status(opts, &wait->status);
		}
		if (ret) {
			release_sgil1_lock();
			return ret;
		}

		if (wait->power_up || wait->power_down || wait->reset) {
			ret = do_wait_power_action(opts, wait);
			if (ret) {
				release_sgil1_lock();
				return ret;
			}
		}

		release_sgil1_lock();

		if (wait->keepalive_seconds <= 0)
			return 0;

		for (;;) {
			struct options cmd_opts;
			char *version;

			sleep(wait->keepalive_seconds);
			if (!find_existing_data_device(opts)) {
				printf("SGI L1 USB device disappeared; returning to wait mode\n");
				break;
			}

			if (acquire_sgil1_lock())
				return 1;
			if (prepare_command_options(opts, &cmd_opts)) {
				release_sgil1_lock();
				break;
			}
			version = l1_text_command(&cmd_opts, "version", false);
			release_sgil1_lock();
			if (!version) {
				printf("SGI L1 keepalive failed; returning to wait mode\n");
				break;
			}
			printf("keepalive version: ");
			print_text_block(version);
			free(version);
		}
	}
}

static int parse_options(int argc, char **argv, struct options *opts,
			 int *command_index)
{
	int i;

	opts->timeout_ms = 3000;
	opts->src_addr = SGIL1_IR_DEFAULT_SRC_ADDR;
	opts->dest_addr = SGIL1_IR_DEFAULT_L1_CMD_ADDR;
	opts->ir_class = SGIL1_IR_DEFAULT_CLASS;
	opts->authority = SGIL1_IR_DEFAULT_AUTHORITY;
	opts->pdata = SGIL1_IR_DEFAULT_PDATA;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--device")) {
			if (++i >= argc) {
				fprintf(stderr, "--device needs a path\n");
				return -1;
			}
			opts->device = argv[i];
		} else if (!strcmp(argv[i], "--status-device")) {
			if (++i >= argc) {
				fprintf(stderr, "--status-device needs a path\n");
				return -1;
			}
			opts->status_device = argv[i];
		} else if (!strcmp(argv[i], "--timeout")) {
			if (++i >= argc) {
				fprintf(stderr, "--timeout needs milliseconds\n");
				return -1;
			}
			if (parse_int_arg(argv[i], -1, INT_MAX,
					  &opts->timeout_ms)) {
				fprintf(stderr, "invalid timeout\n");
				return -1;
			}
		} else if (!strcmp(argv[i], "--src")) {
			if (++i >= argc) {
				fprintf(stderr, "--src needs an address\n");
				return -1;
			}
			if (parse_u32(argv[i], &opts->src_addr)) {
				fprintf(stderr, "invalid --src address\n");
				return -1;
			}
		} else if (!strcmp(argv[i], "--dest")) {
			if (++i >= argc) {
				fprintf(stderr, "--dest needs an address\n");
				return -1;
			}
			if (parse_u32(argv[i], &opts->dest_addr)) {
				fprintf(stderr, "invalid --dest address\n");
				return -1;
			}
			opts->dest_overridden = true;
		} else if (!strcmp(argv[i], "--class")) {
			uint32_t value;

			if (++i >= argc) {
				fprintf(stderr, "--class needs a value\n");
				return -1;
			}
			if (parse_u32(argv[i], &value) || value > UINT8_MAX) {
				fprintf(stderr, "invalid --class value\n");
				return -1;
			}
			opts->ir_class = (uint8_t)value;
		} else if (!strcmp(argv[i], "--auth")) {
			uint32_t value;

			if (++i >= argc) {
				fprintf(stderr, "--auth needs a value\n");
				return -1;
			}
			if (parse_u32(argv[i], &value) || value > 31) {
				fprintf(stderr, "invalid --auth value\n");
				return -1;
			}
			opts->authority = (uint8_t)value;
		} else if (!strcmp(argv[i], "--pdata")) {
			uint32_t value;

			if (++i >= argc) {
				fprintf(stderr, "--pdata needs a value\n");
				return -1;
			}
			if (parse_u32(argv[i], &value) || value > 7) {
				fprintf(stderr, "invalid --pdata value\n");
				return -1;
			}
			opts->pdata = (uint8_t)value;
		} else if (is_force_option(argv[i])) {
			opts->force = true;
		} else if (!strcmp(argv[i], "--debug")) {
			opts->debug = true;
		} else if (!strcmp(argv[i], "--pipe-records")) {
			opts->pipe_records = true;
		} else if (!strcmp(argv[i], "--no-discover")) {
			opts->no_discover = true;
		} else if (!strcmp(argv[i], "--help-all") ||
			   !strcmp(argv[i], "--help-full") ||
			   !strcmp(argv[i], "--all-help") ||
			   !strcmp(argv[i], "--full-help")) {
			usage(stdout, true);
			exit(0);
		} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			bool full = i + 1 < argc &&
				    (!strcmp(argv[i + 1], "all") ||
				     !strcmp(argv[i + 1], "full"));

			usage(stdout, full);
			exit(0);
		} else {
			*command_index = i;
			return 0;
		}
	}

	fprintf(stderr, "missing command; use --help for usage\n");
	return -1;
}

int main(int argc, char **argv)
{
	struct options opts = { 0 };
	const char *cmd;
	int command_index = -1;

	if (parse_options(argc, argv, &opts, &command_index))
		return 2;

	cmd = argv[command_index];

	if (!strcmp(cmd, "wait")) {
		struct wait_options wait;

		if (parse_wait_args(argc, argv, command_index + 1, &wait))
			return 2;
		return do_wait(&opts, &wait);
	}

	if (command_uses_l1_transaction(cmd) && acquire_sgil1_lock())
		return 1;

	if (!strcmp(cmd, "probe"))
		return do_probe(&opts);
	if (!strcmp(cmd, "discover")) {
		uint32_t dest_addr = 0;

		return discover_l1_command_dest(&opts, &dest_addr, true);
	}
	if (!strcmp(cmd, "status")) {
		struct status_options status = {
			.drift_seconds = SGIL1_DEFAULT_TIME_DRIFT_SEC,
		};

		if (command_index + 1 != argc) {
			fprintf(stderr,
				"status does not take options; use date --set-time to update the L1 clock\n");
			return 2;
		}
		return do_consolidated_status(&opts, &status);
	}
	if (!strcmp(cmd, "date") || !strcmp(cmd, "clock") || !strcmp(cmd, "time") ||
	    !strcmp(cmd, "set-clock") || !strcmp(cmd, "set-time")) {
		struct status_options status;

		if (parse_status_args(argc, argv, command_index + 1, &status))
			return 2;
		if (!strcmp(cmd, "set-clock") || !strcmp(cmd, "set-time"))
			status.set_time = true;
		if (status.set_time && !status.timezone)
			set_status_host_timezone(&status);
		return do_clock(&opts, &status);
	}
	if (!strcmp(cmd, "driver-status"))
		return do_driver_status(&opts);
	if (!strcmp(cmd, "read-cfg"))
		return do_read_cfg(&opts);
	if (!strcmp(cmd, "reset-read"))
		return ioctl_command(&opts, SGIL1_RESET_READ, "reset-read");
	if (!strcmp(cmd, "reset-write"))
		return ioctl_command(&opts, SGIL1_RESET_WRITE, "reset-write");
	if (!strcmp(cmd, "reset-pipes"))
		return ioctl_command(&opts, SGIL1_RESET_PIPES, "reset-pipes");
	if (!strcmp(cmd, "reset-device")) {
		if (!opts.force) {
			fprintf(stderr,
				"reset-device requires --force because it resets the USB device\n");
			return 2;
		}
		return ioctl_command(&opts, SGIL1_RESET_DEVICE, "reset-device");
	}
	if (!strcmp(cmd, "raw-send"))
		return do_raw_send(&opts, argc, argv, command_index + 1);
	if (!strcmp(cmd, "raw-recv"))
		return do_raw_recv(&opts);
	if (!strcmp(cmd, "monitor"))
		return do_monitor(&opts);
	if (!strcmp(cmd, "version"))
		return do_l1_command(&opts, "version", false);
	if (!strcmp(cmd, "usb"))
		return do_l1_command(&opts, "usb", false);
	if (!strcmp(cmd, "env"))
		return do_l1_command(&opts, "env", false);
	if (!strcmp(cmd, "log"))
		return do_l1_command(&opts, "log", false);
	if (!strcmp(cmd, "power"))
		return do_power_command(&opts, argc, argv, command_index);
	if (!strcmp(cmd, "command") || !strcmp(cmd, "l1cmd") ||
	    !strcmp(cmd, "send"))
		return do_l1_pass_through_args(&opts, argc, argv,
					       command_index + 1);
	if (!strcmp(cmd, "build-l1cmd"))
		return do_build_l1cmd_args(&opts, argc, argv, command_index + 1);

	if (!strcmp(cmd, "power-up")) {
		bool force = opts.force;

		if (trailing_force_only(argc, argv, command_index, &force))
			return 2;
		return do_power_up_confirmed(&opts, force);
	}
	if (!strcmp(cmd, "power-down")) {
		bool force = opts.force;

		if (trailing_force_only(argc, argv, command_index, &force))
			return 2;
		return do_power_down_confirmed(&opts, force);
	}
	if (!strcmp(cmd, "reset")) {
		bool force = opts.force;

		if (trailing_force_only(argc, argv, command_index, &force))
			return 2;
		return do_l1_command(&opts, "reset", force);
	}

	fprintf(stderr,
		"unknown command: %s; use 'l1cmd' for direct L1 command pass-through or --help for usage\n",
		cmd);
	return 2;
}
