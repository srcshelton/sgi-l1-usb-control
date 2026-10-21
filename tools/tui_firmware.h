/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SGIL1_TUI_FIRMWARE_H
#define SGIL1_TUI_FIRMWARE_H

struct tui_firmware_bank {
	bool present, bootable;
	char status[32], revision[32], built[48];
};

struct tui_firmware {
	struct tui_firmware_bank bank[2];
	int current;
	bool coherent;
};

static bool tui_firmware_revision(const char *text)
{
	for (int component = 0; component < 3; component++) {
		if (!isdigit((unsigned char)*text)) return false;
		while (isdigit((unsigned char)*text)) text++;
		if (component < 2 && *text++ != '.') return false;
	}
	return !*text;
}

/* SGI 007-3938-003, p. 56: flash status reports the booted image and both banks.
 * Unrecognised or contradictory replies may be displayed but cannot authorise
 * booting a different bank. Never infer validity from a version number alone. */
static bool tui_firmware_parse(const char *text, struct tui_firmware *firmware)
{
	bool malformed = false, have_current = false;

	memset(firmware, 0, sizeof(*firmware));
	firmware->current = -1;
	while (text && *text) {
		size_t length = strcspn(text, "\r\n");
		char line[256], first[32], second[32], third[32], fourth[32];
		char bank = 0;
		int fields;

		if (length >= sizeof(line)) {
			malformed = true;
		} else {
			memcpy(line, text, length);
			line[length] = '\0';
			fields = sscanf(line, "%31s %31s %c %31s %31s", first, second, &bank, third, fourth);
			if (fields == 5 && streq_ci(first, "Flash") && streq_ci(second, "image") &&
			    streq_ci(third, "currently") && streq_ci(fourth, "booted")) {
				bank = (char)toupper((unsigned char)bank);
				if (have_current || (bank != 'A' && bank != 'B'))
					malformed = true;
				else {
					have_current = true;
					firmware->current = bank - 'A';
				}
			} else {
				char status[32], revision[32], built[48] = "";

				fields = sscanf(line, " %c %31s %31s %47[^\r\n]", &bank, status, revision, built);
				bank = (char)toupper((unsigned char)bank);
				if (fields >= 3 && (bank == 'A' || bank == 'B')) {
					struct tui_firmware_bank *image = &firmware->bank[bank - 'A'];
					if (image->present) malformed = true;
					image->present = true;
					snprintf(image->status, sizeof(image->status), "%s", status);
					snprintf(image->revision, sizeof(image->revision), "%s", revision);
					size_t end = strlen(built);
					while (end && isspace((unsigned char)built[end - 1])) built[--end] = '\0';
					snprintf(image->built, sizeof(image->built), "%s", built);
					image->bootable = (streq_ci(status, "valid") || streq_ci(status, "default")) &&
						tui_firmware_revision(revision);
				}
			}
		}
		text += length;
		while (*text == '\r' || *text == '\n') text++;
	}
	firmware->coherent = !malformed && have_current &&
		firmware->bank[0].present && firmware->bank[1].present;
	return firmware->coherent;
}

static bool tui_firmware_can_boot_other(const struct tui_firmware *firmware)
{
	return firmware->coherent && firmware->current >= 0 && firmware->current < 2 &&
		firmware->bank[1 - firmware->current].bootable;
}

static bool tui_firmware_same_target(const struct tui_firmware *confirmed,
				    const struct tui_firmware *fresh, int target)
{
	return target >= 0 && target < 2 && tui_firmware_can_boot_other(confirmed) &&
		tui_firmware_can_boot_other(fresh) && confirmed->current == fresh->current &&
		target == 1 - fresh->current &&
		!strcmp(confirmed->bank[target].status, fresh->bank[target].status) &&
		!strcmp(confirmed->bank[target].revision, fresh->bank[target].revision) &&
		!strcmp(confirmed->bank[target].built, fresh->bank[target].built);
}

#endif
