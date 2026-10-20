/* SPDX-License-Identifier: GPL-2.0-or-later */
#define main sgil1ctl_main
#include "../tools/sgil1ctl.c"
#undef main
#include <assert.h>

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

static void test_confirmation(void)
{
	struct watch_tui_state state = { .operator_visible = true };

	keys(&state, "u");
	assert(state.requested_action == WATCH_TUI_ACTION_POWER_UP);
	assert(state.selected_action == WATCH_TUI_ACTION_NONE);
	select_ready(&state, WATCH_TUI_ACTION_POWER_UP);
	keys(&state, "\n");
	assert(state.selected_action == WATCH_TUI_ACTION_NONE);
	assert(state.operator_refresh && state.operator_visible);
	select_ready(&state, WATCH_TUI_ACTION_POWER_UP);
	keys(&state, "YESno\n");
	assert(state.selected_action == WATCH_TUI_ACTION_NONE);
	assert(state.pending_action == WATCH_TUI_ACTION_NONE);
	select_ready(&state, WATCH_TUI_ACTION_POWER_UP);
	keys(&state, "YEZ\bS\n");
	assert(state.pending_action == WATCH_TUI_ACTION_POWER_UP);
	keys(&state, "dYES\n");
	assert(state.pending_action == WATCH_TUI_ACTION_POWER_UP);
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
	keys(&state, "n");
	assert(state.wait_action == WATCH_TUI_ACTION_NONE);
	state.action_busy = true;
	keys(&state, "rYES\n");
	assert(state.wait_action == WATCH_TUI_ACTION_NONE);
	keys(&state, "q");
	assert(watch_stop_requested);
	watch_stop_requested = 0;
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
	state.host_state = WATCH_TUI_HOST_OFF;
	assert(watch_tui_action_available(&state, WATCH_TUI_ACTION_POWER_UP));
	assert(!watch_tui_action_available(&state, WATCH_TUI_ACTION_POWER_DOWN));
	state.host_state = WATCH_TUI_HOST_ON;
	assert(!watch_tui_action_available(&state, WATCH_TUI_ACTION_POWER_UP));
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

int main(void)
{
	test_repeats();
	test_confirmation();
	test_availability_and_clock();
	test_activity();
	puts("PASS TUI state: retained repeats, hidden changes, timestamps, ring eviction and confirmation");
	return 0;
}
