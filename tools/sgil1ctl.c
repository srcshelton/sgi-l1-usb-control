// SPDX-License-Identifier: GPL-2.0-or-later

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
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

#ifdef SGIL1_WITH_TUI
#include <curses.h>
#include <term.h>
#ifdef lines
#undef lines
#endif
#endif

#include "sgi_l1_ioctl.h"

#ifndef SGIL1CTL_VERSION
#define SGIL1CTL_VERSION "unknown"
#endif

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
#define SGIL1_L1_LEGACY_MAX_COMMAND_TEXT 72U
#define SGIL1_L1_EXTENDED_MAX_COMMAND_TEXT 279U
#define SGIL1_L1_MAX_COMMAND_TEXT SGIL1_L1_EXTENDED_MAX_COMMAND_TEXT
#define SGIL1_L1_COMMAND_TRANSFER(text_len) \
	(SGIL1_IR_HEADER_LEN + (2U * SGIL1_IR_ARG_LEN) + \
	 (text_len) + 1U)
#define SGIL1_IR_MAX_FRAMES 32U
#define SGIL1_DRAIN_MAX_FRAMES 256U
#define SGIL1_RESPONSE_SCAN_MAX_FRAMES 256U
#define SGIL1_STALE_RESPONSE_WARN_FRAMES 4U
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
#define SGIL1_LOG_DEFAULT_POLL_MS 1000
#define SGIL1_LOG_BURST_POLL_MS 200
#define SGIL1_LEDS_FOLLOW_POLL_MS 100
#define SGIL1_LEDS_FOLLOW_BACKOFF_MAX_MS 500
#define SGIL1_LEDS_FOLLOW_CONFIRM_BUFFER_SECONDS 5
#define SGIL1_WATCH_LOG_MIN_MS 100
#define SGIL1_WATCH_LED_MIN_MS 100
#define SGIL1_WATCH_LED_IDLE_MAX_MS 500
#define SGIL1_WATCH_FAILURE_BACKOFF_MIN_MS 200
#define SGIL1_WATCH_FAILURE_BACKOFF_MAX_MS 2000
#define SGIL1_QUEUE_PRESSURE_MAX 4
#define SGIL1_QUEUE_PRESSURE_DECAY_MS 10000
#define SGIL1_TUI_LOG_HISTORY_DEFAULT 4096
#define SGIL1_TUI_LED_HISTORY_DEFAULT 512
#define SGIL1_TUI_HISTORY_MAX 1000000
#define SGIL1_TUI_LED_CONTENT_MAX 127
#define SGIL1_TUI_LOG_CONTINUATION_INDENT 18
#define SGIL1_TUI_LED_CONTINUATION_INDENT 14
#define SGIL1_TUI_TIMESTAMP_SIZE 24
#define SGIL1_TUI_ESCAPE_DELAY_MS 0
#define SGIL1_READ_CANCELLED -2

static int sgil1_lock_fd = -1;
static volatile sig_atomic_t watch_stop_requested;
static bool l1_wait_cancel_enabled;
static bool (*l1_wait_input_hook)(void *context);
static void *l1_wait_input_context;

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
	bool dest_auto_discovered;
	bool skip_command_drain;
	bool debug;
	bool show_annotations;
};

struct status_options {
	bool set_time;
	const char *timezone;
	char timezone_buf[SGIL1_TZ_MAX];
	int drift_seconds;
};

struct l1_firmware_version {
	unsigned int major;
	unsigned int minor;
	unsigned int patch;
	bool qualified_command_image;
};

struct wait_options {
	struct status_options status;
	bool power_up;
	bool power_down;
	bool reset;
	bool force;
	bool follow;
	bool background;
	int wait_timeout_seconds;
	int keepalive_seconds;
};

struct log_options {
	bool follow;
	bool repeat_summary;
	int poll_interval_ms;
};

struct leds_options {
	bool follow;
	bool show_annotations;
	int poll_interval_ms;
};

enum watch_palette {
	WATCH_PALETTE_AUTO,
	WATCH_PALETTE_INDIGO,
	WATCH_PALETTE_CRIMSON,
	WATCH_PALETTE_INDY,
	WATCH_PALETTE_INDIGO2,
	WATCH_PALETTE_ONYX,
	WATCH_PALETTE_CHALLENGE,
	WATCH_PALETTE_IMPACT,
	WATCH_PALETTE_O2,
	WATCH_PALETTE_OCTANE,
	WATCH_PALETTE_ONYX2,
	WATCH_PALETTE_OCTANE2,
	WATCH_PALETTE_O2PLUS,
	WATCH_PALETTE_FUEL,
	WATCH_PALETTE_TEZRO,
	WATCH_PALETTE_PERSONAL_IRIS,
	WATCH_PALETTE_MONOCHROME,
	WATCH_PALETTE_COUNT,
};

struct watch_options {
	bool tui;
	bool show_annotations;
	bool alternate_screen;
	bool repeat_summary;
	int log_interval_ms;
	int led_interval_ms;
	size_t log_history;
	size_t led_history;
	enum watch_palette palette;
};

struct debug_options {
	bool list_switches;
	bool update;
	bool force;
	bool have_set;
	bool have_test;
	bool have_boot_stop;
	uint32_t set_value;
	uint32_t enable_mask;
	uint32_t disable_mask;
	uint32_t test_value;
	uint32_t boot_stop_value;
};

enum leds_follow_power_confirm {
	LEDS_FOLLOW_CONFIRM_NONE,
	LEDS_FOLLOW_CONFIRM_POWER_ON,
	LEDS_FOLLOW_CONFIRM_POWER_OFF,
};

static int l1_text_command_status(const struct options *opts, const char *cmd,
				  bool allow_time_setting, char **text);
static char *l1_text_command(const struct options *opts, const char *cmd,
			     bool allow_time_setting);
static void print_text_block(const char *text);
static void print_l1_command_text_block(const char *l1cmd, const char *text);
static int do_debug_command(const struct options *opts, int argc, char **argv,
			    int command_index);
static int do_power_up_confirmed(const struct options *opts, bool confirm);
static int do_power_down_confirmed(const struct options *opts,
				   bool force_second_signal, bool confirm);
static int do_host_softreset_confirmed(const struct options *opts,
				       const char *l1cmd, bool allow_destructive,
				       bool follow);
static int do_leds_follow(const struct options *opts, int poll_interval_ms,
			  enum leds_follow_power_confirm confirm_power);
static int do_watch_command(const struct options *opts, int argc, char **argv,
			    int command_index);
static const char *find_existing_data_device(const struct options *opts);
static int prepare_command_options(const struct options *opts,
				   struct options *cmd_opts);
static uint64_t monotonic_milliseconds(void);
static uint64_t milliseconds_after(uint64_t now, int delay);
static int queue_pressure_scaled_delay(int delay, unsigned int pressure);
static bool log_line_is_queue_full(const char *line);
static bool log_line_is_queue_feedback(const char *line);

static bool is_help_option(const char *arg)
{
	return !strcmp(arg, "-h") || !strcmp(arg, "--help");
}

static void usage(FILE *out, bool full)
{
	fprintf(out,
		"Usage: sgil1ctl [GLOBAL OPTIONS] COMMAND [COMMAND OPTIONS]\n"
		"\n"
			"Global options:\n"
			"  --device PATH         data device path (default: auto)\n"
			"                        auto scans /dev/sgi-l1/l1-*,\n"
			"                        /dev/sgil1_*, and /dev/usb/sgil1_*\n"
			"  --status-device PATH  status device path (default: auto)\n"
			"                        auto tries /dev/sgi-l1/status, /dev/sgil1_cs\n"
			"  --timeout MS          poll timeout for reads (default 3000)\n"
			"  --force               confirm guarded actions or unlisted pass-through\n"
			"  --debug               show IRouter framing diagnostics\n"
			"  --show-annotations    show sources of LED descriptions\n"
			"  --version             show the installed sgil1ctl version\n"
			"  -h, --help            show this help\n"
			"  --help-all            show all commands and low-level options\n"
			"                        global options are parsed before COMMAND\n"
			"                        use 'sgil1ctl COMMAND --help' for command options\n"
		"\n"
		"User commands:\n"
			"  status                print consolidated L1 health/status data\n"
			"  date [OPTIONS]        show or set the L1 date/time\n"
			"  set-date [OPTIONS]    set the L1 date/time from the host\n"
				"                        date options: --set-time, --timezone TZ,\n"
				"                        --drift-seconds SEC (default 60)\n"
			"  wait [-w|--follow] [OPTIONS]\n"
				"                        wait for an L1 USB device, then run status checks\n"
				"                        wait options: --background, --wait-timeout SEC,\n"
				"                        --set-time, --timezone TZ, --drift-seconds SEC,\n"
				"                        --power-up, --power-down, --reset, -w|--follow,\n"
				"                        --keepalive SEC (default 0), --force\n"
			"  version|usb|env       send read-only L1 text commands over USB\n"
			"  log [-w|--follow]     print the L1 log, optionally following new lines\n"
				"                        log options: --poll-interval MS,\n"
				"                        --no-repeat-summary\n"
			"  leds [-w|--follow]    print L1 front-panel LEDs, optionally following changes\n"
			"  watch [OPTIONS]       follow L1 logs and LEDs through one USB scheduler\n"
			"  debug [OPTIONS]       show/decode L1 debug switches and l1dbg state\n"
			"  power [check]         show L1 power data or state\n"
			"  power up [-w|--follow]\n"
			"                        power on system; confirms state afterward\n"
			"  power down [-w|--follow]\n"
			"                        send one power-down signal; confirms state\n"
			"  power reset --force [-w|--follow]\n"
			"                        issue host soft reset; optionally follow LEDs\n"
			"  reset --force [-w|--follow]\n"
			"                        send L1 controller reset; follow LEDs if requested\n"
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
		"  raw-send HEX...       write raw USB transfer; first two bytes\n"
		"                        are overwritten by the driver\n"
		"  raw-recv              read one raw USB transfer and print a hexdump\n"
		"  monitor               print raw USB transfers until interrupted\n"
		"  build-l1cmd <command> [...]\n"
		"                        print IRouter frame for an allowlisted L1 text command\n"
		"\n"
		"Pass-through notes:\n"
		"  l1cmd '*' <command>   SGI broadcast-prefix form; quote '*'\n"
		"                        to avoid shell expansion\n"
		"                        direct USB text is limited to 72 bytes on\n"
		"                        legacy or unknown firmware, and 279 bytes on\n"
		"                        qualified Fuel/PE/O300 images, L1 1.26.5 or newer\n");
}

static void command_usage_footer(FILE *out)
{
	fprintf(out,
		"\n"
		"Global options are parsed before COMMAND; run 'sgil1ctl --help' for them.\n");
}

static bool command_usage(FILE *out, const char *cmd)
{
	if (!strcmp(cmd, "status")) {
		fprintf(out,
			"Usage: sgil1ctl [GLOBAL OPTIONS] status\n"
			"\n"
			"Print consolidated L1 health/status data, including firmware,\n"
			"identity, clock, power, environment, and USB transport state.\n"
			"\n"
			"Options:\n"
			"  None\n");
		command_usage_footer(out);
		return true;
	}
	if (!strcmp(cmd, "date") || !strcmp(cmd, "set-date") ||
	    !strcmp(cmd, "clock") ||
	    !strcmp(cmd, "time") || !strcmp(cmd, "set-clock") ||
	    !strcmp(cmd, "set-time")) {
		fprintf(out,
			"Usage: sgil1ctl [GLOBAL OPTIONS] date [OPTIONS]\n"
			"       sgil1ctl [GLOBAL OPTIONS] set-date [OPTIONS]\n"
			"\n"
			"Show the L1 date/time, or set it from the host when requested.\n"
			"\n"
			"Options:\n"
			"  --set-time            set the L1 date/time from the host\n"
			"  --timezone TZ         set L1 timezone before setting time\n"
			"                        default: host timezone with --set-time\n"
			"  --drift-seconds SEC   set time only when drift is at least SEC\n"
			"                        default: 60\n");
		command_usage_footer(out);
		return true;
	}
	if (!strcmp(cmd, "wait")) {
		fprintf(out,
			"Usage: sgil1ctl [GLOBAL OPTIONS] wait [OPTIONS]\n"
			"\n"
			"Wait for L1 USB, run status checks, and optionally perform a guarded\n"
			"power action.\n"
			"\n"
			"Options:\n"
			"  --background          wait for next bind event, ignoring existing device\n"
			"  --wait-timeout SEC    maximum seconds to wait; default: none\n"
			"  --set-time            set the L1 clock from the host after discovery\n"
			"  --timezone TZ         set L1 timezone before setting time\n"
			"                        default: host timezone with --set-time\n"
			"  --drift-seconds SEC   set time only when drift is at least SEC\n"
			"                        default: 60\n"
			"  --power-up            power on if status reports system off\n"
			"  --power-down          power off after status checks\n"
			"  --reset               issue host soft reset after status checks\n"
			"  -w, --follow          follow LEDs after selected power action\n"
			"  --keepalive SEC       keep waiting after success; re-enter wait\n"
			"                        mode if device disappears; default: 0\n"
			"  --force, --yes        confirm power/reset actions\n");
		command_usage_footer(out);
		return true;
	}
	if (!strcmp(cmd, "log") || !strcmp(cmd, "logs")) {
		fprintf(out,
			"Usage: sgil1ctl [GLOBAL OPTIONS] log [OPTIONS]\n"
			"\n"
			"Print the L1 log, or follow newly observed log lines.\n"
			"\n"
			"Options:\n"
			"  -w, --follow          continue polling for new log lines\n"
			"  --poll-interval MS    steady follow poll interval; minimum: 100\n"
			"  --no-repeat-summary   print repeated lines without summary folding\n");
		command_usage_footer(out);
		return true;
	}
	if (!strcmp(cmd, "leds") || !strcmp(cmd, "led")) {
		fprintf(out,
			"Usage: sgil1ctl [GLOBAL OPTIONS] leds [OPTIONS]\n"
			"\n"
			"Print L1 front-panel LEDs, or follow changed LED output.\n"
			"\n"
			"Options:\n"
			"  -w, --follow          continue polling for changed LED output\n"
			"  --poll-interval MS    steady follow poll interval; minimum: 50\n"
			"  --show-annotations    show sources of LED descriptions\n");
		command_usage_footer(out);
		return true;
	}
	if (!strcmp(cmd, "watch")) {
		fprintf(out,
			"Usage: sgil1ctl [GLOBAL OPTIONS] watch [OPTIONS]\n"
			"\n"
			"Follow L1 logs and decoded LED states. Text output is used unless\n"
			"the optional TUI is selected.\n"
			"\n"
			"Options:\n"
			"  --tui                 use the optional ncurses split-pane interface\n"
			"  --no-alternate-screen keep the TUI on the terminal's primary screen\n"
			"  --log-interval MS     quiet log polling interval; minimum 100,\n"
			"                        default 1000\n"
			"  --led-interval MS     active LED polling interval; minimum 100,\n"
			"                        default 100\n"
			"  --log-history ENTRIES retained TUI log messages; default 4096\n"
			"  --led-history ENTRIES retained TUI LED responses; default 512\n"
			"  --palette NAME        select TUI palette; default auto\n"
			"                        hardware and colour aliases are accepted\n"
			"  --show-annotations    show sources of LED descriptions\n"
			"  --no-repeat-summary   print repeated log messages individually\n"
			"\n"
			"TUI keys:\n"
			"  Tab                   select log or LED pane\n"
			"  Left/Right, </>       move divider; resize pane when stacked\n"
			"  ^/v                   shrink/grow selected pane when stacked\n"
			"  Up/Down, k/j          scroll selected pane by one line\n"
			"  PgUp/PgDn, Ctrl-B/F   scroll selected pane by one page\n"
			"  End, g                return selected pane to live output\n"
			"  a                     toggle Filtered and All observations\n"
			"  t                     show or hide LED timestamps\n"
			"  p                     show or hide LED description sources\n"
			"  c, m                  cycle palettes or toggle monochrome\n"
			"  h, ?                  show Help; Esc or Return closes it\n"
			"  Ctrl-L                redraw the screen\n"
			"  q                     quit\n");
		command_usage_footer(out);
		return true;
	}
	if (!strcmp(cmd, "debug")) {
		fprintf(out,
			"Usage: sgil1ctl [GLOBAL OPTIONS] debug [OPTIONS]\n"
			"\n"
			"Show SGI virtual debug switches decoded from L1 'debug', then show the\n"
			"current L1 'l1dbg' settings. Only update options change virtual debug switches.\n"
			"\n"
			"Options:\n"
			"  --show                show current debug state; default action\n"
			"  --set SWITCHES        set the absolute virtual debug switch value\n"
			"  --enable SWITCH...    set named virtual debug switches\n"
			"  --disable SWITCH...   clear named virtual debug switches\n"
			"  --test MODE           set diagnostic testing mode\n"
			"  --boot-stop POINT     set PROM boot stop point bits\n"
			"                        use POINT 'none' to clear them\n"
			"  --list-switches       list switch, mode, and boot-stop names\n"
			"  --force, --yes        confirm virtual debug switch updates\n");
		command_usage_footer(out);
		return true;
	}
	if (!strcmp(cmd, "power")) {
		fprintf(out,
			"Usage: sgil1ctl [GLOBAL OPTIONS] power [SUBCOMMAND] [OPTIONS]\n"
			"\n"
			"Show L1 power data, power the system on/off, or issue a host\n"
			"soft reset. Use top-level 'reset' for an L1 controller reset.\n"
			"\n"
			"Subcommands:\n"
			"  (none)                show L1 power data\n"
			"  check                 show whether system appears on or off\n"
			"  up                    power on; confirms eventual state\n"
			"  down                  send one power-down signal; confirms eventual state\n"
			"  reset|softreset|softrst  issue host soft reset using L1 softreset\n"
			"\n"
			"Options:\n"
			"  --force, --yes        send second power-down signal; confirm reset\n"
			"                        actions; accepted for power up\n"
			"  -w, --follow          follow LEDs after power up, down, or reset\n"
			"                        buffer up/down LEDs until confirmation\n");
		command_usage_footer(out);
		return true;
	}
	if (!strcmp(cmd, "power-up")) {
		fprintf(out,
			"Usage: sgil1ctl [GLOBAL OPTIONS] power-up [-w|--follow]\n"
			"\n"
			"Compatibility alias for 'power up'.\n"
			"\n"
			"Options:\n"
			"  --force, --yes        accepted for compatibility; not required\n"
			"  -w, --follow          follow LEDs after power up\n");
		command_usage_footer(out);
		return true;
	}
	if (!strcmp(cmd, "power-down")) {
		fprintf(out,
			"Usage: sgil1ctl [GLOBAL OPTIONS] power-down [-w|--follow]\n"
			"\n"
			"Compatibility alias for 'power down'.\n"
			"\n"
			"Options:\n"
			"  --force, --yes        send a second power-down signal\n"
			"  -w, --follow          follow LEDs after power down\n");
		command_usage_footer(out);
		return true;
	}
	if (!strcmp(cmd, "reset")) {
		fprintf(out,
			"Usage: sgil1ctl [GLOBAL OPTIONS] reset --force [-w|--follow]\n"
			"\n"
			"Send the L1 controller reset; use 'power reset --force' for host reboot.\n"
			"\n"
			"Options:\n"
			"  --force, --yes        confirm L1 controller reset\n"
			"  -w, --follow          follow LEDs after reset command path\n");
		command_usage_footer(out);
		return true;
	}
	if (!strcmp(cmd, "l1cmd") || !strcmp(cmd, "command") ||
	    !strcmp(cmd, "send")) {
		fprintf(out,
			"Usage: sgil1ctl [GLOBAL OPTIONS] l1cmd <command> [...]\n"
			"       sgil1ctl [GLOBAL OPTIONS] l1cmd '*' <command> [...]\n"
			"\n"
			"Send a live-help-listed L1 text command over USB.\n"
			"\n"
			"Options:\n"
			"  --force, --yes        send unlisted commands, or confirm guarded\n"
			"                        power/reset pass-throughs\n"
			"\n"
			"Notes:\n"
			"  Quote '*' to use SGI's broadcast-prefix form.\n"
			"  Direct USB text is limited to 72 bytes on legacy or unknown\n"
			"  firmware and 279 bytes on qualified Fuel/PE/O300 images\n"
			"  from L1 1.26.5 onward.\n"
			"  Use 'l1cmd help' to ask the L1 for its own command list.\n");
		command_usage_footer(out);
		return true;
	}
	if (!strcmp(cmd, "build-l1cmd")) {
		fprintf(out,
			"Usage: sgil1ctl [GLOBAL OPTIONS] build-l1cmd <command> [...]\n"
			"\n"
			"Build and print the IRouter frame for an L1 text command without\n"
			"sending it to the device.\n"
			"\n"
			"Options:\n"
			"  --force, --yes        build unlisted commands, or confirm guarded\n"
			"                        power/reset command frames\n"
			"\n"
			"Notes:\n"
			"  Frames with up to 279 bytes of command text can be built.\n"
			"  Confirm the target firmware limit before sending one manually.\n");
		command_usage_footer(out);
		return true;
	}
	if (!strcmp(cmd, "probe")) {
		fprintf(out,
			"Usage: sgil1ctl [GLOBAL OPTIONS] probe\n"
			"\n"
			"Show status device and first available data device.\n"
			"\n"
			"Options:\n"
			"  None\n");
		command_usage_footer(out);
		return true;
	}
	if (!strcmp(cmd, "discover")) {
		fprintf(out,
			"Usage: sgil1ctl [GLOBAL OPTIONS] discover\n"
			"\n"
			"Discover and print the L1 command destination address.\n"
			"\n"
			"Options:\n"
			"  None\n");
		command_usage_footer(out);
		return true;
	}
	if (!strcmp(cmd, "version") || !strcmp(cmd, "usb") ||
	    !strcmp(cmd, "env")) {
		fprintf(out,
			"Usage: sgil1ctl [GLOBAL OPTIONS] %s\n"
			"\n"
			"Send the read-only L1 '%s' text command over USB.\n"
			"\n"
			"Options:\n"
			"  None\n",
			cmd, cmd);
		command_usage_footer(out);
		return true;
	}
	if (!strcmp(cmd, "driver-status")) {
		fprintf(out,
			"Usage: sgil1ctl [GLOBAL OPTIONS] driver-status\n"
			"\n"
			"Read and print the sgil1_cs status bitmap.\n"
			"\n"
			"Options:\n"
			"  None\n");
		command_usage_footer(out);
		return true;
	}
	if (!strcmp(cmd, "read-cfg")) {
		fprintf(out,
			"Usage: sgil1ctl [GLOBAL OPTIONS] read-cfg\n"
			"\n"
			"Read SGIL1_READ_CFG from the data device.\n"
			"\n"
			"Options:\n"
			"  None\n");
		command_usage_footer(out);
		return true;
	}
	if (!strcmp(cmd, "reset-read") || !strcmp(cmd, "reset-write") ||
	    !strcmp(cmd, "reset-pipes")) {
		fprintf(out,
			"Usage: sgil1ctl [GLOBAL OPTIONS] %s\n"
			"\n"
			"Run the %s USB recovery ioctl.\n"
			"\n"
			"Options:\n"
			"  None\n",
			cmd, cmd);
		command_usage_footer(out);
		return true;
	}
	if (!strcmp(cmd, "reset-device")) {
		fprintf(out,
			"Usage: sgil1ctl [GLOBAL OPTIONS] reset-device --force\n"
			"\n"
			"Issue a USB device reset.\n"
			"\n"
			"Options:\n"
			"  --force, --yes        confirm USB device reset\n");
		command_usage_footer(out);
		return true;
	}
	if (!strcmp(cmd, "raw-send")) {
		fprintf(out,
			"Usage: sgil1ctl [GLOBAL OPTIONS] raw-send HEX...\n"
			"\n"
			"Write one raw USB transfer. The first two bytes are overwritten\n"
			"by the driver.\n"
			"\n"
			"Options:\n"
			"  None\n");
		command_usage_footer(out);
		return true;
	}
	if (!strcmp(cmd, "raw-recv")) {
		fprintf(out,
			"Usage: sgil1ctl [GLOBAL OPTIONS] raw-recv\n"
			"\n"
			"Read one raw USB transfer and print a hexdump.\n"
			"\n"
			"Options:\n"
			"  None\n");
		command_usage_footer(out);
		return true;
	}
	if (!strcmp(cmd, "monitor")) {
		fprintf(out,
			"Usage: sgil1ctl [GLOBAL OPTIONS] monitor\n"
			"\n"
			"Print raw USB transfers until interrupted.\n"
			"\n"
			"Options:\n"
			"  None\n");
		command_usage_footer(out);
		return true;
	}

	return false;
}

static bool command_help_requested(int argc, char **argv, int start)
{
	int i;

	for (i = start; i < argc; i++) {
		if (is_help_option(argv[i]))
			return true;
	}

	return false;
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
	       streq_ci(cmd, "leds") || streq_ci(cmd, "debug") ||
	       streq_ci(cmd, "l1dbg") ||
	       streq_ci(cmd, "date") || streq_ci(cmd, "date tz") ||
	       streq_ci(cmd, "serial") || streq_ci(cmd, "serial all") ||
	       streq_ci(cmd, "log") ||
	       streq_ci(cmd, "power") || streq_ci(cmd, "pwr") ||
	       streq_ci(cmd, "power check") || streq_ci(cmd, "pwr check");
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

static bool l1_command_matches_prefix(const char *cmd, const char *prefix)
{
	size_t len = strlen(prefix);

	return startswith_ci(cmd, prefix) &&
	       (cmd[len] == '\0' || isspace((unsigned char)cmd[len]));
}

static bool l1_command_is_destructive(const char *cmd)
{
	return l1_command_matches_prefix(cmd, "power up") ||
	       l1_command_matches_prefix(cmd, "pwr up") ||
	       l1_command_matches_prefix(cmd, "pwr u") ||
	       l1_command_matches_prefix(cmd, "power down") ||
	       l1_command_matches_prefix(cmd, "pwr down") ||
	       l1_command_matches_prefix(cmd, "pwr d") ||
	       l1_command_matches_prefix(cmd, "reset") ||
	       l1_command_matches_prefix(cmd, "softreset") ||
	       l1_command_matches_prefix(cmd, "softrst");
}

static bool parse_l1_firmware_version(const char *text,
				      struct l1_firmware_version *version)
{
	const char *p = text;

	while ((p = strstr(p, "L1 ")) != NULL) {
		if (sscanf(p, "L1 %u.%u.%u", &version->major,
			   &version->minor, &version->patch) == 3) {
			/* Fuel/PE[/O300] is a shared image label, not a chassis model. */
			version->qualified_command_image = contains_ci(text, "Fuel/PE");
			return true;
		}
		p += 3;
	}

	return false;
}

static bool l1_firmware_supports_extended_commands(
	const struct l1_firmware_version *version)
{
	if (!version->qualified_command_image || version->major != 1)
		return false;
	if (version->minor > 26)
		return true;
	return version->minor == 26 && version->patch >= 5;
}

static int validate_l1_command_text_len(const char *cmd)
{
	size_t text_len = strlen(cmd);

	if (text_len <= SGIL1_L1_MAX_COMMAND_TEXT)
		return 0;

	fprintf(stderr,
		"refusing L1 command text of %zu bytes; maximum supported direct-USB command text is %u bytes (%u-byte IRouter transfer)\n",
		text_len, SGIL1_L1_MAX_COMMAND_TEXT,
		SGIL1_L1_COMMAND_TRANSFER(SGIL1_L1_MAX_COMMAND_TEXT));
	return -1;
}

static int validate_live_l1_command_text_len(const struct options *opts,
					     const char *cmd)
{
	struct l1_firmware_version version = { 0 };
	char *version_text = NULL;
	size_t text_len = strlen(cmd);
	unsigned int max_text = SGIL1_L1_LEGACY_MAX_COMMAND_TEXT;
	int ret;

	if (text_len <= SGIL1_L1_LEGACY_MAX_COMMAND_TEXT)
		return 0;
	if (validate_l1_command_text_len(cmd))
		return -1;

	ret = l1_text_command_status(opts, "version", false, &version_text);
	if (ret || !version_text ||
	    !parse_l1_firmware_version(version_text, &version)) {
		fprintf(stderr,
			"could not qualify the live L1 firmware command limit; refusing %zu-byte command above the conservative %u-byte limit\n",
			text_len, SGIL1_L1_LEGACY_MAX_COMMAND_TEXT);
		free(version_text);
		return -1;
	}

	if (l1_firmware_supports_extended_commands(&version))
		max_text = SGIL1_L1_EXTENDED_MAX_COMMAND_TEXT;
	free(version_text);

	if (text_len <= max_text)
		return 0;

	fprintf(stderr,
		"refusing L1 command text of %zu bytes for L1 %u.%u.%u; maximum qualified direct-USB command text is %u bytes (%u-byte IRouter transfer)\n",
		text_len, version.major, version.minor, version.patch, max_text,
		SGIL1_L1_COMMAND_TRANSFER(max_text));
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

static uint16_t next_l1_command_seq(void)
{
	static unsigned int next_seq;
	uint16_t seq;

	if (!next_seq) {
		next_seq = ((unsigned int)getpid() ^ (unsigned int)time(NULL)) %
			   0x7fU;
		if (!next_seq)
			next_seq = 1;
	}

	seq = (uint16_t)next_seq;
	next_seq++;
	if (next_seq > 0x7fU)
		next_seq = 1;

	return seq;
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

static bool irouter_frame_matches_command_response(const uint8_t *buf,
						   size_t len,
						   uint32_t request_src,
						   uint32_t request_dest,
						   uint16_t request_seq)
{
	if (len < SGIL1_IR_HEADER_LEN)
		return false;
	if (get_be16(buf) > len)
		return false;
	if (buf[2] != 2)
		return false;
	if (get_be32(buf + 8) != request_src)
		return false;
	if (get_be32(buf + 12) != request_dest)
		return false;
	if (buf[4] != (request_seq & 0xff))
		return false;

	return true;
}

static bool l1_wait_cancel_requested(void)
{
	if (!l1_wait_cancel_enabled)
		return false;
	if (watch_stop_requested)
		return true;
	return l1_wait_input_hook &&
	       l1_wait_input_hook(l1_wait_input_context);
}

static int poll_l1_fd(int fd, short events, int timeout_ms, short *revents)
{
	int remaining = timeout_ms;

	for (;;) {
		struct pollfd pfd = {
			.fd = fd,
			.events = events,
		};
		int poll_timeout = remaining;
		int ret;

		if (l1_wait_cancel_requested())
			return SGIL1_READ_CANCELLED;
		if (l1_wait_cancel_enabled &&
		    (poll_timeout < 0 || poll_timeout > 100))
			poll_timeout = 100;

		ret = poll(&pfd, 1, poll_timeout);
		if (ret > 0) {
			*revents = pfd.revents;
			return ret;
		}
		if (ret < 0) {
			if (errno == EINTR) {
				if (l1_wait_cancel_requested())
					return SGIL1_READ_CANCELLED;
				continue;
			}
			return ret;
		}
		if (l1_wait_cancel_requested())
			return SGIL1_READ_CANCELLED;
		if (timeout_ms < 0)
			continue;
		remaining -= poll_timeout;
		if (remaining <= 0)
			return 0;
	}
}

static int read_raw_irouter_frame(int fd, int timeout_ms, uint8_t *buf,
				  size_t cap, size_t *out_len)
{
	for (;;) {
		short revents = 0;
		int ret;
		ssize_t n;

		ret = poll_l1_fd(fd, POLLIN | POLLHUP, timeout_ms, &revents);
		if (ret == SGIL1_READ_CANCELLED)
			return ret;
		if (ret < 0) {
			fprintf(stderr, "poll failed: %s\n", strerror(errno));
			return -1;
		}
		if (ret == 0)
			return 0;
		if (revents & POLLHUP) {
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

static unsigned int drain_raw_irouter_frames_fd(int fd, int quiet_ms,
						bool verbose)
{
	uint8_t buf[SGIL1_IO_SIZE];
	unsigned int drained;

	for (drained = 0; drained < SGIL1_DRAIN_MAX_FRAMES; drained++) {
		size_t len = 0;
		int ret = read_raw_irouter_frame(fd, quiet_ms, buf, sizeof(buf),
						 &len);

		if (ret <= 0)
			break;
		if (verbose) {
			printf("drained stale IRouter frame (%zu bytes):\n", len);
			print_irouter_frame(buf, len);
		}
	}

	if (drained == SGIL1_DRAIN_MAX_FRAMES)
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

	drain_raw_irouter_frames_fd(fd, SGIL1_DRAIN_QUIET_MS, verbose);

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
			if (ret == SGIL1_READ_CANCELLED)
				return ret;
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
		short revents = 0;
		int poll_timeout = started && timeout_ms >= 0 ? 500 : timeout_ms;
		uint16_t type;
		uint16_t total_len;
		size_t payload_len;
		int ret;
		ssize_t n;

		ret = poll_l1_fd(fd, POLLIN | POLLHUP, poll_timeout,
				 &revents);
		if (ret == SGIL1_READ_CANCELLED)
			return ret;
		if (ret < 0) {
			fprintf(stderr, "poll failed: %s\n", strerror(errno));
			return -1;
		}
		if (ret == 0) {
			if (started)
				fprintf(stderr,
					"timed out waiting for pipe continuation\n");
			return started ? -1 : 0;
		}
		if (revents & POLLHUP) {
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

static unsigned int drain_pipe_irouter_frames_fd(int fd, int quiet_ms,
						 bool verbose)
{
	uint8_t buf[SGIL1_IO_SIZE];
	unsigned int drained;

	for (drained = 0; drained < SGIL1_DRAIN_MAX_FRAMES; drained++) {
		size_t len = 0;
		int ret = read_pipe_irouter_frame(fd, quiet_ms, buf, sizeof(buf),
						  &len);

		if (ret <= 0)
			break;
		if (verbose) {
			printf("drained stale SGI pipe IRouter frame (%zu bytes):\n",
			       len);
			print_irouter_frame(buf, len);
		}
	}

	if (drained == SGIL1_DRAIN_MAX_FRAMES)
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
	unsigned int accepted_frames = 0;
	unsigned int scanned_frames;
	unsigned int stale_frames = 0;
	struct ir_text_assembly assembly = { 0 };
	uint32_t dest_addr = opts->dest_addr;
	uint16_t seq = next_l1_command_seq();
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
			"refusing L1 command '%s': add --force to confirm system power/reset action\n",
			l1cmd);
		return 2;
	}
	if (validate_live_l1_command_text_len(opts, l1cmd))
		return 2;

	fd = open_data_device(opts, O_RDWR | O_NONBLOCK);
	if (fd < 0)
		return 1;

	if (!opts->skip_command_drain) {
		if (opts->pipe_records)
			drain_pipe_irouter_frames_fd(fd, SGIL1_DRAIN_QUIET_MS,
						     opts->debug);
		else
			drain_raw_irouter_frames_fd(fd, SGIL1_DRAIN_QUIET_MS,
						    opts->debug);
	}

	if (!opts->pipe_records && !opts->no_discover && !opts->dest_overridden) {
		int discover_ret;

		discover_ret = discover_l1_command_dest_fd(opts, fd, &dest_addr,
							   verbose);
		if (discover_ret) {
			ret = discover_ret;
			goto out;
		}
		drain_raw_irouter_frames_fd(fd, SGIL1_DRAIN_QUIET_MS,
					    opts->debug);
	}

	if (build_l1_command_frame(l1cmd, seq, dest_addr, opts->src_addr,
				   opts->ir_class, opts->authority, opts->pdata,
				   tx, sizeof(tx), &tx_len))
		goto out;

	if (opts->pipe_records) {
		if (write_pipe_frame(fd, tx, tx_len, &pipe_records)) {
			recover_l1_pipes_fd(fd, "L1 command write failure");
			goto out;
		}

		if (verbose)
			printf("sent L1 command '%s' in %zu-byte IRouter frame (%zu SGI pipe record%s, seq=0x%02x dest=0x%08x src=0x%08x class=%u auth=%u pdata=%u)\n",
			       l1cmd, tx_len, pipe_records,
			       pipe_records == 1 ? "" : "s", seq & 0xff, dest_addr,
			       opts->src_addr, opts->ir_class,
			       opts->authority, opts->pdata);
	} else {
		if (write_one_transfer(fd, tx, tx_len)) {
			recover_l1_pipes_fd(fd, "L1 command write failure");
			goto out;
		}

		if (verbose)
			printf("sent L1 command '%s' in %zu-byte raw IRouter frame (seq=0x%02x dest=0x%08x src=0x%08x class=%u auth=%u pdata=%u)\n",
			       l1cmd, tx_len, seq & 0xff, dest_addr, opts->src_addr,
			       opts->ir_class, opts->authority, opts->pdata);
	}

	for (scanned_frames = 0; scanned_frames < SGIL1_RESPONSE_SCAN_MAX_FRAMES;
	     scanned_frames++) {
		int timeout = accepted_frames ? 500 : opts->timeout_ms;

		if (opts->pipe_records)
			ret = read_pipe_irouter_frame(fd, timeout, rx,
						      sizeof(rx), &rx_len);
		else
			ret = read_raw_irouter_frame(fd, timeout, rx,
						     sizeof(rx), &rx_len);
		if (ret == SGIL1_READ_CANCELLED)
			goto out;
		if (ret < 0) {
			recover_l1_pipes_fd(fd, "L1 response read failure");
			ret = 1;
			goto out;
		}
		if (ret == 0) {
			if (!accepted_frames) {
				if (response_timeout_is_pending)
					fprintf(stderr,
						"timed out waiting for immediate response; checking command result\n");
				else
					fprintf(stderr,
						"timed out waiting for response\n");
			} else {
				fprintf(stderr,
					"timed out waiting for remaining response frames\n");
			}
			if (!response_timeout_is_pending)
				recover_l1_pipes_fd(fd, "L1 response timeout");
			ret = accepted_frames ? 1 : SGIL1_L1CMD_RESPONSE_TIMEOUT;
			goto out;
		}

		if (verbose)
			printf("read %zu-byte IRouter frame\n", rx_len);

		if (!irouter_frame_matches_command_response(rx, rx_len,
							    opts->src_addr,
							    dest_addr,
							    seq)) {
			stale_frames++;
			if (verbose) {
				uint32_t rx_dest = rx_len >= 16 ?
						   get_be32(rx + 8) : 0;
				uint32_t rx_src = rx_len >= 16 ?
						  get_be32(rx + 12) : 0;

				fprintf(stderr,
					"ignoring stale IRouter frame seq=0x%02x dest=0x%08x src=0x%08x len=%zu while waiting for command seq=0x%02x\n",
					rx_len >= 5 ? rx[4] : 0, rx_dest,
					rx_src, rx_len, seq & 0xff);
			}
			continue;
		}

		if (!accepted_frames && rx_len >= 6) {
			expected_frames = rx[5] & 0x7f;
			if (!expected_frames)
				expected_frames = 1;
		}

		if (verbose)
			print_irouter_frame(rx, rx_len);

		if (collect_ir_text_frame(&assembly, rx, rx_len,
					  accepted_frames == 0)) {
			ret = 1;
			goto out;
		}
		accepted_frames++;

		if (accepted_frames >= expected_frames) {
			if (stale_frames >= SGIL1_STALE_RESPONSE_WARN_FRAMES)
				fprintf(stderr,
					"warning: ignored %u stale IRouter frames before matching '%s' response\n",
					stale_frames, l1cmd);
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

	fprintf(stderr,
		"stopped after scanning %u response frames without a matching '%s' response\n",
		SGIL1_RESPONSE_SCAN_MAX_FRAMES, l1cmd);
	recover_l1_pipes_fd(fd, "stale IRouter response flood");
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
		print_l1_command_text_block(l1cmd, text);
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

	if (!force && l1_command_is_destructive(lookup_cmd)) {
		fprintf(stderr,
			"refusing L1 command '%s': add --force to confirm system power/reset action\n",
			l1cmd);
		free(command_word);
		return 2;
	}
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
		if (!l1_help_advertises_command(help_text, l1cmd))
			command_help = l1_help_for_command(opts, l1cmd);
		if (!l1_help_advertises_command(help_text, l1cmd) &&
		    !(l1_help_text_is_valid(command_help) &&
		      l1_help_advertises_command(command_help, l1cmd))) {
			fprintf(stderr,
				"refusing L1 command '%s': not advertised by live L1 help; add --force to send anyway\n",
				l1cmd);
			free(help_text);
			free(command_help);
			free(command_word);
			return 2;
		}
		advertised_by_help = true;
		free(help_text);
		help_text = NULL;
	}

	ret = run_l1_command_core(opts, l1cmd, true, true, true, false,
				  opts->debug, &text);
	if (ret) {
		free(command_help);
		free(command_word);
		return ret;
	}

	l1_failed = l1_text_indicates_failure(text);
	if (l1_failed) {
		if (advertised_by_help && parent_only &&
		    l1_text_is_command_not_found(text)) {
			if (!command_help)
				command_help = l1_help_for_command(opts, l1cmd);
			suppress_failure_text =
				l1_help_lists_child_command(command_help,
							    command_word);
		}
		if (!suppress_failure_text && !opts->debug)
			print_l1_command_text_block(lookup_cmd, text);
		if (suppress_failure_text) {
			printf("Help for '%s':\n", l1cmd);
			print_text_block(command_help);
		}
	} else if (!opts->debug) {
		print_l1_command_text_block(lookup_cmd, text);
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
			"refusing L1 command '%s': add --force to confirm system power/reset action\n",
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
	const char *action;
	bool force = opts->force;
	bool follow = false;
	int i;
	int ret;

	if (command_index + 1 == argc)
		return do_l1_command(opts, "power", false);
	if (command_index + 2 == argc && streq_ci(argv[command_index + 1],
						  "check"))
		return do_l1_command(opts, "power check", false);

	if (command_index + 2 > argc)
		goto unknown;

	action = argv[command_index + 1];
	if (streq_ci(action, "up") || streq_ci(action, "down") ||
	    streq_ci(action, "reset") || streq_ci(action, "softreset") ||
	    streq_ci(action, "softrst")) {
		struct options follow_opts;
		const struct options *action_opts = opts;

		for (i = command_index + 2; i < argc; i++) {
			if (is_force_option(argv[i])) {
				force = true;
			} else if (!strcmp(argv[i], "-w") ||
				   !strcmp(argv[i], "--follow")) {
				follow = true;
			} else {
				fprintf(stderr, "unknown argument for power %s: %s\n",
					action, argv[i]);
				return 2;
			}
		}

		if (follow && (streq_ci(action, "up") ||
			       streq_ci(action, "down"))) {
			if (prepare_command_options(opts, &follow_opts))
				return 1;
			action_opts = &follow_opts;
		}

		if (streq_ci(action, "up")) {
			(void)force;
			ret = do_power_up_confirmed(action_opts, !follow);
			if (ret || !follow)
				return ret;
			return do_leds_follow(action_opts,
					      SGIL1_LEDS_FOLLOW_POLL_MS,
					      LEDS_FOLLOW_CONFIRM_POWER_ON);
		}
		if (streq_ci(action, "down")) {
			ret = do_power_down_confirmed(action_opts, force,
						      !follow);
			if (ret || !follow)
				return ret;
			return do_leds_follow(action_opts,
					      SGIL1_LEDS_FOLLOW_POLL_MS,
					      LEDS_FOLLOW_CONFIRM_POWER_OFF);
		}

		return do_host_softreset_confirmed(opts,
						   streq_ci(action, "softrst") ?
						   "softrst" : "softreset",
						   force, follow);
	}

unknown:
	fprintf(stderr,
		"unknown power subcommand; use 'power check', 'power up', 'power down', 'power reset', or 'l1cmd power ...'\n");
	return 2;
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
		} else if (!strcmp(argv[i], "-w") ||
			   !strcmp(argv[i], "--follow")) {
			wait->follow = true;
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
	if (wait->follow &&
	    (!wait->power_up && !wait->power_down && !wait->reset)) {
		fprintf(stderr,
			"wait --follow requires --power-up, --power-down, or --reset\n");
		return -1;
	}

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

#define SGIL1_DEBUG_TEST_MASK 0x0003U
#define SGIL1_DEBUG_VERBOSE 0x0004U
#define SGIL1_DEBUG_BOOT_STOP_MASK 0x0018U
#define SGIL1_DEBUG_KNOWN_MASK 0x7fbfU

struct debug_value_name {
	uint32_t value;
	const char *name;
	const char *description;
};

struct debug_flag_desc {
	uint32_t bit;
	const char *name;
	const char *label;
};

struct debug_name_value {
	const char *name;
	uint32_t value;
};

/* SGI L1/L2 Controller Software User's Guide, 007-3938-006, Table 3-2. */
static const struct debug_value_name debug_test_modes[] = {
	{ 0x0, "normal", "normal testing" },
	{ 0x1, "none", "no testing" },
	{ 0x2, "heavy", "heavy testing" },
	{ 0x3, "manufacturing", "manufacturing-level testing" },
};

static const struct debug_name_value debug_test_mode_aliases[] = {
	{ "normal", 0x0 },
	{ "default", 0x0 },
	{ "none", 0x1 },
	{ "no-testing", 0x1 },
	{ "heavy", 0x2 },
	{ "manufacturing", 0x3 },
	{ "manufacturing-level", 0x3 },
};

static const struct debug_value_name debug_boot_stop_points[] = {
	{ 0x00, "none", "normal setting; do not stop" },
	{ 0x08, "global-pod", "global POD" },
	{ 0x10, "local-pod", "local POD" },
	{ 0x18, "memoryless-pod", "memoryless POD" },
};

static const struct debug_name_value debug_boot_stop_aliases[] = {
	{ "none", 0x00 },
	{ "normal", 0x00 },
	{ "default", 0x00 },
	{ "global-pod", 0x08 },
	{ "global", 0x08 },
	{ "local-pod", 0x10 },
	{ "local", 0x10 },
	{ "memoryless-pod", 0x18 },
	{ "memoryless", 0x18 },
	{ "no-memory-pod", 0x18 },
};

static const struct debug_flag_desc debug_flag_descs[] = {
	{ 0x0020, "default-env", "default environment override" },
	{ 0x0080, "do-not-clear-errors", "do-not-clear-errors flag" },
	{ 0x0100, "no-disable", "disabled CPU/memory override" },
	{ 0x0200, "output-prefixes", "POD output prefixes" },
	{ 0x0400, "plain-console", "plain console mode" },
	{ 0x0800, "disable-numalink-discovery", "NUMAlink discovery disable" },
	{ 0x1000, "dump-hw-error-state", "hardware error-state dump" },
	{ 0x2000, "ignore-autoboot", "IO PROM autoboot ignore" },
	{ 0x4000, "disable-io-discovery", "I/O discovery disable" },
};

static const struct debug_name_value debug_flag_aliases[] = {
	{ "verbose", SGIL1_DEBUG_VERBOSE },
	{ "default-env", 0x0020 },
	{ "default-environment", 0x0020 },
	{ "do-not-clear-errors", 0x0080 },
	{ "no-clear-errors", 0x0080 },
	{ "no-disable", 0x0100 },
	{ "override-disabled", 0x0100 },
	{ "output-prefixes", 0x0200 },
	{ "pod-prefixes", 0x0200 },
	{ "plain-console", 0x0400 },
	{ "vanilla-console", 0x0400 },
	{ "disable-numalink-discovery", 0x0800 },
	{ "disable-numalink", 0x0800 },
	{ "dump-hw-error-state", 0x1000 },
	{ "dump-error-state", 0x1000 },
	{ "ignore-autoboot", 0x2000 },
	{ "io-prom-ignore-autoboot", 0x2000 },
	{ "disable-io-discovery", 0x4000 },
	{ "disable-io", 0x4000 },
};

static const char *debug_value_description(const struct debug_value_name *values,
					   size_t count, uint32_t value)
{
	size_t i;

	for (i = 0; i < count; i++)
		if (values[i].value == value)
			return values[i].description;
	return "unknown";
}

static bool debug_lookup_name_value(const struct debug_name_value *values,
				    size_t count, const char *name,
				    uint32_t *value)
{
	size_t i;

	for (i = 0; i < count; i++) {
		if (streq_ci(values[i].name, name)) {
			*value = values[i].value;
			return true;
		}
	}

	return false;
}

static bool parse_debug_switches_from_text(const char *text, uint32_t *switches)
{
	const char *hex;
	char *end = NULL;
	unsigned long parsed;

	if (!text)
		return false;
	hex = strstr(text, "0x");
	if (!hex)
		hex = strstr(text, "0X");
	if (!hex)
		return false;

	errno = 0;
	parsed = strtoul(hex, &end, 0);
	if (errno || end == hex || parsed > UINT32_MAX)
		return false;

	*switches = (uint32_t)parsed;
	return true;
}

static void print_debug_switch_decode(uint32_t switches)
{
	uint32_t test = switches & SGIL1_DEBUG_TEST_MASK;
	uint32_t boot_stop = switches & SGIL1_DEBUG_BOOT_STOP_MASK;
	uint32_t unknown = switches & ~SGIL1_DEBUG_KNOWN_MASK;
	size_t i;

	printf("L1 virtual debug switches are 0x%04x\n",
	       (unsigned int)(switches & 0xffffU));
	printf("L1 virtual diagnostic testing is %s\n",
	       debug_value_description(debug_test_modes,
				       ARRAY_SIZE(debug_test_modes), test));
	printf("L1 virtual diagnostic output level is %s\n",
	       switches & SGIL1_DEBUG_VERBOSE ? "verbose" : "normal");
	printf("L1 virtual boot stop point is %s\n",
	       debug_value_description(debug_boot_stop_points,
				       ARRAY_SIZE(debug_boot_stop_points),
				       boot_stop));
	for (i = 0; i < ARRAY_SIZE(debug_flag_descs); i++) {
		printf("L1 virtual %s is %s\n", debug_flag_descs[i].label,
		       switches & debug_flag_descs[i].bit ? "on" : "off");
	}
	if (unknown)
		printf("L1 virtual unknown/reserved switches are 0x%04x\n",
		       (unsigned int)(unknown & 0xffffU));
	else
		printf("L1 virtual unknown/reserved switches are off\n");
}

static void print_debug_switch_text_block(const char *text, bool include_raw)
{
	uint32_t switches;

	if (parse_debug_switches_from_text(text, &switches)) {
		if (include_raw)
			print_text_block(text);
		print_debug_switch_decode(switches);
	} else {
		print_text_block(text);
	}
}

static int print_debug_report(const struct options *opts, bool fail_on_error)
{
	char *debug_text = NULL;
	char *l1dbg_text = NULL;
	int debug_ret;
	int l1dbg_ret;
	int ret = 0;

	debug_ret = l1_text_command_status(opts, "debug", false, &debug_text);
	l1dbg_ret = l1_text_command_status(opts, "l1dbg", false, &l1dbg_text);

	if (debug_ret) {
		printf("(virtual debug switches unavailable)\n");
		ret = debug_ret;
	} else {
		print_debug_switch_text_block(debug_text, false);
	}

	printf("\n");
	if (l1dbg_ret) {
		printf("(L1 debugging settings unavailable)\n");
		if (!ret)
			ret = l1dbg_ret;
	} else {
		print_text_block(l1dbg_text);
	}

	free(debug_text);
	free(l1dbg_text);
	return fail_on_error && ret ? ret : 0;
}

static void print_debug_switch_list(void)
{
	size_t i;

	printf("Switch flags for --enable/--disable:\n");
	printf("  0x%04x  verbose\n", (unsigned int)SGIL1_DEBUG_VERBOSE);
	for (i = 0; i < ARRAY_SIZE(debug_flag_descs); i++)
		printf("  0x%04x  %s\n",
		       (unsigned int)debug_flag_descs[i].bit,
		       debug_flag_descs[i].name);

	printf("\nDiagnostic testing modes for --test:\n");
	for (i = 0; i < ARRAY_SIZE(debug_test_modes); i++)
		printf("  0x%04x  %s - %s\n",
		       (unsigned int)debug_test_modes[i].value,
		       debug_test_modes[i].name,
		       debug_test_modes[i].description);

	printf("\nBoot stop points for --boot-stop:\n");
	for (i = 0; i < ARRAY_SIZE(debug_boot_stop_points); i++)
		printf("  0x%04x  %s - %s\n",
		       (unsigned int)debug_boot_stop_points[i].value,
		       debug_boot_stop_points[i].name,
		       debug_boot_stop_points[i].description);
	printf("\nUse '--boot-stop none --force' to clear the boot stop bits.\n");
}

struct l1_led_status {
	uint8_t code;
	const char *description;
};

struct text_builder {
	char *buf;
	size_t len;
	size_t cap;
};

/* SGI Fuel Diagnostic Reference Manual, 108-0350-002, Table 3-6. */
static const struct l1_led_status fuel_manual_led_statuses[] = {
	{ 0x00, "In slave loop; 0x00/0x45=okay; solid; 0x00=possible hang (Power-on discovery; no failing component)" },
	{ 0x01, "Initialize the processor, FPRs, and COP0 registers (Power-on discovery; no failing component)" },
	{ 0x02, "Test processor COP1 registers (Power-on discovery; no failing component)" },
	{ 0x03, "Switch to mapped mode (Power-on discovery; no failing component)" },
	{ 0x04, "Test processor primary instruction cache (Power-on discovery; no failing component)" },
	{ 0x05, "Test processor primary data cache (Power-on discovery; no failing component)" },
	{ 0x06, "Test secondary cache (Power-on discovery; no failing component)" },
	{ 0x07, "Flush all caches (Power-on discovery; no failing component)" },
	{ 0x0a, "Invalidate processor primary instruction cache (Power-on discovery; no failing component)" },
	{ 0x0b, "Invalidate processor primary data cache (Power-on discovery; no failing component)" },
	{ 0x0c, "Invalidate secondary cache (Power-on discovery; no failing component)" },
	{ 0x0d, "Successfully jumped to the main() function (Power-on discovery; no failing component)" },
	{ 0x0e, "About to increase PROM access speed (Power-on discovery; no failing component)" },
	{ 0x0f, "Increase PROM access speed (Power-on discovery; no failing component)" },
	{ 0x10, "Unused (PLED_INITDCACHE)" },
	{ 0x18, "UART putc timed out (Power-on discovery; no failing component)" },
	{ 0x1d, "About to initialize selected UART (Power-on discovery; no failing component)" },
	{ 0x1e, "Done initializing selected UART (Power-on discovery; no failing component)" },
	{ 0x21, "About to enter POD mode, C portion (Power-on discovery; no failing component)" },
	{ 0x22, "Just about to enter POD prompt loop (Power-on discovery; no failing component)" },
	{ 0x23, "About to enter POD mode, assembler portion (Power-on discovery; no failing component)" },
	{ 0x24, "Performing local arbitration (CPU A/B) (Power-on discovery; no failing component)" },
	{ 0x28, "About to perform first local barrier (Power-on discovery; no failing component)" },
	{ 0x2a, "About to configure Dex mode stack and data (Power-on discovery; no failing component)" },
	{ 0x2b, "Reached main() (Power-on discovery; no failing component)" },
	{ 0x31, "In entry because of nonmaskable interrupt (NMI) (Power-on discovery; no failing component)" },
	{ 0x35, "About to initialize hub real-time counter (Power-on discovery; no failing component)" },
	{ 0x36, "Done initializing hub real-time counter (Power-on discovery; no failing component)" },
	{ 0x38, "First local barrier succeeded (Power-on discovery; no failing component)" },
	{ 0x3c, "About to jump to UALIAS space (Power-on discovery; no failing component)" },
	{ 0x3d, "Jumped to UALIAS space (Power-on discovery; no failing component)" },
	{ 0x3e, "About to jump to cached space (Power-on discovery; no failing component)" },
	{ 0x3f, "Jumped to cached space (Power-on discovery; no failing component)" },
	{ 0x40, "About to test stack area of memory (Power-on discovery; no failing component)" },
	{ 0x41, "Done testing stack area of memory (Power-on discovery; no failing component)" },
	{ 0x45, "Slave loop; 0x00/0x45=okay; solid; 0x45=possible hang (Power-on discovery; no failing component)" },
	{ 0x46, "Received launch interrupt (Power-on discovery; no failing component)" },
	{ 0x47, "Calling launched function (Power-on discovery; no failing component)" },
	{ 0x48, "Launched function returned (Power-on discovery; no failing component)" },
	{ 0x49, "Unused (PLED_UARTBASE)" },
	{ 0x4a, "About to initialize hub MD and SIMM controls (Power-on discovery; no failing component)" },
	{ 0x4b, "About to probe and configure memory size (Power-on discovery; no failing component)" },
	{ 0x4f, "About to discover hub I/O (Power-on discovery; no failing component)" },
	{ 0x51, "About to write router configuration information into KLCONFIG (Power-on discovery; no failing component)" },
	{ 0x52, "About to initialize I/O section of hub (Power-on discovery; no failing component)" },
	{ 0x53, "About to probe I/O section for console (Power-on discovery; no failing component)" },
	{ 0x54, "Console probing completed (Power-on discovery; no failing component)" },
	{ 0x55, "Global master in PROM (Power-on discovery; no failing component)" },
	{ 0x56, "Done initializing I/O section of hub (Power-on discovery; no failing component)" },
	{ 0x57, "Reset error state saved (Power-on discovery; no failing component)" },
	{ 0x58, "Hub error registers cleared (Power-on discovery; no failing component)" },
	{ 0x59, "Hub error checking enabled (Power-on discovery; no failing component)" },
	{ 0x5a, "Done discovering hub I/O (Power-on discovery; no failing component)" },
	{ 0x5b, "About to initialize NMI handler area (Power-on discovery; no failing component)" },
	{ 0x5c, "About to test hub interrupts (Power-on discovery; no failing component)" },
	{ 0x5d, "About to perform early reset of hub I/O section (Power-on discovery; no failing component)" },
	{ 0x5e, "CPU came out of reset (Power-on discovery; no failing component)" },
	{ 0x5f, "Going to reset I2C (Power-on discovery; no failing component)" },
	{ 0x60, "Finished resetting I2C (Power-on discovery; no failing component)" },
	{ 0x70, "Running BIST on bank 0 (Power-on discovery; no failing component)" },
	{ 0x71, "Running BIST on bank 1 (Power-on discovery; no failing component)" },
	{ 0x72, "Running BIST on bank 2 (Power-on discovery; no failing component)" },
	{ 0x73, "Running BIST on bank 3 (Power-on discovery; no failing component)" },
	{ 0x74, "Running BIST on bank 4 (Power-on discovery; no failing component)" },
	{ 0x75, "Running BIST on bank 5 (Power-on discovery; no failing component)" },
	{ 0x76, "Running BIST on bank 6 (Power-on discovery; no failing component)" },
	{ 0x77, "Running BIST on bank 7 (Power-on discovery; no failing component)" },
	{ 0x81, "CP1 failed (Processor; failing component: PIMM)" },
	{ 0x82, "Restart master unable to load IO7 PROM (Processor; failing component: PIMM)" },
	{ 0x83, "Primary instruction cache test failed (Primary instruction cache; failing component: PIMM)" },
	{ 0x84, "Primary data cache test failed (Primary data cache; failing component: PIMM)" },
	{ 0x85, "Secondary cache test failed (Secondary instruction cache; failing component: PIMM)" },
	{ 0x86, "CPU was disabled (failing component: PIMM)" },
	{ 0x87, "Real-time counter failure (failing component: PIMM)" },
	{ 0x8f, "OS requested LEDs (no failing component)" },
	{ 0x91, "Hub local test failed (Hub; failing component: IP34 motherboard)" },
	{ 0x93, "Some node not premium memory (Hub; failing component: IP34 motherboard)" },
	{ 0x97, "Main() returned (Hub; failing component: IP34 motherboard)" },
	{ 0x98, "No local memory / memory configuration test failed (Hub/Memory; failing component: IP34 motherboard or DIMM)" },
	{ 0x99, "I2C cannot happen error (Hub; failing component: IP34 motherboard)" },
	{ 0x9a, "CPU disabled by environment variable (Hub; failing component: IP34 motherboard)" },
	{ 0x9b, "Memory download failure (Hub; failing component: IP34 motherboard)" },
	{ 0x9c, "Cannot set core debug register (Hub; failing component: IP34 motherboard)" },
	{ 0x9e, "Hub KLCONFIG failed (Hub; failing component: IP34 motherboard)" },
	{ 0x9f, "Router KLCONFIG failed (Hub; failing component: IP34 motherboard)" },
	{ 0xa0, "Hub I/O init failed (Hub; failing component: IP34 motherboard)" },
	{ 0xa1, "Node KLCONFIG failed (Hub; failing component: IP34 motherboard)" },
	{ 0xa4, "Hub chip failed l/abist (Hub; failing component: IP34 motherboard)" },
	{ 0xa6, "Waiting for reset to go (Hub; failing component: IP34 motherboard)" },
	{ 0xa7, "LLP failed after reset (Hub; failing component: IP34 motherboard)" },
	{ 0xa8, "LLP never up after reset (Hub; failing component: IP34 motherboard)" },
	{ 0xa9, "No good local memory / memory configuration test failed (Hub/Memory; failing component: IP34 motherboard or DIMM)" },
	{ 0xab, "Network discovery failed (Hub; failing component: IP34 motherboard)" },
	{ 0xac, "NASID calculation failed (Hub; failing component: IP34 motherboard)" },
	{ 0xad, "Route calculation failed (Hub; failing component: IP34 motherboard)" },
	{ 0xae, "Route distribution failed (Hub; failing component: IP34 motherboard)" },
	{ 0xaf, "NASID distribution failed (Hub; failing component: IP34 motherboard)" },
	{ 0xb0, "Master assigned no NASID (Hub; failing component: IP34 motherboard)" },
	{ 0xb1, "NASID/module ID arbitration failure" },
	{ 0xb4, "Error copying mode bits (Hub; failing component: IP34 motherboard)" },
	{ 0xb5, "Error calculating backplane frequency (Hub; failing component: IP34 motherboard)" },
};

/* SGI L1 and L2 Controller Software User's Guide, 007-3938-001, Table 3-6. */
static const struct l1_led_status controller_led_statuses[] = {
	{ 0x08, "PLED_CHUBLOCAL" },
	{ 0x09, "PLED_CKHUBCONFIG" },
};

/* Additive mappings recovered from the IP35 PROM and L1 1.48.1 tables. */
static const struct l1_led_status l1_1_48_1_led_statuses[] = {
	{ 0x11, "PLED_INITICACHE" },
	{ 0x12, "PLED_INITCOP0" },
	{ 0x13, "PLED_FLUSHTLB" },
	{ 0x14, "PLED_CLEARTAGS" },
	{ 0x15, "PLED_CCLFAILED_INITUART" },
	{ 0x16, "PLED_HUBINIT" },
	{ 0x17, "PLED_HUBCFAILED_INITUART" },
	{ 0x19, "PLED_HUBINITDONE" },
	{ 0x1a, "PLED_ELSCPROBE" },
	{ 0x1b, "PLED_JUNKPROBE" },
	{ 0x1c, "PLED_DONEPROBE" },
	{ 0x1f, "PLED_CKHUBCHIP" },
	{ 0x20, "PLED_PODMAIN" },
	{ 0x25, "PLED_SCINIT" },
	{ 0x26, "PLED_BMARB" },
	{ 0x27, "PLED_BMASTER" },
	{ 0x29, "PLED_CKPDCACHE1" },
	{ 0x2c, "PLED_LOADPROM" },
	{ 0x2d, "PLED_CKSCACHE1" },
	{ 0x2e, "PLED_CKBT" },
	{ 0x2f, "PLED_INSLAVE" },
	{ 0x30, "PLED_PROMJUMP" },
	{ 0x32, "PLED_INV_IDCACHES" },
	{ 0x33, "PLED_INV_SCACHE" },
	{ 0x34, "PLED_WRCONFIG" },
	{ 0x37, "PLED_LOCK" },
	{ 0x39, "PLED_LOCKOK" },
	{ 0x3a, "PLED_FPROMINIT" },
	{ 0x3b, "PLED_FPROMINITDONE" },
	{ 0x42, "PLED_SLAVEINT" },
	{ 0x43, "PLED_SLAVECALL" },
	{ 0x44, "PLED_SLAVEREND" },
	{ 0x4c, "PLED_I2CINIT" },
	{ 0x4d, "PLED_I2CDONE" },
	{ 0x4e, "PLED_CONFIG_INIT" },
	{ 0x50, "PLED_HUB_CONFIG" },
	{ 0x80, "POD Mode (0x80/0xBC=okay, solid 0x80=possibly hung polling UART)" },
	{ 0x88, "FLED_ECC" },
	{ 0x89, "FLED_XTLBMISS: XTLB miss exception." },
	{ 0x8a, "FLED_UTLBMISS: UTLB miss exception." },
	{ 0x8b, "FLED_KTLBMISS: KTLB miss exception." },
	{ 0x8c, "FLED_GENERAL: General exception." },
	{ 0x8d, "FLED_NOTIMPL: Exception not implemented." },
	{ 0x8e, "FLED_CACHE: Cache error exception." },
	{ 0x90, "FLED_HUBINITS: Hub inits failed." },
	{ 0x92, "FLED_HUBCONFIG: Hub config failed." },
	{ 0x94, "FLED_UNUSED1: NMI re-POD requested." },
	{ 0x95, "FLED_HUBUART: Hub UART init failed." },
	{ 0x96, "FLED_HUBCCS: Hub cross-CPU inits failed." },
	{ 0x9d, "FLED_IODISCOVER: IO discovery failed." },
	{ 0xa2, "FLED_RTRCHIP: RTR chip failed diagnostics." },
	{ 0xa3, "FLED_LINKDEAD: LLP link failed diagnostics." },
	{ 0xa5, "FLED_RTRBIST: RTR chip failed l/abist." },
	{ 0xb2, "FLED_MIXED_SN00: SN0 mixed with SN00??" },
	{ 0xb3, "FLED_ERRPART: Error partition configuration." },
	{ 0xbc, "POD Mode (0x80/0xBC=okay, solid 0xBC=possibly hung polling UART)" },
	{ 0xfe, "raw PROM value (no L1 mapping)" },
	{ 0xff, "Console poll found data for reading" },
};

static const struct l1_led_status *l1_led_status_for_code(unsigned int code)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(fuel_manual_led_statuses); i++)
		if (fuel_manual_led_statuses[i].code == code)
			return &fuel_manual_led_statuses[i];
	for (i = 0; i < ARRAY_SIZE(controller_led_statuses); i++)
		if (controller_led_statuses[i].code == code)
			return &controller_led_statuses[i];
	for (i = 0; i < ARRAY_SIZE(l1_1_48_1_led_statuses); i++)
		if (l1_1_48_1_led_statuses[i].code == code)
			return &l1_1_48_1_led_statuses[i];
	return NULL;
}

static const char *l1_led_status_source_name(unsigned int code)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(controller_led_statuses); i++)
		if (controller_led_statuses[i].code == code)
			return "SGI L1/L2 Controller Software User's Guide";
	for (i = 0; i < ARRAY_SIZE(l1_1_48_1_led_statuses); i++) {
		if (l1_1_48_1_led_statuses[i].code != code)
			continue;
		if (code == 0xfe)
			return "IP35 PROM; L1 1.48.1 has no description";
		if (code == 0xff)
			return "IP35 PROM and L1 1.48.1 firmware";
		return "L1 1.48.1 firmware";
	}
	if (code == 0x10 || code == 0x3d || code == 0x3e || code == 0x49 ||
	    code == 0xb1)
		return "SGI Fuel manual and L1 1.48.1 firmware";
	return "SGI Fuel Diagnostic Reference Manual";
}

/* These tables describe MIPS IP35 diagnostics, not every L1 platform. */
struct led_profile {
	bool ip35;
	bool l1_1_48_1;
};

static struct led_profile led_profile_for_version(const char *text)
{
	struct led_profile profile = { 0 };
	struct l1_firmware_version version;
	const char *version_start;
	int consumed = 0;

	if (!text || !parse_l1_firmware_version(text, &version))
		return profile;
	version_start = strstr(text, "L1 ");
	if (!version_start ||
	    sscanf(version_start, "L1 %u.%u.%u%n", &version.major,
		   &version.minor, &version.patch, &consumed) != 3 ||
	    (version_start[consumed] &&
	     !isspace((unsigned char)version_start[consumed])))
		return profile;
	profile.ip35 = version.major == 1 &&
		(contains_ci(text, "[Fuel/PE 1MB image]") ||
		 contains_ci(text, "[Fuel/PE/O300 1MB image]"));
	profile.l1_1_48_1 = profile.ip35 && version.minor == 48 &&
		version.patch == 1;
	return profile;
}

static void read_led_profile(const struct options *opts,
			     struct led_profile *profile)
{
	char *text = NULL;

	memset(profile, 0, sizeof(*profile));
	if (!l1_text_command_status(opts, "version", false, &text))
		*profile = led_profile_for_version(text);
	free(text);
}

static int text_builder_append(struct text_builder *builder, const char *text,
			       size_t len)
{
	char *new_buf;
	size_t needed = builder->len + len + 1;
	size_t new_cap;

	if (needed < builder->len)
		return -1;
	if (needed > builder->cap) {
		new_cap = builder->cap ? builder->cap : 256;
		while (new_cap < needed) {
			if (new_cap > SIZE_MAX / 2)
				return -1;
			new_cap *= 2;
		}
		new_buf = realloc(builder->buf, new_cap);
		if (!new_buf) {
			perror("realloc");
			return -1;
		}
		builder->buf = new_buf;
		builder->cap = new_cap;
	}

	memcpy(builder->buf + builder->len, text, len);
	builder->len += len;
	builder->buf[builder->len] = '\0';
	return 0;
}

static int text_builder_append_text_block(struct text_builder *builder,
					  const char *text)
{
	size_t len = text ? strlen(text) : 0;

	if (text_builder_append(builder, text ? text : "", len))
		return -1;
	if (!len || text[len - 1] != '\n')
		return text_builder_append(builder, "\n", 1);
	return 0;
}

static void text_builder_flush_and_clear(struct text_builder *builder)
{
	if (!builder->len)
		return;

	printf("%s", builder->buf);
	if (builder->buf[builder->len - 1] != '\n')
		putchar('\n');
	fflush(stdout);
	builder->len = 0;
	if (builder->buf)
		builder->buf[0] = '\0';
}

static bool leds_code_is_console_activity(unsigned int code)
{
	return code == 0x7f || code == 0xff;
}

static bool leds_code_is_filtered(unsigned int code)
{
	return leds_code_is_console_activity(code) || code == 0xfe;
}

static int hex_digit_value(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Recognise CPU labels without imposing a processor count or rewriting them. */
static const char *leds_cpu_value(const char *line, const char *end)
{
	const char *p = line;
	const char *identifier;

	while (p < end && isspace((unsigned char)*p))
		p++;
	if (end - p < 4 || strncasecmp(p, "CPU", 3) ||
	    !isspace((unsigned char)p[3]))
		return NULL;
	p += 4;
	while (p < end && isspace((unsigned char)*p))
		p++;
	identifier = p;
	while (p < end && isalnum((unsigned char)*p))
		p++;
	if (p == identifier || p == end || *p++ != ':')
		return NULL;
	while (p < end && isspace((unsigned char)*p))
		p++;
	return p;
}

/* A code must occupy a complete byte token at the start of a CPU/history value. */
static bool leds_line_find_code(const char *line, size_t len, bool in_cpu,
				const char **code_start, const char **code_end,
				unsigned int *code)
{
	const char *end = line + len;
	const char *p = leds_cpu_value(line, end);
	const char *start;
	unsigned int value = 0;
	size_t digits = 0;

	if (!p) {
		if (!in_cpu || !len || !isspace((unsigned char)*line))
			return false;
		p = line;
		while (p < end && isspace((unsigned char)*p))
			p++;
	}
	start = p;
	if (end - p < 3 || p[0] != '0' || (p[1] != 'x' && p[1] != 'X'))
		return false;
	p += 2;
	while (p < end && hex_digit_value(*p) >= 0) {
		if (++digits > 2)
			return false;
		value = (value << 4) | (unsigned int)hex_digit_value(*p++);
	}
	if (!digits || (p < end && *p != ':' && !isspace((unsigned char)*p)))
		return false;
	*code_start = start;
	*code_end = p;
	*code = value;
	return true;
}

static bool leds_description_equals(const char *text, size_t len,
				     const char *expected)
{
	return len == strlen(expected) && !strncasecmp(text, expected, len);
}

static bool leds_description_missing(const char *text, size_t len)
{
	return !len || leds_description_equals(text, len, "unknown LED status") ||
		leds_description_equals(text, len, "unknown LED status.") ||
		leds_description_equals(text, len, "(no description available)") ||
		leds_description_equals(text, len, "no description available");
}

static bool leds_description_needs_correction(
	const struct led_profile *profile, unsigned int code,
	const char *text, size_t len)
{
	const char *symbol = NULL;
	size_t symbol_len;

	if (!profile->l1_1_48_1)
		return false;
	/* Exact symbols from the 1.48.1 table; 0x3e duplicates 0x3d there. */
	switch (code) {
	case 0x10:
		symbol = "PLED_INITDCACHE";
		break;
	case 0x3d:
	case 0x3e:
		symbol = "PLED_JUMPRAMUOK";
		break;
	case 0x49:
		symbol = "PLED_UARTBASE";
		break;
	case 0xb1:
		symbol = "FLED_NO_MODULEID";
		break;
	default:
		return false;
	}
	symbol_len = strlen(symbol);
	if (len >= symbol_len && !strncasecmp(text, symbol, symbol_len) &&
	    (len == symbol_len || text[symbol_len] == ':' ||
	     isspace((unsigned char)text[symbol_len]))) {
		text += symbol_len;
		len -= symbol_len;
		if (len && *text == ':') {
			text++;
			len--;
		}
		while (len && isspace((unsigned char)*text)) {
			text++;
			len--;
		}
		if (leds_description_missing(text, len))
			return true;
	}
	return code == 0xb1 &&
		leds_description_equals(text, len, "Moduleid arbitration failed.");
}

static int text_builder_append_decoded_leds_line(
	struct text_builder *builder, const char *line, size_t len,
	const char *code_start, const char *code_end, unsigned int code,
	const char *prefix, size_t prefix_len, const struct led_profile *profile,
	bool annotations)
{
	const struct l1_led_status *status = profile->ip35 ?
		l1_led_status_for_code(code) : NULL;
	const char *description = code_end;
	const char *end = line + len;
	const char *source = "controller";
	bool replace;
	char decoded[512];
	int decoded_len;

	if (description < end && *description == ':')
		description++;
	while (description < end && isspace((unsigned char)*description))
		description++;
	while (end > description && isspace((unsigned char)end[-1]))
		end--;
	replace = status &&
		(leds_description_missing(description, (size_t)(end - description)) ||
		 leds_description_needs_correction(profile, code, description,
						    (size_t)(end - description)));
	if (text_builder_append(builder, prefix ? prefix : line,
				 prefix ? prefix_len : (size_t)(code_start - line)))
		return -1;
	if (replace) {
		decoded_len = snprintf(decoded, sizeof(decoded), "0x%02X: %s", code,
				       status->description);
		if (decoded_len < 0 || decoded_len >= (int)sizeof(decoded) ||
		    text_builder_append(builder, decoded, (size_t)decoded_len))
			return -1;
		source = l1_led_status_source_name(code);
	} else if (text_builder_append(builder, code_start,
					len - (size_t)(code_start - line))) {
		return -1;
	}
	if (annotations) {
		if (text_builder_append(builder, " [source: ", 10) ||
		    text_builder_append(builder, source, strlen(source)) ||
		    text_builder_append(builder, "]", 1))
			return -1;
	}
	return 0;
}

static char *decode_leds_text(const char *text, const struct led_profile *profile,
			     bool include_filtered_codes, bool annotations)
{
	struct text_builder builder = { 0 };
	const char *line = text ? text : "";
	char *pending_cpu_prefix = NULL;
	size_t pending_cpu_prefix_len = 0;
	bool have_output = false;
	bool in_cpu = false;

	while (*line) {
		const char *next = strchr(line, '\n');
		size_t len = next ? (size_t)(next - line) : strlen(line);
		const char *code_start = NULL;
		const char *code_end = NULL;
		unsigned int code = 0;
		bool cpu_prefix = leds_cpu_value(line, line + len) != NULL;
		bool have_code = leds_line_find_code(line, len, in_cpu, &code_start,
							&code_end, &code);

		/* A heading, absent CPU or unrecognised row ends a history group. */
		in_cpu = have_code;
		if (cpu_prefix || !have_code) {
			free(pending_cpu_prefix);
			pending_cpu_prefix = NULL;
		}
		if (have_code && profile->ip35 && !include_filtered_codes &&
		    leds_code_is_filtered(code)) {
			if (cpu_prefix) {
				pending_cpu_prefix_len = (size_t)(code_start - line);
				pending_cpu_prefix = strndup(line, pending_cpu_prefix_len);
				if (!pending_cpu_prefix)
					goto fail;
			}
			goto next_line;
		}
		if (have_output && text_builder_append(&builder, "\n", 1))
			goto fail;
		if (have_code) {
			if (text_builder_append_decoded_leds_line(
				&builder, line, len, code_start, code_end, code,
				pending_cpu_prefix, pending_cpu_prefix_len,
				profile, annotations))
				goto fail;
		} else if (text_builder_append(&builder, line, len)) {
			goto fail;
		}
		have_output = true;
		free(pending_cpu_prefix);
		pending_cpu_prefix = NULL;
next_line:
		if (!next)
			break;
		line = next + 1;
	}
	if (!builder.buf && text_builder_append(&builder, "", 0))
		goto fail;
	free(pending_cpu_prefix);
	return builder.buf;
fail:
	free(pending_cpu_prefix);
	free(builder.buf);
	return NULL;
}

static bool leds_text_has_current_console_activity(
	const char *text, const struct led_profile *profile)
{
	const char *line = text ? text : "";

	if (!profile->ip35)
		return false;
	while (*line) {
		const char *next = strchr(line, '\n');
		size_t len = next ? (size_t)(next - line) : strlen(line);
		const char *code_start;
		const char *code_end;
		unsigned int code;

		if (leds_line_find_code(line, len, false, &code_start, &code_end, &code) &&
		    leds_code_is_console_activity(code))
			return true;
		if (!next)
			break;
		line = next + 1;
	}
	return false;
}

static void print_leds_text_block(const char *text, const struct led_profile *profile,
				 bool all, bool annotations)
{
	char *decoded = decode_leds_text(text, profile, all, annotations);

	if (decoded && !*decoded)
		printf("No diagnostic LED status available.\n");
	else
		print_text_block(decoded ? decoded : text);
	free(decoded);
}

static void print_l1_command_text_block(const char *l1cmd, const char *text)
{
	if (streq_ci(l1cmd, "debug"))
		print_debug_switch_text_block(text, true);
	else
		print_text_block(text);
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
	cmd_opts->dest_auto_discovered = true;
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

	printf("\nDebug\n");
	(void)print_debug_report(&cmd_opts, false);

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

struct log_line_list {
	char **lines;
	size_t count;
};

struct log_repeat_state {
	char *previous_key;
	unsigned int pending_count;
};

struct log_line_sink {
	int (*emit)(void *context, const char *line);
	void *context;
};

static void free_log_line_list(struct log_line_list *list)
{
	size_t i;

	for (i = 0; i < list->count; i++)
		free(list->lines[i]);
	free(list->lines);
	memset(list, 0, sizeof(*list));
}

static int append_log_line(struct log_line_list *list, const char *start,
			   size_t len)
{
	char **next;
	char *line;

	while (len && start[len - 1] == '\r')
		len--;
	if (!len)
		return 0;
	if (list->count == SIZE_MAX / sizeof(*list->lines)) {
		fprintf(stderr, "too many log lines\n");
		return -1;
	}

	line = malloc(len + 1);
	if (!line) {
		perror("malloc");
		return -1;
	}
	memcpy(line, start, len);
	line[len] = '\0';

	next = realloc(list->lines, (list->count + 1) * sizeof(*list->lines));
	if (!next) {
		perror("realloc");
		free(line);
		return -1;
	}

	list->lines = next;
	list->lines[list->count++] = line;
	return 0;
}

static int split_log_lines(const char *text, struct log_line_list *list)
{
	const char *start = text;
	const char *p = text;

	memset(list, 0, sizeof(*list));
	while (*p) {
		if (*p == '\n') {
			if (append_log_line(list, start, (size_t)(p - start))) {
				free_log_line_list(list);
				return -1;
			}
			start = p + 1;
		}
		p++;
	}

	if (p != start && append_log_line(list, start, (size_t)(p - start))) {
		free_log_line_list(list);
		return -1;
	}

	return 0;
}

static bool log_lines_match_suffix_prefix(const struct log_line_list *previous,
					  const struct log_line_list *current,
					  size_t overlap)
{
	size_t previous_start = previous->count - overlap;
	size_t i;

	for (i = 0; i < overlap; i++) {
		if (strcmp(previous->lines[previous_start + i],
			   current->lines[i]))
			return false;
	}

	return true;
}

static size_t log_line_overlap(const struct log_line_list *previous,
			       const struct log_line_list *current)
{
	size_t max = previous->count < current->count ?
		     previous->count : current->count;
	size_t overlap;

	for (overlap = max; overlap > 0; overlap--) {
		if (log_lines_match_suffix_prefix(previous, current, overlap))
			return overlap;
	}

	return 0;
}

static bool l1_log_timestamp_prefix(const char *line, const char **message)
{
	unsigned int month;
	unsigned int day;
	unsigned int year;
	unsigned int hour;
	unsigned int minute;
	unsigned int second;
	int used = 0;
	const char *p;
	const char *q;
	size_t token_len;

	if (sscanf(line, "%u/%u/%u %u:%u:%u%n", &month, &day, &year,
		   &hour, &minute, &second, &used) != 6)
		return false;
	if (!used || month < 1 || month > 12 || day < 1 || day > 31 ||
	    hour > 23 || minute > 59 || second > 60)
		return false;

	p = line + used;
	while (isspace((unsigned char)*p))
		p++;

	q = p;
	while (*q && isupper((unsigned char)*q))
		q++;
	token_len = (size_t)(q - p);
	if (token_len >= 2 && token_len <= 8 &&
	    isspace((unsigned char)*q)) {
		p = q;
		while (isspace((unsigned char)*p))
			p++;
	}

	*message = *p ? p : line;
	return true;
}

static char *log_repeat_key_for_line(const char *line)
{
	const char *message = line;

	l1_log_timestamp_prefix(line, &message);
	return strdup(message);
}

static int stdout_log_line_emit(void *context, const char *line)
{
	(void)context;
	return puts(line) == EOF ? -1 : 0;
}

static int flush_log_repeat_summary(struct log_repeat_state *state,
				    const struct log_line_sink *sink)
{
	char *summary;
	size_t len;
	int ret;

	if (!state->pending_count)
		return 0;

	len = strlen(state->previous_key) + 80;
	summary = malloc(len);
	if (!summary) {
		perror("malloc");
		return -1;
	}
	snprintf(summary, len, "previous message repeated %u additional time%s: %s",
		 state->pending_count, state->pending_count == 1 ? "" : "s",
		 state->previous_key);
	ret = sink->emit(sink->context, summary);
	free(summary);
	state->pending_count = 0;
	return ret;
}

static void free_log_repeat_state(struct log_repeat_state *state)
{
	free(state->previous_key);
	memset(state, 0, sizeof(*state));
}

static int print_follow_log_line(struct log_repeat_state *repeat,
				 const char *line, bool repeat_summary,
				 const struct log_line_sink *sink)
{
	char *key;

	if (!repeat_summary)
		return sink->emit(sink->context, line);

	key = log_repeat_key_for_line(line);
	if (!key) {
		perror("strdup");
		return -1;
	}

	if (repeat->previous_key && !strcmp(repeat->previous_key, key)) {
		repeat->pending_count++;
		free(key);
		return 0;
	}

	if (flush_log_repeat_summary(repeat, sink)) {
		free(key);
		return -1;
	}
	if (sink->emit(sink->context, line)) {
		free(key);
		return -1;
	}
	free(repeat->previous_key);
	repeat->previous_key = key;
	return 0;
}

static int print_follow_log_lines(struct log_repeat_state *repeat,
				  const struct log_line_list *lines,
				  size_t start, bool repeat_summary,
				  const struct log_line_sink *sink)
{
	size_t i;

	for (i = start; i < lines->count; i++) {
		if (print_follow_log_line(repeat, lines->lines[i],
					  repeat_summary, sink))
			return -1;
	}
	if (flush_log_repeat_summary(repeat, sink))
		return -1;
	fflush(stdout);
	return 0;
}

static int parse_log_args(int argc, char **argv, int start,
			  struct log_options *log)
{
	int i;

	memset(log, 0, sizeof(*log));
	log->repeat_summary = true;
	log->poll_interval_ms = SGIL1_LOG_DEFAULT_POLL_MS;

	for (i = start; i < argc; i++) {
		if (!strcmp(argv[i], "-w") || !strcmp(argv[i], "--follow")) {
			log->follow = true;
		} else if (!strcmp(argv[i], "--poll-interval")) {
			if (++i >= argc) {
				fprintf(stderr, "--poll-interval needs milliseconds\n");
				return -1;
			}
			if (parse_int_arg(argv[i], 100, INT_MAX,
					  &log->poll_interval_ms)) {
				fprintf(stderr,
					"invalid --poll-interval value; minimum is 100 ms\n");
				return -1;
			}
		} else if (!strcmp(argv[i], "--no-repeat-summary")) {
			log->repeat_summary = false;
		} else {
			fprintf(stderr, "unknown argument for log: %s\n", argv[i]);
			return -1;
		}
	}

	return 0;
}

static int do_log_follow(const struct options *opts,
			 const struct log_options *log)
{
	const struct log_line_sink sink = {
		.emit = stdout_log_line_emit,
	};
	struct options cmd_opts;
	struct log_line_list previous = { 0 };
	struct log_repeat_state repeat = { 0 };
	bool have_previous = false;
	uint64_t next_pressure_decay = UINT64_MAX;
	unsigned int queue_pressure = 0;
	int ret = 1;

	if (prepare_command_options(opts, &cmd_opts))
		return 1;

	for (;;) {
		struct log_line_list current;
		char *text = NULL;
		size_t start = 0;
		size_t new_lines;
		size_t i;
		bool substantive_new = false;
		uint64_t now;
		int delay;
		int command_ret;

		command_ret = l1_text_command_status(&cmd_opts, "log", false,
						     &text);
		if (command_ret) {
			ret = command_ret;
			free(text);
			goto out;
		}
		if (split_log_lines(text, &current)) {
			free(text);
			goto out;
		}
		free(text);
		now = monotonic_milliseconds();
		if (queue_pressure && now >= next_pressure_decay) {
			queue_pressure--;
			next_pressure_decay = queue_pressure ?
				milliseconds_after(
					now, SGIL1_QUEUE_PRESSURE_DECAY_MS) :
				UINT64_MAX;
			fprintf(stderr,
				"Log follow: L1 USB queue pressure level %u/%u; recovering\n",
				queue_pressure, SGIL1_QUEUE_PRESSURE_MAX);
		}

		if (have_previous) {
			start = log_line_overlap(&previous, &current);
			if (!start && previous.count && current.count)
				fprintf(stderr,
					"warning: L1 log buffer advanced without overlap; entries may have been missed\n");
		}

		new_lines = current.count - start;
		for (i = start; i < current.count; i++) {
			if (have_previous &&
			    log_line_is_queue_full(current.lines[i])) {
				if (queue_pressure < SGIL1_QUEUE_PRESSURE_MAX) {
					queue_pressure++;
					fprintf(stderr,
						"Log follow: L1 USB queue pressure level %u/%u; polling slowed\n",
						queue_pressure,
						SGIL1_QUEUE_PRESSURE_MAX);
				}
				next_pressure_decay = milliseconds_after(
					now, SGIL1_QUEUE_PRESSURE_DECAY_MS);
			} else if (!log_line_is_queue_feedback(
					   current.lines[i])) {
				substantive_new = true;
			}
		}
		if (new_lines &&
		    print_follow_log_lines(&repeat, &current, start,
					   log->repeat_summary, &sink)) {
			free_log_line_list(&current);
			goto out;
		}

		free_log_line_list(&previous);
		previous = current;
		have_previous = true;
		delay = substantive_new ? SGIL1_LOG_BURST_POLL_MS :
					  log->poll_interval_ms;
		sleep_milliseconds(
			queue_pressure_scaled_delay(delay, queue_pressure));
	}

out:
	(void)flush_log_repeat_summary(&repeat, &sink);
	free_log_line_list(&previous);
	free_log_repeat_state(&repeat);
	return ret;
}

static int do_log_command(const struct options *opts, int argc, char **argv,
			  int command_index)
{
	struct log_options log;

	if (parse_log_args(argc, argv, command_index + 1, &log))
		return 2;
	if (log.follow)
		return do_log_follow(opts, &log);

	return do_l1_command(opts, "log", false);
}

struct watch_palette_info {
	enum watch_palette id;
	const char *name;
	const char *label;
	const char *inspiration;
};

static const struct watch_palette_info watch_palettes[] = {
	{ WATCH_PALETTE_INDIGO, "indigo", "Indigo", "SGI Indigo" },
	{ WATCH_PALETTE_CRIMSON, "crimson", "Crimson", "SGI Crimson" },
	{ WATCH_PALETTE_INDY, "indy", "Indy", "SGI Indy" },
	{ WATCH_PALETTE_INDIGO2, "indigo2", "Indigo2",
	  "SGI Indigo2" },
	{ WATCH_PALETTE_ONYX, "onyx", "Onyx", "SGI Onyx" },
	{ WATCH_PALETTE_CHALLENGE, "challenge", "Challenge",
	  "SGI Challenge" },
	{ WATCH_PALETTE_IMPACT, "impact", "IMPACT",
	  "SGI Indigo2 IMPACT" },
	{ WATCH_PALETTE_O2, "o2", "O2", "SGI O2 / Origin" },
	{ WATCH_PALETTE_OCTANE, "octane", "Octane", "SGI Octane" },
	{ WATCH_PALETTE_ONYX2, "onyx2", "Onyx2", "SGI Onyx2" },
	{ WATCH_PALETTE_OCTANE2, "octane2", "Octane2", "SGI Octane2" },
	{ WATCH_PALETTE_O2PLUS, "o2plus", "O2+", "SGI O2+" },
	{ WATCH_PALETTE_FUEL, "fuel", "Fuel", "SGI Fuel" },
	{ WATCH_PALETTE_TEZRO, "tezro", "Tezro", "SGI Tezro" },
	{ WATCH_PALETTE_PERSONAL_IRIS, "personal-iris", "Personal IRIS",
	  "SGI Personal IRIS" },
};

static void normalize_palette_name(const char *name, char *normalized,
				   size_t normalized_size)
{
	size_t used = 0;

	while (*name && used + 1 < normalized_size) {
		if (isalnum((unsigned char)*name)) {
			normalized[used++] = (char)tolower((unsigned char)*name);
		} else if (*name == '+' && used + 4 < normalized_size) {
			memcpy(normalized + used, "plus", 4);
			used += 4;
		}
		name++;
	}
	normalized[used] = '\0';
	if (!strncmp(normalized, "sgi", 3))
		memmove(normalized, normalized + 3, strlen(normalized + 3) + 1);
}

static bool parse_watch_palette(const char *name, enum watch_palette *palette)
{
	char normalized[64];

	normalize_palette_name(name, normalized, sizeof(normalized));
	if (!strcmp(normalized, "auto"))
		*palette = WATCH_PALETTE_AUTO;
	else if (!strcmp(normalized, "indigo") ||
		 !strcmp(normalized, "purple"))
		*palette = WATCH_PALETTE_INDIGO;
	else if (!strcmp(normalized, "crimson"))
		*palette = WATCH_PALETTE_CRIMSON;
	else if (!strcmp(normalized, "indy") ||
		 !strcmp(normalized, "lightblue") ||
		 !strcmp(normalized, "skyblue"))
		*palette = WATCH_PALETTE_INDY;
	else if (!strcmp(normalized, "indigo2") ||
		 !strcmp(normalized, "teal"))
		*palette = WATCH_PALETTE_INDIGO2;
	else if (!strcmp(normalized, "onyx"))
		*palette = WATCH_PALETTE_ONYX;
	else if (!strcmp(normalized, "challenge") ||
		 !strcmp(normalized, "bluegrey") ||
		 !strcmp(normalized, "bluegray"))
		*palette = WATCH_PALETTE_CHALLENGE;
	else if (!strcmp(normalized, "impact") ||
		 !strcmp(normalized, "indigo2impact") ||
		 !strcmp(normalized, "magenta"))
		*palette = WATCH_PALETTE_IMPACT;
	else if (!strcmp(normalized, "o2") || !strcmp(normalized, "blue") ||
		 !strcmp(normalized, "midblue") ||
		 !strncmp(normalized, "origin", 6))
		*palette = WATCH_PALETTE_O2;
	else if (!strcmp(normalized, "octane") ||
		 !strcmp(normalized, "green"))
		*palette = WATCH_PALETTE_OCTANE;
	else if (!strncmp(normalized, "onyx2", 5) ||
		 !strncmp(normalized, "onyx3", 5))
		*palette = WATCH_PALETTE_ONYX2;
	else if (!strcmp(normalized, "octane2"))
		*palette = WATCH_PALETTE_OCTANE2;
	else if (!strcmp(normalized, "o2plus") ||
		 !strcmp(normalized, "violet"))
		*palette = WATCH_PALETTE_O2PLUS;
	else if (!strcmp(normalized, "fuel") || !strcmp(normalized, "red"))
		*palette = WATCH_PALETTE_FUEL;
	else if (!strcmp(normalized, "tezro") ||
		 !strcmp(normalized, "burgundy"))
		*palette = WATCH_PALETTE_TEZRO;
	else if (!strcmp(normalized, "personaliris") ||
		 !strcmp(normalized, "brown"))
		*palette = WATCH_PALETTE_PERSONAL_IRIS;
	else if (!strcmp(normalized, "monochrome") ||
		 !strcmp(normalized, "mono"))
		*palette = WATCH_PALETTE_MONOCHROME;
	else
		return false;
	return true;
}

#ifdef SGIL1_WITH_TUI
static const struct watch_palette_info *watch_palette_info_for_id(
	enum watch_palette id)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(watch_palettes); i++)
		if (watch_palettes[i].id == id)
			return &watch_palettes[i];
	return &watch_palettes[0];
}

enum watch_tui_focus {
	WATCH_TUI_FOCUS_LOG,
	WATCH_TUI_FOCUS_LEDS,
};

struct watch_tui_line_history {
	char **lines;
	size_t capacity;
	size_t start;
	size_t count;
};

struct watch_tui_led_snapshot {
	struct log_line_list all_lines;
	struct log_line_list filtered_lines;
	struct log_line_list annotated_all_lines;
	struct log_line_list annotated_filtered_lines;
	char (*timestamps)[SGIL1_TUI_TIMESTAMP_SIZE];
	size_t timestamp_capacity;
	size_t timestamp_start;
	size_t timestamp_count;
};

struct watch_tui_led_history {
	struct watch_tui_led_snapshot *snapshots;
	size_t capacity;
	size_t start;
	size_t run_count;
	size_t response_count;
};

struct watch_tui_state {
	struct watch_tui_line_history raw_log_history;
	struct watch_tui_line_history log_history;
	struct watch_tui_led_history led_history;
	size_t log_scroll;
	size_t led_scroll;
	int led_width;
	int led_height;
	enum watch_tui_focus focus;
	char status[256];
	bool color_capable;
	bool all_mode;
	bool show_led_timestamps;
	bool show_annotations;
	bool help_visible;
	bool alternate_screen;
	bool initialized;
	bool monochrome;
	bool palette_ready;
	bool palette_automatic;
	bool palette_detection_failed;
	enum watch_palette palette;
	bool automatic_palette_ready;
	enum watch_palette automatic_palette;
	int background;
};

struct watch_tui_rect {
	int y;
	int x;
	int height;
	int width;
};

enum watch_tui_color_pair {
	WATCH_TUI_COLOR_ACCENT = 1,
	WATCH_TUI_COLOR_WARNING,
};

enum watch_tui_background {
	WATCH_TUI_BACKGROUND_DARK,
	WATCH_TUI_BACKGROUND_LIGHT,
};

enum watch_tui_line_style {
	WATCH_TUI_LINE_NORMAL,
	WATCH_TUI_LINE_TIMESTAMP,
};

struct watch_tui_content {
	const void *context;
	size_t line_count;
	const char *(*line_at)(const void *context, size_t index,
			       enum watch_tui_line_style *style);
	int continuation_indent;
};

static bool watch_tui_colors_enabled(const struct watch_tui_state *state)
{
	return state->color_capable &&
	       state->palette_ready && !state->monochrome;
}
#endif

struct watch_display {
	bool tui;
	unsigned int queue_pressure;
	bool console_activity_seen;
	uint64_t console_activity_last_seen;
#ifdef SGIL1_WITH_TUI
	struct watch_tui_state tui_state;
#endif
};

struct watch_signal_state {
	struct sigaction old_int;
	struct sigaction old_term;
	struct sigaction old_hup;
	unsigned int installed;
};

static void watch_signal_handler(int signal_number)
{
	(void)signal_number;
	watch_stop_requested = 1;
}

static void restore_watch_signal_handlers(struct watch_signal_state *state);

static int install_watch_signal_handlers(struct watch_signal_state *state)
{
	struct sigaction action;

	memset(state, 0, sizeof(*state));
	memset(&action, 0, sizeof(action));
	action.sa_handler = watch_signal_handler;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGINT, &action, &state->old_int))
		goto error;
	state->installed = 1;
	if (sigaction(SIGTERM, &action, &state->old_term))
		goto error;
	state->installed = 2;
	if (sigaction(SIGHUP, &action, &state->old_hup))
		goto error;
	state->installed = 3;
	return 0;

error:
	{
		int saved_errno = errno;

		fprintf(stderr, "could not install watch signal handlers: %s\n",
			strerror(saved_errno));
		restore_watch_signal_handlers(state);
		errno = saved_errno;
		return -1;
	}
}

static void restore_watch_signal_handlers(struct watch_signal_state *state)
{
	if (state->installed >= 3)
		(void)sigaction(SIGHUP, &state->old_hup, NULL);
	if (state->installed >= 2)
		(void)sigaction(SIGTERM, &state->old_term, NULL);
	if (state->installed >= 1)
		(void)sigaction(SIGINT, &state->old_int, NULL);
	state->installed = 0;
}

static uint64_t monotonic_milliseconds(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now))
		return 0;
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

static uint64_t milliseconds_after(uint64_t now, int delay)
{
	uint64_t later = now + (uint64_t)delay;

	return later < now ? UINT64_MAX : later;
}

static int queue_pressure_scaled_delay(int delay, unsigned int pressure)
{
	int64_t scaled = (int64_t)delay * (2 + (int)pressure);

	scaled /= 2;
	return scaled > INT_MAX ? INT_MAX : (int)scaled;
}

static int watch_next_led_idle_delay(int current, int minimum)
{
	int next = current + current / 2;

	if (next <= current)
		next = current + 1;
	if (next < minimum)
		next = minimum;
	if (next > SGIL1_WATCH_LED_IDLE_MAX_MS)
		next = SGIL1_WATCH_LED_IDLE_MAX_MS;
	return next;
}

static int watch_next_failure_backoff(int current)
{
	int next = current + current / 2;

	if (next <= current)
		next = current + 1;
	if (next > SGIL1_WATCH_FAILURE_BACKOFF_MAX_MS)
		next = SGIL1_WATCH_FAILURE_BACKOFF_MAX_MS;
	return next;
}

#ifdef SGIL1_WITH_TUI
static void watch_tui_layout(const struct watch_tui_state *state,
			     struct watch_tui_rect *log_rect,
			     struct watch_tui_rect *led_rect)
{
	int content_height = LINES - 2;

	if (COLS >= 100) {
		int led_width = COLS / 3;
		int log_width;

		if (led_width > SGIL1_TUI_LED_CONTENT_MAX + 2)
			led_width = SGIL1_TUI_LED_CONTENT_MAX + 2;
		if (state->led_width)
			led_width = state->led_width;
		if (led_width < 24)
			led_width = 24;
		if (led_width > COLS - 24)
			led_width = COLS - 24;
		log_width = COLS - led_width;

		*log_rect = (struct watch_tui_rect) {
			.y = 1,
			.x = 0,
			.height = content_height,
			.width = log_width,
		};
		*led_rect = (struct watch_tui_rect) {
			.y = 1,
			.x = log_width,
			.height = content_height,
			.width = led_width,
		};
	} else {
		int led_height = content_height / 3;

		if (led_height < 4)
			led_height = 4;
		if (state->led_height)
			led_height = state->led_height;
		if (led_height < 3)
			led_height = 3;
		if (led_height > content_height - 3)
			led_height = content_height - 3;
		*led_rect = (struct watch_tui_rect) {
			.y = 1,
			.x = 0,
			.height = led_height,
			.width = COLS,
		};
		*log_rect = (struct watch_tui_rect) {
			.y = 1 + led_height,
			.x = 0,
			.height = content_height - led_height,
			.width = COLS,
		};
	}
}

static size_t watch_tui_history_index(size_t start, size_t offset,
				      size_t capacity)
{
	return (start + offset) % capacity;
}

static const char *watch_tui_log_line_at(const void *context, size_t index,
					 enum watch_tui_line_style *style)
{
	const struct watch_tui_line_history *history = context;

	*style = WATCH_TUI_LINE_NORMAL;
	if (index >= history->count)
		return "";
	return history->lines[watch_tui_history_index(
		history->start, index, history->capacity)];
}

static const struct watch_tui_led_snapshot *watch_tui_led_snapshot_at(
	const struct watch_tui_led_history *history, size_t index)
{
	if (index >= history->run_count)
		return NULL;
	return &history->snapshots[watch_tui_history_index(
		history->start, index, history->capacity)];
}

static const char *watch_tui_led_timestamp_at(
	const struct watch_tui_led_snapshot *snapshot, size_t index)
{
	if (index >= snapshot->timestamp_count)
		return "";
	return snapshot->timestamps[watch_tui_history_index(
		snapshot->timestamp_start, index, snapshot->timestamp_capacity)];
}

static bool log_line_lists_equal(const struct log_line_list *left,
				 const struct log_line_list *right)
{
	size_t i;

	if (left->count != right->count)
		return false;
	for (i = 0; i < left->count; i++)
		if (strcmp(left->lines[i], right->lines[i]))
			return false;
	return true;
}

static size_t watch_tui_led_visible_line_count(
	const struct watch_tui_state *state)
{
	const struct watch_tui_led_history *history = &state->led_history;
	const struct log_line_list *previous = NULL;
	size_t count = 0;
	size_t i;

	for (i = 0; i < history->run_count; i++) {
		const struct watch_tui_led_snapshot *snapshot =
			watch_tui_led_snapshot_at(history, i);

		if (state->all_mode) {
			size_t per_response = snapshot->all_lines.count +
				(state->show_led_timestamps ? 1U : 0U);

			if (snapshot->timestamp_count &&
			    per_response > SIZE_MAX / snapshot->timestamp_count)
				return SIZE_MAX;
			if (snapshot->timestamp_count * per_response >
			    SIZE_MAX - count)
				return SIZE_MAX;
			count += snapshot->timestamp_count * per_response;
			continue;
		}

		if (!snapshot->filtered_lines.count ||
		    (previous && log_line_lists_equal(
			previous, &snapshot->filtered_lines)))
			continue;
		previous = &snapshot->filtered_lines;
		if (snapshot->filtered_lines.count +
		    (state->show_led_timestamps ? 1U : 0U) > SIZE_MAX - count)
			return SIZE_MAX;
		count += snapshot->filtered_lines.count +
			 (state->show_led_timestamps ? 1U : 0U);
	}
	return count;
}

static size_t watch_tui_led_filtered_state_count(
	const struct watch_tui_led_history *history)
{
	const struct log_line_list *previous = NULL;
	size_t count = 0;
	size_t i;

	for (i = 0; i < history->run_count; i++) {
		const struct watch_tui_led_snapshot *snapshot =
			watch_tui_led_snapshot_at(history, i);

		if (!snapshot->filtered_lines.count ||
		    (previous && log_line_lists_equal(
			previous, &snapshot->filtered_lines)))
			continue;
		previous = &snapshot->filtered_lines;
		count++;
	}
	return count;
}

static const char *watch_tui_led_line_at(const void *context, size_t index,
					 enum watch_tui_line_style *style)
{
	const struct watch_tui_state *state = context;
	const struct watch_tui_led_history *history = &state->led_history;
	const struct log_line_list *previous = NULL;
	size_t i;

	for (i = 0; i < history->run_count; i++) {
		const struct watch_tui_led_snapshot *snapshot =
			watch_tui_led_snapshot_at(history, i);
		size_t observation;

		if (!state->all_mode) {
			if (!snapshot->filtered_lines.count ||
			    (previous && log_line_lists_equal(
				previous, &snapshot->filtered_lines)))
				continue;
			previous = &snapshot->filtered_lines;
			if (state->show_led_timestamps) {
				if (!index) {
					*style = WATCH_TUI_LINE_TIMESTAMP;
					return watch_tui_led_timestamp_at(snapshot, 0);
				}
				index--;
			}
			if (index < snapshot->filtered_lines.count) {
				*style = WATCH_TUI_LINE_NORMAL;
				return state->show_annotations ?
					snapshot->annotated_filtered_lines.lines[index] :
					snapshot->filtered_lines.lines[index];
			}
			index -= snapshot->filtered_lines.count;
			continue;
		}

		for (observation = 0;
		     observation < snapshot->timestamp_count; observation++) {
			if (state->show_led_timestamps) {
				if (!index) {
					*style = WATCH_TUI_LINE_TIMESTAMP;
					return watch_tui_led_timestamp_at(
						snapshot, observation);
				}
				index--;
			}
			if (index < snapshot->all_lines.count) {
				*style = WATCH_TUI_LINE_NORMAL;
				return state->show_annotations ?
					snapshot->annotated_all_lines.lines[index] :
					snapshot->all_lines.lines[index];
			}
			index -= snapshot->all_lines.count;
		}
	}

	*style = WATCH_TUI_LINE_NORMAL;
	return "";
}

static void watch_tui_content_for_focus(const struct watch_tui_state *state,
					enum watch_tui_focus focus,
					struct watch_tui_content *content)
{
	memset(content, 0, sizeof(*content));
	if (focus == WATCH_TUI_FOCUS_LOG) {
		const struct watch_tui_line_history *history = state->all_mode ?
			&state->raw_log_history : &state->log_history;

		content->context = history;
		content->line_count = history->count;
		content->line_at = watch_tui_log_line_at;
		content->continuation_indent =
			SGIL1_TUI_LOG_CONTINUATION_INDENT;
	} else {
		content->context = state;
		content->line_count = watch_tui_led_visible_line_count(state);
		content->line_at = watch_tui_led_line_at;
		content->continuation_indent =
			SGIL1_TUI_LED_CONTINUATION_INDENT;
	}
}

static size_t watch_tui_wrap_segment(const char *line, size_t offset,
				     size_t width, size_t *segment_len)
{
	size_t remaining = strlen(line + offset);
	size_t len;
	size_t next;
	size_t i;

	if (!remaining) {
		*segment_len = 0;
		return offset;
	}
	if (remaining <= width) {
		*segment_len = remaining;
		return offset + remaining;
	}

	len = width;
	next = offset + width;
	for (i = width; i > 1; i--) {
		if (!isspace((unsigned char)line[offset + i - 1]))
			continue;
		len = i - 1;
		next = offset + i;
		while (line[next] && isspace((unsigned char)line[next]))
			next++;
		break;
	}
	*segment_len = len;
	return next;
}

static size_t watch_tui_line_rows(const char *line, size_t width,
				  size_t continuation_indent)
{
	size_t offset = 0;
	size_t rows = 0;
	bool first = true;

	if (!width)
		return 0;
	do {
		size_t indent = first ? 0 : continuation_indent;
		size_t available;
		size_t segment_len;

		if (indent >= width)
			indent = width > 1 ? width - 1 : 0;
		available = width - indent;
		offset = watch_tui_wrap_segment(line, offset, available,
						&segment_len);
		(void)segment_len;
		rows++;
		first = false;
	} while (line[offset]);
	return rows;
}

static size_t watch_tui_content_rows(const struct watch_tui_content *content,
				     size_t width)
{
	size_t rows = 0;
	size_t i;

	for (i = 0; i < content->line_count; i++) {
		enum watch_tui_line_style style;
		const char *line = content->line_at(content->context, i, &style);
		size_t line_rows = watch_tui_line_rows(
			line, width, (size_t)content->continuation_indent);

		(void)style;
		if (line_rows > SIZE_MAX - rows)
			return SIZE_MAX;
		rows += line_rows;
	}
	return rows;
}

static unsigned int watch_tui_position_percent(size_t end, size_t total)
{
	uint64_t numerator;
	unsigned int percent;

	if (!total || end >= total)
		return 100;
	numerator = (uint64_t)end * 100U;
	percent = (unsigned int)(numerator / (uint64_t)total);
	if (numerator % (uint64_t)total)
		percent++;
	return percent;
}

static void watch_tui_draw_pane(const struct watch_tui_state *state,
				const struct watch_tui_rect *rect,
				const char *title,
				const struct watch_tui_content *content,
				size_t scroll, bool focused)
{
	WINDOW *window;
	size_t visible;
	size_t total;
	size_t end;
	size_t start;
	size_t row = 0;
	size_t i;
	char position[8];
	int position_x;

	if (rect->height < 3 || rect->width < 4)
		return;
	window = derwin(stdscr, rect->height, rect->width, rect->y, rect->x);
	if (!window)
		return;
	werase(window);
	if (focused && watch_tui_colors_enabled(state))
		wattron(window, COLOR_PAIR(WATCH_TUI_COLOR_ACCENT));
	box(window, 0, 0);
	if (focused)
		wattron(window, A_BOLD);
	mvwaddnstr(window, 0, 2, title, rect->width - 4);
	if (focused)
		wattroff(window, A_BOLD);
	if (focused && watch_tui_colors_enabled(state))
		wattroff(window, COLOR_PAIR(WATCH_TUI_COLOR_ACCENT));

	visible = (size_t)(rect->height - 2);
	total = watch_tui_content_rows(content, (size_t)(rect->width - 2));
	if (scroll > (total > visible ? total - visible : 0))
		scroll = total > visible ? total - visible : 0;
	end = total - scroll;
	start = end > visible ? end - visible : 0;
	for (i = 0; i < content->line_count && row < end; i++) {
		enum watch_tui_line_style style;
		const char *line = content->line_at(content->context, i, &style);
		size_t offset = 0;
		bool first = true;

		do {
			size_t indent = first ? 0 :
				(size_t)content->continuation_indent;
			size_t width = (size_t)(rect->width - 2);
			size_t available;
			size_t segment_len;
			size_t next;

			if (indent >= width)
				indent = width > 1 ? width - 1 : 0;
			available = width - indent;
			next = watch_tui_wrap_segment(line, offset, available,
							 &segment_len);
			if (row >= start) {
				int y = 1 + (int)(row - start);

				if (style == WATCH_TUI_LINE_TIMESTAMP) {
					if (watch_tui_colors_enabled(state))
						wattron(window, COLOR_PAIR(
							WATCH_TUI_COLOR_ACCENT));
					wattron(window, A_DIM);
				}
				if (indent)
					mvwhline(window, y, 1, ' ', (int)indent);
				mvwaddnstr(window, y, 1 + (int)indent,
					   line + offset, (int)segment_len);
				if (style == WATCH_TUI_LINE_TIMESTAMP) {
					wattroff(window, A_DIM);
					if (watch_tui_colors_enabled(state))
						wattroff(window, COLOR_PAIR(
							WATCH_TUI_COLOR_ACCENT));
				}
			}
			row++;
			offset = next;
			first = false;
		} while (line[offset] && row < end);
	}
	snprintf(position, sizeof(position), "%u%%",
		 watch_tui_position_percent(end, total));
	position_x = rect->width - (int)strlen(position) - 1;
	if (position_x > 1) {
		if (focused && watch_tui_colors_enabled(state))
			wattron(window, COLOR_PAIR(WATCH_TUI_COLOR_ACCENT));
		if (focused)
			wattron(window, A_BOLD);
		mvwaddnstr(window, rect->height - 1, position_x, position,
			   rect->width - position_x - 1);
		if (focused)
			wattroff(window, A_BOLD);
		if (focused && watch_tui_colors_enabled(state))
			wattroff(window, COLOR_PAIR(WATCH_TUI_COLOR_ACCENT));
	}
	wnoutrefresh(window);
	delwin(window);
}

static void watch_tui_format_console_activity(
	const struct watch_display *display, char *text, size_t text_size)
{
	uint64_t now;
	uint64_t elapsed_seconds;

	if (!display->console_activity_seen) {
		snprintf(text, text_size, "Console last active: never");
		return;
	}

	now = monotonic_milliseconds();
	elapsed_seconds = now >= display->console_activity_last_seen ?
		(now - display->console_activity_last_seen) / 1000U : 0;
	if (!elapsed_seconds)
		snprintf(text, text_size, "Console last active: now");
	else if (elapsed_seconds < 60)
		snprintf(text, text_size, "Console last active: %" PRIu64 "s ago",
			 elapsed_seconds);
	else if (elapsed_seconds < 60U * 60U)
		snprintf(text, text_size, "Console last active: %" PRIu64 "m ago",
			 elapsed_seconds / 60U);
	else if (elapsed_seconds < 24U * 60U * 60U)
		snprintf(text, text_size, "Console last active: %" PRIu64 "h ago",
			 elapsed_seconds / (60U * 60U));
	else
		snprintf(text, text_size, "Console last active: %" PRIu64 "d ago",
			 elapsed_seconds / (24U * 60U * 60U));
}

static int watch_tui_background_hint(void)
{
	const char *colorfgbg = getenv("COLORFGBG");
	const char *background;
	char *end;
	long value;

	if (!colorfgbg || !*colorfgbg)
		return WATCH_TUI_BACKGROUND_DARK;
	background = strrchr(colorfgbg, ';');
	background = background ? background + 1 : colorfgbg;
	errno = 0;
	value = strtol(background, &end, 10);
	if (errno || end == background || *end)
		return WATCH_TUI_BACKGROUND_DARK;
	if (value == 7 || value == 15 || value == 231 || value >= 250)
		return WATCH_TUI_BACKGROUND_LIGHT;
	return WATCH_TUI_BACKGROUND_DARK;
}

struct watch_tui_palette_colors {
	enum watch_palette id;
	short light_256;
	short dark_256;
	short light_basic;
	short dark_basic;
};

/* Fixed xterm-256 shades with basic-colour fallbacks for older terminals. */
static const struct watch_tui_palette_colors watch_tui_palette_colors[] = {
	{ WATCH_PALETTE_INDIGO, 54, 99, COLOR_MAGENTA, COLOR_MAGENTA },
	{ WATCH_PALETTE_CRIMSON, 88, 196, COLOR_RED, COLOR_RED },
	{ WATCH_PALETTE_INDY, 25, 81, COLOR_BLUE, COLOR_CYAN },
	{ WATCH_PALETTE_INDIGO2, 23, 44, COLOR_CYAN, COLOR_CYAN },
	{ WATCH_PALETTE_ONYX, 53, 98, COLOR_MAGENTA, COLOR_MAGENTA },
	{ WATCH_PALETTE_CHALLENGE, 24, 74, COLOR_BLUE, COLOR_BLUE },
	{ WATCH_PALETTE_IMPACT, 89, 170, COLOR_MAGENTA, COLOR_MAGENTA },
	{ WATCH_PALETTE_O2, 18, 39, COLOR_BLUE, COLOR_BLUE },
	{ WATCH_PALETTE_OCTANE, 22, 70, COLOR_GREEN, COLOR_GREEN },
	{ WATCH_PALETTE_ONYX2, 55, 135, COLOR_MAGENTA, COLOR_MAGENTA },
	{ WATCH_PALETTE_OCTANE2, 19, 75, COLOR_BLUE, COLOR_BLUE },
	{ WATCH_PALETTE_O2PLUS, 56, 141, COLOR_MAGENTA, COLOR_MAGENTA },
	{ WATCH_PALETTE_FUEL, 124, 203, COLOR_RED, COLOR_RED },
	{ WATCH_PALETTE_TEZRO, 90, 168, COLOR_MAGENTA, COLOR_MAGENTA },
	{ WATCH_PALETTE_PERSONAL_IRIS, 94, 179, COLOR_RED, COLOR_YELLOW },
};

static const struct watch_palette_info watch_basic_palettes[] = {
	{ WATCH_PALETTE_INDIGO, "purple", "Purple",
	  "SGI Indigo / Onyx / IMPACT / Onyx2 / O2+ / Tezro" },
	{ WATCH_PALETTE_CRIMSON, "red", "Red", "SGI Crimson / Fuel" },
	{ WATCH_PALETTE_INDY, "blue", "Blue",
	  "SGI Indy / Challenge / O2 / Origin / Octane2" },
	{ WATCH_PALETTE_INDIGO2, "teal", "Teal", "SGI Indigo2" },
	{ WATCH_PALETTE_OCTANE, "green", "Green", "SGI Octane" },
	{ WATCH_PALETTE_PERSONAL_IRIS, "brown", "Brown",
	  "SGI Personal IRIS" },
};

static enum watch_palette watch_tui_effective_palette(
	enum watch_palette palette)
{
	if (COLORS >= 256)
		return palette;

	switch (palette) {
	case WATCH_PALETTE_ONYX:
	case WATCH_PALETTE_IMPACT:
	case WATCH_PALETTE_ONYX2:
	case WATCH_PALETTE_O2PLUS:
	case WATCH_PALETTE_TEZRO:
		return WATCH_PALETTE_INDIGO;
	case WATCH_PALETTE_FUEL:
		return WATCH_PALETTE_CRIMSON;
	case WATCH_PALETTE_CHALLENGE:
	case WATCH_PALETTE_O2:
	case WATCH_PALETTE_OCTANE2:
		return WATCH_PALETTE_INDY;
	default:
		return palette;
	}
}

static const struct watch_palette_info *watch_tui_palette_info(
	const struct watch_tui_state *state)
{
	size_t i;

	if (COLORS >= 256)
		return watch_palette_info_for_id(state->palette);
	for (i = 0; i < ARRAY_SIZE(watch_basic_palettes); i++)
		if (watch_basic_palettes[i].id == state->palette)
			return &watch_basic_palettes[i];
	return &watch_basic_palettes[0];
}

static const char *watch_tui_palette_setting(
	const struct watch_tui_state *state, char *setting, size_t setting_size,
	bool compact)
{
	const struct watch_palette_info *palette = watch_tui_palette_info(state);

	if (state->palette_automatic) {
		if (!state->automatic_palette_ready)
			snprintf(setting, setting_size, "Auto");
		else {
			palette = watch_palette_info_for_id(
				state->automatic_palette);
			snprintf(setting, setting_size,
				 compact ? "Auto(%s)" : "Auto (%s)",
				 palette->label);
		}
	} else {
		snprintf(setting, setting_size, "%s", palette->label);
	}
	return setting;
}

static const struct watch_tui_palette_colors *watch_tui_colors_for_palette(
	enum watch_palette palette)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(watch_tui_palette_colors); i++)
		if (watch_tui_palette_colors[i].id == palette)
			return &watch_tui_palette_colors[i];
	return &watch_tui_palette_colors[0];
}

static bool watch_tui_apply_palette(struct watch_tui_state *state,
				    enum watch_palette palette)
{
	const struct watch_tui_palette_colors *colors;
	short accent;
	short warning;

	if (!state->color_capable)
		return false;
	palette = watch_tui_effective_palette(palette);
	colors = watch_tui_colors_for_palette(palette);
	if (COLORS >= 256) {
		accent = state->background == WATCH_TUI_BACKGROUND_LIGHT ?
			 colors->light_256 : colors->dark_256;
	} else {
		accent = state->background == WATCH_TUI_BACKGROUND_LIGHT ?
			 colors->light_basic : colors->dark_basic;
	}
	warning = state->background == WATCH_TUI_BACKGROUND_LIGHT ?
		  COLOR_RED : COLOR_YELLOW;
	if (init_pair(WATCH_TUI_COLOR_ACCENT, accent, -1) == ERR ||
	    init_pair(WATCH_TUI_COLOR_WARNING, warning, -1) == ERR)
		return false;
	state->palette = palette;
	return true;
}

static enum watch_palette watch_tui_palette_for_platform(const char *text)
{
	if (contains_ci(text, "Tezro"))
		return WATCH_PALETTE_TEZRO;
	if (contains_ci(text, "O2+"))
		return WATCH_PALETTE_O2PLUS;
	if (contains_ci(text, "Fuel"))
		return WATCH_PALETTE_FUEL;
	if (contains_ci(text, "Crimson"))
		return WATCH_PALETTE_CRIMSON;
	if (contains_ci(text, "Indigo2 IMPACT"))
		return WATCH_PALETTE_IMPACT;
	if (contains_ci(text, "Personal IRIS"))
		return WATCH_PALETTE_PERSONAL_IRIS;
	if (contains_ci(text, "Octane2"))
		return WATCH_PALETTE_OCTANE2;
	if (contains_ci(text, "Onyx2") || contains_ci(text, "Onyx3"))
		return WATCH_PALETTE_ONYX2;
	if (contains_ci(text, "Onyx"))
		return WATCH_PALETTE_ONYX;
	if (contains_ci(text, "Origin") || contains_ci(text, "O2"))
		return WATCH_PALETTE_O2;
	if (contains_ci(text, "Octane"))
		return WATCH_PALETTE_OCTANE;
	if (contains_ci(text, "Indigo2"))
		return WATCH_PALETTE_INDIGO2;
	if (contains_ci(text, "Challenge"))
		return WATCH_PALETTE_CHALLENGE;
	if (contains_ci(text, "Indy"))
		return WATCH_PALETTE_INDY;
	return WATCH_PALETTE_INDIGO;
}

static void watch_tui_draw_help_border(WINDOW *window)
{
#if defined(NCURSES_WIDECHAR) && defined(WACS_D_VLINE) && \
	defined(WACS_D_HLINE) && defined(WACS_D_ULCORNER) && \
	defined(WACS_D_URCORNER) && defined(WACS_D_LLCORNER) && \
	defined(WACS_D_LRCORNER)
	if (wborder_set(window, WACS_D_VLINE, WACS_D_VLINE,
			WACS_D_HLINE, WACS_D_HLINE, WACS_D_ULCORNER,
			WACS_D_URCORNER, WACS_D_LLCORNER,
			WACS_D_LRCORNER) != ERR)
		return;
#endif
	wattron(window, A_BOLD);
	box(window, 0, 0);
	wattroff(window, A_BOLD);
}

static void watch_tui_help_text(WINDOW *window, int row, int column,
				const char *text, int width, bool active)
{
	if (active)
		wattron(window, A_REVERSE);
	mvwaddnstr(window, row, column, text, width);
	if (active)
		wattroff(window, A_REVERSE);
}

static void watch_tui_draw_wide_help(const struct watch_tui_state *state,
				     WINDOW *window, int width)
{
	char palette_setting[32];
	int heading_x = (width - 4) / 2;

	watch_tui_palette_setting(state, palette_setting,
				  sizeof(palette_setting), false);

	wattron(window, A_BOLD);
	mvwaddnstr(window, 2, heading_x, "Keys", 4);
	mvwaddnstr(window, 4, 4, "Key", 18);
	mvwaddnstr(window, 4, 24, "Action", 30);
	mvwaddnstr(window, 4, 56, "Setting", width - 60);
	wattroff(window, A_BOLD);

	mvwaddnstr(window, 5, 4, "Tab", 18);
	mvwaddnstr(window, 5, 24, "Select pane", 30);
	watch_tui_help_text(window, 5, 56, "Log", 3,
			    state->focus == WATCH_TUI_FOCUS_LOG);
	mvwaddnstr(window, 5, 60, "/", 1);
	watch_tui_help_text(window, 5, 62, "LEDs", 4,
			    state->focus == WATCH_TUI_FOCUS_LEDS);
	mvwaddnstr(window, 6, 4, "Left/Right, </>", 18);
	if (COLS < 100)
		mvwaddnstr(window, 6, 56, "Also ^/v when stacked", width - 60);
	mvwaddnstr(window, 6, 24, COLS >= 100 ?
		   "Move divider left/right" : "Shrink/grow selected pane", 30);
	mvwaddnstr(window, 7, 4, "Up/Down, k/j", 18);
	mvwaddnstr(window, 7, 24, "Scroll by line", 30);
	mvwaddnstr(window, 8, 4, "PgUp/PgDn, ^B/^F", 18);
	mvwaddnstr(window, 8, 24, "Scroll by page", 30);
	mvwaddnstr(window, 9, 4, "End, g", 18);
	mvwaddnstr(window, 9, 24, "Return to live output", 30);
	mvwaddnstr(window, 10, 4, "a", 18);
	mvwaddnstr(window, 10, 24, "Observation view", 30);
	watch_tui_help_text(window, 10, 56, "Filtered", 8, !state->all_mode);
	mvwaddnstr(window, 10, 65, "/", 1);
	watch_tui_help_text(window, 10, 67, "All", 3, state->all_mode);
	mvwaddnstr(window, 11, 4, "t", 18);
	mvwaddnstr(window, 11, 24, "LED timestamps", 30);
	watch_tui_help_text(window, 11, 56, "Off", 3,
			    !state->show_led_timestamps);
	mvwaddnstr(window, 11, 60, "/", 1);
	watch_tui_help_text(window, 11, 62, "On", 2,
			    state->show_led_timestamps);
	mvwaddnstr(window, 12, 4, "p", 18);
	mvwaddnstr(window, 12, 24, "LED description sources", 30);
	watch_tui_help_text(window, 12, 56, "Off", 3, !state->show_annotations);
	mvwaddnstr(window, 12, 60, "/", 1);
	watch_tui_help_text(window, 12, 62, "On", 2, state->show_annotations);
	mvwaddnstr(window, 13, 4, "c", 18);
	mvwaddnstr(window, 13, 24, "Cycle hardware palette", 30);
	mvwaddnstr(window, 13, 56, palette_setting, width - 60);
	mvwaddnstr(window, 14, 4, "m", 18);
	mvwaddnstr(window, 14, 24, "Colour output", 30);
	watch_tui_help_text(window, 14, 56, "Colour", 6,
			    !state->monochrome && state->color_capable);
	mvwaddnstr(window, 14, 63, "/", 1);
	watch_tui_help_text(window, 14, 65, "Mono", 4,
			    state->monochrome || !state->color_capable);
	mvwaddnstr(window, 15, 4, "Ctrl-L", 18);
	mvwaddnstr(window, 15, 24, "Redraw screen", 30);
	mvwaddnstr(window, 16, 4, "h, ?, Esc, Return", 18);
	mvwaddnstr(window, 16, 24, "Close Help", 30);
}

static void watch_tui_draw_compact_help(const struct watch_tui_state *state,
					WINDOW *window, int width)
{
	char view[16];
	char timestamps[8];
	char colour[16];
	char palette_setting[24];

	snprintf(view, sizeof(view), "%s", state->all_mode ? "All" : "Filtered");
	snprintf(timestamps, sizeof(timestamps), "%s",
		 state->show_led_timestamps ? "On" : "Off");
	snprintf(colour, sizeof(colour), "%s",
		 state->monochrome || !state->color_capable ? "Mono" : "Colour");
	watch_tui_palette_setting(state, palette_setting,
				  sizeof(palette_setting), true);
	mvwaddnstr(window, 1, 2, "Tab select pane", width - 4);
	mvwaddnstr(window, 2, 2, COLS >= 100 ?
		   "Left/Right </> move divider" :
		   "L/R </> ^/v shrink/grow pane", width - 4);
	mvwaddnstr(window, 3, 2, "U/D k/j line ^B/^F page g live", width - 4);
	mvwprintw(window, 4, 2, "a view:%-8s t time:%s", view, timestamps);
	mvwprintw(window, 5, 2, "p sources:%s",
		  state->show_annotations ? "On" : "Off");
	mvwprintw(window, 6, 2, "c palette:%-13.13s m:%s",
		  palette_setting, colour);
	mvwaddnstr(window, 7, 2, "^L redraw", width - 4);
	mvwaddnstr(window, 8, 2, "h/?/Esc/Ret close", width - 4);
}

static void watch_tui_draw_help(const struct watch_tui_state *state)
{
	WINDOW *window;
	bool wide = COLS >= 88 && LINES >= 21;
	int width = wide ? 86 : COLS - 2;
	int height = wide ? 19 : 10;
	int y;
	int x;

	if (!state->help_visible)
		return;
	if (height > LINES)
		height = LINES;
	if (width < 4 || height < 3)
		return;
	y = (LINES - height) / 2;
	x = (COLS - width) / 2;
	window = derwin(stdscr, height, width, y, x);
	if (!window)
		return;
	werase(window);
	if (watch_tui_colors_enabled(state))
		wattron(window, COLOR_PAIR(WATCH_TUI_COLOR_ACCENT));
	watch_tui_draw_help_border(window);
	wattron(window, A_BOLD);
	mvwaddnstr(window, 0, 3, " Help ", width - 6);
	wattroff(window, A_BOLD);
	if (watch_tui_colors_enabled(state))
		wattroff(window, COLOR_PAIR(WATCH_TUI_COLOR_ACCENT));
	if (wide)
		watch_tui_draw_wide_help(state, window, width);
	else
		watch_tui_draw_compact_help(state, window, width);
	wnoutrefresh(window);
	delwin(window);
}

static void watch_tui_render(struct watch_display *display)
{
	struct watch_tui_state *state = &display->tui_state;
	struct watch_tui_rect log_rect;
	struct watch_tui_rect led_rect;
	struct watch_tui_content log_content;
	struct watch_tui_content led_content;
	const struct watch_tui_line_history *shown_log;
	char header_left[160];
	char header_right[256];
	char header_right_short[80];
	char footer[320];
	char activity[64];
	char log_title[80];
	char led_title[80];
	size_t filtered_led_states;
	int header_left_width;
	int header_right_x;
	int activity_x;
	int footer_width;
	int header_attributes = A_REVERSE | A_BOLD;

	if (!state->initialized)
		return;
	if (LINES < 10 || COLS < 40) {
		erase();
		mvaddnstr(0, 0, "sgil1ctl watch", COLS);
		mvaddnstr(2, 0, "Terminal must be at least 40x10.", COLS);
		refresh();
		return;
	}

	watch_tui_layout(state, &log_rect, &led_rect);
	watch_tui_content_for_focus(state, WATCH_TUI_FOCUS_LOG, &log_content);
	watch_tui_content_for_focus(state, WATCH_TUI_FOCUS_LEDS, &led_content);
	shown_log = state->all_mode ? &state->raw_log_history :
				    &state->log_history;
	erase();
	snprintf(header_left, sizeof(header_left),
		 " sgil1ctl watch | %s | queue pressure %u/%u",
		 state->all_mode ? "All" : "Filtered",
		 display->queue_pressure, SGIL1_QUEUE_PRESSURE_MAX);
	if (!state->color_capable || state->monochrome) {
		snprintf(header_right, sizeof(header_right), " Monochrome ");
		snprintf(header_right_short, sizeof(header_right_short), " Mono ");
	} else if (!state->palette_ready) {
		snprintf(header_right, sizeof(header_right), " Default palette%s ",
			 state->palette_detection_failed ?
				 " (auto unavailable)" : " (detecting)");
		snprintf(header_right_short, sizeof(header_right_short),
			 " Default ");
	} else {
		const struct watch_palette_info *palette =
			watch_tui_palette_info(state);

		snprintf(header_right, sizeof(header_right), " %s palette: %s%s ",
			 palette->label, palette->inspiration,
			 state->palette_automatic ? " (auto)" : "");
		snprintf(header_right_short, sizeof(header_right_short), " %s%s ",
			 palette->label,
			 state->palette_automatic ? " (auto)" : "");
	}
	if (strlen(header_left) + strlen(header_right) + 1U > (size_t)COLS)
		snprintf(header_right, sizeof(header_right), "%s",
			 header_right_short);
	header_right_x = COLS - (int)strlen(header_right);
	if (header_right_x < 0)
		header_right_x = 0;
	header_left_width = header_right_x > 1 ? header_right_x - 1 : 0;
	if (watch_tui_colors_enabled(state))
		header_attributes |= COLOR_PAIR(display->queue_pressure ?
			WATCH_TUI_COLOR_WARNING : WATCH_TUI_COLOR_ACCENT);
	attron(header_attributes);
	mvhline(0, 0, ' ', COLS);
	if (header_left_width)
		mvaddnstr(0, 0, header_left, header_left_width);
	mvaddnstr(0, header_right_x, header_right, COLS - header_right_x);
	attroff(header_attributes);

	snprintf(log_title, sizeof(log_title), " L1 log %zu/%zu ",
		 shown_log->count, shown_log->capacity);
	filtered_led_states = watch_tui_led_filtered_state_count(
		&state->led_history);
	if (state->all_mode) {
		snprintf(led_title, sizeof(led_title), " LEDs %zu/%zu samples ",
			 state->led_history.response_count,
			 state->led_history.capacity);
	} else {
		snprintf(led_title, sizeof(led_title),
			 " LEDs %zu state%s | %zu/%zu samples ",
			 filtered_led_states, filtered_led_states == 1 ? "" : "s",
			 state->led_history.response_count,
			 state->led_history.capacity);
	}
	watch_tui_draw_pane(state, &log_rect, log_title, &log_content,
			    state->log_scroll,
			    state->focus == WATCH_TUI_FOCUS_LOG);
	watch_tui_draw_pane(state, &led_rect, led_title, &led_content,
			    state->led_scroll,
			    state->focus == WATCH_TUI_FOCUS_LEDS);

	snprintf(footer, sizeof(footer), " %s%s | h/? Help",
		 state->status[0] ? state->status : "Monitoring L1",
		 state->focus == WATCH_TUI_FOCUS_LOG ? " | log selected" :
							 " | LEDs selected");
	watch_tui_format_console_activity(display, activity, sizeof(activity));
	activity_x = COLS - (int)strlen(activity) - 1;
	if (activity_x < 0)
		activity_x = 0;
	footer_width = activity_x > 1 ? activity_x - 1 : 0;
	attron(A_REVERSE);
	mvhline(LINES - 1, 0, ' ', COLS);
	if (footer_width)
		mvaddnstr(LINES - 1, 0, footer, footer_width);
	mvaddnstr(LINES - 1, activity_x, activity, COLS - activity_x);
	attroff(A_REVERSE);
	wnoutrefresh(stdscr);
	watch_tui_draw_help(state);
	doupdate();
}

static int watch_tui_init_histories(struct watch_tui_state *state,
				    size_t log_capacity,
				    size_t led_capacity)
{
	if (log_capacity > SIZE_MAX / sizeof(*state->log_history.lines) ||
	    led_capacity > SIZE_MAX / sizeof(*state->led_history.snapshots)) {
		fprintf(stderr, "TUI history entry count is too large\n");
		return -1;
	}

	state->raw_log_history.lines = calloc(
		log_capacity, sizeof(*state->raw_log_history.lines));
	if (!state->raw_log_history.lines) {
		perror("calloc");
		return -1;
	}
	state->raw_log_history.capacity = log_capacity;
	state->log_history.lines = calloc(
		log_capacity, sizeof(*state->log_history.lines));
	if (!state->log_history.lines) {
		perror("calloc");
		free(state->raw_log_history.lines);
		memset(&state->raw_log_history, 0,
		       sizeof(state->raw_log_history));
		return -1;
	}
	state->log_history.capacity = log_capacity;
	state->led_history.snapshots = calloc(
		led_capacity, sizeof(*state->led_history.snapshots));
	if (!state->led_history.snapshots) {
		perror("calloc");
		free(state->raw_log_history.lines);
		free(state->log_history.lines);
		memset(&state->raw_log_history, 0,
		       sizeof(state->raw_log_history));
		memset(&state->log_history, 0, sizeof(state->log_history));
		return -1;
	}
	state->led_history.capacity = led_capacity;
	return 0;
}

static size_t watch_tui_rows_for_focus(const struct watch_tui_state *state,
				       enum watch_tui_focus focus)
{
	struct watch_tui_rect log_rect;
	struct watch_tui_rect led_rect;
	struct watch_tui_content content;
	const struct watch_tui_rect *rect;

	watch_tui_layout(state, &log_rect, &led_rect);
	watch_tui_content_for_focus(state, focus, &content);
	rect = focus == WATCH_TUI_FOCUS_LOG ? &log_rect : &led_rect;
	return rect->width > 2 ? watch_tui_content_rows(
		&content, (size_t)(rect->width - 2)) : 0;
}

static size_t watch_tui_visible_rows(const struct watch_tui_state *state,
				     enum watch_tui_focus focus)
{
	struct watch_tui_rect log_rect;
	struct watch_tui_rect led_rect;
	int height;

	watch_tui_layout(state, &log_rect, &led_rect);
	height = focus == WATCH_TUI_FOCUS_LOG ? log_rect.height :
						     led_rect.height;
	return height > 2 ? (size_t)(height - 2) : 1;
}

static size_t watch_tui_max_scroll(const struct watch_tui_state *state,
				   enum watch_tui_focus focus)
{
	size_t rows = watch_tui_rows_for_focus(state, focus);
	size_t visible = watch_tui_visible_rows(state, focus);

	return rows > visible ? rows - visible : 0;
}

static int watch_tui_append_log(struct watch_tui_state *state,
				const char *line, bool raw)
{
	struct watch_tui_line_history *history = raw ?
		&state->raw_log_history : &state->log_history;
	bool visible = state->all_mode == raw;
	size_t before_rows = visible && state->log_scroll ?
		watch_tui_rows_for_focus(state, WATCH_TUI_FOCUS_LOG) : 0;
	size_t index;
	char *copy = strdup(line);

	if (!copy) {
		perror("strdup");
		return -1;
	}
	if (history->count == history->capacity) {
		index = history->start;
		free(history->lines[index]);
		history->start = watch_tui_history_index(
			history->start, 1, history->capacity);
	} else {
		index = watch_tui_history_index(
			history->start, history->count, history->capacity);
		history->count++;
	}
	history->lines[index] = copy;
	if (visible && state->log_scroll) {
		size_t after_rows = watch_tui_rows_for_focus(
			state, WATCH_TUI_FOCUS_LOG);

		if (after_rows > before_rows &&
		    after_rows - before_rows <= SIZE_MAX - state->log_scroll)
			state->log_scroll += after_rows - before_rows;
		if (state->log_scroll > watch_tui_max_scroll(
				state, WATCH_TUI_FOCUS_LOG))
			state->log_scroll = watch_tui_max_scroll(
				state, WATCH_TUI_FOCUS_LOG);
	}
	return 0;
}

static void watch_tui_format_snapshot_time(char *buf, size_t size)
{
	struct timespec now;
	struct tm tm;
	unsigned int milliseconds;

	if (clock_gettime(CLOCK_REALTIME, &now) ||
	    !localtime_r(&now.tv_sec, &tm)) {
		snprintf(buf, size, "[time unavailable]");
		return;
	}
	milliseconds = (unsigned int)(now.tv_nsec / 1000000L);
	snprintf(buf, size, "%02u/%02u/%02u %02u:%02u:%02u.%03u",
		 (unsigned int)tm.tm_mon % 12U + 1U,
		 (unsigned int)tm.tm_mday % 32U,
		 (unsigned int)(tm.tm_year + 1900) % 100U,
		 (unsigned int)tm.tm_hour % 24U,
		 (unsigned int)tm.tm_min % 60U,
		 (unsigned int)tm.tm_sec % 61U, milliseconds % 1000U);
}

static void watch_tui_free_led_snapshot(
	struct watch_tui_led_snapshot *snapshot)
{
	free_log_line_list(&snapshot->all_lines);
	free_log_line_list(&snapshot->filtered_lines);
	free_log_line_list(&snapshot->annotated_all_lines);
	free_log_line_list(&snapshot->annotated_filtered_lines);
	free(snapshot->timestamps);
	memset(snapshot, 0, sizeof(*snapshot));
}

static int watch_tui_append_led_timestamp(
	struct watch_tui_led_snapshot *snapshot, const char *timestamp)
{
	if (snapshot->timestamp_count == snapshot->timestamp_capacity) {
		size_t new_capacity = snapshot->timestamp_capacity ?
			snapshot->timestamp_capacity * 2 : 4;
		char (*replacement)[SGIL1_TUI_TIMESTAMP_SIZE];
		size_t i;

		if (new_capacity < snapshot->timestamp_capacity ||
		    new_capacity > SIZE_MAX / sizeof(*replacement)) {
			fprintf(stderr, "too many retained LED timestamps\n");
			return -1;
		}
		replacement = calloc(new_capacity, sizeof(*replacement));
		if (!replacement) {
			perror("calloc");
			return -1;
		}
		for (i = 0; i < snapshot->timestamp_count; i++)
			memcpy(replacement[i], watch_tui_led_timestamp_at(
				snapshot, i), SGIL1_TUI_TIMESTAMP_SIZE);
		free(snapshot->timestamps);
		snapshot->timestamps = replacement;
		snapshot->timestamp_capacity = new_capacity;
		snapshot->timestamp_start = 0;
	}

	snprintf(snapshot->timestamps[watch_tui_history_index(
		 snapshot->timestamp_start, snapshot->timestamp_count,
		 snapshot->timestamp_capacity)], SGIL1_TUI_TIMESTAMP_SIZE,
		 "%s", timestamp);
	snapshot->timestamp_count++;
	return 0;
}

static void watch_tui_drop_oldest_led_response(
	struct watch_tui_led_history *history)
{
	struct watch_tui_led_snapshot *oldest;

	if (!history->response_count || !history->run_count)
		return;
	oldest = &history->snapshots[history->start];
	oldest->timestamp_start = watch_tui_history_index(
		oldest->timestamp_start, 1, oldest->timestamp_capacity);
	oldest->timestamp_count--;
	history->response_count--;
	if (oldest->timestamp_count)
		return;
	watch_tui_free_led_snapshot(oldest);
	history->start = watch_tui_history_index(
		history->start, 1, history->capacity);
	history->run_count--;
}

static int watch_tui_append_leds(struct watch_tui_state *state,
				 const char *all_text,
				 const char *filtered_text, const char *raw,
				 const struct led_profile *profile)
{
	struct watch_tui_led_history *history = &state->led_history;
	struct watch_tui_led_snapshot replacement = { 0 };
	char *annotated_all = decode_leds_text(raw, profile, true, true);
	char *annotated_filtered = decode_leds_text(raw, profile, false, true);
	size_t before_rows = state->led_scroll ?
		watch_tui_rows_for_focus(state, WATCH_TUI_FOCUS_LEDS) : 0;
	char timestamp[SGIL1_TUI_TIMESTAMP_SIZE];
	struct watch_tui_led_snapshot *last = NULL;
	size_t index;

	if (!annotated_all || !annotated_filtered ||
	    split_log_lines(all_text ? all_text : "", &replacement.all_lines) ||
	    split_log_lines(filtered_text ? filtered_text : "",
			    &replacement.filtered_lines) ||
	    split_log_lines(annotated_all, &replacement.annotated_all_lines) ||
	    split_log_lines(annotated_filtered, &replacement.annotated_filtered_lines) ||
	    replacement.all_lines.count != replacement.annotated_all_lines.count ||
	    replacement.filtered_lines.count != replacement.annotated_filtered_lines.count) {
		free(annotated_all);
		free(annotated_filtered);
		watch_tui_free_led_snapshot(&replacement);
		return -1;
	}
	free(annotated_all);
	free(annotated_filtered);
	watch_tui_format_snapshot_time(timestamp, sizeof(timestamp));
	if (history->response_count == history->capacity)
		watch_tui_drop_oldest_led_response(history);
	if (history->run_count)
		last = &history->snapshots[watch_tui_history_index(
			history->start, history->run_count - 1,
			history->capacity)];
	if (last && log_line_lists_equal(
			&last->all_lines, &replacement.all_lines) &&
	    log_line_lists_equal(&last->filtered_lines, &replacement.filtered_lines) &&
	    log_line_lists_equal(&last->annotated_all_lines,
				 &replacement.annotated_all_lines)) {
		if (watch_tui_append_led_timestamp(last, timestamp)) {
			watch_tui_free_led_snapshot(&replacement);
			return -1;
		}
		watch_tui_free_led_snapshot(&replacement);
	} else {
		if (watch_tui_append_led_timestamp(&replacement, timestamp)) {
			watch_tui_free_led_snapshot(&replacement);
			return -1;
		}
		index = watch_tui_history_index(
			history->start, history->run_count, history->capacity);
		history->snapshots[index] = replacement;
		history->run_count++;
	}
	history->response_count++;
	if (state->led_scroll) {
		size_t after_rows = watch_tui_rows_for_focus(
			state, WATCH_TUI_FOCUS_LEDS);

		if (after_rows > before_rows &&
		    after_rows - before_rows <= SIZE_MAX - state->led_scroll)
			state->led_scroll += after_rows - before_rows;
		if (state->led_scroll > watch_tui_max_scroll(
				state, WATCH_TUI_FOCUS_LEDS))
			state->led_scroll = watch_tui_max_scroll(
				state, WATCH_TUI_FOCUS_LEDS);
	}
	return 0;
}

static void watch_tui_adjust_scroll(struct watch_tui_state *state,
				    bool upward, size_t amount)
{
	size_t *scroll = state->focus == WATCH_TUI_FOCUS_LOG ?
			 &state->log_scroll : &state->led_scroll;
	size_t maximum = watch_tui_max_scroll(state, state->focus);

	if (upward) {
		if (*scroll > maximum)
			*scroll = maximum;
		if (amount > maximum - *scroll)
			*scroll = maximum;
		else
			*scroll += amount;
	} else if (amount >= *scroll) {
		*scroll = 0;
	} else {
		*scroll -= amount;
	}
}

static void watch_tui_clamp_scroll(struct watch_tui_state *state)
{
	size_t maximum = watch_tui_max_scroll(state, WATCH_TUI_FOCUS_LOG);

	if (state->log_scroll > maximum)
		state->log_scroll = maximum;
	maximum = watch_tui_max_scroll(state, WATCH_TUI_FOCUS_LEDS);
	if (state->led_scroll > maximum)
		state->led_scroll = maximum;
}

static void watch_tui_resize_pane(struct watch_tui_state *state, bool increase)
{
	struct watch_tui_rect log_rect;
	struct watch_tui_rect led_rect;
	bool wide = COLS >= 100;
	int change = increase ? 1 : -1;
	int size;
	int minimum = wide ? 24 : 3;
	int total = wide ? COLS : LINES - 2;

	if (COLS < 40 || LINES < 10)
		return;
	watch_tui_layout(state, &log_rect, &led_rect);
	size = wide ? led_rect.width : led_rect.height;
	/* Horizontal keys move the divider; stacked panes resize by focus. */
	if (wide || state->focus == WATCH_TUI_FOCUS_LOG)
		change = -change;
	size += change;
	if (size < minimum)
		size = minimum;
	if (size > total - minimum)
		size = total - minimum;
	if (wide)
		state->led_width = size;
	else
		state->led_height = size;
	watch_tui_clamp_scroll(state);
	snprintf(state->status, sizeof(state->status), "%s pane: %d %s",
		 state->focus == WATCH_TUI_FOCUS_LOG ? "Log" : "LED",
		 state->focus == WATCH_TUI_FOCUS_LOG ? total - size : size,
		 wide ? "columns" : "rows");
}

static void watch_tui_cycle_palette(struct watch_tui_state *state,
				    bool backward)
{
	const struct watch_palette_info *palettes;
	size_t palette_count;
	bool restore_automatic = false;
	size_t i;
	size_t next = 0;

	if (!state->color_capable) {
		snprintf(state->status, sizeof(state->status),
			 "Terminal colour is unavailable");
		return;
	}
	if (COLORS >= 256) {
		palettes = watch_palettes;
		palette_count = ARRAY_SIZE(watch_palettes);
	} else {
		palettes = watch_basic_palettes;
		palette_count = ARRAY_SIZE(watch_basic_palettes);
	}
	next = backward ? palette_count - 1 : 0;
	if (state->palette_ready && !state->palette_automatic) {
		for (i = 0; i < palette_count; i++) {
			if (palettes[i].id != state->palette)
				continue;
			if ((backward ? i == 0 : i + 1 == palette_count) &&
			    state->automatic_palette_ready)
				restore_automatic = true;
			else
				next = backward ? (i + palette_count - 1) %
						   palette_count :
						   (i + 1) % palette_count;
			break;
		}
	}
	state->monochrome = false;
	if (watch_tui_apply_palette(state, restore_automatic ?
			state->automatic_palette : palettes[next].id)) {
		state->palette_ready = true;
		state->palette_automatic = restore_automatic;
		state->palette_detection_failed = false;
		snprintf(state->status, sizeof(state->status), "Monitoring L1");
	}
}

static bool watch_tui_wait(struct watch_display *display, int milliseconds)
{
	struct watch_tui_state *state = &display->tui_state;
	int timeout = milliseconds > 100 ? 100 : milliseconds;
	int key;

	if (timeout < 0)
		timeout = 100;
	wtimeout(stdscr, timeout);
	key = wgetch(stdscr);
	if (state->help_visible &&
	    (key == 27 || key == '\n' || key == '\r' || key == KEY_ENTER ||
	     key == 'q' || key == 'Q')) {
		state->help_visible = false;
		watch_tui_render(display);
		return watch_stop_requested != 0;
	}
	switch (key) {
	case 'q':
	case 'Q':
		watch_stop_requested = 1;
		break;
	case '\t':
		state->focus = state->focus == WATCH_TUI_FOCUS_LOG ?
			       WATCH_TUI_FOCUS_LEDS : WATCH_TUI_FOCUS_LOG;
		break;
	case KEY_LEFT:
	case '<':
		watch_tui_resize_pane(state, false);
		break;
	case KEY_RIGHT:
	case '>':
		watch_tui_resize_pane(state, true);
		break;
	case 'a':
	case 'A':
		state->all_mode = !state->all_mode;
		state->log_scroll = 0;
		state->led_scroll = 0;
		snprintf(state->status, sizeof(state->status), "%s view",
			 state->all_mode ? "All observations" :
					   "Filtered");
		break;
	case 't':
	case 'T':
		state->show_led_timestamps = !state->show_led_timestamps;
		state->led_scroll = 0;
		snprintf(state->status, sizeof(state->status),
			 "LED timestamps %s",
			 state->show_led_timestamps ? "shown" : "hidden");
		break;
	case 'h':
	case 'H':
	case '?':
		state->help_visible = !state->help_visible;
		break;
	case 'p':
	case 'P':
		state->show_annotations = !state->show_annotations;
		state->led_scroll = 0;
		snprintf(state->status, sizeof(state->status), "LED sources %s",
			 state->show_annotations ? "shown" : "hidden");
		break;
	case 'c':
	case 'C':
		watch_tui_cycle_palette(state, key == 'C');
		break;
	case 'm':
	case 'M':
		if (!state->color_capable) {
			snprintf(state->status, sizeof(state->status),
				 "Terminal colour is unavailable");
			break;
		}
		state->monochrome = !state->monochrome;
		snprintf(state->status, sizeof(state->status), "Monitoring L1");
		break;
	case '\f':
		clearok(stdscr, true);
		break;
	case '^':
	case 'v':
	case 'V':
		if (COLS < 100)
			watch_tui_resize_pane(state, key != '^');
		break;
	case KEY_UP:
	case 'k':
	case 'K':
		watch_tui_adjust_scroll(state, true, 1);
		break;
	case KEY_DOWN:
	case 'j':
	case 'J':
		watch_tui_adjust_scroll(state, false, 1);
		break;
	case KEY_PPAGE:
	case 2: /* Ctrl-B */
		watch_tui_adjust_scroll(state, true,
					watch_tui_visible_rows(state, state->focus));
		break;
	case KEY_NPAGE:
	case 6: /* Ctrl-F */
		watch_tui_adjust_scroll(state, false,
					watch_tui_visible_rows(state, state->focus));
		break;
	case KEY_END:
	case 'g':
	case 'G':
		if (state->focus == WATCH_TUI_FOCUS_LOG)
			state->log_scroll = 0;
		else
			state->led_scroll = 0;
		break;
	case KEY_RESIZE:
		if (COLS >= 40 && LINES >= 10)
			watch_tui_clamp_scroll(state);
		clearok(stdscr, true);
		break;
	default:
		break;
	}
	if (key != ERR)
		watch_tui_render(display);
	return watch_stop_requested != 0;
}

static void watch_tui_free_line_history(
	struct watch_tui_line_history *history)
{
	size_t i;

	for (i = 0; i < history->count; i++)
		free(history->lines[watch_tui_history_index(
			history->start, i, history->capacity)]);
	free(history->lines);
	memset(history, 0, sizeof(*history));
}

static void watch_tui_free(struct watch_tui_state *state)
{
	struct watch_tui_led_history *led = &state->led_history;
	size_t i;

	watch_tui_free_line_history(&state->raw_log_history);
	watch_tui_free_line_history(&state->log_history);
	for (i = 0; i < led->run_count; i++) {
		struct watch_tui_led_snapshot *snapshot =
			&led->snapshots[watch_tui_history_index(
				led->start, i, led->capacity)];

		watch_tui_free_led_snapshot(snapshot);
	}
	free(led->snapshots);
	memset(state, 0, sizeof(*state));
}
#endif

static void watch_display_status(struct watch_display *display,
				 const char *message)
{
#ifdef SGIL1_WITH_TUI
	if (display->tui) {
		snprintf(display->tui_state.status,
			 sizeof(display->tui_state.status), "%s", message);
		watch_tui_render(display);
		return;
	}
#else
	(void)display;
#endif
	fprintf(stderr, "[watch] %s\n", message);
	fflush(stderr);
}

static int watch_display_log_line(void *context, const char *line)
{
	struct watch_display *display = context;

#ifdef SGIL1_WITH_TUI
	if (display->tui) {
		if (watch_tui_append_log(&display->tui_state, line, false))
			return -1;
		return 0;
	}
#else
	(void)display;
#endif
	if (printf("[log] %s\n", line) < 0)
		return -1;
	fflush(stdout);
	return 0;
}

static int watch_display_raw_log_line(struct watch_display *display,
				      const char *line)
{
#ifdef SGIL1_WITH_TUI
	if (display->tui)
		return watch_tui_append_log(&display->tui_state, line, true);
#else
	(void)display;
	(void)line;
#endif
	return 0;
}

static void watch_display_refresh(struct watch_display *display)
{
#ifdef SGIL1_WITH_TUI
	if (display->tui)
		watch_tui_render(display);
#else
	(void)display;
#endif
}

static void watch_display_leds(struct watch_display *display,
			       const char *all_text,
			       const char *filtered_text,
			       bool changed, bool console_activity,
			       const char *raw, const struct led_profile *profile,
			       bool annotations)
{
	const char *line = filtered_text;
	char *annotated = NULL;

	if (console_activity) {
		display->console_activity_seen = true;
		display->console_activity_last_seen = monotonic_milliseconds();
	}
#ifdef SGIL1_WITH_TUI
	if (display->tui) {
		if (watch_tui_append_leds(&display->tui_state, all_text,
					  filtered_text, raw, profile)) {
			snprintf(display->tui_state.status,
				 sizeof(display->tui_state.status),
				 "Could not retain LED response");
		}
		watch_tui_render(display);
		return;
	}
#else
	(void)all_text;
#endif
	if (!changed || !filtered_text || !*filtered_text)
		return;
	if (annotations) {
		annotated = decode_leds_text(raw, profile, false, true);
		if (annotated)
			line = annotated;
	}
	while (*line) {
		const char *next = strchr(line, '\n');
		size_t len = next ? (size_t)(next - line) : strlen(line);

		printf("[led] %.*s\n", (int)len, line);
		if (!next)
			break;
		line = next + 1;
	}
	free(annotated);
	fflush(stdout);
}

static bool watch_display_wait(struct watch_display *display, int milliseconds)
{
#ifdef SGIL1_WITH_TUI
	if (display->tui)
		return watch_tui_wait(display, milliseconds);
#else
	(void)display;
#endif
	sleep_milliseconds(milliseconds);
	return watch_stop_requested != 0;
}

static bool watch_display_input_hook(void *context)
{
	return watch_display_wait(context, 0);
}

static int watch_display_init(struct watch_display *display,
			      const struct watch_options *watch)
{
	memset(display, 0, sizeof(*display));
	display->tui = watch->tui;
#ifdef SGIL1_WITH_TUI
	if (watch->tui) {
		struct watch_tui_state *state = &display->tui_state;

		if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
			fprintf(stderr,
				"sgil1ctl watch --tui requires an interactive terminal\n");
			return -1;
		}
		if (watch_tui_init_histories(state, watch->log_history,
					     watch->led_history))
			return -1;
		state->alternate_screen = watch->alternate_screen;
		state->show_annotations = watch->show_annotations;
		(void)setlocale(LC_CTYPE, "");
		if (!initscr()) {
			fprintf(stderr, "could not initialize ncurses TUI\n");
			watch_tui_free(state);
			return -1;
		}
		state->initialized = true;
		if (!state->alternate_screen) {
			char *enter_ca = tigetstr("smcup");
			char *leave_ca = tigetstr("rmcup");

			/* Suppress curses' deferred alternate-screen transitions. */
			if (enter_ca && enter_ca != (char *)-1)
				enter_ca[0] = '\0';
			if (leave_ca && leave_ca != (char *)-1)
				leave_ca[0] = '\0';
			clearok(stdscr, true);
		}
		cbreak();
		noecho();
		keypad(stdscr, true);
		(void)set_escdelay(SGIL1_TUI_ESCAPE_DELAY_MS);
		(void)curs_set(0);
		state->background = watch_tui_background_hint();
		state->palette = watch->palette == WATCH_PALETTE_AUTO ||
				 watch->palette == WATCH_PALETTE_MONOCHROME ?
				 WATCH_PALETTE_INDIGO : watch->palette;
		state->monochrome = watch->palette == WATCH_PALETTE_MONOCHROME;
		state->palette_ready = watch->palette != WATCH_PALETTE_AUTO;
		state->palette_automatic = watch->palette == WATCH_PALETTE_AUTO;
		if (has_colors() && start_color() == OK &&
		    use_default_colors() == OK) {
			state->color_capable = true;
			if (watch->palette != WATCH_PALETTE_AUTO &&
			    !watch_tui_apply_palette(state, state->palette)) {
				state->color_capable = false;
				state->monochrome = true;
			}
		} else {
			state->monochrome = true;
		}
		state->focus = WATCH_TUI_FOCUS_LOG;
		snprintf(state->status, sizeof(state->status), "Monitoring L1");
		watch_tui_render(display);
	}
#else
	(void)watch;
#endif
	return 0;
}

static void watch_display_read_led_profile(
	struct watch_display *display,
	const struct options *cmd_opts, struct led_profile *profile)
{
	char *version = NULL;
	int ret = l1_text_command_status(cmd_opts, "version", false, &version);

	*profile = led_profile_for_version(ret ? NULL : version);
#ifdef SGIL1_WITH_TUI
	enum watch_palette palette;

	if (!display->tui || !display->tui_state.palette_automatic) {
		free(version);
		return;
	}
	if (ret) {
		display->tui_state.palette_detection_failed = true;
		watch_tui_render(display);
		free(version);
		return;
	}
	palette = watch_tui_palette_for_platform(version);
	free(version);
	display->tui_state.palette_detection_failed = false;
	display->tui_state.automatic_palette_ready = true;
	display->tui_state.automatic_palette = palette;
	if (!display->tui_state.color_capable) {
		display->tui_state.palette_ready = true;
	} else if (!watch_tui_apply_palette(&display->tui_state, palette)) {
		display->tui_state.color_capable = false;
		display->tui_state.monochrome = true;
	} else {
		display->tui_state.palette_ready = true;
	}
	watch_tui_render(display);
#else
	(void)display;

	free(version);
#endif
}

static void watch_display_finish(struct watch_display *display)
{
#ifdef SGIL1_WITH_TUI
	if (display->tui) {
		(void)curs_set(1);
		/* Screen can ignore alternate-screen requests, leaving the TUI visible. */
		if (LINES > 0) {
			attrset(A_NORMAL);
			move(LINES - 1, 0);
			hline(' ', COLS);
			move(LINES - 1, 0);
			refresh();
		}
		keypad(stdscr, false);
		endwin();
		watch_tui_free(&display->tui_state);
	}
#else
	(void)display;
#endif
}

static bool log_line_is_queue_full(const char *line)
{
	return contains_ci(line, "USB_WQUE") && contains_ci(line, "Q full");
}

static bool log_line_is_queue_feedback(const char *line)
{
	return contains_ci(line, "USB_WQUE") &&
	       (contains_ci(line, "Q full") || contains_ci(line, "Q avail"));
}

static void watch_report_queue_pressure(struct watch_display *display,
					unsigned int pressure,
					bool increasing)
{
	char message[160];

	display->queue_pressure = pressure;
	snprintf(message, sizeof(message),
		 "L1 USB queue pressure level %u/%u; polling %s",
		 pressure, SGIL1_QUEUE_PRESSURE_MAX,
		 increasing ? "slowed" : "recovering");
	watch_display_status(display, message);
}

static int parse_watch_args(int argc, char **argv, int start,
			    struct watch_options *watch)
{
	const char *tui_option = NULL;
	int i;

	memset(watch, 0, sizeof(*watch));
	watch->alternate_screen = true;
	watch->repeat_summary = true;
	watch->log_interval_ms = SGIL1_LOG_DEFAULT_POLL_MS;
	watch->led_interval_ms = SGIL1_WATCH_LED_MIN_MS;
	watch->log_history = SGIL1_TUI_LOG_HISTORY_DEFAULT;
	watch->led_history = SGIL1_TUI_LED_HISTORY_DEFAULT;
	watch->palette = WATCH_PALETTE_AUTO;

	for (i = start; i < argc; i++) {
		if (!strcmp(argv[i], "--tui")) {
			watch->tui = true;
		} else if (!strcmp(argv[i], "--show-annotations")) {
			watch->show_annotations = true;
		} else if (!strcmp(argv[i], "--no-alternate-screen")) {
			watch->alternate_screen = false;
		} else if (!strcmp(argv[i], "--log-interval")) {
			if (++i >= argc) {
				fprintf(stderr, "--log-interval needs milliseconds\n");
				return -1;
			}
			if (parse_int_arg(argv[i], SGIL1_WATCH_LOG_MIN_MS,
					  INT_MAX, &watch->log_interval_ms)) {
				fprintf(stderr,
					"invalid --log-interval value; minimum is %d ms\n",
					SGIL1_WATCH_LOG_MIN_MS);
				return -1;
			}
		} else if (!strcmp(argv[i], "--led-interval")) {
			if (++i >= argc) {
				fprintf(stderr, "--led-interval needs milliseconds\n");
				return -1;
			}
			if (parse_int_arg(argv[i], SGIL1_WATCH_LED_MIN_MS,
					  SGIL1_WATCH_LED_IDLE_MAX_MS,
					  &watch->led_interval_ms)) {
				fprintf(stderr,
					"invalid --led-interval value; expected %d..%d ms\n",
					SGIL1_WATCH_LED_MIN_MS,
					SGIL1_WATCH_LED_IDLE_MAX_MS);
				return -1;
			}
		} else if (!strcmp(argv[i], "--log-history") ||
			   !strcmp(argv[i], "--led-history")) {
			bool log_history = !strcmp(argv[i], "--log-history");
			int entries;

			tui_option = argv[i];
			if (++i >= argc) {
				fprintf(stderr, "%s needs an entry count\n",
					log_history ? "--log-history" :
						      "--led-history");
				return -1;
			}
			if (parse_int_arg(argv[i], 1, SGIL1_TUI_HISTORY_MAX,
					  &entries)) {
				fprintf(stderr,
					"invalid %s value; expected 1..%d entries\n",
					log_history ? "--log-history" :
							      "--led-history",
					SGIL1_TUI_HISTORY_MAX);
				return -1;
			}
			if (log_history)
				watch->log_history = (size_t)entries;
			else
				watch->led_history = (size_t)entries;
		} else if (!strcmp(argv[i], "--palette")) {
			size_t j;

			tui_option = argv[i];
			if (++i >= argc) {
				fprintf(stderr, "--palette needs a name\n");
				return -1;
			}
			if (parse_watch_palette(argv[i], &watch->palette))
				continue;
			fprintf(stderr, "unknown TUI palette: %s\n",
				argv[i]);
			fprintf(stderr, "available palettes: auto");
			for (j = 0; j < ARRAY_SIZE(watch_palettes); j++)
				fprintf(stderr, ", %s", watch_palettes[j].name);
			fprintf(stderr, ", monochrome\n");
			return -1;
		} else if (!strcmp(argv[i], "--no-repeat-summary")) {
			watch->repeat_summary = false;
		} else {
			fprintf(stderr, "unknown watch option: %s\n", argv[i]);
			return -1;
		}
	}

	if (!watch->alternate_screen && !watch->tui) {
		fprintf(stderr, "--no-alternate-screen requires --tui\n");
		return -1;
	}
	if (tui_option && !watch->tui) {
		fprintf(stderr, "%s applies only with --tui\n", tui_option);
		return -1;
	}
#ifndef SGIL1_WITH_TUI
	if (watch->tui) {
		fprintf(stderr,
			"sgil1ctl watch --tui is unavailable:\n"
			"this edition was built without optional ncurses TUI support;\n"
			"rebuild with 'make -C tools WITH_TUI=1'\n");
		return -1;
	}
#endif

	return 0;
}

static int run_watch_scheduler(const struct options *opts,
			       struct options *cmd_opts,
			       const struct watch_options *watch,
			       struct watch_display *display,
			       struct led_profile *profile)
{
	const struct log_line_sink log_sink = {
		.emit = watch_display_log_line,
		.context = display,
	};
	struct log_line_list previous_log = { 0 };
	struct log_repeat_state repeat = { 0 };
	char *previous_leds = NULL;
	uint64_t now = monotonic_milliseconds();
	uint64_t next_log = now;
	uint64_t next_led = now;
	uint64_t next_pressure_decay = UINT64_MAX;
	int led_delay = watch->led_interval_ms;
	int failure_backoff = SGIL1_WATCH_FAILURE_BACKOFF_MIN_MS;
	unsigned int consecutive_failures = 0;
	unsigned int queue_pressure = 0;
	bool have_previous_log = false;
	bool route_valid = true;
	bool profile_valid = true;
	bool last_was_led = false;
	int ret = 0;

	cmd_opts->skip_command_drain = true;
	while (!watch_stop_requested) {
		bool do_led;
		bool do_log;

		now = monotonic_milliseconds();
		if (queue_pressure && now >= next_pressure_decay) {
			queue_pressure--;
			next_pressure_decay = queue_pressure ?
				milliseconds_after(now,
					SGIL1_QUEUE_PRESSURE_DECAY_MS) :
				UINT64_MAX;
			watch_report_queue_pressure(display, queue_pressure, false);
		}

		do_led = now >= next_led;
		do_log = now >= next_log;
		if (!do_led && !do_log) {
			uint64_t next = next_led < next_log ? next_led : next_log;
			uint64_t remaining = next > now ? next - now : 0;
			int wait_ms = remaining > INT_MAX ? INT_MAX : (int)remaining;

			if (watch_display_wait(display, wait_ms))
				break;
			continue;
		}

		if (!route_valid) {
			struct options replacement;

			if (prepare_command_options(opts, &replacement)) {
				char message[128];

				if (watch_stop_requested)
					break;

				now = monotonic_milliseconds();
				snprintf(message, sizeof(message),
					 "could not restore L1 route; retrying in %d ms",
					 failure_backoff);
				watch_display_status(display, message);
				next_led = next_log =
					milliseconds_after(now, failure_backoff);
				failure_backoff =
					watch_next_failure_backoff(failure_backoff);
				continue;
			}
			replacement.skip_command_drain = true;
			*cmd_opts = replacement;
			route_valid = true;
			watch_display_status(display, "restored L1 command route");
		}

		do_led = do_led && (!do_log || !last_was_led);
		if (do_led) {
			char *raw = NULL;
			char *all_decoded = NULL;
			char *filtered = NULL;
			bool changed;
			bool console_activity;
			int command_ret;

			if (!profile_valid) {
				watch_display_read_led_profile(display, cmd_opts, profile);
				profile_valid = true;
			}
			command_ret = l1_text_command_status(cmd_opts, "leds", false,
							     &raw);
			if (command_ret) {
				char message[128];

				now = monotonic_milliseconds();
				free(raw);
				if (command_ret == SGIL1_READ_CANCELLED &&
				    watch_stop_requested)
					break;
				consecutive_failures++;
				profile_valid = false;
				snprintf(message, sizeof(message),
					 "LED request failed; retrying transport in %d ms",
					 failure_backoff);
				watch_display_status(display, message);
				next_led = next_log =
					milliseconds_after(now, failure_backoff);
				failure_backoff =
					watch_next_failure_backoff(failure_backoff);
				if (consecutive_failures >= 3 &&
				    cmd_opts->dest_auto_discovered)
					route_valid = false;
				last_was_led = true;
				continue;
			}

			if (consecutive_failures)
				watch_display_status(display, "L1 transport recovered");
			now = monotonic_milliseconds();
			consecutive_failures = 0;
			failure_backoff = SGIL1_WATCH_FAILURE_BACKOFF_MIN_MS;
			console_activity =
				leds_text_has_current_console_activity(raw, profile);
			all_decoded = decode_leds_text(raw, profile, true, false);
			filtered = decode_leds_text(raw, profile, false, false);
			if (!all_decoded)
				all_decoded = strdup(raw ? raw : "");
			if (!filtered)
				filtered = strdup(raw ? raw : "");
			if (!all_decoded || !filtered) {
				free(raw);
				perror("strdup");
				free(all_decoded);
				free(filtered);
				ret = 1;
				break;
			}

			changed = !previous_leds || strcmp(previous_leds, filtered);
			watch_display_leds(display, all_decoded, filtered, changed,
					   console_activity, raw, profile,
					   watch->show_annotations);
			free(raw);
			free(all_decoded);
			if (changed) {
				free(previous_leds);
				previous_leds = filtered;
				filtered = NULL;
				led_delay = watch->led_interval_ms;
			} else {
				led_delay = watch_next_led_idle_delay(
					led_delay, watch->led_interval_ms);
			}
			free(filtered);
			next_led = milliseconds_after(
				now, queue_pressure_scaled_delay(led_delay,
								 queue_pressure));
			last_was_led = true;
			continue;
		}

		{
			struct log_line_list current;
			char *text = NULL;
			size_t start = 0;
			size_t i;
			bool substantive_new = false;
			int command_ret;

			command_ret = l1_text_command_status(cmd_opts, "log", false,
							     &text);
			if (command_ret) {
				char message[128];

				now = monotonic_milliseconds();
				free(text);
				if (command_ret == SGIL1_READ_CANCELLED &&
				    watch_stop_requested)
					break;
				consecutive_failures++;
				profile_valid = false;
				snprintf(message, sizeof(message),
					 "log request failed; retrying transport in %d ms",
					 failure_backoff);
				watch_display_status(display, message);
				next_led = next_log =
					milliseconds_after(now, failure_backoff);
				failure_backoff =
					watch_next_failure_backoff(failure_backoff);
				if (consecutive_failures >= 3 &&
				    cmd_opts->dest_auto_discovered)
					route_valid = false;
				last_was_led = false;
				continue;
			}

			if (consecutive_failures)
				watch_display_status(display, "L1 transport recovered");
			now = monotonic_milliseconds();
			consecutive_failures = 0;
			failure_backoff = SGIL1_WATCH_FAILURE_BACKOFF_MIN_MS;
			if (split_log_lines(text, &current)) {
				free(text);
				ret = 1;
				break;
			}
			free(text);

			if (have_previous_log) {
				start = log_line_overlap(&previous_log, &current);
				if (!start && previous_log.count && current.count)
					watch_display_status(display,
						"L1 log advanced without overlap; entries may have been missed");
			}

			for (i = start; i < current.count; i++) {
				if (watch_display_raw_log_line(
						display, current.lines[i])) {
					free_log_line_list(&current);
					ret = 1;
					goto cleanup;
				}
				if (!have_previous_log)
					continue;
				if (log_line_is_queue_full(current.lines[i])) {
					if (queue_pressure <
					    SGIL1_QUEUE_PRESSURE_MAX) {
						queue_pressure++;
						watch_report_queue_pressure(
							display, queue_pressure, true);
					}
					next_pressure_decay = milliseconds_after(
						now,
						SGIL1_QUEUE_PRESSURE_DECAY_MS);
				} else if (!log_line_is_queue_feedback(
						   current.lines[i])) {
					substantive_new = true;
				}
			}

			if (current.count > start &&
			    print_follow_log_lines(&repeat, &current, start,
						   watch->repeat_summary,
						   &log_sink)) {
				free_log_line_list(&current);
				ret = 1;
				break;
			}
			watch_display_refresh(display);

			free_log_line_list(&previous_log);
			previous_log = current;
			have_previous_log = true;
			next_log = milliseconds_after(
				now, queue_pressure_scaled_delay(
					substantive_new ? SGIL1_LOG_BURST_POLL_MS :
							  watch->log_interval_ms,
					queue_pressure));
			last_was_led = false;
		}
	}

cleanup:
	(void)flush_log_repeat_summary(&repeat, &log_sink);
	watch_display_refresh(display);
	free(previous_leds);
	free_log_line_list(&previous_log);
	free_log_repeat_state(&repeat);
	return ret;
}

static int do_watch_command(const struct options *opts, int argc, char **argv,
			    int command_index)
{
	struct watch_signal_state signals;
	struct watch_options watch;
	struct watch_display display;
	struct led_profile profile;
	struct options cmd_opts;
	int ret;

	if (parse_watch_args(argc, argv, command_index + 1, &watch))
		return 2;
	watch.show_annotations |= opts->show_annotations;
	watch_stop_requested = 0;
	if (watch_display_init(&display, &watch))
		return 1;
	if (install_watch_signal_handlers(&signals)) {
		watch_display_finish(&display);
		return 1;
	}

	l1_wait_input_hook = watch_display_input_hook;
	l1_wait_input_context = &display;
	l1_wait_cancel_enabled = true;
	if (prepare_command_options(opts, &cmd_opts))
		ret = watch_stop_requested ? 0 : 1;
	else {
		watch_display_read_led_profile(&display, &cmd_opts, &profile);
		ret = run_watch_scheduler(opts, &cmd_opts, &watch, &display, &profile);
	}
	l1_wait_cancel_enabled = false;
	l1_wait_input_hook = NULL;
	l1_wait_input_context = NULL;
	restore_watch_signal_handlers(&signals);
	watch_display_finish(&display);
	return ret;
}

static bool debug_arg_is_option(const char *arg)
{
	return arg[0] == '-';
}

static int parse_debug_switch_list(int argc, char **argv, int *index,
				   const char *option, uint32_t *mask)
{
	int i = *index + 1;
	uint32_t value;
	bool found = false;

	while (i < argc && !debug_arg_is_option(argv[i])) {
		if (!debug_lookup_name_value(debug_flag_aliases,
					     ARRAY_SIZE(debug_flag_aliases),
					     argv[i], &value)) {
			fprintf(stderr, "unknown debug switch for %s: %s\n",
				option, argv[i]);
			return -1;
		}
		*mask |= value;
		found = true;
		i++;
	}

	if (!found) {
		fprintf(stderr, "%s needs at least one switch name\n", option);
		return -1;
	}

	*index = i - 1;
	return 0;
}

static int parse_debug_args(int argc, char **argv, int start,
			    struct debug_options *debug)
{
	int i;

	memset(debug, 0, sizeof(*debug));

	for (i = start; i < argc; i++) {
		uint32_t value;

		if (is_force_option(argv[i])) {
			debug->force = true;
		} else if (!strcmp(argv[i], "--show")) {
			/* Plain 'sgil1ctl debug' is already the show action. */
		} else if (!strcmp(argv[i], "--list-switches") ||
			   !strcmp(argv[i], "--list")) {
			debug->list_switches = true;
		} else if (!strcmp(argv[i], "--set")) {
			if (++i >= argc) {
				fprintf(stderr, "--set needs a switch value\n");
				return -1;
			}
			if (parse_u32(argv[i], &value) || value > 0xffffU) {
				fprintf(stderr,
					"invalid --set value; expected 0..0xffff\n");
				return -1;
			}
			debug->set_value = value;
			debug->have_set = true;
			debug->update = true;
		} else if (!strcmp(argv[i], "--enable")) {
			if (parse_debug_switch_list(argc, argv, &i,
						    "--enable",
						    &debug->enable_mask))
				return -1;
			debug->update = true;
		} else if (!strcmp(argv[i], "--disable")) {
			if (parse_debug_switch_list(argc, argv, &i,
						    "--disable",
						    &debug->disable_mask))
				return -1;
			debug->update = true;
		} else if (!strcmp(argv[i], "--test")) {
			if (++i >= argc) {
				fprintf(stderr, "--test needs a mode name\n");
				return -1;
			}
			if (!debug_lookup_name_value(debug_test_mode_aliases,
						     ARRAY_SIZE(debug_test_mode_aliases),
						     argv[i], &value)) {
				fprintf(stderr,
					"unknown diagnostic testing mode: %s\n",
					argv[i]);
				return -1;
			}
			debug->test_value = value;
			debug->have_test = true;
			debug->update = true;
		} else if (!strcmp(argv[i], "--boot-stop")) {
			if (++i >= argc) {
				fprintf(stderr, "--boot-stop needs a point name\n");
				return -1;
			}
			if (!debug_lookup_name_value(debug_boot_stop_aliases,
						     ARRAY_SIZE(debug_boot_stop_aliases),
						     argv[i], &value)) {
				fprintf(stderr, "unknown boot stop point: %s\n",
					argv[i]);
				return -1;
			}
			debug->boot_stop_value = value;
			debug->have_boot_stop = true;
			debug->update = true;
		} else {
			fprintf(stderr, "unknown debug option: %s\n", argv[i]);
			return -1;
		}
	}

	if (debug->list_switches && debug->update) {
		fprintf(stderr,
			"--list-switches cannot be combined with debug update options\n");
		return -1;
	}

	return 0;
}

static int read_current_debug_switches(const struct options *opts,
				       uint32_t *switches)
{
	char *text = NULL;
	int ret;

	ret = l1_text_command_status(opts, "debug", false, &text);
	if (ret) {
		free(text);
		return ret;
	}
	if (!parse_debug_switches_from_text(text, switches)) {
		fprintf(stderr,
			"could not parse current L1 virtual debug switches\n");
		if (!opts->debug)
			print_text_block(text);
		free(text);
		return 1;
	}

	free(text);
	return 0;
}

static int write_debug_switches(const struct options *opts, uint32_t switches)
{
	char command[32];
	char *text = NULL;
	char *l1dbg_text = NULL;
	int len;
	int ret;

	len = snprintf(command, sizeof(command), "debug 0x%04x",
		       (unsigned int)(switches & 0xffffU));
	if (len < 0 || len >= (int)sizeof(command))
		return 1;

	ret = run_l1_command_core(opts, command, true, false, true, false,
				  opts->debug, &text);
	if (ret) {
		free(text);
		return ret;
	}

	if (!opts->debug)
		print_debug_switch_text_block(text, false);
	free(text);

	if (!opts->debug) {
		ret = l1_text_command_status(opts, "l1dbg", false, &l1dbg_text);
		printf("\n");
		if (ret)
			printf("(L1 debugging settings unavailable)\n");
		else
			print_text_block(l1dbg_text);
		free(l1dbg_text);
	}
	return 0;
}

static int do_debug_command(const struct options *opts, int argc, char **argv,
			    int command_index)
{
	struct options cmd_opts;
	struct debug_options debug;
	uint32_t switches;
	int ret;

	if (parse_debug_args(argc, argv, command_index + 1, &debug))
		return 2;

	if (debug.list_switches) {
		print_debug_switch_list();
		return 0;
	}

	if (prepare_command_options(opts, &cmd_opts))
		return 1;

	if (!debug.update)
		return print_debug_report(&cmd_opts, true);

	if (!(debug.force || opts->force)) {
		fprintf(stderr,
			"debug update requires --force because it changes L1 virtual debug switches\n");
		return 2;
	}

	if (debug.have_set) {
		switches = debug.set_value;
	} else {
		ret = read_current_debug_switches(&cmd_opts, &switches);
		if (ret)
			return ret;
	}

	switches |= debug.enable_mask;
	switches &= ~debug.disable_mask;
	if (debug.have_test) {
		switches &= ~SGIL1_DEBUG_TEST_MASK;
		switches |= debug.test_value;
	}
	if (debug.have_boot_stop) {
		switches &= ~SGIL1_DEBUG_BOOT_STOP_MASK;
		switches |= debug.boot_stop_value;
	}

	return write_debug_switches(&cmd_opts, switches);
}

static int parse_leds_args(int argc, char **argv, int start,
			   struct leds_options *leds)
{
	int i;

	memset(leds, 0, sizeof(*leds));
	leds->poll_interval_ms = SGIL1_LEDS_FOLLOW_POLL_MS;

	for (i = start; i < argc; i++) {
		if (!strcmp(argv[i], "-w") || !strcmp(argv[i], "--follow")) {
			leds->follow = true;
		} else if (!strcmp(argv[i], "--show-annotations")) {
			leds->show_annotations = true;
		} else if (!strcmp(argv[i], "--poll-interval")) {
			if (++i >= argc) {
				fprintf(stderr, "--poll-interval needs milliseconds\n");
				return -1;
			}
			if (parse_int_arg(argv[i], 50, INT_MAX,
					  &leds->poll_interval_ms)) {
				fprintf(stderr,
					"invalid --poll-interval value; minimum is 50 ms\n");
				return -1;
			}
		} else {
			fprintf(stderr, "unknown argument for leds: %s\n", argv[i]);
			return -1;
		}
	}

	return 0;
}

static int next_leds_follow_backoff_ms(int current)
{
	int next = current + (current / 2);

	if (next <= current)
		next = current + 1;
	if (next > SGIL1_LEDS_FOLLOW_BACKOFF_MAX_MS)
		next = SGIL1_LEDS_FOLLOW_BACKOFF_MAX_MS;
	return next;
}

static bool leds_follow_confirm_wants_on(enum leds_follow_power_confirm confirm)
{
	return confirm == LEDS_FOLLOW_CONFIRM_POWER_ON;
}

static const char *leds_follow_confirm_action(
	enum leds_follow_power_confirm confirm)
{
	return confirm == LEDS_FOLLOW_CONFIRM_POWER_OFF ? "down" : "up";
}

static const char *leds_follow_confirm_state(
	enum leds_follow_power_confirm confirm)
{
	return confirm == LEDS_FOLLOW_CONFIRM_POWER_OFF ? "off" : "on";
}

static void maybe_confirm_power_during_leds_follow(const struct options *opts,
						   enum leds_follow_power_confirm confirm,
						   bool *confirmed,
						   time_t *next_attempt)
{
	char *power_check;
	time_t now;

	if (confirm == LEDS_FOLLOW_CONFIRM_NONE || !confirmed || *confirmed)
		return;

	now = time(NULL);
	if (*next_attempt && now != (time_t)-1 && now < *next_attempt)
		return;

	power_check = l1_text_command(opts, "power check", false);
	if (power_check) {
		bool wants_on = leds_follow_confirm_wants_on(confirm);
		bool matched = wants_on ? power_text_says_on(power_check) :
					  power_text_says_off(power_check);

		if (matched) {
			printf("\nPower-%s: confirmed system appears %s\n\n",
			       leds_follow_confirm_action(confirm),
			       leds_follow_confirm_state(confirm));
			fflush(stdout);
			*confirmed = true;
		}
		free(power_check);
	}

	if (!*confirmed) {
		now = time(NULL);
		*next_attempt = now == (time_t)-1 ? 0 : now + 1;
	}
}

static int do_leds_follow(const struct options *opts, int poll_interval_ms,
			  enum leds_follow_power_confirm confirm_power)
{
	struct options route_opts = *opts;
	struct options cmd_opts = *opts;
	struct text_builder pending = { 0 };
	struct led_profile profile = { 0 };
	char *previous = NULL;
	bool prepared = false;
	bool power_confirmed = confirm_power == LEDS_FOLLOW_CONFIRM_NONE;
	bool buffer_leds = confirm_power != LEDS_FOLLOW_CONFIRM_NONE;
	time_t next_power_confirm_attempt = 0;
	time_t buffer_deadline = 0;
	int backoff_ms = poll_interval_ms;

	if (backoff_ms < SGIL1_LEDS_FOLLOW_POLL_MS)
		backoff_ms = SGIL1_LEDS_FOLLOW_POLL_MS;
	if (buffer_leds) {
		time_t now = time(NULL);

		buffer_deadline = now == (time_t)-1 ? 0 :
				  now + SGIL1_LEDS_FOLLOW_CONFIRM_BUFFER_SECONDS;
	}

	for (;;) {
		char *text = NULL;
		char *decoded = NULL;
		bool changed;
		int ret;

		if (!prepared) {
			if (prepare_command_options(&route_opts, &cmd_opts)) {
				fprintf(stderr,
					"LEDs follow: could not prepare L1 command route; backing off %d ms\n",
					backoff_ms);
				sleep_milliseconds(backoff_ms);
				backoff_ms =
					next_leds_follow_backoff_ms(backoff_ms);
				continue;
			}
			read_led_profile(&cmd_opts, &profile);
			prepared = true;
		}

		ret = run_l1_command_core(&cmd_opts, "leds", false, false, false,
					  false, opts->debug, &text);
		if (ret) {
			free(text);
			prepared = false;
			if (route_opts.dest_auto_discovered) {
				route_opts.dest_overridden = false;
				route_opts.dest_auto_discovered = false;
			}
			fprintf(stderr,
				"LEDs follow: leds command failed; backing off %d ms\n",
				backoff_ms);
			sleep_milliseconds(backoff_ms);
			backoff_ms = next_leds_follow_backoff_ms(backoff_ms);
			continue;
		}

		backoff_ms = poll_interval_ms;
		decoded = decode_leds_text(text, &profile, opts->debug,
					   opts->show_annotations);
		if (decoded) {
			free(text);
			text = decoded;
			decoded = NULL;
		}
		changed = !previous || strcmp(previous, text);
		maybe_confirm_power_during_leds_follow(&cmd_opts,
						       confirm_power,
						       &power_confirmed,
						       &next_power_confirm_attempt);
		if (power_confirmed && buffer_leds) {
			text_builder_flush_and_clear(&pending);
			buffer_leds = false;
		}
		if (!power_confirmed && buffer_leds && buffer_deadline) {
			time_t now = time(NULL);

			if (now != (time_t)-1 && now >= buffer_deadline) {
				text_builder_flush_and_clear(&pending);
				buffer_leds = false;
			}
		}
		if (changed && *text) {
			if (buffer_leds) {
				if (text_builder_append_text_block(&pending,
								   text)) {
					free(text);
					free(previous);
					free(pending.buf);
					return 1;
				}
			} else {
				print_text_block(text);
				fflush(stdout);
			}
		}
		if (changed) {
			free(previous);
			previous = text;
			text = NULL;
		}
		free(text);
		sleep_milliseconds(changed ? 0 : poll_interval_ms);
	}

	return 0;
}

static int do_leds_command(const struct options *opts, int argc, char **argv,
			   int command_index)
{
	struct leds_options leds;
	struct options cmd_opts;
	struct led_profile profile;
	char *text = NULL;
	int ret;

	if (parse_leds_args(argc, argv, command_index + 1, &leds))
		return 2;
	if (prepare_command_options(opts, &cmd_opts))
		return 1;
	cmd_opts.show_annotations |= leds.show_annotations;
	if (leds.follow)
		return do_leds_follow(&cmd_opts, leds.poll_interval_ms,
				      LEDS_FOLLOW_CONFIRM_NONE);

	read_led_profile(&cmd_opts, &profile);
	ret = run_l1_command_core(&cmd_opts, "leds", false, opts->force, false,
				  false, opts->debug, &text);
	if (ret) {
		free(text);
		return ret;
	}
	if (!opts->debug || cmd_opts.show_annotations)
		print_leds_text_block(text, &profile, opts->debug,
				     cmd_opts.show_annotations);
	free(text);
	return 0;
}

static int parse_force_follow_args(int argc, char **argv, int command_index,
				   bool *force, bool *follow)
{
	int i;

	for (i = command_index + 1; i < argc; i++) {
		if (is_force_option(argv[i])) {
			*force = true;
		} else if (!strcmp(argv[i], "-w") ||
			   !strcmp(argv[i], "--follow")) {
			*follow = true;
		} else {
			fprintf(stderr, "unknown argument for %s: %s\n",
				argv[command_index], argv[i]);
			return -1;
		}
	}

	return 0;
}

static int do_reset_command(const struct options *opts, int argc, char **argv,
			    int command_index)
{
	char *text = NULL;
	bool force = opts->force;
	bool follow = false;
	int ret;

	if (parse_force_follow_args(argc, argv, command_index, &force, &follow))
		return 2;
	if (!force) {
		fprintf(stderr,
			"refusing L1 command 'reset': add --force to confirm system power/reset action\n");
		return 2;
	}

	ret = run_l1_command_core(opts, "reset", true, false, false, true,
				  opts->debug, &text);
	if (ret == SGIL1_L1CMD_RESPONSE_TIMEOUT) {
		printf("Reset: command sent; response timed out, which can happen if USB drops during reset\n");
		ret = 0;
	} else if (!ret && !opts->debug) {
		print_text_block(text);
	}
	free(text);
	if (ret || !follow)
		return ret;

	return do_leds_follow(opts, SGIL1_LEDS_FOLLOW_POLL_MS,
			      LEDS_FOLLOW_CONFIRM_NONE);
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
				printf("Power-%s: confirmed system appears %s\n",
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
					"Power-%s: timed out waiting for system to appear %s\n",
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

static int do_power_up_confirmed(const struct options *opts, bool confirm)
{
	int ret;

	ret = run_l1_command_core(opts, "power up", true, false,
				  false, true, opts->debug, NULL);
	if (ret != 0 && ret != SGIL1_L1CMD_RESPONSE_TIMEOUT)
		return ret;

	if (!confirm) {
		printf("Power-up: entering LED follow before power-state confirmation\n");
		return 0;
	}

	return wait_for_power_on(opts, power_confirm_timeout_ms(opts));
}

static int do_power_down_confirmed(const struct options *opts,
				   bool force_second_signal, bool confirm)
{
	char *response = NULL;
	bool requested_second;
	int ret;

	ret = run_l1_command_core(opts, "power down", true, false,
				  false, false, opts->debug, &response);
	if (ret)
		goto out;

	requested_second = power_down_text_requests_second_command(response);
	if (requested_second && !force_second_signal) {
		printf("Power-down: L1 requested second signal; use --force to send it\n");
	}
	if (force_second_signal) {
		if (requested_second)
			printf("Power-down: L1 requested second signal; issuing it\n");
		else
			printf("Power-down: --force set; issuing second power-down signal\n");
		free(response);
		response = NULL;
		ret = run_l1_command_core(opts, "power down", true,
					  false, false, false, opts->debug, NULL);
		if (ret)
			goto out;
	}

	if (!confirm) {
		printf("Power-down: entering LED follow before power-state confirmation\n");
		ret = 0;
		goto out;
	}

	ret = wait_for_power_off(opts, power_confirm_timeout_ms(opts));

out:
	free(response);
	return ret;
}

static int do_host_softreset_confirmed(const struct options *opts,
				       const char *l1cmd, bool allow_destructive,
				       bool follow)
{
	char *text = NULL;
	int ret;

	if (!allow_destructive) {
		fprintf(stderr,
			"power reset requires --force because it resets/restarts the system\n");
		return 2;
	}

	ret = run_l1_command_core(opts, l1cmd, true, false, false, true,
				  opts->debug, &text);
	if (ret == SGIL1_L1CMD_RESPONSE_TIMEOUT) {
		printf("Power-reset: command sent; response timed out, which can happen if USB drops during host reset\n");
		ret = 0;
	} else if (!ret && !opts->debug) {
		print_text_block(text);
	}
	free(text);
	if (ret || !follow)
		return ret;

	return do_leds_follow(opts, SGIL1_LEDS_FOLLOW_POLL_MS,
			      LEDS_FOLLOW_CONFIRM_NONE);
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

static int maybe_power_up_from_wait(const struct options *opts, bool follow,
				    struct options *follow_opts,
				    bool *follow_opts_valid)
{
	struct options local_opts;
	struct options *cmd_opts = follow && follow_opts ? follow_opts :
				   &local_opts;
	char *power_check;
	int ret = 0;

	if (follow_opts_valid)
		*follow_opts_valid = false;

	if (prepare_command_options(opts, cmd_opts))
		return 1;

	power_check = l1_text_command(cmd_opts, "power check", false);
	if (!power_check)
		return 1;

	printf("\nPower-up check\n");
	print_text_block(power_check);
	if (power_text_says_on(power_check)) {
		printf("Power-up: system already appears on; no action taken\n");
	} else if (power_text_says_off(power_check)) {
		printf("Power-up: system appears off; issuing power up\n");
		ret = do_power_up_confirmed(cmd_opts, !follow);
	} else {
		fprintf(stderr,
			"Power-up: cannot determine power state; no action taken\n");
		ret = 1;
	}

	if (!ret && follow && follow_opts && follow_opts_valid)
		*follow_opts_valid = true;

	free(power_check);
	return ret;
}

static int do_reset_for_wait(const struct options *opts)
{
	printf("\nPower-reset: issuing host soft reset from wait mode\n");
	return do_host_softreset_confirmed(opts, "softreset", true, false);
}

static int do_wait_power_action(const struct options *opts,
				const struct wait_options *wait,
				struct options *follow_opts,
				bool *follow_opts_valid)
{
	if (wait->power_up)
		return maybe_power_up_from_wait(opts, wait->follow, follow_opts,
						follow_opts_valid);
	if (wait->power_down) {
		const struct options *action_opts = opts;
		int ret;

		if (follow_opts_valid)
			*follow_opts_valid = false;
		printf("\nPower-down: issuing power down from wait mode\n");
		if (wait->follow && follow_opts) {
			if (prepare_command_options(opts, follow_opts))
				return 1;
			action_opts = follow_opts;
		}
		ret = do_power_down_confirmed(action_opts, true, !wait->follow);
		if (ret)
			return ret;
		if (wait->follow && follow_opts && follow_opts_valid)
			*follow_opts_valid = true;
		return 0;
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
			"WARNING: wait --power-down is armed; when an L1 USB device connects, sgil1ctl will power off the system.\n");
	} else if (wait->reset) {
		fprintf(stderr,
			"WARNING: wait --reset is armed; when an L1 USB device connects, sgil1ctl will reset/restart the system.\n");
	}

	if (force)
		return 0;

	if (wait->power_up)
		fprintf(stderr,
			"wait --power-up requires --force because it changes system power state\n");
	else if (wait->power_down)
		fprintf(stderr,
			"wait --power-down requires --force because it will power off the system on connection\n");
	else
		fprintf(stderr,
			"wait --reset requires --force because it will reset/restart the system on connection\n");

	return 2;
}

static int do_wait(const struct options *opts, const struct wait_options *wait)
{
	int ret;

	ret = validate_wait_power_action(opts, wait);
	if (ret)
		return ret;

	for (;;) {
		struct options follow_opts;
		bool follow_opts_valid = false;
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
			ret = do_wait_power_action(opts, wait, &follow_opts,
						   &follow_opts_valid);
			if (ret) {
				release_sgil1_lock();
				return ret;
			}
		}

		if (wait->follow) {
			const struct options *led_opts =
				follow_opts_valid ? &follow_opts : opts;

			ret = do_leds_follow(led_opts,
					     SGIL1_LEDS_FOLLOW_POLL_MS,
					     wait->power_up ?
					     LEDS_FOLLOW_CONFIRM_POWER_ON :
					     wait->power_down ?
					     LEDS_FOLLOW_CONFIRM_POWER_OFF :
					     LEDS_FOLLOW_CONFIRM_NONE);
			release_sgil1_lock();
			return ret;
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
			opts->dest_auto_discovered = false;
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
		} else if (!strcmp(argv[i], "--show-annotations")) {
			opts->show_annotations = true;
		} else if (!strcmp(argv[i], "--version")) {
			printf("sgil1ctl %s\n", SGIL1CTL_VERSION);
			exit(0);
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
		} else if (is_help_option(argv[i])) {
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

	if (command_help_requested(argc, argv, command_index + 1)) {
		if (command_usage(stdout, cmd))
			return 0;
		fprintf(stderr, "unknown command: %s; use --help for usage\n",
			cmd);
		return 2;
	}

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
	if (!strcmp(cmd, "date") || !strcmp(cmd, "set-date") ||
	    !strcmp(cmd, "clock") || !strcmp(cmd, "time") ||
	    !strcmp(cmd, "set-clock") || !strcmp(cmd, "set-time")) {
		struct status_options status;

		if (parse_status_args(argc, argv, command_index + 1, &status))
			return 2;
		if (!strcmp(cmd, "set-date") || !strcmp(cmd, "set-clock") ||
		    !strcmp(cmd, "set-time"))
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
	if (!strcmp(cmd, "log") || !strcmp(cmd, "logs"))
		return do_log_command(&opts, argc, argv, command_index);
	if (!strcmp(cmd, "leds") || !strcmp(cmd, "led"))
		return do_leds_command(&opts, argc, argv, command_index);
	if (!strcmp(cmd, "watch"))
		return do_watch_command(&opts, argc, argv, command_index);
	if (!strcmp(cmd, "debug"))
		return do_debug_command(&opts, argc, argv, command_index);
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
		bool follow = false;
		struct options follow_opts;
		const struct options *action_opts = &opts;
		int ret;

		if (parse_force_follow_args(argc, argv, command_index, &force,
					    &follow))
			return 2;
		if (follow) {
			if (prepare_command_options(&opts, &follow_opts))
				return 1;
			action_opts = &follow_opts;
		}
		(void)force;
		ret = do_power_up_confirmed(action_opts, !follow);
		if (ret || !follow)
			return ret;
		return do_leds_follow(action_opts, SGIL1_LEDS_FOLLOW_POLL_MS,
				      LEDS_FOLLOW_CONFIRM_POWER_ON);
	}
	if (!strcmp(cmd, "power-down")) {
		bool force = opts.force;
		bool follow = false;
		struct options follow_opts;
		const struct options *action_opts = &opts;
		int ret;

		if (parse_force_follow_args(argc, argv, command_index, &force,
					    &follow))
			return 2;
		if (follow) {
			if (prepare_command_options(&opts, &follow_opts))
				return 1;
			action_opts = &follow_opts;
		}
		ret = do_power_down_confirmed(action_opts, force, !follow);
		if (ret || !follow)
			return ret;
		return do_leds_follow(action_opts, SGIL1_LEDS_FOLLOW_POLL_MS,
				      LEDS_FOLLOW_CONFIRM_POWER_OFF);
	}
	if (!strcmp(cmd, "reset")) {
		return do_reset_command(&opts, argc, argv, command_index);
	}

	fprintf(stderr,
		"unknown command: %s; use 'l1cmd' for direct L1 command pass-through or --help for usage\n",
		cmd);
	return 2;
}
