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


def check_pane_controls(capture, send, resize):
    def wide_widths(expected):
        rendered = capture()
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
        log_top = next(i for i, line in enumerate(lines) if " L1 log" in line)
        actual = (len(lines) - 1 - log_top, log_top - 1)
        if actual != expected:
            raise AssertionError(f"pane heights {actual}, expected {expected}:\n{rendered}")
        return rendered

    def help_order(compact=False):
        send(b"?")
        rendered = capture()
        lines = rendered.rstrip("\n").split("\n")
        entries = (["Tab select pane", "shrink/grow pane", "t time:",
                    "p sources:", "^L redraw", "h/?/Esc/Ret close"] if compact else
                   ["Select pane", "Move divider left/right", "LED timestamps",
                    "LED description sources", "Redraw screen", "Close Help"])
        rows = [next(i for i, line in enumerate(lines) if entry in line)
                for entry in entries]
        operator_row = next(i for i, line in enumerate(lines) if "Operator controls" in line)
        if operator_row + 1 != rows[0]:
            raise AssertionError(f"Operator controls is not first in Help:\n{rendered}")
        if rows != sorted(rows) or rows[1] != rows[0] + 1 or rows[3] != rows[2] + 1:
            raise AssertionError(f"wrong help order:\n{rendered}")
        if rows[-1] != rows[-2] + 1:
            raise AssertionError(f"Redraw and Close are not adjacent:\n{rendered}")
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


def check_operator_controls(capture, send, resize, command_log, power_file):
    actions = {"CMD power up", "CMD power down", "CMD softreset", "CMD reset"}

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
        expect("Type YES, then Return to send once")
        expect("Blank input + Return aborts action")

    send(b"\f")
    rendered = capture()
    if not re.search(r"\b(?:[2-9]|[1-9][0-9]+) samples\b", rendered):
        raise AssertionError(f"no rendered repeat count:\n{rendered}")
    send(b"t")
    expect("samples; last")
    send(b"t")
    send(b"uYES\r")
    if sent():
        raise AssertionError("operator command was available outside the menu")
    send(b"?")
    help_title = next(line[17:103] for line in capture().splitlines() if " Help " in line)
    send(b"*")
    menu = expect("Host: off")
    operator_title = next(line[17:103] for line in menu.splitlines() if " Operator controls " in line)
    if help_title[:3] != operator_title[:3] or help_title[-3:] != operator_title[-3:]:
        raise AssertionError(f"Help/Operator border mismatch: {help_title!r} / {operator_title!r}")
    if "q quit monitoring" in menu or "q stops monitoring" in menu:
        raise AssertionError("ambiguous quit instruction remains in Operator controls")
    expect("*d Power down")
    expect("*r Reset/restart host")
    expect("Unavailable due to current host state")
    rows = menu.splitlines()
    if next(i for i, row in enumerate(rows) if "l Reset L1" in row) != next(i for i, row in enumerate(rows) if "r Reset/restart host" in row) + 2:
        raise AssertionError("missing blank row between host and L1 reset")
    if "L1: " not in menu or "L1 behind by ~" not in menu:
        raise AssertionError(f"operator clock information missing:\n{menu}")
    footer = next(line for line in menu.splitlines() if "Console last active:" in line)
    if "[out of sync]" not in footer or footer.index("L1 ") > footer.index("Console last active:"):
        raise AssertionError(f"status clock missing or misplaced: {footer}")
    resize(11, 40)
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
    select(b"u")
    send(b"NO\r")
    expect("Host: off")
    if "Confirm:" in capture() or sent():
        raise AssertionError("invalid confirmation did not abort to the menu")
    select(b"u")
    send(b"\r")
    if "Confirm:" in capture() or sent():
        raise AssertionError("blank confirmation did not abort to the menu")
    select(b"u")
    power_file.write_text("on")  # External state change while the prompt is open.
    send(b"YES\r")
    expect("Host: on")
    if sent():
        raise AssertionError("state change during confirmation did not cancel power up")
    power_file.write_text("off")
    send(b"\033*")
    expect("Host: off")
    select(b"U")
    send(b"YES\r")
    expect("power up: command acknowledged")
    if sent() != ["CMD power up"]:
        raise AssertionError(f"wrong power-up commands: {sent()}")
    after_up = commands()[commands().index("CMD power up") + 1:]
    if "CMD leds" not in after_up or after_up[0] != "CMD leds":
        raise AssertionError(f"power-state confirmation delayed boot LEDs: {after_up}")
    send(b"*")
    expect("Host: on")
    expect("*u Power up")
    select(b"d")
    send(b"YES\r")
    expect("Second power-down signal")
    if sent().count("CMD power down") != 1:
        raise AssertionError("second signal was automatically sent")
    send(b"YES\r")
    expect("power down: command acknowledged")
    if sent().count("CMD power down") != 2:
        raise AssertionError("second signal was not sent exactly once")
    send(b"*")
    expect("Host: off")
    send(b"r")
    expect("Action unavailable: host is off")
    power_file.write_text("unknown")
    send(b"\033*")
    expect("Host: unknown")
    expect("*u Power up")
    expect("*d Power down")
    power_file.write_text("on")
    send(b"\033*")
    select(b"r")
    send(b"YES\r")
    expect("softreset: command acknowledged")
    send(b"*")
    select(b"l")
    send(b"YES\r")
    expect("reset: command acknowledged")
    send(b"a")
    expect("reset: command acknowledged")
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
    select(b"l")
    send(b"*")
    if sent() != ["CMD power up", "CMD power down", "CMD power down", "CMD softreset", "CMD reset"]:
        raise AssertionError(f"operator actions sent wrong commands: {sent()}")
    resize(24, 120)
    print("PASS operator controls: shared styling, host-state gating/rechecks, abort, clock sync and early LEDs", flush=True)


def check_operator_timeout(capture, send, command_log):
    send(b"*")
    send(b"u")
    send(b"YES\r")
    rendered = capture()
    if "outcome unknown" not in rendered:
        raise AssertionError(f"timeout was not reported as uncertain:\n{rendered}")
    send(b"\r\r")
    commands = command_log.read_text().splitlines()
    if commands.count("CMD power up") != 1:
        raise AssertionError("uncertain power action was retried")
    if "Operator controls" in capture():
        raise AssertionError("operator menu remained open after timeout")
    if commands[-1] not in ("CMD log", "CMD leds", "CMD version", "CMD date", "CMD date tz"):
        raise AssertionError("monitoring did not resume after timeout")
    print("PASS operator timeout: unknown outcome, one send and resumed monitoring", flush=True)


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
        send({"up": b"u", "reset": b"r", "down": b"d", "unconfirmed": b"u"}[mode])
        expect("Blank input + Return aborts action")
        if mode != "unconfirmed":
            send(b"YES\r")
            expect({"up": "u [x]", "reset": "r [x]", "down": "d [x]"}[mode])
        if mode == "up":
            send(b"t")
            expect("t [x]")
    elif mode in ("cli", "skip"):
        expect("u [x]")
    presence.touch()
    send(b"")
    send(b"")  # Allow the bounded presence poll and initial requests to complete.
    rendered = capture()
    if "Waiting for USB" in rendered or "Confirm:" in rendered:
        raise AssertionError(f"wait did not return to monitoring:\n{rendered}")
    expected = {"up": "CMD power up", "cli": "CMD power up", "reset": "CMD softreset",
                "down": "CMD power down"}.get(mode)
    actions = [line for line in commands() if line in
               {"CMD power up", "CMD power down", "CMD softreset", "CMD reset"}]
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
    power_file.write_text("off" if exercise_controls or operator_timeout or wait_mode in ("up", "cli", "unconfirmed") else "on")
    presence = directory / "usb-present"
    extra_env = ({"SGIL1_MOCK_LEDS_RESPONSE": "CPU  A: 0x09: pane resizing preserves every "
                  "word of this long LED status message\n"} if exercise_controls else {})
    extra_env.update(SGIL1_MOCK_LOG=str(command_log), SGIL1_MOCK_POWER_DOWN_CONFIRM="1",
                     SGIL1_MOCK_POWER_FILE=str(power_file))
    if operator_timeout:
        extra_env["SGIL1_MOCK_POWER_UP_TIMEOUT"] = "1"
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
        while not hardcopy.exists() and time.monotonic() < deadline:
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
            def send(keys):
                os.write(master, keys)
                drain(0.4 if len(keys) < 30 else 1.5)

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
                check_operator_controls(capture, send, resize, command_log, power_file)
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
