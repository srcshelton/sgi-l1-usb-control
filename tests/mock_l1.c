// SPDX-License-Identifier: GPL-2.0-or-later

#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "sgi_l1_ioctl.h"

#define MOCK_BASE_FD 10000
#define MOCK_MAX_FDS 64
#define MOCK_MAX_QUEUE 64
#define MOCK_FRAME_MAX 4096

#define IR_DEFAULT_DEST 0x00041003U
#define IR_DISCOVERY_DEST 0x0ffff00eU
#define IR_DISCOVERY_SRC 0x84000101U
#define IR_DISCOVERY_REPLY_SRC 0x00041000U
#define IR_HEADER_LEN 20U
#define IR_ARG_LEN 8U
#define IR_ARG_BASE 0x10U

enum mock_fd_type {
	MOCK_FD_NONE,
	MOCK_FD_DATA,
	MOCK_FD_STATUS,
	MOCK_FD_LOCK,
};

struct queued_frame {
	size_t len;
	uint8_t data[MOCK_FRAME_MAX];
};

struct mock_fd {
	enum mock_fd_type type;
	bool used;
	size_t head;
	size_t tail;
	struct queued_frame queue[MOCK_MAX_QUEUE];
};

static struct mock_fd mock_fds[MOCK_MAX_FDS];
static bool mock_initialized;
static bool mock_power_on = true;
static bool mock_power_down_confirm;
static bool mock_power_up_timeout;
static int mock_power_down_count;
static int mock_log_call_count;
static int mock_leds_call_count;
static long mock_max_write;

static int (*real_open_fn)(const char *pathname, int flags, ...);
static int (*real_open64_fn)(const char *pathname, int flags, ...);
static int (*real_close_fn)(int fd);
static ssize_t (*real_read_fn)(int fd, void *buf, size_t count);
static ssize_t (*real_write_fn)(int fd, const void *buf, size_t count);
static int (*real_poll_fn)(struct pollfd *fds, nfds_t nfds, int timeout);
static int (*real_ioctl_fn)(int fd, unsigned long request, ...);
static int (*real_stat_fn)(const char *pathname, struct stat *st);
static int (*real_lstat_fn)(const char *pathname, struct stat *st);
static int (*real_fchmod_fn)(int fd, mode_t mode);
static int (*real_flock_fn)(int fd, int operation);
static int (*real_usleep_fn)(useconds_t usec);
static unsigned int (*real_sleep_fn)(unsigned int seconds);

static void load_symbol(void *fn, const char *name)
{
	void *sym = dlsym(RTLD_NEXT, name);

	memcpy(fn, &sym, sizeof(sym));
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

static void put_be16(uint8_t *buf, uint16_t value)
{
	buf[0] = (uint8_t)(value >> 8);
	buf[1] = (uint8_t)value;
}

static void put_be32(uint8_t *buf, uint32_t value)
{
	buf[0] = (uint8_t)(value >> 24);
	buf[1] = (uint8_t)(value >> 16);
	buf[2] = (uint8_t)(value >> 8);
	buf[3] = (uint8_t)value;
}

static void init_real_symbols(void)
{
	if (mock_initialized)
		return;

	load_symbol(&real_open_fn, "open");
	load_symbol(&real_open64_fn, "open64");
	load_symbol(&real_close_fn, "close");
	load_symbol(&real_read_fn, "read");
	load_symbol(&real_write_fn, "write");
	load_symbol(&real_poll_fn, "poll");
	load_symbol(&real_ioctl_fn, "ioctl");
	load_symbol(&real_stat_fn, "stat");
	load_symbol(&real_lstat_fn, "lstat");
	load_symbol(&real_fchmod_fn, "fchmod");
	load_symbol(&real_flock_fn, "flock");
	load_symbol(&real_usleep_fn, "usleep");
	load_symbol(&real_sleep_fn, "sleep");

	mock_initialized = true;
}

static void init_mock_state(void)
{
	const char *power = getenv("SGIL1_MOCK_POWER");
	const char *max_write = getenv("SGIL1_MOCK_MAX_WRITE");

	init_real_symbols();

	if (power && !strcmp(power, "off"))
		mock_power_on = false;
	mock_power_down_confirm = getenv("SGIL1_MOCK_POWER_DOWN_CONFIRM") != NULL;
	mock_power_up_timeout = getenv("SGIL1_MOCK_POWER_UP_TIMEOUT") != NULL;
	if (max_write && *max_write)
		mock_max_write = strtol(max_write, NULL, 0);
}

__attribute__((constructor))
static void mock_constructor(void)
{
	init_mock_state();
}

static struct mock_fd *fd_entry(int fd)
{
	int index = fd - MOCK_BASE_FD;

	if (index < 0 || index >= MOCK_MAX_FDS || !mock_fds[index].used)
		return NULL;

	return &mock_fds[index];
}

static int enqueue_text_response(struct mock_fd *m, const char *text,
				 uint32_t dest, uint32_t src, uint8_t seq);

static bool is_data_path(const char *path)
{
	const char *index = getenv("SGIL1_MOCK_DATA_INDEX");
	char stable[64];
	char legacy[64];
	char usb[64];

	if (!index || !*index)
		index = "0";

	snprintf(stable, sizeof(stable), "/dev/sgi-l1/l1-%s", index);
	snprintf(legacy, sizeof(legacy), "/dev/sgil1_%s", index);
	snprintf(usb, sizeof(usb), "/dev/usb/sgil1_%s", index);

	return !strcmp(path, stable) ||
	       !strcmp(path, legacy) ||
	       !strcmp(path, usb);
}

static bool is_status_path(const char *path)
{
	return !strcmp(path, "/dev/sgi-l1/status") ||
	       !strcmp(path, "/dev/sgil1_cs");
}

static bool is_lock_path(const char *path)
{
	return !strcmp(path, "/var/lock/sgil1ctl.lock") ||
	       !strcmp(path, "/tmp/sgil1ctl.lock");
}

static int alloc_mock_fd(enum mock_fd_type type)
{
	int i;

	for (i = 0; i < MOCK_MAX_FDS; i++) {
		if (!mock_fds[i].used) {
			memset(&mock_fds[i], 0, sizeof(mock_fds[i]));
			mock_fds[i].used = true;
			mock_fds[i].type = type;
			return MOCK_BASE_FD + i;
		}
	}

	errno = EMFILE;
	return -1;
}

static int open_mock_path(const char *pathname)
{
	if (is_data_path(pathname)) {
		int fd = alloc_mock_fd(MOCK_FD_DATA);

		if (fd >= 0 && getenv("SGIL1_MOCK_STALE_ON_OPEN"))
			enqueue_text_response(fd_entry(fd), "stale drain\n",
					      IR_DEFAULT_DEST, IR_DISCOVERY_SRC,
					      0);
		return fd;
	}
	if (is_status_path(pathname))
		return alloc_mock_fd(MOCK_FD_STATUS);
	if (is_lock_path(pathname))
		return alloc_mock_fd(MOCK_FD_LOCK);

	return -1;
}

static void log_command(const char *kind, const char *text)
{
	const char *path = getenv("SGIL1_MOCK_LOG");
	FILE *fp;

	if (!path || !*path)
		return;

	fp = fopen(path, "a");
	if (!fp)
		return;
	fprintf(fp, "%s %s\n", kind, text);
	fclose(fp);
}

static int enqueue_frame(struct mock_fd *m, const uint8_t *data, size_t len)
{
	struct queued_frame *frame;

	if (len > MOCK_FRAME_MAX) {
		errno = EMSGSIZE;
		return -1;
	}
	if (m->tail - m->head >= MOCK_MAX_QUEUE) {
		errno = ENOBUFS;
		return -1;
	}

	frame = &m->queue[m->tail % MOCK_MAX_QUEUE];
	memcpy(frame->data, data, len);
	frame->len = len;
	m->tail++;
	return 0;
}

static int enqueue_text_response(struct mock_fd *m, const char *text,
				 uint32_t dest, uint32_t src, uint8_t seq)
{
	uint8_t frame[MOCK_FRAME_MAX];
	size_t text_len = strlen(text) + 1;
	size_t payload_off = IR_HEADER_LEN + IR_ARG_LEN;
	size_t frame_len = payload_off + text_len;

	if (frame_len > sizeof(frame)) {
		errno = EMSGSIZE;
		return -1;
	}

	memset(frame, 0, frame_len);
	put_be16(frame, (uint16_t)frame_len);
	frame[2] = 2;
	frame[3] = 0;
	frame[4] = seq;
	frame[5] = 0x81;
	put_be32(frame + 8, dest);
	put_be32(frame + 12, src);
	frame[16] = 1;

	frame[IR_HEADER_LEN] = 0x0a;
	frame[IR_HEADER_LEN + 1] = 1;
	put_be16(frame + IR_HEADER_LEN + 2, (uint16_t)text_len);
	put_be32(frame + IR_HEADER_LEN + 4, IR_ARG_BASE + payload_off);
	memcpy(frame + payload_off, text, text_len);

	return enqueue_frame(m, frame, frame_len);
}

static int enqueue_discovery_response(struct mock_fd *m)
{
	uint8_t frame[IR_HEADER_LEN];

	if (getenv("SGIL1_MOCK_NO_DISCOVERY"))
		return 0;

	memset(frame, 0, sizeof(frame));
	put_be16(frame, sizeof(frame));
	frame[2] = 2;
	frame[3] = 0;
	frame[5] = 0x81;
	put_be32(frame + 8, IR_DISCOVERY_SRC);
	put_be32(frame + 12, IR_DISCOVERY_REPLY_SRC);

	return enqueue_frame(m, frame, sizeof(frame));
}

static const char *mock_log_response(void)
{
	if (!getenv("SGIL1_MOCK_LOG_FOLLOW"))
		return "05/27/2026 12:38:00 L1 booted\n";

	if (mock_log_call_count++ == 0)
		return "05/27/2026 12:38:00 L1 booted\n"
		       "05/27/2026 12:38:01 USB ready\n";
	if (mock_log_call_count == 2)
		return "05/27/2026 12:38:00 L1 booted\n"
		       "05/27/2026 12:38:01 USB ready\n"
		       "05/27/2026 12:38:02 fan stable\n"
		       "05/27/2026 12:38:03 fan stable\n"
		       "05/27/2026 12:38:04 fan stable\n";

	return "05/27/2026 12:38:00 L1 booted\n"
	       "05/27/2026 12:38:01 USB ready\n"
	       "05/27/2026 12:38:02 fan stable\n"
	       "05/27/2026 12:38:03 fan stable\n"
	       "05/27/2026 12:38:04 fan stable\n"
	       "05/27/2026 12:38:05 voltage nominal\n";
}

static const char *known_response_for_command(const char *cmd)
{
	if (!strcmp(cmd, "help"))
		return "Commands are:\n"
		       "*                  version|ver usb env date serial log leds power|pwr reset softreset|softrst flash fan\n"
		       "                   help|hlp\n\n";
	if (!strcmp(cmd, "help flash"))
		return "flash default reset\n"
		       "        determines default image at boot-time\n"
		       "flash default a\n"
		       "        set image A as the default flash image\n"
		       "flash default b\n"
		       "        set image B as the default flash image\n"
		       "flash status\n"
		       "        display status of flash images\n";
	if (!strcmp(cmd, "flash"))
		return "ERROR: command not found.\n";
	if (!strcmp(cmd, "date help"))
		return "ERROR: command not found.\n";
	if (!strcmp(cmd, "version") || !strcmp(cmd, "ver"))
		return "L1 1.24.11 (Image B), Built 10/29/2003 00:05:26    [Fuel/PE 1MB image]\n";
	if (!strcmp(cmd, "usb"))
		return "\nDevice: 0  Disconnects: 0  Bus Resets:  2\n\n"
		       "Endpoint State    Status    Stalls Errors Timeouts\n"
		       "-------- -----    ------    ------ ------ --------\n"
		       "Control  Active   Suspended 1      0      0\n"
		       "Read     Active   Ready     0      0      0\n"
		       "Write    Active   Ready     0      0      0\n";
	if (!strcmp(cmd, "env") || !strcmp(cmd, "env check"))
		return "Environmental monitoring is enabled and running.\n\n"
		       "Description    State       Warning Limits     Fault Limits       Current\n"
		       "-------------- ----------  -----------------  -----------------  -------\n"
		       "           12V    Enabled  10%  10.80/ 13.20  20%   9.60/ 14.40   11.69\n"
		       "FAN 0  EXHAUST    Enabled          920         1298\n"
		       "NODE 0            Enabled   Disabled   Disabled   70C/158F   44C/111F\n";
	if (!strcmp(cmd, "fan"))
		return "fan(s) are on.\nfan 0 EXHAUST  rpm 1298\n";
	if (!strcmp(cmd, "leds")) {
		if (getenv("SGIL1_MOCK_LEDS_FOLLOW")) {
			if (mock_leds_call_count++ == 0)
				return "CPU  A: 0x55: unknown LED status.\n";
			if (mock_leds_call_count == 2)
				return "CPU  A: 0x70: unknown LED status.\n";
			return "CPU  A: 0x70: unknown LED status.\n";
		}
		if (getenv("SGIL1_MOCK_LEDS_UNKNOWN"))
			return "CPU  A: 0x55: unknown LED status.\n"
			       "CPU  B: 0x81: unknown LED status.\n"
			       "CPU  C: 0xB5: unknown LED status.\n"
			       "CPU  D: 0x0: unknown LED status.\n"
			       "CPU  E: 0xff: Console poll found data for reading\n";
		return "LEDs: power-off standby\n";
	}
	if (!strcmp(cmd, "serial") || !strcmp(cmd, "serial all"))
		return "BSN: NCJ502    SSN: 08:00:69:10:6C:E3    Time: 05/27/2026 12:38:03 BST\n"
		       "Public Key data in EEPROM is invalid\n";
	if (!strcmp(cmd, "date"))
		return getenv("SGIL1_MOCK_DATE") ? getenv("SGIL1_MOCK_DATE") :
		       "05/27/2026 12:38:03 BST\n";
	if (!strcmp(cmd, "date tz"))
		return getenv("SGIL1_MOCK_TZ") ? getenv("SGIL1_MOCK_TZ") :
		       "GMT0BST\n";
	if (!strncmp(cmd, "date tz ", 8))
		return "timezone set\n";
	if (!strncmp(cmd, "date ", 5))
		return "date set\n";
	if (!strcmp(cmd, "power") || !strcmp(cmd, "power check") ||
	    !strcmp(cmd, "pwr") || !strcmp(cmd, "pwr check"))
		return mock_power_on ? "power appears on\n" :
				       "power appears off\n";
	if (!strcmp(cmd, "power vrm") || !strcmp(cmd, "pwr vrm"))
		return "Supply          State Voltage    Margin  Value\n"
		       "--------------  ----- ---------  ------- -----\n"
		       "           12V     on   11.687V      N/A\n";
	if (!strcmp(cmd, "log"))
		return mock_log_response();
	if (!strcmp(cmd, "reset"))
		return "reset issued\n";
	if (!strcmp(cmd, "softreset") || !strcmp(cmd, "softrst"))
		return "soft reset issued\n";

	return "ERROR: command not found.\n";
}

static char *extract_command_text(const uint8_t *buf, size_t len)
{
	unsigned int argc;
	unsigned int i;

	if (len < IR_HEADER_LEN)
		return NULL;

	argc = buf[16];
	for (i = 0; i < argc; i++) {
		size_t desc = IR_HEADER_LEN + i * IR_ARG_LEN;
		uint16_t size;
		uint32_t value;
		size_t off;
		char *cmd;

		if (desc + IR_ARG_LEN > len || buf[desc] != 0x0a)
			continue;
		size = get_be16(buf + desc + 2);
		value = get_be32(buf + desc + 4);
		if (!size || value < IR_ARG_BASE)
			continue;
		off = value - IR_ARG_BASE;
		if (off >= len)
			continue;
		if (size > len - off)
			size = (uint16_t)(len - off);

		cmd = calloc(1, (size_t)size + 1);
		if (!cmd)
			return NULL;
		memcpy(cmd, buf + off, size);
		cmd[size] = '\0';
		return cmd;
	}

	return NULL;
}

static const char *strip_broadcast_prefix(const char *cmd)
{
	if (cmd[0] != '*')
		return cmd;
	cmd++;
	while (*cmd == ' ' || *cmd == '\t')
		cmd++;
	return *cmd ? cmd : "*";
}

static ssize_t handle_data_write(struct mock_fd *m, const void *buf, size_t count)
{
	const uint8_t *bytes = buf;
	uint32_t dest;
	uint32_t src;
	char *raw_cmd;
	const char *cmd;
	const char *response;

	if (mock_max_write > 0 && count > (size_t)mock_max_write) {
		errno = EMSGSIZE;
		return -1;
	}

	if (count < IR_HEADER_LEN)
		return (ssize_t)count;

	dest = get_be32(bytes + 8);
	src = get_be32(bytes + 12);
	if (dest == IR_DISCOVERY_DEST)
		return enqueue_discovery_response(m) ? -1 : (ssize_t)count;

	raw_cmd = extract_command_text(bytes, count);
	if (!raw_cmd)
		return (ssize_t)count;
	cmd = strip_broadcast_prefix(raw_cmd);
	log_command("CMD", cmd);

	if (!strcmp(cmd, "power up") || !strcmp(cmd, "pwr up") ||
	    !strcmp(cmd, "pwr u")) {
		mock_power_on = true;
		if (mock_power_up_timeout) {
			free(raw_cmd);
			return (ssize_t)count;
		}
		response = "power up OK\n";
	} else if (!strcmp(cmd, "power down") || !strcmp(cmd, "pwr down") ||
		   !strcmp(cmd, "pwr d")) {
		if (mock_power_down_confirm && mock_power_down_count++ == 0) {
			response = "Type power down again to confirm: pwr d\n";
		} else {
			mock_power_on = false;
			response = "power appears off\n";
		}
	} else {
		response = known_response_for_command(cmd);
	}

	if (getenv("SGIL1_MOCK_STALE_BEFORE_RESPONSE") &&
	    enqueue_text_response(m, "s\n", src, dest ? dest : IR_DEFAULT_DEST, 0)) {
		free(raw_cmd);
		return -1;
	}

	if (enqueue_text_response(m, response, src, dest ? dest : IR_DEFAULT_DEST,
				  bytes[4])) {
		free(raw_cmd);
		return -1;
	}

	free(raw_cmd);
	return (ssize_t)count;
}

int open(const char *pathname, int flags, ...)
{
	mode_t mode = 0;
	int mock_fd;

	init_real_symbols();
	if (flags & O_CREAT) {
		va_list ap;

		va_start(ap, flags);
		mode = (mode_t)va_arg(ap, int);
		va_end(ap);
	}

	mock_fd = open_mock_path(pathname);
	if (mock_fd >= 0)
		return mock_fd;

	return real_open_fn(pathname, flags, mode);
}

int open64(const char *pathname, int flags, ...)
{
	mode_t mode = 0;
	int mock_fd;

	init_real_symbols();
	if (flags & O_CREAT) {
		va_list ap;

		va_start(ap, flags);
		mode = (mode_t)va_arg(ap, int);
		va_end(ap);
	}

	mock_fd = open_mock_path(pathname);
	if (mock_fd >= 0)
		return mock_fd;

	if (real_open64_fn)
		return real_open64_fn(pathname, flags, mode);
	return real_open_fn(pathname, flags, mode);
}

int __open_2(const char *pathname, int flags)
{
	int mock_fd;

	init_real_symbols();
	mock_fd = open_mock_path(pathname);
	if (mock_fd >= 0)
		return mock_fd;

	return real_open_fn(pathname, flags);
}

int close(int fd)
{
	struct mock_fd *m = fd_entry(fd);

	init_real_symbols();
	if (m) {
		memset(m, 0, sizeof(*m));
		return 0;
	}

	return real_close_fn(fd);
}

ssize_t write(int fd, const void *buf, size_t count)
{
	struct mock_fd *m = fd_entry(fd);

	init_real_symbols();
	if (!m)
		return real_write_fn(fd, buf, count);
	if (m->type == MOCK_FD_DATA)
		return handle_data_write(m, buf, count);
	if (m->type == MOCK_FD_LOCK)
		return (ssize_t)count;

	errno = EBADF;
	return -1;
}

ssize_t read(int fd, void *buf, size_t count)
{
	struct mock_fd *m = fd_entry(fd);

	init_real_symbols();
	if (!m)
		return real_read_fn(fd, buf, count);

	if (m->type == MOCK_FD_STATUS) {
		uint8_t status[SGIL1_MAX_DEVICES] = { 0 };
		size_t n = count < sizeof(status) ? count : sizeof(status);

		status[0] = 1;
		memcpy(buf, status, n);
		return (ssize_t)n;
	}

	if (m->type == MOCK_FD_DATA) {
		struct queued_frame *frame;

		if (m->head == m->tail) {
			errno = EAGAIN;
			return -1;
		}
		frame = &m->queue[m->head % MOCK_MAX_QUEUE];
		if (count < frame->len) {
			errno = EINVAL;
			return -1;
		}
		memcpy(buf, frame->data, frame->len);
		m->head++;
		return (ssize_t)frame->len;
	}

	errno = EBADF;
	return -1;
}

int poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
	nfds_t i;
	int ready = 0;
	bool saw_mock = false;

	init_real_symbols();

	for (i = 0; i < nfds; i++) {
		struct mock_fd *m = fd_entry(fds[i].fd);

		if (!m)
			continue;
		saw_mock = true;
		fds[i].revents = 0;
		if (m->type == MOCK_FD_DATA && m->head != m->tail &&
		    (fds[i].events & POLLIN)) {
			fds[i].revents = POLLIN;
			ready++;
		}
	}

	if (saw_mock)
		return ready;

	return real_poll_fn(fds, nfds, timeout);
}

int ioctl(int fd, unsigned long request, ...)
{
	struct mock_fd *m = fd_entry(fd);
	va_list ap;
	void *arg = NULL;

	init_real_symbols();
	if (!m) {
		int ret;

		va_start(ap, request);
		arg = va_arg(ap, void *);
		va_end(ap);
		ret = real_ioctl_fn(fd, request, arg);
		return ret;
	}

	switch (request) {
	case SGIL1_RESET_READ:
	case SGIL1_RESET_WRITE:
	case SGIL1_RESET_PIPES:
	case SGIL1_RESET_DEVICE:
		log_command("IOCTL", request == SGIL1_RESET_DEVICE ?
			    "reset-device" : "reset-pipes");
		return 0;
	case SGIL1_READ_CFG: {
		struct sgil1_cfg *cfg;

		va_start(ap, request);
		cfg = va_arg(ap, struct sgil1_cfg *);
		va_end(ap);
		memset(cfg, 0, sizeof(*cfg));
		cfg->bus = 2;
		cfg->dev = 10;
		cfg->level = 2;
		cfg->path[0] = 1;
		cfg->path[1] = 1;
		return 0;
	}
	case SGIL1_ST_READ_REV: {
		char *rev;

		va_start(ap, request);
		rev = va_arg(ap, char *);
		va_end(ap);
		memset(rev, 0, 64);
		snprintf(rev, 64, "sgi-l1-usb mock");
		return 0;
	}
	case SGIL1_ST_READ_DEV_CFG: {
		struct sgil1_cfg *cfg;

		va_start(ap, request);
		cfg = va_arg(ap, struct sgil1_cfg *);
		va_end(ap);
		if (cfg->dev != 0) {
			errno = ENODEV;
			return -1;
		}
		memset(cfg, 0, sizeof(*cfg));
		cfg->bus = 2;
		cfg->dev = 10;
		cfg->level = 2;
		cfg->path[0] = 1;
		cfg->path[1] = 1;
		return 0;
	}
	default:
		errno = ENOTTY;
		return -1;
	}
}

static int stat_mock_path(const char *pathname, struct stat *st)
{
	if (!strcmp(pathname, "/dev/sgi-l1")) {
		memset(st, 0, sizeof(*st));
		st->st_mode = S_IFDIR | 0755;
		return 0;
	}
	if (is_data_path(pathname) || is_status_path(pathname) ||
	    is_lock_path(pathname)) {
		memset(st, 0, sizeof(*st));
		st->st_mode = S_IFCHR | 0660;
		return 0;
	}

	errno = ENOENT;
	return -1;
}

int stat(const char *pathname, struct stat *st)
{
	init_real_symbols();
	if (stat_mock_path(pathname, st) == 0)
		return 0;
	if (errno != ENOENT)
		return -1;
	return real_stat_fn(pathname, st);
}

int lstat(const char *pathname, struct stat *st)
{
	init_real_symbols();
	if (stat_mock_path(pathname, st) == 0)
		return 0;
	if (errno != ENOENT)
		return -1;
	return real_lstat_fn(pathname, st);
}

int fchmod(int fd, mode_t mode)
{
	init_real_symbols();
	if (fd_entry(fd))
		return 0;
	return real_fchmod_fn(fd, mode);
}

int flock(int fd, int operation)
{
	init_real_symbols();
	if (fd_entry(fd))
		return 0;
	return real_flock_fn(fd, operation);
}

int usleep(useconds_t usec)
{
	init_real_symbols();
	if (getenv("SGIL1_MOCK_NO_SLEEP"))
		return 0;
	return real_usleep_fn(usec);
}

unsigned int sleep(unsigned int seconds)
{
	init_real_symbols();
	if (getenv("SGIL1_MOCK_NO_SLEEP"))
		return 0;
	return real_sleep_fn(seconds);
}
