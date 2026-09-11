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
            "SGIL1_MOCK_WATCH_ACTIVITY_ONLY": "1",
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
        drain(master, output, 1.0)
        deadline = time.monotonic() + 5.0
        initial = bytearray()
        while time.monotonic() < deadline:
            initial = redraw(proc, master)
            output.extend(initial)
            if (
                b"L1 log 3/3" in initial
                and b"LEDs 0 states | 2/2 samples" in initial
            ):
                break
        else:
            raise AssertionError("TUI histories did not reach their configured limits")
        if b"0xFE" in initial:
            raise AssertionError("filtered TUI view exposed a raw PROM value")
        if b"05/27/2026 12:38:04 voltage nominal" in initial:
            raise AssertionError("filtered TUI view exposed a raw repeated log")
        if re.search(rb"\d{2}/\d{2}/\d{2} \d{2}:\d{2}:\d{2}\.\d{3}", initial):
            raise AssertionError("LED timestamps were not hidden by default")
        if b"Fuel palette" not in initial or b"\x1b[38;5;124m" not in initial:
            raise AssertionError("Fuel palette was not selected automatically")

        os.write(master, b"?")
        help_view = redraw(proc, master)
        output.extend(help_view)
        if (
            b"Help" not in help_view
            or b"Keys" not in help_view
            or b"Observation view" not in help_view
            or b"Filtered" not in help_view
            or b"All" not in help_view
        ):
            raise AssertionError("wide TUI Help view was incomplete")
        if not re.search(rb"\x1b\[[0-9;]*7mFiltered", help_view):
            raise AssertionError("active Help setting was not highlighted")

        os.write(master, b"h")
        output.extend(redraw(proc, master))

        os.write(master, b"a")
        all_view = redraw(proc, master)
        output.extend(all_view)
        if (
            b"| All |" not in all_view
            or b"0xFE: raw PROM value" not in all_view
            or b"05/27/2026 12:38:04 voltage nominal" not in all_view
        ):
            raise AssertionError("All view did not reveal retained raw observations")

        os.write(master, b"t")
        timestamped = redraw(proc, master)
        output.extend(timestamped)
        if not re.search(
            rb"\d{2}/\d{2}/\d{2} \d{2}:\d{2}:\d{2}\.\d{3}",
            timestamped,
        ):
            raise AssertionError("TUI did not show timestamped LED responses")

        fcntl.ioctl(
            master,
            termios.TIOCSWINSZ,
            struct.pack("HHHH", 10, 40, 0, 0),
        )
        os.write(master, b"h")
        help_view = redraw(proc, master)
        output.extend(help_view)
        if (
            b"a view:All" not in help_view
            or b"h/? close Help" not in help_view
            or b"q quit" not in help_view
        ):
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
        if b"Fuel palette (monochrome)" not in monochrome:
            raise AssertionError("monochrome mode was not identified")
        if b"\x1b[38;5;124m" in monochrome:
            raise AssertionError("monochrome mode retained colour output")

        os.write(master, b"m")
        restored = redraw(proc, master)
        output.extend(restored)
        if b"Fuel palette" not in restored or b"\x1b[38;5;124m" not in restored:
            raise AssertionError("monochrome toggle did not restore the palette")

        os.write(master, b"c")
        cycled = redraw(proc, master)
        output.extend(cycled)
        if (
            b"Personal IRIS palette" not in cycled
            or b"SGI Personal IRIS" not in cycled
            or b"\x1b[38;5;94m" not in cycled
        ):
            raise AssertionError("colour cycling did not select the next palette")

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
    if b"\x1b[38;5;124m" not in output or b"\x1b[38;5;94m" not in output:
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
