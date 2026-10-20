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

static void test_confirmation(void)
{
	struct watch_tui_state state = { .operator_visible = true };

	keys(&state, "u\nYESno\n");
	assert(state.pending_action == WATCH_TUI_ACTION_NONE);
	keys(&state, "YEZ\bS\n");
	assert(state.pending_action == WATCH_TUI_ACTION_POWER_UP);
	keys(&state, "dYES\n"); /* No additional action while queued. */
	assert(state.pending_action == WATCH_TUI_ACTION_POWER_UP);
	state.pending_action = WATCH_TUI_ACTION_NONE;
	keys(&state, "rYES\033");
	assert(!state.operator_visible);
	assert(state.selected_action == WATCH_TUI_ACTION_NONE);
	assert(state.pending_action == WATCH_TUI_ACTION_NONE);
	state.operator_visible = true;
	state.action_busy = true;
	keys(&state, "lYES\n");
	assert(state.pending_action == WATCH_TUI_ACTION_NONE);
}

int main(void)
{
	test_repeats();
	test_confirmation();
	puts("PASS TUI state: retained repeats, hidden changes, timestamps, ring eviction and confirmation");
	return 0;
}
