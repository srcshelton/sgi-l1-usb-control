#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

import fcntl
import json
import os
import pty
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
                   ["Select pane", "Shrink/grow selected pane", "LED timestamps",
                    "LED description sources", "Redraw screen", "Close Help"])
        rows = [next(i for i, line in enumerate(lines) if entry in line)
                for entry in entries]
        if rows != sorted(rows) or rows[1] != rows[0] + 1 or rows[3] != rows[2] + 1:
            raise AssertionError(f"wrong help order:\n{rendered}")
        if rows[-1] != rows[-2] + 1:
            raise AssertionError(f"Redraw and Close are not adjacent:\n{rendered}")
        send(b"\r")

    wide_widths((80, 40))
    help_order()
    send(b"<")
    wide_widths((79, 41))
    send(b"\033OC")  # Right, with application cursor keys enabled.
    wide_widths((80, 40))
    send(b"\t>")
    wide_widths((79, 41))
    send(b"\033OD")
    wide_widths((80, 40))
    send(b"^v\033OA\033OB")  # Vertical keys do not resize wide panes.
    wide_widths((80, 40))
    send(b">" * 120)
    wide_widths((24, 96))
    send(b"<" * 120)
    rendered = wide_widths((96, 24))
    led_text = " ".join(line[97:-1].strip() for line in rendered.rstrip("\n").split("\n")[2:-3])
    if "paneresizingpreserveseverywordofthislongLEDstatusmessage" not in "".join(led_text.split()):
        raise AssertionError(f"LED text was lost when rewrapped:\n{rendered}")
    send(b">" * 16)
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


def run_case(screen, temporary, altscreen, primary, stop):
    name = f"altscreen-{altscreen}-{'primary' if primary else 'default'}-{stop}"
    directory = temporary / name
    directory.mkdir()
    session = f"sgil1ctl-{os.getpid()}-{name}"
    runner = directory / "runner.py"
    hardcopy = directory / "screen.txt"
    screenrc = directory / "screenrc"
    child_state = directory / "child.json"
    result = directory / "result.txt"
    exercise_controls = altscreen == "off" and not primary and stop == "q"
    extra_env = ({"SGIL1_MOCK_LEDS_RESPONSE": "CPU  A: 0x09: pane resizing preserves every "
                  "word of this long LED status message\n"} if exercise_controls else {})
    command = [
        str(BIN), "--device", "/dev/sgi-l1/l1-0",
        "--status-device", "/dev/sgi-l1/status", "--timeout", "10",
        "watch", "--tui", "--log-interval", "100", "--led-interval", "100",
        "--log-history", "64", "--led-history", "32",
    ]
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
                if "Fuel" in rendered and "Console last active:" in rendered:
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
        if exercise_controls:
            def send(keys):
                os.write(master, keys)
                drain(0.4 if len(keys) < 30 else 1.5)

            def resize(rows, cols):
                fcntl.ioctl(master, termios.TIOCSWINSZ,
                            struct.pack("HHHH", rows, cols, 0, 0))
                os.kill(proc.pid, signal.SIGWINCH)
                drain(0.4)

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
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, OSError, subprocess.SubprocessError) as error:
        print(f"TUI terminal smoke test failed: {error}", file=sys.stderr)
        sys.exit(1)
