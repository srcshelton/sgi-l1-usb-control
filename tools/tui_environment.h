/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SGIL1_TUI_ENVIRONMENT_H
#define SGIL1_TUI_ENVIRONMENT_H
#include <math.h>

enum tui_environment_kind { TUI_ENV_UNKNOWN, TUI_ENV_VOLTAGE, TUI_ENV_FAN, TUI_ENV_TEMP };
enum tui_environment_severity { TUI_ENV_NORMAL, TUI_ENV_WARNING, TUI_ENV_FAULT };
struct tui_environment_temperature {
	size_t start, end;
	double celsius, fahrenheit;
};
struct tui_environment_row {
	char *raw, *text, *key, *signature;
	enum tui_environment_kind kind;
	enum tui_environment_severity severity, reported;
	size_t state_first, state_last, value_first, value_last;
	struct tui_environment_temperature temperature[4];
	size_t temperatures;
	bool temperature_current;
	uint64_t changed_until;
};
struct tui_environment {
	struct tui_environment_row *rows;
	size_t count, faults, warnings;
	char *text;
};

static size_t tui_environment_cells(const char *text, size_t bytes)
{
	mbstate_t conversion = { 0 };
	size_t offset = 0, cells = 0;

	while (offset < bytes) {
		wchar_t character;
		size_t length = mbrtowc(&character, text + offset, bytes - offset, &conversion);
		int width;

		if (!length || length == (size_t)-1 || length == (size_t)-2) {
			memset(&conversion, 0, sizeof(conversion));
			length = 1; width = 1;
		} else {
			width = wcwidth(character);
			if (width < 0) width = 1;
		}
		offset += length;
		cells += (size_t)width;
	}
	return cells;
}

static void tui_environment_free(struct tui_environment *view)
{
	for (size_t i = 0; i < view->count; i++) {
		free(view->rows[i].raw); free(view->rows[i].text);
		free(view->rows[i].key); free(view->rows[i].signature);
	}
	free(view->rows); free(view->text);
	memset(view, 0, sizeof(*view));
}

/* Compare sensor data, not column padding, row order or the selected units. */
static char *tui_environment_signature(const char *text)
{
	char *result = malloc(strlen(text) + 1);
	size_t used = 0;
	bool space = false;

	if (!result) return NULL;
	for (; *text; text++) {
		if (isspace((unsigned char)*text)) { space = used != 0; continue; }
		if (space) result[used++] = ' ';
		result[used++] = *text;
		space = false;
	}
	result[used] = '\0';
	return result;
}

static bool tui_environment_tail(const char *text)
{
	while (isspace((unsigned char)*text)) text++;
	return !*text;
}

/* SGI 007-3938-003, pp. 53-54: limits apply to enabled sensors; waiting and
 * disabled readings are not evidence of a fault. Keep explicit latched faults.
 * Accept only complete finite numeric fields; unknown firmware text stays text. */
static void tui_environment_parse_row(struct tui_environment_row *row,
				      enum tui_environment_kind kind)
{
	static const char *states[] = { "Wait Pwr", "Waiting", "Disabled", "Enabled",
		"Fault", "Warning", "Critical", "Advisory" };
	const char *state = NULL, *data;
	size_t length = 0;
	bool active;

	if (kind == TUI_ENV_UNKNOWN) return;
	for (const char *p = row->raw; *p && !state; p++) {
		if (p != row->raw && !isspace((unsigned char)p[-1])) continue;
		for (size_t i = 0; i < ARRAY_SIZE(states); i++) {
			size_t n = strlen(states[i]);
			if (!strncmp(p, states[i], n) && (!p[n] || isspace((unsigned char)p[n]))) {
				state = p; length = n; break;
			}
		}
	}
	if (!state || state == row->raw) return;
	bool description = false;
	for (const char *p = row->raw; p < state; p++)
		if (!isspace((unsigned char)*p)) description = true;
	if (!description) return;
	row->kind = kind;
	row->key = strndup(row->raw, (size_t)(state - row->raw));
	if (row->key) {
		char *key = tui_environment_signature(row->key);
		free(row->key); row->key = key;
	}
	row->signature = tui_environment_signature(state);
	row->state_first = tui_environment_cells(row->raw, (size_t)(state - row->raw));
	row->state_last = row->state_first + length;
	active = !strncmp(state, "Enabled", length) || !strncmp(state, "Fault", length) ||
		!strncmp(state, "Warning", length) || !strncmp(state, "Critical", length) ||
		!strncmp(state, "Advisory", length);
	if (!strncmp(state, "Fault", length) || !strncmp(state, "Critical", length))
		row->severity = TUI_ENV_FAULT;
	else if (!strncmp(state, "Warning", length) || !strncmp(state, "Advisory", length))
		row->severity = TUI_ENV_WARNING;
	row->reported = row->severity;
	data = state + length;
	if (kind == TUI_ENV_VOLTAGE) {
		double percent1, low1, high1, percent2, low2, high2, value;
		int first = 0, end = 0;

		if (sscanf(data, " %lf%% %lf/ %lf %lf%% %lf/ %lf %n%lf %n",
			&percent1, &low1, &high1, &percent2, &low2, &high2, &first, &value, &end) == 7 &&
		    end && tui_environment_tail(data + end) &&
		    isfinite(value) && isfinite(low1) && isfinite(high1) && isfinite(low2) && isfinite(high2) &&
		    low2 <= low1 && low1 <= high1 && high1 <= high2) {
			row->value_first = tui_environment_cells(row->raw, (size_t)(data - row->raw) + (size_t)first);
			row->value_last = row->value_first + strcspn(data + first, " \t\r\n");
			if (active && (value < low2 || value > high2)) row->severity = TUI_ENV_FAULT;
			else if (active && (value < low1 || value > high1) && row->severity < TUI_ENV_WARNING)
				row->severity = TUI_ENV_WARNING;
		}
	} else if (kind == TUI_ENV_FAN) {
		double limit, value;
		int first = 0, end = 0;

		if (sscanf(data, " %lf %n%lf %n", &limit, &first, &value, &end) == 2 &&
		    end && tui_environment_tail(data + end) && isfinite(limit) && isfinite(value) && limit >= 0 && value >= 0) {
			row->value_first = tui_environment_cells(row->raw, (size_t)(data - row->raw) + (size_t)first);
			row->value_last = row->value_first + strcspn(data + first, " \t\r\n");
			if (active && value < limit && row->severity < TUI_ENV_WARNING)
				row->severity = TUI_ENV_WARNING;
		}
	} else if (kind == TUI_ENV_TEMP) {
		const char *p = data;

		while (*p && row->temperatures < ARRAY_SIZE(row->temperature)) {
			char *end;
			double value = strtod(p, &end);
			char unit = (char)toupper((unsigned char)*end);
			if (end == p || !isfinite(value) || (unit != 'C' && unit != 'F')) { p++; continue; }
			struct tui_environment_temperature *temp = &row->temperature[row->temperatures++];
			temp->start = (size_t)(p - row->raw);
			temp->celsius = unit == 'C' ? value : (value - 32) * 5 / 9;
			temp->fahrenheit = unit == 'F' ? value : value * 9 / 5 + 32;
			p = end + 1;
			if (*p == '/') {
				const char *second = p + 1;
				double paired = strtod(second, &end);
				char paired_unit = (char)toupper((unsigned char)*end);
				if (end != second && isfinite(paired) && paired_unit != unit && (paired_unit == 'C' || paired_unit == 'F')) {
					if (paired_unit == 'C') temp->celsius = paired;
					else temp->fahrenheit = paired;
					p = end + 1;
				}
			}
			if (!isfinite(temp->celsius) || !isfinite(temp->fahrenheit)) {
				row->temperatures = 0;
				return;
			}
			temp->end = (size_t)(p - row->raw);
		}
		/* Known layouts: advisory/critical/fault/current, or Autofan + fault/current.
		 * A missing current reading must not turn the last limit into a reading. */
		if (row->temperatures == 4 && !tui_environment_tail(p)) row->temperatures = 0;
		row->temperature_current = (row->temperatures == 2 || row->temperatures == 4) && tui_environment_tail(p);
		if (row->temperature_current && active) {
			size_t n = row->temperatures;
			double value = row->temperature[n - 1].celsius;
			if (value > row->temperature[n - 2].celsius) row->severity = TUI_ENV_FAULT;
			else if (n == 4 && (value > row->temperature[0].celsius || value > row->temperature[1].celsius) && row->severity < TUI_ENV_WARNING)
				row->severity = TUI_ENV_WARNING;
		}
	}
}

static int tui_environment_format(struct tui_environment *view, bool fahrenheit)
{
	struct text_builder all = { 0 };
	const char *degree = MB_CUR_MAX > 1 && wcwidth(0xb0) == 1 ? "\xc2\xb0" : "";

	for (size_t i = 0; i < view->count; i++) {
		struct tui_environment_row *row = &view->rows[i];
		struct text_builder line = { 0 };
		size_t offset = 0;

		for (size_t j = 0; j < row->temperatures; j++) {
			const struct tui_environment_temperature *temp = &row->temperature[j];
			char value[64];
			size_t width = temp->end - temp->start, cells, padding;

			snprintf(value, sizeof(value), "%g%s%c", fahrenheit ? temp->fahrenheit : temp->celsius,
				degree, fahrenheit ? 'F' : 'C');
			cells = tui_environment_cells(value, strlen(value));
			padding = width > cells ? width - cells : 0;
			if (text_builder_append(&line, row->raw + offset, temp->start - offset)) goto fail_line;
			while (padding--)
				if (text_builder_append(&line, " ", 1)) goto fail_line;
			if (row->temperature_current && j == row->temperatures - 1) row->value_first = tui_environment_cells(line.buf, line.len);
			if (text_builder_append(&line, value, strlen(value))) goto fail_line;
			if (row->temperature_current && j == row->temperatures - 1) row->value_last = tui_environment_cells(line.buf, line.len);
			offset = temp->end;
		}
		if (text_builder_append(&line, row->raw + offset, strlen(row->raw + offset))) goto fail_line;
		free(row->text); row->text = line.buf;
		if (text_builder_append(&all, row->text, strlen(row->text)) || text_builder_append(&all, "\n", 1)) goto fail;
		continue;
fail_line:
		free(line.buf);
		goto fail;
	}
	free(view->text); view->text = all.buf;
	return 0;
fail:
	free(all.buf);
	return -1;
}

static int tui_environment_update(struct tui_environment *view, const char *text, bool fahrenheit, uint64_t now)
{
	struct tui_environment next = { 0 };
	enum tui_environment_kind kind = TUI_ENV_UNKNOWN;
	const char *line = text ? text : "";
	size_t capacity = 0;

	while (*line) {
		size_t length = strcspn(line, "\n");
		if (next.count == capacity) {
			capacity = capacity ? capacity * 2 : 32;
			struct tui_environment_row *rows = realloc(next.rows, capacity * sizeof(*rows));
			if (!rows) goto fail;
			next.rows = rows;
		}
		struct tui_environment_row *row = &next.rows[next.count++];
		memset(row, 0, sizeof(*row));
		if (length && line[length - 1] == '\r') length--;
		row->raw = strndup(line, length);
		if (!row->raw) goto fail;
		if (strstr(row->raw, "Description") && strstr(row->raw, "State")) {
			kind = strstr(row->raw, "Warning Limits") ? TUI_ENV_VOLTAGE :
				strstr(row->raw, "Warning RPM") ? TUI_ENV_FAN :
				strstr(row->raw, "Temp") ? TUI_ENV_TEMP : TUI_ENV_UNKNOWN;
		} else {
			tui_environment_parse_row(row, kind);
			if (row->kind) {
				if (!row->key || !row->signature) goto fail;
				if (view->count) row->changed_until = now > UINT64_MAX - 10000 ? UINT64_MAX : now + 10000;
				for (size_t j = 0; j < view->count; j++) {
					const struct tui_environment_row *old = &view->rows[j];
					if (old->kind == row->kind && old->key && !strcmp(old->key, row->key)) {
						if (!strcmp(old->signature, row->signature)) row->changed_until = old->changed_until;
						break;
					}
				}
			}
		}
		next.faults += row->severity == TUI_ENV_FAULT;
		next.warnings += row->severity == TUI_ENV_WARNING;
		line = strchr(line, '\n');
		if (!line) break;
		line++;
	}
	if (tui_environment_format(&next, fahrenheit)) goto fail;
	tui_environment_free(view);
	*view = next;
	return 0;
fail:
	tui_environment_free(&next);
	return -1;
}
#endif
