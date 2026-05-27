// SPDX-License-Identifier: GPL-2.0-or-later

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "sgi_l1_ioctl.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define SGIL1_REV_LEN 64
#define SGIL1_IO_SIZE 4096

static const char *data_candidates[] = {
	"/dev/sgi-l1/fuel-l1",
	"/dev/sgi-l1/l1-0",
	"/dev/sgil1_0",
	"/dev/usb/sgil1_0",
};

static const char *status_candidates[] = {
	"/dev/sgi-l1/status",
	"/dev/sgil1_cs",
};

struct options {
	const char *device;
	const char *status_device;
	int timeout_ms;
	bool yes;
};

static void usage(FILE *out)
{
	fprintf(out,
		"Usage: sgil1ctl [OPTIONS] COMMAND [ARGS...]\n"
		"\n"
		"Options:\n"
		"  --device PATH         data device path\n"
		"  --status-device PATH  status device path\n"
		"  --timeout MS          poll timeout for reads (default 3000)\n"
		"  --yes                 allow commands that require confirmation\n"
		"  -h, --help            show this help\n"
		"\n"
		"Commands:\n"
		"  probe                 show status device and first data device\n"
		"  status                read sgil1_cs status bitmap\n"
		"  read-cfg              read SGIL1_READ_CFG from the data device\n"
		"  reset-read            reset the read URB\n"
		"  reset-write           clear the write endpoint halt\n"
		"  reset-pipes           clear read and write endpoint halts\n"
		"  reset-device          issue a USB device reset\n"
		"  raw-send HEX...       write a complete raw frame; first two bytes are overwritten by the driver\n"
		"  raw-recv              read one raw frame and print a hexdump\n"
		"  monitor               print raw frames until interrupted\n"
		"  power|env|power-up|power-down|reset\n"
		"                        reserved protocol commands; currently refuse to act\n");
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

static int open_data_device(const struct options *opts, int flags)
{
	const char *path = opts->device;
	int fd;

	if (!path)
		path = first_existing(data_candidates, ARRAY_SIZE(data_candidates));

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

static int do_status(const struct options *opts)
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
	int fd;
	int ret = 0;

	fd = open_status_device(opts, O_RDONLY);
	if (fd >= 0) {
		memset(rev, 0, sizeof(rev));
		if (ioctl(fd, SGIL1_ST_READ_REV, rev) == 0)
			printf("driver=%s\n", rev);
		close(fd);
	}

	ret = do_status(opts);
	if (ret)
		return ret;

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

static int refuse_protocol_command(const char *cmd)
{
	fprintf(stderr,
		"%s is not implemented yet: the IRouter packet format for this L1 must be confirmed first\n",
		cmd);
	return 2;
}

static int parse_options(int argc, char **argv, struct options *opts,
			 int *command_index)
{
	int i;

	opts->timeout_ms = 3000;

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
			opts->timeout_ms = atoi(argv[i]);
			if (opts->timeout_ms < -1) {
				fprintf(stderr, "invalid timeout\n");
				return -1;
			}
		} else if (!strcmp(argv[i], "--yes")) {
			opts->yes = true;
		} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			usage(stdout);
			exit(0);
		} else {
			*command_index = i;
			return 0;
		}
	}

	fprintf(stderr, "missing command\n");
	return -1;
}

int main(int argc, char **argv)
{
	struct options opts = { 0 };
	const char *cmd;
	int command_index = -1;

	if (parse_options(argc, argv, &opts, &command_index)) {
		usage(stderr);
		return 2;
	}

	cmd = argv[command_index];

	if (!strcmp(cmd, "probe"))
		return do_probe(&opts);
	if (!strcmp(cmd, "status"))
		return do_status(&opts);
	if (!strcmp(cmd, "read-cfg"))
		return do_read_cfg(&opts);
	if (!strcmp(cmd, "reset-read"))
		return ioctl_command(&opts, SGIL1_RESET_READ, "reset-read");
	if (!strcmp(cmd, "reset-write"))
		return ioctl_command(&opts, SGIL1_RESET_WRITE, "reset-write");
	if (!strcmp(cmd, "reset-pipes"))
		return ioctl_command(&opts, SGIL1_RESET_PIPES, "reset-pipes");
	if (!strcmp(cmd, "reset-device")) {
		if (!opts.yes) {
			fprintf(stderr,
				"reset-device requires --yes because it resets the USB device\n");
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

	if (!strcmp(cmd, "power") || !strcmp(cmd, "env") ||
	    !strcmp(cmd, "power-up") || !strcmp(cmd, "power-down") ||
	    !strcmp(cmd, "reset"))
		return refuse_protocol_command(cmd);

	fprintf(stderr, "unknown command: %s\n", cmd);
	usage(stderr);
	return 2;
}
