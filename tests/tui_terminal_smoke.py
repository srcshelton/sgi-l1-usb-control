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
