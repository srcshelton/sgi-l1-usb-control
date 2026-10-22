/* SPDX-License-Identifier: GPL-2.0-or-later */
#define main sgil1ctl_main
#include "../tools/sgil1ctl.c"
#undef main
#include <assert.h>
#include <dirent.h>
#include <sys/resource.h>

static void append(struct watch_tui_state *state, const char *raw,
                   const char *timestamp)
{
	const struct led_profile profile = { .ip35 = true };
	struct watch_tui_led_snapshot *last;
	char *all = decode_leds_text(raw, &profile, true, false);
	char *filtered = decode_leds_text(raw, &profile, false, false);
	size_t index;

	assert(all && filtered);
	assert(!watch_tui_append_leds(state, all, filtered, raw, &profile));
	free(all);
	free(filtered);
	last = &state->led_history.snapshots[watch_tui_history_index(
		state->led_history.start, state->led_history.run_count - 1,
		state->led_history.capacity)];
	index = watch_tui_history_index(last->timestamp_start,
		last->timestamp_count - 1, last->timestamp_capacity);
	snprintf(last->timestamps[index], SGIL1_TUI_TIMESTAMP_SIZE, "%s", timestamp);
}

static void expect_line(struct watch_tui_state *state, size_t index,
                        const char *expected)
{
	enum watch_tui_line_style style;
	assert(!strcmp(watch_tui_led_line_at(state, index, &style), expected));
}

static void test_repeats(void)
{
	struct watch_tui_state state = { 0 };
	unsigned int i;

	assert(!watch_tui_init_histories(&state, 4, 5));
	for (i = 0; i < 100; i++) {
		char stamp[16];
		snprintf(stamp, sizeof(stamp), "t%03u", i);
		append(&state, "CPU  A: 0x55\n", stamp);
	}
	assert(state.led_history.response_count == 5);
	assert(watch_tui_led_visible_line_count(&state) == 2);
	expect_line(&state, 1, "5 samples");
	state.show_led_timestamps = true;
	assert(watch_tui_led_visible_line_count(&state) == 3);
	expect_line(&state, 0, "t095");
	expect_line(&state, 2, "5 samples; last t099");
	state.show_annotations = true;
	expect_line(&state, 2, "5 samples; last t099");
	state.all_mode = true;
	assert(watch_tui_led_visible_line_count(&state) == 10);
	expect_line(&state, 8, "t099");
	watch_tui_free(&state);

	assert(!watch_tui_init_histories(&state, 4, 5));
	append(&state, "CPU  A: 0xff\n        0x55\n", "first");
	append(&state, "CPU  A: 0x7f\n        0x55\n", "second");
	append(&state, "CPU  A: 0xfe\n", "hidden");
	append(&state, "CPU  A: 0x55\n", "last");
	assert(watch_tui_led_visible_line_count(&state) == 2);
	expect_line(&state, 1, "3 samples");
	state.show_led_timestamps = true;
	expect_line(&state, 0, "first");
	expect_line(&state, 2, "3 samples; last last");
	append(&state, "CPU  A: 0x70\n", "change");
	assert(watch_tui_led_visible_line_count(&state) == 5);
	append(&state, "CPU  A: 0x55\n", "return");
	assert(watch_tui_led_filtered_state_count(&state.led_history) == 3);
	expect_line(&state, 0, "second");
	expect_line(&state, 2, "2 samples; last last");
	state.all_mode = true;
	assert(watch_tui_led_visible_line_count(&state) == 11);
	watch_tui_free(&state);
}

static void keys(struct watch_tui_state *state, const char *input)
{
	while (*input)
		watch_tui_operator_key(state, (unsigned char)*input++);
}

static void select_ready(struct watch_tui_state *state, enum watch_tui_action action)
{
	state->operator_refresh = false;
	state->requested_action = WATCH_TUI_ACTION_NONE;
	state->selected_action = action;
	state->confirmation[0] = '\0';
}

static void test_second_power_down(void)
{
	struct watch_tui_state state = { .operator_visible = true };
	struct log_line_list lines = { 0 };

	assert(!split_log_lines("old entry\nPower Down: issue 'pwr d' again to immediately power down.\n", &lines));
	watch_tui_power_down_log(&state, &lines, 1);
	assert(state.requested_action == WATCH_TUI_ACTION_NONE);
	state.power_down_log_pending = true;
	watch_tui_power_down_log(&state, &lines, lines.count); /* Stale entries do not arm it. */
	assert(!state.power_down_log_pending && state.requested_action == WATCH_TUI_ACTION_NONE);
	watch_tui_power_down_log(&state, &lines, 1); /* A later read cannot resurrect it. */
	assert(state.requested_action == WATCH_TUI_ACTION_NONE);
	state.power_down_log_pending = true;
	watch_tui_power_down_log(&state, &lines, 1);
	assert(state.requested_action == WATCH_TUI_ACTION_POWER_DOWN_AGAIN);
	assert(!state.power_down_log_pending);
	select_ready(&state, WATCH_TUI_ACTION_POWER_DOWN_AGAIN);
	keys(&state, "n");
	assert(state.pending_action == WATCH_TUI_ACTION_NONE && state.operator_refresh);
	select_ready(&state, WATCH_TUI_ACTION_POWER_DOWN_AGAIN);
	keys(&state, "\r");
	assert(state.pending_action == WATCH_TUI_ACTION_NONE && state.operator_refresh);
	select_ready(&state, WATCH_TUI_ACTION_POWER_DOWN_AGAIN);
	keys(&state, "y");
	assert(state.pending_action == WATCH_TUI_ACTION_POWER_DOWN_AGAIN);
	keys(&state, "yyyy");
	assert(state.pending_action == WATCH_TUI_ACTION_POWER_DOWN_AGAIN);
	free_log_line_list(&lines);
	puts("PASS second power down: next log only, no stale prompts, yes once and safe cancellation");
}

static void test_environment_geometry(void)
{
	struct watch_tui_state state = { 0 };
	int saved_lines = LINES, saved_cols = COLS;
	int width, height, first, rows;
	size_t count, longest;
	char wide[201];

	LINES = 80; COLS = 120;
	state.environment_text = strdup("one\n\n  columns 1234567890\nlast\n\n  \n\t\n");
	assert(state.environment_text);
	watch_tui_environment_size(&state, &width, &height, &first, &rows, &count, &longest);
	assert(count == 4 && height == 10 && rows == 4 && width == 34);
	assert((LINES - height) / 2 >= 3 && first == 2);
	state.environment_row = state.environment_column = SIZE_MAX;
	watch_tui_environment_size(&state, &width, &height, &first, &rows, &count, &longest);
	assert(!state.environment_row && !state.environment_column);
	free(state.environment_text);
	memset(wide, 'x', sizeof(wide) - 1); wide[sizeof(wide) - 1] = '\0';
	state.environment_text = wide;
	COLS = 300;
	watch_tui_environment_size(&state, &width, &height, &first, &rows, &count, &longest);
	assert(width == 204 && height == 6 && count == 1 && rows == 1);
	LINES = 10; COLS = 40;
	watch_tui_environment_key(&state, KEY_END);
	state.environment_column = SIZE_MAX;
	watch_tui_environment_size(&state, &width, &height, &first, &rows, &count, &longest);
	assert(width == 38 && height == 4 && first == 1 && rows == 1);
	assert(state.environment_column == 166 && !state.environment_row);
	watch_tui_environment_key(&state, 'r');
	assert(state.environment_requested);
	watch_tui_environment_key(&state, '\n');
	assert(!state.environment_visible && !state.environment_requested);
	{
		struct watch_tui_environment_footer footer;

		state.environment_row = state.environment_column = 0;
		watch_tui_environment_footer(&state, 71, 40, 42, 70, &footer);
		assert(!strcmp(footer.line_range, "Lines 1-40 of 42") && !*footer.column_range && footer.rows == 1);
		watch_tui_environment_footer(&state, 201, 42, 42, 200, &footer);
		assert(!*footer.line_range && !*footer.column_range && !*footer.position);
		watch_tui_environment_footer(&state, 34, 1, 1, 200, &footer);
		assert(!*footer.line_range && !strcmp(footer.column_range, "Cols 1-34 of 200"));
	}
	LINES = saved_lines; COLS = saved_cols;
	puts("PASS Environment geometry: live content dimensions, blank rows, margins, compact view and scroll bounds");
}

static void test_diagnostic_capture(void)
{
	struct watch_display display = { .tui = true };
	struct watch_tui_state *state = &display.tui_state;
	int original, terminal[2];
	char buffer[32];

	assert(!pipe2(terminal, O_CLOEXEC | O_NONBLOCK));
	original = dup(STDERR_FILENO);
	assert(original >= 0 && dup2(terminal[1], STDERR_FILENO) >= 0);
	assert(!watch_tui_init_histories(state, 16, 4));
	assert(!watch_tui_capture_stderr(state));
	fprintf(stderr, "command timed-out\nset-pipes after L1 response timeout failed: Broken pipe\n");
	watch_tui_drain_diagnostics(&display);
	assert(state->raw_log_history.count == 2 && state->log_history.count == 2);
	assert(strstr(state->diagnostic_message, "Broken pipe"));
	assert(state->diagnostic_until > monotonic_milliseconds());
	assert(strstr(state->raw_log_history.lines[0], "[transport] command timed-out"));
	assert(read(terminal[0], buffer, sizeof(buffer)) < 0 && errno == EAGAIN);
	fprintf(stderr, "partial diagnostic");
	watch_tui_drain_diagnostics(&display);
	assert(state->raw_log_history.count == 3);
	watch_tui_free(state);
	fprintf(stderr, "restored stderr"); fflush(stderr);
	assert(read(terminal[0], buffer, sizeof(buffer)) == 15);
	assert(!memcmp(buffer, "restored stderr", 15));
	assert(dup2(original, STDERR_FILENO) >= 0);
	close(original); close(terminal[0]); close(terminal[1]);
	puts("PASS diagnostics: stderr isolated, complete/partial lines retained, transient status and stderr restored");
}

static void test_text_input_cursor(void)
{
	struct watch_tui_state state = { 0 };

	assert(!watch_tui_text_input_active(&state));
	state.save.phase = TUI_SAVE_PATH;
	assert(watch_tui_text_input_active(&state));
	state.save.capture_pending = true;
	assert(!watch_tui_text_input_active(&state));
	state.save.capture_pending = false;
	state.save.phase = TUI_SAVE_WRITING;
	assert(!watch_tui_text_input_active(&state));
	state.save.phase = TUI_SAVE_DONE;
	assert(!watch_tui_text_input_active(&state));
	state.save.phase = TUI_SAVE_NONE;
	state.operator_visible = true;
	state.selected_action = WATCH_TUI_ACTION_HOST_RESET;
	assert(watch_tui_text_input_active(&state));
	state.selected_action = WATCH_TUI_ACTION_NONE;
	state.pending_action = WATCH_TUI_ACTION_HOST_RESET;
	assert(!watch_tui_text_input_active(&state));
	state.pending_action = WATCH_TUI_ACTION_NONE;
	state.selected_action = WATCH_TUI_ACTION_POWER_DOWN_AGAIN;
	assert(!watch_tui_text_input_active(&state));
	assert(watch_tui_input_cells("abc", 3) == 3);
	puts("PASS text entry: cursor only for filename and word confirmation input");
}

static void test_confirmation(void)
{
	struct watch_tui_state state = { .operator_visible = true };

	keys(&state, "u");
	assert(state.requested_action == WATCH_TUI_ACTION_POWER_UP);
	assert(state.selected_action == WATCH_TUI_ACTION_NONE);
	select_ready(&state, WATCH_TUI_ACTION_HOST_RESET);
	keys(&state, "\n");
	assert(state.selected_action == WATCH_TUI_ACTION_NONE);
	assert(state.operator_refresh && state.operator_visible);
	select_ready(&state, WATCH_TUI_ACTION_HOST_RESET);
	keys(&state, "YESno\n");
	assert(state.selected_action == WATCH_TUI_ACTION_NONE);
	assert(state.pending_action == WATCH_TUI_ACTION_NONE);
	select_ready(&state, WATCH_TUI_ACTION_HOST_RESET);
	keys(&state, "YEZ\bS\n");
	assert(state.pending_action == WATCH_TUI_ACTION_HOST_RESET);
	keys(&state, "dYES\n");
	assert(state.pending_action == WATCH_TUI_ACTION_HOST_RESET);
	state.pending_action = WATCH_TUI_ACTION_NONE;
	select_ready(&state, WATCH_TUI_ACTION_HOST_RESET);
	keys(&state, "YES\033");
	assert(!state.operator_visible);
	assert(state.selected_action == WATCH_TUI_ACTION_NONE);
	state.waiting_for_usb = true;
	keys(&state, "\033*\n");
	assert(state.waiting_for_usb);
	keys(&state, "u\n");
	assert(state.wait_action == WATCH_TUI_ACTION_NONE);
	keys(&state, "dYES\n");
	assert(state.wait_action == WATCH_TUI_ACTION_POWER_DOWN);
	keys(&state, "u");
	assert(state.wait_action == WATCH_TUI_ACTION_NONE);
	keys(&state, "\n");
	assert(state.wait_action == WATCH_TUI_ACTION_NONE);
	state.wait_set_time = true;
	state.wait_action = WATCH_TUI_ACTION_POWER_DOWN;
	keys(&state, "a");
	assert(state.wait_action == WATCH_TUI_ACTION_NONE);
	assert(state.wait_set_time);
	state.action_busy = true;
	keys(&state, "eYES\n");
	assert(state.wait_action == WATCH_TUI_ACTION_NONE);
	keys(&state, "q");
	assert(watch_stop_requested);
	watch_stop_requested = 0;
}

static void test_firmware_banks(void)
{
	const char *normal = "Flash image B currently booted\r\n \t\r\n"
		"Image Status Revision Built\r\n----- -------- ---------- -----\r\n"
		"A valid 1.48.1 01/22/2007 11:34:20\r\n"
		"B default 1.24.11 10/29/2003 00:05:26\r\n";
	struct tui_firmware firmware, changed;
	struct watch_tui_state state = { 0 };

	assert(tui_firmware_parse(normal, &firmware));
	assert(firmware.current == 1 && tui_firmware_can_boot_other(&firmware));
	assert(!strcmp(firmware.bank[0].revision, "1.48.1"));
	assert(!strcmp(firmware.bank[1].built, "10/29/2003 00:05:26"));
	assert(tui_firmware_same_target(&firmware, &firmware, 0));
	assert(!tui_firmware_same_target(&firmware, &firmware, 1));
	state.firmware = firmware;
	assert(watch_tui_action_available(&state, WATCH_TUI_ACTION_L1_BANK_A));
	assert(!watch_tui_action_available(&state, WATCH_TUI_ACTION_L1_BANK_B));
	assert(!strcmp(watch_tui_action_command(WATCH_TUI_ACTION_L1_BANK_A), "reboot_l1 a"));
	assert(!strcmp(watch_tui_action_command(WATCH_TUI_ACTION_L1_BANK_B), "reboot_l1 b"));
	changed = firmware;
	changed.current = 0;
	assert(!tui_firmware_same_target(&firmware, &changed, 0));
	changed = firmware;
	snprintf(changed.bank[0].revision, sizeof(changed.bank[0].revision), "1.48.2");
	assert(!tui_firmware_same_target(&firmware, &changed, 0));
	assert(!tui_firmware_parse(NULL, &changed));
	assert(!tui_firmware_can_boot_other(&changed));
	assert(!tui_firmware_parse("Flash image A currently booted\nB valid 1.2.3\n", &changed));
	assert(!tui_firmware_can_boot_other(&changed));
	assert(tui_firmware_parse("Flash image A currently booted\nA default 1.2.3\nB invalid 9.9.9\n", &changed));
	assert(!tui_firmware_can_boot_other(&changed));
	assert(!tui_firmware_parse("Flash image A currently booted\nFlash image B currently booted\nA valid 1.2.3\nB valid 1.2.3\n", &changed));
	assert(!tui_firmware_can_boot_other(&changed));
	assert(!tui_firmware_parse("Flash image A currently booted\nA valid 1.2.3\nA valid 1.2.3\nB valid 1.2.3\n", &changed));
	assert(tui_firmware_parse("Flash image A currently booted\nA default 1.2.3\nB unknown 1.2.3\n", &changed));
	assert(!tui_firmware_can_boot_other(&changed));
	assert(tui_firmware_parse("Flash image A currently booted\nA default 1.2.3\nB valid nonsense\n", &changed));
	assert(!tui_firmware_can_boot_other(&changed));
	puts("PASS L1 firmware: bank/version/build parsing, validity, contradictory replies and target changes");
}

static void test_availability_and_clock(void)
{
	struct watch_tui_state state = { 0 };
	char text[128];
	struct timespec host = { .tv_sec = 1005 };
	time_t parsed;
	double fraction;

	assert(!watch_tui_action_available(&state, WATCH_TUI_ACTION_POWER_UP));
	assert(!watch_tui_action_available(&state, WATCH_TUI_ACTION_HOST_RESET));
	assert(watch_tui_action_available(&state, WATCH_TUI_ACTION_L1_RESET));
	assert(!strcmp(watch_tui_action_command(WATCH_TUI_ACTION_L1_RESET), "reboot_l1 current"));
	assert(!strcmp(watch_tui_action_command(WATCH_TUI_ACTION_HOST_RESET), "softreset"));
	state.host_state = WATCH_TUI_HOST_OFF;
	assert(watch_tui_action_available(&state, WATCH_TUI_ACTION_POWER_UP));
	assert(watch_tui_action_available(&state, WATCH_TUI_ACTION_L1_RESET));
	assert(!watch_tui_action_available(&state, WATCH_TUI_ACTION_POWER_DOWN));
	state.host_state = WATCH_TUI_HOST_ON;
	assert(!watch_tui_action_available(&state, WATCH_TUI_ACTION_POWER_UP));
	assert(watch_tui_action_available(&state, WATCH_TUI_ACTION_L1_RESET));
	assert(watch_tui_action_available(&state, WATCH_TUI_ACTION_POWER_DOWN));
	assert(watch_tui_action_available(&state, WATCH_TUI_ACTION_HOST_RESET));
	watch_tui_format_delta(22.0 * 365 * 86400 + 2 * 86400, text, sizeof(text));
	assert(strstr(text, "22y 2d"));
	watch_tui_format_delta(-0.125, text, sizeof(text));
	assert(strstr(text, "ahead by ~0.125s"));
	assert(!watch_tui_parse_clock("01/01/2000 01:00:00.125", "CET-1", &parsed, &fraction));
	assert(parsed == 946684800 && fraction == 0.125);
	assert(!watch_tui_clock_status(&state, 1000, &host, false, text, sizeof(text)));
	assert(!strcmp(text, "L1 unavailable"));
	state.clock_valid = true;
	state.clock_epoch = 1000;
	state.clock_sample_ms = 1000;
	strcpy(state.l1_timezone, "UTC0");
	assert(!watch_tui_clock_status(&state, 1000, &host, false, text, sizeof(text)));
	host.tv_nsec = 1000000;
	assert(watch_tui_clock_status(&state, 1000, &host, false, text, sizeof(text)));
	assert(strstr(text, "[out of sync]"));
	host.tv_sec = 995;
	host.tv_nsec = 0;
	assert(!watch_tui_clock_status(&state, 1000, &host, true, text, sizeof(text)));
	host.tv_sec = 994;
	assert(watch_tui_clock_status(&state, 1000, &host, true, text, sizeof(text)));
	host.tv_sec = 1010;
	assert(!watch_tui_clock_status(&state, 11000, &host, true, text, sizeof(text)));
}

static void test_activity(void)
{
	struct watch_display display = { .tui = true };
	struct led_profile profile = { .ip35 = true };

	assert(!watch_tui_init_histories(&display.tui_state, 4, 4));
	watch_display_activity(&display, "");
	assert(!display.console_activity_seen);
	assert(!watch_display_raw_log_line(&display, "new log output"));
	assert(display.console_activity_seen);
	display.console_activity_seen = false;
	watch_display_leds(&display, "CPU  A: 0x09", "CPU  A: 0x09", false,
		false, "CPU  A: 0x09", &profile, false);
	assert(display.console_activity_seen);
	display.console_activity_seen = false;
	watch_display_leds(&display, "CPU  A: 0xfe", "", false,
		false, "CPU  A: 0xfe", &profile, false);
	assert(display.console_activity_seen);
	watch_tui_free(&display.tui_state);
}

static void save_test_file(const char *path, const char *text, mode_t mode)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
	assert(fd >= 0);
	assert(!tui_save_write_all(fd, text, strlen(text)));
	assert(!close(fd));
	assert(!chmod(path, mode));
}

static void save_test_contents(const char *path, const char *expected)
{
	char buffer[8192];
	int fd = open(path, O_RDONLY);
	ssize_t count;

	assert(fd >= 0);
	count = read(fd, buffer, sizeof(buffer) - 1);
	assert(count >= 0);
	buffer[count] = '\0';
	assert(!strcmp(buffer, expected));
	close(fd);
}

static void save_test_init(struct tui_save *save, const char *path, const char *data)
{
	memset(save, 0, sizeof(*save));
	save->initialized = true;
	save->directory_fd = -1;
	save->data = strdup(data);
	assert(save->data);
	save->bytes = strlen(data);
	snprintf(save->input, sizeof(save->input), "%s", path);
}

static void test_save_files(void)
{
	char root[PATH_MAX - 128], path[PATH_MAX], other[PATH_MAX], directory[PATH_MAX];
	struct tui_save save;
	struct stat st;
	struct rlimit limit, original;
	void (*old_signal)(int);
	DIR *contents;
	struct dirent *entry;

	assert(getenv("TMPDIR"));
	snprintf(root, sizeof(root), "%s/sgil1-save-XXXXXX", getenv("TMPDIR"));
	assert(mkdtemp(root));
	snprintf(path, sizeof(path), "%s/data '$(echo bad)`.txt", root);
	save_test_init(&save, path, "first\n");
	assert(tui_save_prepare(&save) == 0);
	if (tui_save_commit(&save)) {
		fprintf(stderr, "Save test failed: %s\n", save.message);
		abort();
	}
	save_test_contents(path, "first\n");
	assert(!stat(path, &st) && (st.st_mode & 0777) == 0600);
	tui_save_reset(&save);
	save_test_init(&save, path, "next\n");
	assert(tui_save_prepare(&save) == 1);
	save.append = true;
	if (tui_save_commit(&save)) {
		fprintf(stderr, "Save test failed: %s\n", save.message);
		abort();
	}
	save_test_contents(path, "first\nnext\n");
	tui_save_reset(&save);
	assert(!chmod(path, 0640));
	save_test_init(&save, path, "replace\n");
	assert(tui_save_prepare(&save) == 1);
	if (tui_save_commit(&save)) {
		fprintf(stderr, "Save test failed: %s\n", save.message);
		abort();
	}
	save_test_contents(path, "replace\n");
	assert(!stat(path, &st) && (st.st_mode & 0777) == 0640);
	tui_save_reset(&save);
	/* A replaced or edited target must not be overwritten from a stale prompt. */
	save_test_init(&save, path, "bad");
	assert(tui_save_prepare(&save) == 1);
	assert(!unlink(path));
	save_test_file(path, "someone else's data", 0600);
	assert(tui_save_commit(&save) < 0);
	save_test_contents(path, "someone else's data");
	tui_save_reset(&save);
	snprintf(other, sizeof(other), "%s/new.txt", root);
	save_test_init(&save, other, "bad");
	assert(tui_save_prepare(&save) == 0);
	save_test_file(other, "created during prompt", 0600);
	assert(tui_save_commit(&save) < 0);
	save_test_contents(other, "created during prompt");
	tui_save_reset(&save);
	assert(!unlink(other));
	/* Neither linked files nor special files may be used as save destinations. */
	assert(!symlink(path, other));
	save_test_init(&save, other, "bad");
	assert(tui_save_prepare(&save) < 0);
	tui_save_reset(&save);
	assert(!unlink(other));
	assert(!link(path, other));
	save_test_init(&save, path, "bad");
	assert(tui_save_prepare(&save) < 0);
	tui_save_reset(&save);
	assert(!unlink(other));
	assert(!mkfifo(other, 0600));
	save_test_init(&save, other, "bad");
	assert(tui_save_prepare(&save) < 0);
	tui_save_reset(&save);
	assert(!unlink(other));
	/* A partial staging write must leave existing data unchanged and clean up. */
	save_test_init(&save, path, "longer than limit");
	assert(tui_save_prepare(&save) == 1);
	assert(!getrlimit(RLIMIT_FSIZE, &original));
	limit = original;
	limit.rlim_cur = 4;
	old_signal = signal(SIGXFSZ, SIG_IGN);
	assert(!setrlimit(RLIMIT_FSIZE, &limit));
	assert(tui_save_commit(&save) < 0);
	assert(!setrlimit(RLIMIT_FSIZE, &original));
	signal(SIGXFSZ, old_signal);
	save_test_contents(path, "someone else's data");
	tui_save_reset(&save);
	/* Check actual permissions even when the test runner itself is root. */
	assert(!chmod(root, 0777));
	assert(!chmod(path, 0400));
	{
		pid_t child = fork();
		int status;

		assert(child >= 0);
		if (!child) {
			if (!geteuid()) {
				if (setgid(65534) || setuid(65534))
					_exit(2);
			}
			save_test_init(&save, path, "bad");
			_exit(tui_save_prepare(&save) < 0 ? 0 : 1);
		}
		assert(waitpid(child, &status, 0) == child);
		assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	}
	assert(!chmod(path, 0600));
	/* Directory preference is deterministic and falls through invalid entries. */
	snprintf(directory, sizeof(directory), "%s/default", root);
	assert(!mkdir(directory, 0700));
	{
		char *tmpdir = strdup(getenv("TMPDIR"));
		char *tmp = getenv("TMP") ? strdup(getenv("TMP")) : NULL;
		char *runtime = getenv("XDG_RUNTIME_DIR") ? strdup(getenv("XDG_RUNTIME_DIR")) : NULL;
		char chosen[PATH_MAX];

		setenv("TMPDIR", "/nonexistent-sgil1-directory", 1);
		setenv("TMP", directory, 1);
		setenv("XDG_RUNTIME_DIR", root, 1);
		assert(tui_save_default_directory(chosen, sizeof(chosen)) && !strcmp(chosen, directory));
		setenv("TMPDIR", root, 1);
		assert(tui_save_default_directory(chosen, sizeof(chosen)) && !strcmp(chosen, root));
		setenv("TMPDIR", "/proc", 1);
		setenv("TMP", "/nonexistent-sgil1-directory", 1);
		assert(tui_save_default_directory(chosen, sizeof(chosen)) && !strcmp(chosen, root));
		setenv("XDG_RUNTIME_DIR", "/nonexistent-sgil1-directory", 1);
		assert(tui_save_default_directory(chosen, sizeof(chosen)));
		assert(!strcmp(chosen, "/var/tmp") || !strcmp(chosen, "/tmp"));
		setenv("TMPDIR", tmpdir, 1);
		if (tmp) setenv("TMP", tmp, 1); else unsetenv("TMP");
		if (runtime) setenv("XDG_RUNTIME_DIR", runtime, 1); else unsetenv("XDG_RUNTIME_DIR");
		free(tmpdir); free(tmp); free(runtime);
	}
	assert(!rmdir(directory));
	contents = opendir(root);
	assert(contents);
	while ((entry = readdir(contents)))
		assert(strncmp(entry->d_name, ".sgil1ctl-save-", 14));
	closedir(contents);
	assert(!unlink(path));
	assert(!rmdir(root));
	puts("PASS Save buffer files: create, append, overwrite, permissions, stale targets, links, partial writes and directory order");
}

static void test_save_snapshot(void)
{
	struct watch_tui_state state = { 0 };

	assert(!watch_tui_init_histories(&state, 4, 4));
	assert(!watch_tui_append_log(&state, "hidden or folded raw log", true));
	watch_tui_save_begin(&state);
	watch_tui_save_capture(&state);
	assert(!strcmp(state.save.data, "hidden or folded raw log\n"));
	assert(!watch_tui_append_log(&state, "later log", true));
	assert(!strstr(state.save.data, "later log"));
	assert(state.save.snapshot_valid);
	state.save.snapshot_valid = false;
	state.save.phase = TUI_SAVE_ERROR;
	watch_tui_save_key(&state, '\n');
	assert(state.save.capture_pending);
	tui_save_reset(&state.save);
	append(&state, "CPU  A: 0xff\n        0x55\n", "first");
	append(&state, "CPU  A: 0xff\n        0x55\n", "second");
	state.focus = WATCH_TUI_FOCUS_LEDS;
	watch_tui_save_begin(&state);
	watch_tui_save_capture(&state);
	assert(state.save.entries == 2);
	assert(strstr(state.save.data, "first\n"));
	assert(strstr(state.save.data, "second\n"));
	assert(contains_ci(state.save.data, "0xff"));
	watch_tui_free(&state);
	puts("PASS Save buffer snapshot: all retained data, repeats, hidden LEDs and stable capture");
}

static void test_save_clipboard(void)
{
	char root[PATH_MAX - 128], helper[PATH_MAX], copied[PATH_MAX];
	char *old_path = getenv("PATH") ? strdup(getenv("PATH")) : NULL;
	struct tui_save save;
	int i;

	snprintf(root, sizeof(root), "%s/sgil1-copy-XXXXXX", getenv("TMPDIR"));
	assert(mkdtemp(root));
	snprintf(helper, sizeof(helper), "%s/pbcopy", root);
	snprintf(copied, sizeof(copied), "%s/copied.txt", root);
	setenv("SGIL1_TEST_CLIPBOARD", copied, 1);
	setenv("PATH", root, 1);
	save_test_init(&save, "", "");
	tui_save_find_clipboard(&save);
	assert(!save.clipboard_kind);
	save_test_file(helper, "#!/bin/sh\n/bin/cat > \"$SGIL1_TEST_CLIPBOARD\"\n", 0700);
	snprintf(save.path, sizeof(save.path), "/path/with 'quotes' $(touch BAD) and spaces");
	tui_save_find_clipboard(&save);
	assert(save.clipboard_kind == 1);
	tui_save_copy_start(&save, monotonic_milliseconds());
	for (i = 0; save.copy_pid > 0 && i < 100; i++) {
		usleep(10000);
		tui_save_copy_poll(&save, monotonic_milliseconds());
	}
	assert(!save.copy_pid && strstr(save.message, "copied to clipboard"));
	save_test_contents(copied, save.path);
	save_test_file(helper, "#!/bin/sh\necho 'display access denied' >&2\nexit 3\n", 0700);
	tui_save_copy_start(&save, monotonic_milliseconds());
	for (i = 0; save.copy_pid > 0 && i < 100; i++) {
		usleep(10000);
		tui_save_copy_poll(&save, monotonic_milliseconds());
	}
	assert(!save.copy_pid && strstr(save.message, "display access denied"));
	save_test_file(helper, "#!/bin/sh\nexec /bin/sleep 60\n", 0700);
	tui_save_copy_start(&save, monotonic_milliseconds());
	tui_save_copy_poll(&save, monotonic_milliseconds() + 6000);
	for (i = 0; save.copy_pid > 0 && i < 100; i++) {
		usleep(10000);
		tui_save_copy_poll(&save, monotonic_milliseconds());
	}
	assert(!save.copy_pid && strstr(save.message, "timed out"));
	assert(!unlink(helper));
	{
		char *old_wayland = getenv("WAYLAND_DISPLAY") ? strdup(getenv("WAYLAND_DISPLAY")) : NULL;
		char *old_display = getenv("DISPLAY") ? strdup(getenv("DISPLAY")) : NULL;
		const char *names[] = { "wl-copy", "xclip", "xsel" };
		const char *arguments[] = { "", "-selection clipboard -in", "--clipboard --input" };

		for (size_t provider = 0; provider < ARRAY_SIZE(names); provider++) {
			char script[256];

			snprintf(helper, sizeof(helper), "%s/%s", root, names[provider]);
			snprintf(script, sizeof(script), "#!/bin/sh\n[ \"$*\" = '%s' ] || exit 4\n/bin/cat > \"$SGIL1_TEST_CLIPBOARD\"\n", arguments[provider]);
			save_test_file(helper, script, 0700);
			unsetenv("WAYLAND_DISPLAY"); unsetenv("DISPLAY");
			tui_save_find_clipboard(&save);
			assert(!save.clipboard_kind);
			setenv(provider ? "DISPLAY" : "WAYLAND_DISPLAY", "test-display", 1);
			tui_save_find_clipboard(&save);
			assert(save.clipboard_kind == (int)provider + 2);
			tui_save_copy_start(&save, monotonic_milliseconds());
			for (i = 0; save.copy_pid > 0 && i < 100; i++) {
				usleep(10000);
				tui_save_copy_poll(&save, monotonic_milliseconds());
			}
			assert(!save.copy_pid && strstr(save.message, "copied to clipboard"));
			save_test_contents(copied, save.path);
			assert(!unlink(helper));
		}
		if (old_wayland) setenv("WAYLAND_DISPLAY", old_wayland, 1); else unsetenv("WAYLAND_DISPLAY");
		if (old_display) setenv("DISPLAY", old_display, 1); else unsetenv("DISPLAY");
		free(old_wayland); free(old_display);
	}
	tui_save_reset(&save);
	if (old_path) setenv("PATH", old_path, 1); else unsetenv("PATH");
	free(old_path);
	unsetenv("SGIL1_TEST_CLIPBOARD");
	assert(!unlink(copied));
	assert(!rmdir(root));
	puts("PASS Save buffer clipboard: hidden when unavailable, literal path copy, failure and timeout");
}
static void test_menu_rendering(void)
{
	struct watch_tui_state state = { .operator_visible = true, .host_state = WATCH_TUI_HOST_OFF };
	FILE *input = fopen("/dev/null", "r"), *output = fopen("/dev/null", "w");
	SCREEN *screen;
	char text[128];
	chtype cell;

	assert(watch_tui_accelerator("Power up", 'u') == 6);
	assert(watch_tui_accelerator("Reset/restart host", 'e') == 1);
	assert(watch_tui_accelerator("Reset L1", 'l') == 6);
	assert(watch_tui_accelerator("Reboot current", 'C') == 7);
	assert(watch_tui_accelerator("Boot other", 'o') == 5);
	assert(watch_tui_accelerator("No match", 'z') == SIZE_MAX);
	for (int width = 0; width <= 200; width++) {
		const char *keys = watch_tui_footer_keys(width);
		assert(strlen(keys) <= (size_t)width);
		assert(width < 6 || strstr(keys, "q Quit"));
	}
	watch_tui_operator_key(&state, 'r');
	assert(state.operator_refresh && state.requested_action == WATCH_TUI_ACTION_NONE);
	state.operator_refresh = false;
	watch_tui_operator_key(&state, 'e');
	assert(state.requested_action == WATCH_TUI_ACTION_HOST_RESET);
	state.requested_action = WATCH_TUI_ACTION_NONE;
	state.host_state = WATCH_TUI_HOST_OFF;
	assert(input && output);
	screen = newterm("xterm-256color", output, input);
	assert(screen);
	assert(resizeterm(24, 120) != ERR);
	watch_tui_draw_operator(&state);
	cell = mvwinch(stdscr, 9, 27);
	assert((cell & A_CHARTEXT) == 'u' && (cell & A_UNDERLINE));
	cell = mvwinch(stdscr, 10, 28);
	assert((cell & A_CHARTEXT) == 'd' && (cell & A_UNDERLINE) && (cell & A_DIM));
	assert(mvwinch(stdscr, 16, 19) & A_DIM);
#ifdef A_ITALIC
	assert(mvwinch(stdscr, 16, 19) & A_ITALIC);
#endif
	assert((mvwinch(stdscr, 17, 19) & A_CHARTEXT) != ' ');
	tui_firmware_parse("Flash image B currently booted\n"
		"A valid 1.48.1 01/22/2007 11:34:20\n"
		"B default 1.24.11 10/29/2003 00:05:26\n", &state.firmware);
	state.firmware_visible = true;
	watch_tui_draw_operator(&state);
	mvwinnstr(stdscr, 13, 19, text, 40);
	assert(strstr(text, "c Reboot current (Bank B)"));
	mvwinnstr(stdscr, 14, 19, text, 40);
	assert(strstr(text, "o Boot other (Bank A)"));
	cell = mvwinch(stdscr, 13, 28);
	assert((cell & A_CHARTEXT) == 'c' && (cell & A_UNDERLINE));
	cell = mvwinch(stdscr, 14, 26);
	assert((cell & A_CHARTEXT) == 'o' && (cell & A_UNDERLINE));
	assert(mvwinch(stdscr, 16, 19) & A_DIM);
	state.firmware.bank[0].bootable = false;
	watch_tui_draw_operator(&state);
	cell = mvwinch(stdscr, 14, 27);
	assert((cell & A_CHARTEXT) == 'o' && (cell & A_UNDERLINE) && (cell & A_DIM));
	watch_tui_operator_key(&state, '*');
	assert(!state.firmware_visible && state.operator_visible);
	assert(resizeterm(10, 40) != ERR);
	state.operator_refresh = false;
	watch_tui_operator_key(&state, KEY_END);
	assert(state.operator_scroll == 6);
	watch_tui_draw_operator(&state);
	mvwinnstr(stdscr, 3, 3, text, 30);
	assert(strstr(text, "l Reset L1"));
	watch_tui_operator_key(&state, KEY_HOME);
	assert(!state.operator_scroll);
	endwin();
	delscreen(screen);
	fclose(input); fclose(output);
	puts("PASS menu rendering: accelerator placement/attributes, disabled legends, bank targets, scrolling and refresh keys");
}

int main(void)
{
	char fallback[PATH_MAX] = "";
	bool made_parent = false;

	/* TMPDIR is optional on developer machines and GitHub-hosted runners.
	 * Keep fallback fixtures in a named workspace directory and remove them. */
	if (!getenv("TMPDIR") || !*getenv("TMPDIR")) {
		char workspace[PATH_MAX - 160];
		const char *session = getenv("CODEX_SESSION_ID");

		if (!session || !*session)
			session = getenv("GITHUB_RUN_ID");
		assert(getcwd(workspace, sizeof(workspace)));
		if (!mkdir(".worktrees", 0700))
			made_parent = true;
		else
			assert(errno == EEXIST);
		snprintf(fallback, sizeof(fallback), "%s/.worktrees/tui-state-%.64s-XXXXXX",
			 workspace, session && *session ? session : "standalone");
		assert(mkdtemp(fallback));
		assert(!setenv("TMPDIR", fallback, 1));
		printf("TUI state fixtures: %s (removed on success)\n", fallback);
	}
	test_repeats();
	test_confirmation();
	test_text_input_cursor();
	test_second_power_down();
	test_environment_geometry();
	test_diagnostic_capture();
	test_availability_and_clock();
	test_firmware_banks();
	test_activity();
	test_save_files();
	test_save_snapshot();
	test_save_clipboard();
	test_menu_rendering();
	puts("PASS TUI state: retained repeats, hidden changes, timestamps, ring eviction and confirmation");
	if (*fallback) {
		assert(!rmdir(fallback));
		if (made_parent)
			assert(!rmdir(".worktrees"));
	}
	return 0;
}
