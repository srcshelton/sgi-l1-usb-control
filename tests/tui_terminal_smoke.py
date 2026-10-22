#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

import fcntl
import json
import os
import pty
import re
import select
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "tools" / "sgil1ctl"
MOCK = ROOT / "tests" / "mock_l1.so"
PROMPT = "SGIL1_PROMPT>"
SHELL_MARKER = "SHELL BEFORE WATCH"
HARDSTATUS = "SCREEN-HARDSTATUS"
ROWS, COLS = 24, 120


def check_modal_footer(rendered, controls):
    lines = rendered.split("\n")
    row = next(i for i, line in enumerate(lines) if controls in line)
    ruler = lines[row - 1]
    if not any(char != " " and char * 20 in ruler for char in set(ruler)):
        raise AssertionError(f"Missing separator above {controls!r}:\n{rendered}")


def check_pane_controls(capture, send, resize):
    def wide_widths(expected):
        rendered = capture()
        if "q Quit" not in rendered:
            raise AssertionError(f"Quit missing from monitoring status bar:\n{rendered}")
        # Screen can encode ACS borders as form feeds; split only at newlines.
        title = rendered.rstrip("\n").split("\n")[1]
        if " LEDs" not in title:
            raise AssertionError(f"missing LED pane title:\n{rendered}")
        split = title.index(" LEDs") - 2
        actual = (split, len(title) - split)
        if actual != expected:
            raise AssertionError(f"pane widths {actual}, expected {expected}:\n{rendered}")
        return rendered

    def stacked_heights(expected):
        rendered = capture()
        lines = rendered.rstrip("\n").split("\n")
        if "q Quit" not in rendered:
            raise AssertionError(f"Quit missing from monitoring status bar:\n{rendered}")
        log_top = next(i for i, line in enumerate(lines) if " L1 log" in line)
        actual = (len(lines) - 1 - log_top, log_top - 1)
        if actual != expected:
            raise AssertionError(f"pane heights {actual}, expected {expected}:\n{rendered}")
        return rendered

    def help_order(compact=False):
        send(b"?")
        rendered = capture()
        lines = rendered.rstrip("\n").split("\n")
        entries = ([("Tab pane" if any("Tab pane" in line for line in lines) else "Tab select pane"), "shrink/grow pane", "t time:",
                    "p sources:", "q Quit", "h/?/Esc/Ret close"] if compact else
                   ["Select pane", "Move divider left/right", "LED timestamps",
                    "LED description sources", "Redraw screen", "Close Help"])
        rows = [next(i for i, line in enumerate(lines) if entry in line)
                for entry in entries]
        operator_row = next(i for i, line in enumerate(lines) if "Operator controls" in line)
        save_row = next(i for i, line in enumerate(lines) if "Save buffer" in line)
        env_row = next(i for i, line in enumerate(lines) if "Environment" in line)
        observation_row = next(i for i, line in enumerate(lines) if ("a view:" if compact else "View") in line)
        if env_row not in (operator_row, operator_row + 1) or env_row + 1 != rows[0] or save_row + 1 != observation_row:
            raise AssertionError(f"Incorrect action order in Help:\n{rendered}")
        if rows != sorted(rows) or rows[1] not in (rows[0], rows[0] + 1) or rows[3] != rows[2] + 1:
            raise AssertionError(f"wrong help order:\n{rendered}")
        if rows[-1] != rows[-2] + (1 if compact else 2):
            raise AssertionError(f"Quit is not immediately above Close:\n{rendered}")
        if not compact and "Quit" not in lines[rows[-1] - 1]:
            raise AssertionError(f"Quit missing from Help:\n{rendered}")
        if not any("^L redraw" in line for line in lines) and compact:
            raise AssertionError(f"Redraw missing from compact Help:\n{rendered}")
        title = next(line for line in lines if " Help " in line)
        frame = re.search(r"TPP Help P+W", title)
        if not frame or lines[rows[-1] + 1][frame.start() + 1:frame.end() - 1].strip():
            raise AssertionError(f"Help has no blank row after Close:\n{rendered}")
        send(b"\r")

    wide_widths((80, 40))
    help_order()
    for focus in ("log", "LEDs"):
        for left, right in ((b"<", b">"), (b"\033OD", b"\033OC")):
            # Every key moves the rendered divider in its own direction.
            send(left)
            wide_widths((79, 41))
            send(right)
            wide_widths((80, 40))
        if focus == "log":
            send(b"\t")
    send(b"^v\033OA\033OB")  # Vertical keys do not resize wide panes.
    wide_widths((80, 40))
    send(b"<" * 120)
    wide_widths((24, 96))
    send(b">" * 120)
    rendered = wide_widths((96, 24))
    led_text = " ".join(line[97:-1].strip() for line in rendered.rstrip("\n").split("\n")[2:-3])
    if "paneresizingpreserveseverywordofthislongLEDstatusmessage" not in "".join(led_text.split()):
        raise AssertionError(f"LED text was lost when rewrapped:\n{rendered}")
    send(b"<" * 16)
    wide_widths((80, 40))
    resize(24, 160)
    wide_widths((120, 40))
    resize(24, 120)
    wide_widths((80, 40))

    resize(24, 80)
    stacked_heights((14, 7))
    help_order(compact=True)
    send(b"^")
    stacked_heights((15, 6))
    send(b"V")
    stacked_heights((14, 7))
    send(b"\033OA")
    stacked_heights((14, 7))
    send(b"\033OB")
    stacked_heights((14, 7))
    send(b"\t^")
    stacked_heights((13, 8))
    send(b"v")
    stacked_heights((14, 7))
    send(b"\033OA")
    stacked_heights((14, 7))
    send(b"\033OB")
    stacked_heights((14, 7))
    send(b"<")
    stacked_heights((13, 8))
    send(b"\033OC")
    stacked_heights((14, 7))
    send(b"v" * 30)
    stacked_heights((18, 3))
    send(b"^" * 30)
    stacked_heights((3, 18))
    send(b"kj\002\006g")  # Scroll/page/live retain the chosen size.
    stacked_heights((3, 18))
    send(b"v" * 11)
    stacked_heights((14, 7))

    resize(11, 40)  # Ten application rows plus Screen's hardstatus.
    stacked_heights((3, 5))
    help_order(compact=True)
    send(b">")
    stacked_heights((4, 4))
    resize(9, 30)
    send(b"<>^v")
    if "Terminal must be at least" not in capture():
        raise AssertionError("undersized terminal did not show its size notice")
    resize(24, 120)
    wide_widths((80, 40))
    resize(24, 80)
    stacked_heights((17, 4))
    resize(24, 120)
    print("PASS pane controls: focus, aliases, limits, wrapping, resize and help order", flush=True)


def check_environment(capture, send, resize, command_log, environment):
    def expect(text):
        rendered = capture()
        if text not in rendered:
            raise AssertionError(f"missing {text!r}:\n{rendered}")
        return rendered

    def env_count():
        return command_log.read_text().splitlines().count("CMD env")

    send(b"?e")
    expect(" Environment ")
    expect("Environmental monitoring is enabled")
    expect("Warning Limits     Fault Limits       Current")
    rendered = capture()
    footer = next(line for line in rendered.split("\n") if "Lines " in line)
    if "Cols " in footer or "Arrows/PgUp/PgDn | r Refresh | Return/Esc Close" not in footer:
        raise AssertionError(f"Environment footer did not group keys/conditional ranges: {footer!r}")
    if not re.search(r"Lines 1-\d+ of 41", footer):
        raise AssertionError(f"Environment footer did not report a visible range: {footer!r}")
    lines = rendered.split("\n")
    title = next(line for line in lines if " Environment " in line)
    border = re.search(r"TPP Environment P+W", title)
    keys = "Arrows/PgUp/PgDn | r Refresh | Return/Esc Close"
    if not border or footer.index(keys) + len(keys) != border.end() - 2 or footer.index("Lines ") != border.start() + 2:
        raise AssertionError("Environment footer is not left/right aligned")
    ruler = lines[lines.index(footer) - 1]
    if not any(char != " " and char * 20 in ruler for char in set(ruler)):
        raise AssertionError("Environment output has no footer separator")
    if env_count() != 1:
        raise AssertionError("Environment entry did not issue one env request")
    longest = max(len(line) for line in environment.read_text().rstrip().splitlines())
    resize(24, 60)
    expect("Lines 1-11 of 41")
    expect(f"Cols 1-54 of {longest}")
    expect("r Refresh | Return/Esc Close")
    send(b"\x06")
    expect("Lines 12-22 of 41")
    send(b"\x02")
    expect("Lines 1-11 of 41")
    send(b">")
    expect(f"Cols 2-55 of {longest}")
    send(b"\033OH")
    resize(24, 120)
    send(b"\033OF")  # End reaches the final temperature rows.
    expect("Not currently available")
    send(b"\033OH")
    expect("Environmental monitoring is enabled")
    resize(80, 120)
    rendered = expect("Environmental monitoring is enabled")
    expect("Not currently available")
    if "Lines " in rendered or "Cols " in rendered:
        raise AssertionError("Environment showed position counters when all output fits")
    lines = rendered.split("\n")
    top = next(i for i, line in enumerate(lines) if " Environment " in line)
    bottom = next(i for i, line in enumerate(lines) if "r Refresh | Return/Esc Close" in line) + 1
    if top < 3 or bottom > 75 or bottom - top + 1 != len(environment.read_text().rstrip().splitlines()) + 5:
        raise AssertionError("Environment did not size to content with three-row margins")
    resize(40, 40)
    expect("Lines 1-26 of 41")
    expect(f"Cols 1-34 of {longest}")
    expect("Arrows/PgUp/PgDn")
    expect("r Refresh")
    expect("Return/Esc")
    resize(11, 40)
    send(b">" * 100)
    send(b"\033OF")
    expect("available")
    rendered = expect("r Refresh | Return/Esc")
    if not any(char != " " and char * 20 in rendered.split("\n")[5] for char in set(rendered.split("\n")[5])):
        raise AssertionError("minimum-size Environment view lost its footer ruler")
    resize(24, 120)
    send(b"\033OH")
    sample = environment.read_text()
    environment.write_text("ERROR: environmental controller unavailable\n")
    send(b"r")
    expect("env failed:")
    expect("previous reading retained")
    expect("Environmental monitoring is enabled")
    environment.write_text("SGIL1_MOCK_TIMEOUT\n")
    send(b"r")
    expect("response timed out")
    footer = capture().split("\n")[-2]
    if "Transport:" not in footer or "Broken pipe" not in footer:
        raise AssertionError(f"pipe-reset diagnostic disrupted the TUI: {footer!r}")
    environment.write_text(sample.replace("0.188", "0.199"))
    send(b"r")
    expect("0.199")
    if env_count() != 4:
        raise AssertionError("Environment refresh issued duplicate requests")
    commands = command_log.read_text().splitlines()
    if commands[-1] not in ("CMD log", "CMD leds", "CMD date", "CMD date tz"):
        raise AssertionError("monitoring did not continue while Environment was open")
    send(b"\r")
    if " Environment " in capture():
        raise AssertionError("Return did not close Environment")
    send(b"e")
    if env_count() != 5:
        raise AssertionError("reopening Environment did not refresh")
    send(b"\033")
    deadline = time.monotonic() + 6
    while "Transport:" in capture().split("\n")[-2] and time.monotonic() < deadline:
        send(b"")
    if "Console last active:" not in capture().split("\n")[-2]:
        raise AssertionError("pipe-reset status did not expire")
    print("PASS Environment: full table, scrolling, resize, refresh, retained errors and continued monitoring", flush=True)


def check_operator_controls(capture, send, resize, command_log, power_file):
    actions = {"CMD power up", "CMD power down", "CMD softreset", "CMD reset", "CMD reboot_l1 current"}

    def commands():
        return command_log.read_text().splitlines()

    def sent():
        return [line for line in commands() if line in actions]

    def expect(text):
        rendered = capture()
        if text not in rendered:
            raise AssertionError(f"missing {text!r}:\n{rendered}")
        return rendered

    def select(key):
        send(key)
        if key == b"l":
            expect("L1 firmware / reset")
            send(b"c")
        expect("Type YES, then Return to send once")
        check_modal_footer(expect("Blank input + Return aborts action"), "Esc/* Back")

    send(b"\f")
    rendered = capture()
    if not re.search(r"\b(?:[2-9]|[1-9][0-9]+) samples\b", rendered):
        raise AssertionError(f"no rendered repeat count:\n{rendered}")
    send(b"t")
    expect("samples; last")
    send(b"t")
    send(b"udrl")  # Power shortcuts remain inactive outside Operator controls.
    if sent():
        raise AssertionError("operator command was available outside the menu")
    send(b"?")
    help_title = next(line for line in capture().splitlines() if " Help " in line)
    send(b"*")
    menu = expect("Host: off")
    check_modal_footer(menu, "r Refresh | Esc/* Back")
    operator_title = next(line for line in menu.splitlines() if " Operator controls " in line)
    if not re.search(r"TPP Help P+W", help_title) or not re.search(r"TPP Operator controls P+W", operator_title):
        raise AssertionError(f"Help/Operator border mismatch: {help_title!r} / {operator_title!r}")
    if "q quit monitoring" in menu or "q stops monitoring" in menu:
        raise AssertionError("ambiguous quit instruction remains in Operator controls")
    expect("*d Power down")
    expect("*e Reset/restart host")
    expect("Unavailable due to current host state")
    rows = menu.splitlines()
    legend = next(i for i, row in enumerate(rows) if "Unavailable due to current host state" in row)
    controls = next(i for i, row in enumerate(rows) if "r Refresh | Esc/* Back" in row)
    if controls != legend + 2:
        raise AssertionError(f"Operator menu has an extra gap above its ruler:\n{menu}")
    if next(i for i, row in enumerate(rows) if "l Reset L1" in row) != next(i for i, row in enumerate(rows) if "e Reset/restart host" in row) + 2:
        raise AssertionError("missing blank row between host and L1 reset")
    if "L1: " not in menu or "L1 behind by ~" not in menu:
        raise AssertionError(f"operator clock information missing:\n{menu}")
    footer = next(line for line in menu.splitlines() if "Console last active:" in line)
    if "[out of sync]" not in footer or footer.index("L1 ") > footer.index("Console last active:"):
        raise AssertionError(f"status clock missing or misplaced: {footer}")
    resize(17, 80)  # Sixteen application rows still need the compact footer.
    check_modal_footer(expect("l Reset L1"), "r Refresh | Esc/* Back")
    resize(11, 40)
    send(b"\033OF")  # End reveals the lower menu rows at minimum height.
    check_modal_footer(expect("l Reset L1"), "r Refresh | Esc/* Back")
    send(b"\033")  # The minimum-height modal covers the status row.
    expect("[out of sync]")
    resize(24, 120)
    send(b"*")
    expect("Host: off")
    checks = commands().count("CMD power check")
    send(b"d")
    expect("Action unavailable: host is off")
    if "Confirm:" in capture() or commands().count("CMD power check") <= checks:
        raise AssertionError("unavailable action was not rechecked/blocked")
    send(b"U")  # A verified off host powers on without typing YES.
    expect("power up: command acknowledged")
    if sent() != ["CMD power up"]:
        raise AssertionError(f"wrong power-up commands: {sent()}")
    after_up = commands()[commands().index("CMD power up") + 1:]
    if "CMD leds" not in after_up or after_up[0] != "CMD leds":
        raise AssertionError(f"power-state confirmation delayed boot LEDs: {after_up}")
    send(b"*")
    expect("Host: on")
    expect("*u Power up")
    before_refresh = sent()
    checks = commands().count("CMD power check")
    send(b"r")
    expect("Host: on")
    if sent() != before_refresh or commands().count("CMD power check") <= checks or "Confirm:" in capture():
        raise AssertionError("Refresh did not recheck status without requesting an action")
    select(b"e")
    send(b"NO\r")
    expect("Host: on")
    if "Confirm:" in capture() or sent() != ["CMD power up"]:
        raise AssertionError("invalid reset confirmation did not abort")
    select(b"e")
    send(b"\r")
    if "Confirm:" in capture() or sent() != ["CMD power up"]:
        raise AssertionError("blank reset confirmation did not abort")
    select(b"e")
    power_file.write_text("off")
    send(b"YES\r")
    if sent() != ["CMD power up"]:
        raise AssertionError("host-state change during reset confirmation was ignored")
    power_file.write_text("on")
    send(b"\033*")
    select(b"d")
    send(b"YES\r")
    expect("Second power-down signal")
    if sent().count("CMD power down") != 1:
        raise AssertionError("second signal was automatically sent")
    check_modal_footer(expect("Send 'pwr d' a second time?"), "y Yes | n No | Return/Esc Cancel")
    send(b"y")
    expect("power down: command acknowledged")
    if sent().count("CMD power down") != 2:
        raise AssertionError("second signal was not sent exactly once")
    send(b"*")
    expect("Host: off")
    send(b"e")
    expect("Action unavailable: host is off")
    power_file.write_text("unknown")
    send(b"\033*")
    expect("Host: unknown")
    expect("*u Power up")
    expect("*d Power down")
    # Controller reboot remains available with an unknown or off host.
    select(b"l")
    send(b"YES\r")
    expect("reboot_l1 current: command acknowledged")
    power_file.write_text("off")
    send(b"*")
    expect("Host: off")
    select(b"l")
    send(b"YES\r")
    expect("reboot_l1 current: command acknowledged")
    if power_file.read_text() != "off":
        raise AssertionError("controller reboot changed host power state")
    power_file.write_text("on")
    send(b"*")
    select(b"e")
    send(b"YES\r")
    expect("softreset: command acknowledged")
    send(b"*")
    select(b"l")
    send(b"YES\r")
    expect("reboot_l1 current: command acknowledged")
    send(b"a")
    expect("reboot_l1 current: command acknowledged")
    send(b"a*")
    select(b"s")
    send(b"YES\r")
    expect("Host: on")
    if not any(line.startswith("CMD date tz ") for line in commands()) or not any(re.match(r"CMD date [0-9]", line) for line in commands()):
        raise AssertionError("clock sync did not send timezone and date commands")
    if "[out of sync]" in next(line for line in capture().splitlines() if "Console last active:" in line):
        raise AssertionError("clock drift warning remained after clock sync")
    send(b"\033")
    resize(11, 40)
    send(b"*")
    expect("*u Power up")
    send(b"\033OF")
    select(b"l")
    send(b"*")
    send(b"\033OF")
    expect("c Reboot current")
    send(b"*")
    expect("Host: on")
    send(b"*")
    if " Operator controls " in capture() or " L1 firmware / reset " in capture():
        raise AssertionError("back navigation did not close Operator controls")
    if sent() != ["CMD power up", "CMD power down", "CMD power down", "CMD reboot_l1 current", "CMD reboot_l1 current", "CMD softreset", "CMD reboot_l1 current"]:
        raise AssertionError(f"operator actions sent wrong commands: {sent()}")
    resize(24, 120)
    print("PASS operator controls: shared styling, host-state gating/rechecks, abort, clock sync and early LEDs", flush=True)


def check_firmware_banks(capture, send, command_log, power_file, flash_file):
    normal = flash_file.read_text()

    def expect(text):
        rendered = capture()
        if text not in rendered:
            raise AssertionError(f"Missing firmware text {text!r}:\n{rendered}")
        return rendered

    def reboots():
        return [line for line in command_log.read_text().splitlines() if line.startswith("CMD reboot_l1 ")]

    def open_menu():
        send(b"*")
        send(b"l")
        expect("L1 firmware / reset")

    power_file.write_text("off")
    open_menu()
    expect("Running bank: B")
    expect("A: 1.48.1 [valid]")
    expect("B: 1.24.11 [default] running")
    expect("01/22/2007 11:34:20")
    expect("10/29/2003 00:05:26")
    rendered = capture()
    lines = rendered.split("\n")
    expected = ["Running bank: B", "A: 1.48.1", "B: 1.24.11", "c Reboot current (Bank B)",
                "o Boot other (Bank A)", "* Other bank unknown/not bootable"]
    positions = [next(i for i, line in enumerate(lines) if value in line) for value in expected]
    if [b - a for a, b in zip(positions, positions[1:])] != [2, 3, 3, 1, 2]:
        raise AssertionError(f"Firmware sections have incorrect spacing:\n{rendered}")
    check_modal_footer(rendered, "r Refresh | Esc/* Back")
    footer_row = next(i for i, line in enumerate(lines) if "r Refresh | Esc/* Back" in line)
    if footer_row != positions[-1] + 2 or "unavailable;" in lines[footer_row]:
        raise AssertionError(f"Firmware footer has a redundant gap or legend:\n{rendered}")
    before = reboots()
    send(b"o")
    expect("Boot L1 bank A")
    expect("Bank A: 1.48.1 [valid]")
    send(b"\r")
    expect("L1 firmware / reset")
    if reboots() != before:
        raise AssertionError("blank bank-switch confirmation sent a reboot")
    flash_file.write_text(normal.replace("A valid", "A invalid"))
    send(b"r")
    expect("*o Boot other")
    send(b"o")
    if "Confirm:" in capture() or reboots() != before:
        raise AssertionError("invalid alternate bank could be booted")
    flash_file.write_text("ERROR: flash status unavailable\n")
    send(b"r")
    expect("Flash status unavailable")
    expect("*o Boot other")
    send(b"o")
    if "Confirm:" in capture() or reboots() != before:
        raise AssertionError("missing firmware status could authorise another bank")
    flash_file.write_text(normal)
    send(b"r")
    send(b"o")
    expect("Bank A: 1.48.1 [valid]")
    flash_file.write_text(normal.replace("1.48.1", "1.48.2"))
    send(b"YES\r")
    expect("Firmware changed/unavailable; confirm again")
    if reboots() != before:
        raise AssertionError("a changed bank was booted using stale confirmation")
    flash_file.write_text(normal)
    send(b"r")
    send(b"o")
    expect("Boot L1 bank A")
    send(b"YES\r")
    expect("reboot_l1 a: command acknowledged")
    if reboots() != before + ["CMD reboot_l1 a"] or power_file.read_text() != "off":
        raise AssertionError("alternate bank reboot was repeated or changed host power")
    open_menu()
    expect("Running bank: A")
    expect("A: 1.48.1 [valid] running")
    expect("c Reboot current (Bank A)")
    expect("o Boot other (Bank B)")
    send(b"o")
    expect("Boot L1 bank B")
    expect("Bank B: 1.24.11 [default]")
    send(b"YES\r")
    expect("reboot_l1 b: command acknowledged")
    if reboots() != before + ["CMD reboot_l1 a", "CMD reboot_l1 b"]:
        raise AssertionError("other-bank reboot targeted the wrong bank")
    if any(line == "CMD reset" for line in command_log.read_text().splitlines()):
        raise AssertionError("a controller reboot sent the host reset command")
    print("PASS L1 firmware: versions/builds, current/other bank, invalid/missing status, stale confirmation and one-shot boot", flush=True)


def check_save_buffer(capture, send, resize, directory, helper, clipboard):
    exports = directory / "exports"
    exports.mkdir()

    def expect(text):
        rendered = capture()
        if text not in rendered:
            raise AssertionError(f"missing {text!r}:\n{rendered}")
        controls = next((value for value in ("Return Save", "Return Close", "Return Edit path", "o Overwrite")
                         if value in rendered), None)
        if controls:
            check_modal_footer(rendered, controls)
        return rendered

    def begin(leds=False):
        send(b"s")
        rendered = expect("Save buffer")
        if ("Full retained LED" in rendered) != leds:
            send(b"\033\ts")
            rendered = expect("Save buffer")
        if ("Full retained LED" in rendered) != leds:
            raise AssertionError("Save captured the wrong pane")

    def submit(path):
        send(str(path).encode() + b"\r")

    path = exports / "log 'quotes' ; $(echo no-shell) λ.txt"
    begin()
    send(b"cursor-check")
    send(b"\033OD", cursor_back=1)
    send(b"\x15")
    submit(path)
    expect("Buffer saved")
    expect("c Copy path")
    if "Rows " in capture() or "Up/Down scroll" in capture():
        raise AssertionError("Buffer saved still offers scrolling or position counters")
    original = path.read_text()
    if "voltage nominal" not in original or "previous message repeated" in original:
        raise AssertionError(f"Save omitted raw/folded log contents: {original}")
    if "Power Down: issue 'pwr d' again to immediately power down." not in original:
        raise AssertionError("second-signal request was lost from the log buffer")
    if "[transport] reset-pipes after L1 response timeout failed: Broken pipe" not in original:
        raise AssertionError("pipe-reset diagnostic was lost from the log buffer")
    send(b"c")
    expect("File path copied to clipboard")
    if clipboard.read_text() != str(path):
        raise AssertionError("clipboard did not receive the exact literal absolute path")
    helper.write_text("#!/bin/sh\necho 'desktop access denied' >&2\nexit 3\n")
    helper.chmod(0o700)
    send(b"c")
    expect("Clipboard failed: desktop access denied")
    if "Buffer saved" not in capture() or path.read_text() != original:
        raise AssertionError("clipboard failure affected the saved file/completion view")
    send(b"\033")
    expect("Buffer saved")  # Successful saves require Return acknowledgement.
    send(b"\r")
    begin()
    submit(path)
    expect("File has content")
    send(b"\r")
    if path.read_text() != original or "Buffer saved" in capture():
        raise AssertionError("Return did not cancel a conflicting save")
    begin()
    submit(path)
    expect("File has content")
    send(b"a")
    expect("Buffer saved")
    if path.read_text() != original + original:
        raise AssertionError("append did not preserve all existing and new data")
    send(b"\r")
    begin()
    submit(path)
    send(b"o")
    expect("Buffer saved")
    if path.read_text() != original:
        raise AssertionError("overwrite did not replace the file with the full snapshot")
    send(b"\r")
    begin()
    submit(exports / "missing" / "file.txt")
    expect("Save failed")
    expect("Cannot access destination directory")
    send(b"\r\x15")
    submit(exports / "retry.txt")
    expect("Buffer saved")
    send(b"\r")
    victim = exports / "victim.txt"
    victim.write_text("keep this file")
    link = exports / "linked.txt"
    link.symlink_to(victim)
    begin()
    submit(link)
    expect("Save failed")
    if victim.read_text() != "keep this file":
        raise AssertionError("symlink target was changed")
    send(b"\033")
    begin(leds=True)
    led_path = exports / "leds.txt"
    submit(led_path)
    expect("Buffer saved")
    led_text = led_path.read_text()
    if led_text.count("0x09") < 2 or not re.search(r"\d{2}/\d{2}/\d{2} \d{2}:\d{2}:\d{2}\.\d{3}", led_text):
        raise AssertionError("full LED export omitted repeats or timestamps")
    send(b"\r")
    # Completion paths longer than the view remain scrollable, including at 40x10.
    long_dir = exports
    for _ in range(16):
        long_dir = long_dir / ("long-directory-" + "x" * 64)
    long_dir.mkdir(parents=True)
    long_path = long_dir / "end-of-long-path.txt"
    begin(leds=True)
    submit(long_path)
    expect("Buffer saved")
    resize(11, 40)
    send(b"\033OB" * 100)
    rendered = capture()
    # The basename can wrap at different columns as the runner path changes.
    visible_path = "".join(row[3:37].rstrip() for row in rendered.split("\n")[1:8])
    if "end-of-long-path.txt" not in visible_path:
        raise AssertionError(f"last page omitted the full filename: {visible_path!r}\n{rendered}")
    send(b"\033OA")
    expect("Buffer saved")
    send(b"\r")
    resize(24, 120)
    if not long_path.exists():
        raise AssertionError("long save path was not written")
    # Relative names are resolved under the explicitly supplied TMPDIR.
    begin()
    send(b"relative-save.txt\r")
    expect("Buffer saved")
    if not (directory / "relative-save.txt").exists():
        raise AssertionError("relative path did not use TMPDIR")
    send(b"\r")
    print("PASS Save buffer: full snapshots, paths, conflict choices, errors, clipboard and long-path scrolling", flush=True)


def check_operator_timeout(capture, send, command_log):
    send(b"*")
    send(b"u")
    rendered = capture()
    if "outcome unknown" not in rendered:
        raise AssertionError(f"timeout was not reported as uncertain:\n{rendered}")
    if "Transport:" not in rendered.split("\n")[-2] or "timed out waiting" not in rendered.split("\n")[-2]:
        raise AssertionError(f"transport diagnostic was not confined to the status bar:\n{rendered}")
    if "[transport] timed out waiting for" not in rendered:
        raise AssertionError("timeout diagnostic was not retained in the log buffer")
    send(b"\r\r")
    commands = command_log.read_text().splitlines()
    if commands.count("CMD power up") != 1:
        raise AssertionError("uncertain power action was retried")
    if "Operator controls" in capture():
        raise AssertionError("operator menu remained open after timeout")
    if commands[-1] not in ("CMD log", "CMD leds", "CMD version", "CMD date", "CMD date tz"):
        raise AssertionError("monitoring did not resume after timeout")
    deadline = time.monotonic() + 6
    while "Transport:" in capture().split("\n")[-2] and time.monotonic() < deadline:
        send(b"")
    if "Console last active:" not in capture().split("\n")[-2]:
        raise AssertionError("temporary transport status did not expire")
    print("PASS operator timeout: captured diagnostics, temporary status, unknown outcome and no retry", flush=True)


def check_usb_wait(capture, send, presence, command_log, mode):
    def expect(text):
        rendered = capture()
        if text not in rendered:
            raise AssertionError(f"missing {text!r}:\n{rendered}")
        return rendered

    def commands():
        return command_log.read_text().splitlines() if command_log.exists() else []

    send(b"\033*\r")
    expect("Waiting for USB")
    if any(line.startswith("CMD ") or line.startswith("LOCK ") for line in commands()):
        raise AssertionError("missing device was probed or locked")
    if mode == "quit":
        return
    if mode in ("up", "reset", "down", "unconfirmed"):
        send({"up": b"u", "reset": b"e", "down": b"d", "unconfirmed": b"u"}[mode])
        expect("Blank input + Return aborts action")
        if mode != "unconfirmed":
            send(b"YES\r")
            expect({"up": "u [x]", "reset": "e [x]", "down": "d [x]"}[mode])
        if mode == "up":
            send(b"t")
            expect("t [x]")
    elif mode in ("cli", "skip"):
        expect("u [x]")
    presence.touch()
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        send(b"")
        rendered = capture()
        if "Waiting for USB" not in rendered and "Confirm:" not in rendered:
            break
    else:
        raise AssertionError(f"wait did not return to monitoring:\n{rendered}")
    expected = {"up": "CMD power up", "cli": "CMD power up", "reset": "CMD softreset",
                "down": "CMD power down"}.get(mode)
    actions = [line for line in commands() if line in
               {"CMD power up", "CMD power down", "CMD softreset", "CMD reset", "CMD reboot_l1 current"}]
    if actions != ([expected] if expected else []):
        raise AssertionError(f"wrong reconnect actions: {actions}, mode {mode}")
    if mode in ("up", "cli"):
        trace = commands()
        position = trace.index("CMD power up")
        if trace[position + 1] != "CMD leds":
            raise AssertionError(f"power-state wait delayed LEDs: {trace[position + 1:]}")
        if not any(re.match(r"CMD date [0-9]", line) for line in trace[:position]):
            raise AssertionError("armed clock sync did not run before power up")
    if mode == "up":
        presence.unlink()
        send(b"")
        expect("Waiting for USB")
        expect("u [ ]")
        expect("t [ ]")
        presence.touch()
        send(b"")
        send(b"")
        if commands().count("CMD power up") != 1:
            raise AssertionError("reconnect action was rearmed on a later disconnect")
    if "Console last active: never" in capture():
        raise AssertionError("received log/LED output did not update activity")
    print(f"PASS mandatory USB wait: {mode}", flush=True)


def run_case(screen, temporary, altscreen, primary, stop, operator_timeout=False, wait_mode=None):
    name = f"altscreen-{altscreen}-{'primary' if primary else 'default'}-{stop}"
    if operator_timeout:
        name += "-operator-timeout"
    if wait_mode:
        name += f"-wait-{wait_mode}"
    directory = temporary / name
    directory.mkdir()
    session = f"sgil1ctl-{os.getpid()}-{name}"
    runner = directory / "runner.py"
    hardcopy = directory / "screen.txt"
    screenrc = directory / "screenrc"
    child_state = directory / "child.json"
    result = directory / "result.txt"
    exercise_controls = altscreen == "off" and not primary and stop == "q" and not operator_timeout and not wait_mode
    command_log = directory / "commands.log"
    power_file = directory / "power.txt"
    clipboard = directory / "clipboard.txt"
    environment = directory / "environment.txt"
    flash_file = directory / "flash.txt"
    helpers = directory / "helpers"
    helper = helpers / "pbcopy"
    if exercise_controls:
        flash_file.write_text("Flash image B currently booted\nImage Status Revision Built\n"
                              "A valid 1.48.1 01/22/2007 11:34:20\n"
                              "B default 1.24.11 10/29/2003 00:05:26\n")
        helpers.mkdir()
        environment.write_text((ROOT / "tests" / "fixtures" / "env.txt").read_text() + "\n \n\t\n")
        helper.write_text("#!/bin/sh\n/bin/cat > \"$SGIL1_TEST_CLIPBOARD\"\n")
        helper.chmod(0o700)
    power_file.write_text("off" if exercise_controls or operator_timeout or wait_mode in ("up", "cli", "unconfirmed") else "on")
    presence = directory / "usb-present"
    extra_env = ({"SGIL1_MOCK_LEDS_RESPONSE": "CPU  A: 0x09: pane resizing preserves every "
                  "word of this long LED status message\n"} if exercise_controls else {})
    extra_env.update(SGIL1_MOCK_LOG=str(command_log), SGIL1_MOCK_POWER_DOWN_CONFIRM="1",
                     SGIL1_MOCK_POWER_FILE=str(power_file))
    if exercise_controls:
        extra_env["SGIL1_MOCK_POWER_DOWN_LOG_CONFIRM"] = "1"
        extra_env["SGIL1_MOCK_RESET_PIPES_EPIPE"] = "1"
    if exercise_controls:
        extra_env.update(PATH=str(helpers) + ":" + os.environ.get("PATH", "/usr/bin:/bin"),
                         SGIL1_TEST_CLIPBOARD=str(clipboard), TMPDIR=str(directory),
                         SGIL1_MOCK_ENV_FILE=str(environment), SGIL1_MOCK_FLASH_FILE=str(flash_file))
    if operator_timeout:
        extra_env["SGIL1_MOCK_POWER_UP_TIMEOUT"] = "1"
        extra_env["SGIL1_MOCK_RESET_PIPES_EPIPE"] = "1"
    if wait_mode:
        extra_env["SGIL1_MOCK_PRESENT_FILE"] = str(presence)
        extra_env.pop("SGIL1_MOCK_POWER_DOWN_CONFIRM", None)
    command = [
        str(BIN), "--device", "/dev/sgi-l1/l1-0",
        "--status-device", "/dev/sgi-l1/status", "--timeout", "10",
        "watch", "--tui", "--log-interval", "100", "--led-interval", "100",
        "--log-history", "64", "--led-history", "32",
    ]
    if wait_mode in ("cli", "skip"):
        command = command[:command.index("watch")] + ["wait", "--tui", "--power-up", "--force"]
        if wait_mode == "cli":
            command.append("--set-time")
    if primary:
        command.append("--no-alternate-screen")
    screenrc.write_text(
        "startup_message off\n"
        "term screen-256color\n"
        f"altscreen {altscreen}\n"
        f'hardstatus alwayslastline "{HARDSTATUS}"\n'
    )
    runner.write_text(
        "import json, os, subprocess, sys, time\n"
        "from pathlib import Path\n"
        "size = os.get_terminal_size()\n"
        f"sys.stdout.write('\\033[2J\\033[H{SHELL_MARKER}\\n' "
        "+ f'\\033[{size.lines};1H')\n"
        "sys.stdout.flush()\n"
        "env = os.environ.copy()\n"
        f"env.update(LD_PRELOAD={str(MOCK)!r}, SGIL1_MOCK='1', "
        "SGIL1_MOCK_WATCH='1', SGIL1_MOCK_WATCH_ACTIVITY_ONLY='1')\n"
        f"env.update({extra_env!r})\n"
        f"proc = subprocess.Popen({command!r}, env=env)\n"
        f"Path({str(child_state)!r}).write_text(json.dumps(dict("
        "pid=proc.pid, term=os.environ['TERM'], rows=size.lines)))\n"
        "status = proc.wait()\n"
        f"sys.stdout.write('{PROMPT} ')\n"
        "sys.stdout.flush()\n"
        f"Path({str(result)!r}).write_text(str(status))\n"
        "time.sleep(30)\n"
    )

    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ,
                struct.pack("HHHH", ROWS, COLS, 0, 0))

    def controlling_terminal():
        os.setsid()
        fcntl.ioctl(0, termios.TIOCSCTTY, 0)

    env = os.environ.copy()
    env.update(TERM="xterm-256color", LC_ALL="C.UTF-8")
    proc = subprocess.Popen(
        [screen, "-S", session, "-c", str(screenrc), sys.executable, str(runner)],
        stdin=slave, stdout=slave, stderr=slave, env=env,
        preexec_fn=controlling_terminal,
    )
    os.close(slave)
    terminal_output = bytearray()

    def drain(timeout=0.1):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            ready, _, _ = select.select([master], [], [], 0.02)
            if ready:
                try:
                    data = os.read(master, 65536)
                except OSError:
                    break
                if not data:
                    break
                terminal_output.extend(data)

    def screen_command(*args, check=True):
        return subprocess.run(
            [screen, "-S", session, *args], check=check,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5,
        )

    def capture():
        hardcopy.unlink(missing_ok=True)
        screen_command("-p", "0", "-X", "hardcopy", str(hardcopy))
        deadline = time.monotonic() + 2
        # Screen can create the hardcopy before it has written its contents.
        while (not hardcopy.exists() or not hardcopy.stat().st_size) and time.monotonic() < deadline:
            drain(0.02)
        return hardcopy.read_text(errors="replace")

    try:
        deadline = time.monotonic() + 10
        rendered = ""
        while time.monotonic() < deadline:
            drain()
            if child_state.exists():
                rendered = capture()
                if ("Waiting for USB" in rendered if wait_mode else
                    "Fuel" in rendered and "Console last active:" in rendered):
                    break
            if proc.poll() is not None:
                raise AssertionError(f"Screen exited during {name}: {terminal_output!r}")
        else:
            raise AssertionError(f"TUI did not become ready in {name}:\n{rendered}")

        child = json.loads(child_state.read_text())
        if child["term"] != "screen-256color" or child["rows"] != ROWS - 1:
            raise AssertionError(f"wrong terminal/hardstatus layout: {child}")
        if HARDSTATUS.encode() not in terminal_output:
            raise AssertionError("attached Screen did not render its hardstatus")
        if exercise_controls or operator_timeout or wait_mode:
            def send(keys, cursor_back=0):
                os.write(master, keys)
                drain(0.4 if len(keys) < 30 else 1.5)
                rendered = capture()
                lines = rendered.split("\n")
                prompt = next(((i, line) for i, line in enumerate(lines) if "Confirm: " in line), None)
                if prompt:
                    y, line = prompt
                    x = line.index("Confirm: ") + len("Confirm: ")
                    while x < len(line) and line[x] in "YESNO":
                        x += 1
                elif "File path [" in rendered:
                    y = next(i for i, line in enumerate(lines) if "File path [" in line) + 1
                    frame = re.search(r"TPP Save buffer P+W", next(line for line in lines if " Save buffer " in line))
                    if not frame:
                        raise AssertionError(f"Missing Save frame:\n{rendered}")
                    field_start = frame.start() + 2
                    field = lines[y][field_start:frame.end() - 2].rstrip()
                    x = field_start + len(field) - cursor_back
                else:
                    y = None
                if y is not None and keys:
                    info = screen_command("-Q", "info").stdout
                    position = re.search(rb"\((\d+),(\d+)\)/", info)
                    if not position or int(position[2]) != y + 1 or (x is not None and int(position[1]) != x + 1):
                        raise AssertionError(f"Input cursor is outside its field: {info!r}\n{rendered}")
                    modes = re.findall(rb"\x1b\[\?25([hl])", terminal_output)
                    if not modes or modes[-1] != b"h":
                        raise AssertionError("GNU Screen left the input cursor hidden")

            def resize(rows, cols):
                fcntl.ioctl(master, termios.TIOCSWINSZ,
                            struct.pack("HHHH", rows, cols, 0, 0))
                os.kill(proc.pid, signal.SIGWINCH)
                drain(0.4)

            if wait_mode:
                check_usb_wait(capture, send, presence, command_log, wait_mode)
            elif operator_timeout:
                check_operator_timeout(capture, send, command_log)
            else:
                check_environment(capture, send, resize, command_log, environment)
                check_operator_controls(capture, send, resize, command_log, power_file)
                check_firmware_banks(capture, send, command_log, power_file, flash_file)
                check_save_buffer(capture, send, resize, directory, helper, clipboard)
                # Restore the fixture's original focus before navigation checks.
                send(b"s")
                if "Full retained LED" in capture():
                    send(b"\033\t")
                else:
                    send(b"\033")
                check_pane_controls(capture, send, resize)
        if stop == "q":
            os.write(master, b"q")
        else:
            os.kill(child["pid"], getattr(signal, stop))

        deadline = time.monotonic() + 3
        while not result.exists() and time.monotonic() < deadline:
            drain()
        if not result.exists() or result.read_text() != "0":
            raise AssertionError(f"TUI did not exit successfully in {name}")
        drain()
        rendered = capture()
        prompt_lines = [line for line in rendered.splitlines()
                        if line.startswith(PROMPT)]
        if not prompt_lines or prompt_lines[-1].strip() != PROMPT:
            raise AssertionError(f"{name}: stale content on prompt row:\n{rendered}")
        if "Console last active:" in rendered or "log selected | h/? Help" in rendered:
            raise AssertionError(f"{name}: footer survived exit:\n{rendered}")
        restores_shell = altscreen == "on" and not primary
        if restores_shell:
            if SHELL_MARKER not in rendered or "L1 log" in rendered:
                raise AssertionError(f"{name}: shell screen was not restored:\n{rendered}")
        elif "L1 log" not in rendered or "LEDs" not in rendered:
            raise AssertionError(f"{name}: primary TUI history was erased:\n{rendered}")
        print(f"PASS {name}: clean prompt; hardstatus reserved its own row", flush=True)
    finally:
        screen_command("-X", "quit", check=False)
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=3)
        os.close(master)


def main():
    screen = shutil.which("screen")
    if not screen:
        raise AssertionError("GNU Screen is required for this TUI smoke test")
    with tempfile.TemporaryDirectory(prefix="sgil1ctl-screen-") as temporary:
        for altscreen in ("off", "on"):
            for primary in (False, True):
                for stop in ("q", "SIGINT", "SIGTERM"):
                    run_case(screen, Path(temporary), altscreen, primary, stop)
        run_case(screen, Path(temporary), "off", False, "q", operator_timeout=True)
        for mode in ("up", "cli", "skip", "reset", "down", "unconfirmed", "quit"):
            run_case(screen, Path(temporary), "off", False, "q", wait_mode=mode)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, OSError, subprocess.SubprocessError) as error:
        print(f"TUI terminal smoke test failed: {error}", file=sys.stderr)
        sys.exit(1)
