#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

import fcntl
import os
import pty
import re
import select
import signal
import struct
import subprocess
import sys
import termios
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "tools" / "sgil1ctl"
MOCK = ROOT / "tests" / "mock_l1.so"


def drain(master, output, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        readable, _, _ = select.select([master], [], [], 0.05)
        if not readable:
            continue
        try:
            output.extend(os.read(master, 65536))
        except OSError:
            break


def redraw(proc, master, timeout=0.3):
    output = bytearray()
    os.kill(proc.pid, signal.SIGWINCH)
    drain(master, output, timeout)
    return output


def main():
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 120, 0, 0))
    env = os.environ.copy()
    env.update(
        {
            "LD_PRELOAD": str(MOCK),
            "SGIL1_MOCK": "1",
            "SGIL1_MOCK_WATCH": "1",
            "TERM": "xterm-256color",
            "COLORFGBG": "0;15",
            "TZ": "Europe/London",
        }
    )
    proc = subprocess.Popen(
        [
            str(BIN),
            "--device",
            "/dev/sgi-l1/l1-0",
            "--status-device",
            "/dev/sgi-l1/status",
            "--timeout",
            "10",
            "watch",
            "--tui",
            "--no-alternate-screen",
            "--log-interval",
            "100",
            "--led-interval",
            "100",
            "--log-history",
            "3",
            "--led-history",
            "2",
        ],
        cwd=ROOT,
        env=env,
        stdin=slave,
        stdout=slave,
        stderr=slave,
        close_fds=True,
    )
    os.close(slave)
    output = bytearray()
    try:
        drain(master, output, 1.5)
        initial = redraw(proc, master)
        output.extend(initial)
        if b"L1 log 3/3" not in initial or b"LEDs 2/2" not in initial:
            raise AssertionError("TUI histories did not reach their configured limits")
        if b"0xFE" in initial:
            raise AssertionError("filtered TUI view exposed a raw PROM value")
        if b"05/27/2026 12:38:04 voltage nominal" in initial:
            raise AssertionError("filtered TUI view exposed a raw repeated log")
        if re.search(rb"\d{2}/\d{2}/\d{2} \d{2}:\d{2}:\d{2}\.\d{3}", initial):
            raise AssertionError("LED timestamps were not hidden by default")
        if b"\x1b[34m" not in initial or b"\x1b[31m" not in initial:
            raise AssertionError("light-background palette was not selected")

        os.write(master, b"t")
        timestamped = redraw(proc, master)
        output.extend(timestamped)
        if not re.search(
            rb"\d{2}/\d{2}/\d{2} \d{2}:\d{2}:\d{2}\.\d{3}",
            timestamped,
        ):
            raise AssertionError("TUI did not show timestamped LED responses")

        os.write(master, b"a")
        all_view = redraw(proc, master)
        output.extend(all_view)
        if (
            b"| all |" not in all_view
            or b"0xFE: raw PROM value" not in all_view
            or b"05/27/2026 12:38:04 voltage nominal" not in all_view
        ):
            raise AssertionError("All view did not reveal retained raw observations")

        fcntl.ioctl(
            master,
            termios.TIOCSWINSZ,
            struct.pack("HHHH", 10, 40, 0, 0),
        )
        os.write(master, b"h")
        help_view = redraw(proc, master)
        output.extend(help_view)
        if b"a Filtered/All" not in help_view or b"q quit" not in help_view:
            raise AssertionError("minimum-size TUI key guide was incomplete")

        fcntl.ioctl(
            master,
            termios.TIOCSWINSZ,
            struct.pack("HHHH", 24, 120, 0, 0),
        )
        os.write(master, b"h")
        output.extend(redraw(proc, master))
        os.write(master, b"m")
        monochrome = redraw(proc, master)
        output.extend(monochrome)
        if b"\x1b[36m" in monochrome or b"\x1b[33m" in monochrome:
            raise AssertionError("monochrome mode retained colour output")

        os.write(master, b"c")
        light = redraw(proc, master)
        output.extend(light)
        if b"\x1b[34m" not in light:
            raise AssertionError("colour cycling did not restore the palette")

        os.write(master, b"c")
        dark = redraw(proc, master)
        output.extend(dark)
        if b"\x1b[36m" not in dark or b"\x1b[33m" not in dark:
            raise AssertionError("colour cycling did not select the dark palette")

        os.write(master, b"q")
        proc.wait(timeout=1)
        drain(master, output, 0.2)
    finally:
        os.close(master)
        if proc.poll() is None:
            proc.terminate()
            proc.wait(timeout=2)

    if proc.returncode != 0:
        raise AssertionError(f"TUI exited with status {proc.returncode}")
    if b"\x1b[34m" not in output or b"\x1b[31m" not in output:
        raise AssertionError("TUI did not render its restrained colour accents")
    if not re.search(rb"\x1b\[[012]?[KJ]", output):
        raise AssertionError("TUI did not clear the final primary-screen line")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, OSError, subprocess.SubprocessError) as error:
        print(f"TUI smoke test failed: {error}", file=sys.stderr)
        sys.exit(1)
