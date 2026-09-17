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
ANSI_CONTROL = re.compile(rb"\x1b(?:\[[0-?]*[ -/]*[@-~]|\([A-Za-z0-9]|[=>])")


def plain_text(output):
    return ANSI_CONTROL.sub(b"", bytes(output)).replace(b"\r", b"\n")


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
    time.sleep(0.12)
    os.kill(proc.pid, signal.SIGWINCH)
    drain(master, output, timeout)
    return output


def check_reverse_palette_cycle(proc, master, output, automatic, last, previous, first):
    # Begin and end on the first manual palette, crossing Auto in both directions.
    for key, expected in [(b"C", automatic), (b"C", last), (b"C", previous),
                          (b"c", last), (b"c", automatic), (b"c", first)]:
        os.write(master, key)
        rendered = redraw(proc, master)
        output.extend(rendered)
        if expected not in rendered:
            raise AssertionError(f"palette {key!r} did not select {expected!r}")


def run_basic_palette_smoke():
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 120, 0, 0))
    env = os.environ.copy()
    env.update(
        {
            "LD_PRELOAD": str(MOCK),
            "SGIL1_MOCK": "1",
            "SGIL1_MOCK_WATCH": "1",
            "SGIL1_MOCK_WATCH_ACTIVITY_ONLY": "1",
            "TERM": "screen",
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
        while time.monotonic() < deadline:
            current = redraw(proc, master)
            output.extend(current)
            if b"Red palette: SGI Crimson / Fuel (auto)" in current:
                break
        else:
            raise AssertionError("basic-colour Fuel palette was not collapsed")

        os.write(master, b"c")
        cycled = redraw(proc, master)
        output.extend(cycled)
        if (
            b"Purple palette: SGI Indigo / Onyx / IMPACT / Onyx2 / O2+ / Tezro"
            not in cycled
        ):
            raise AssertionError("basic-colour cycling did not leave automatic mode first")

        check_reverse_palette_cycle(
            proc, master, output, b"Red palette: SGI Crimson / Fuel (auto)",
            b"Brown palette: SGI Personal IRIS", b"Green palette: SGI Octane",
            b"Purple palette: SGI Indigo",
        )
        os.write(master, b"Q")
        deadline = time.monotonic() + 2.0
        while proc.poll() is None and time.monotonic() < deadline:
            drain(master, output, 0.1)
        if proc.poll() is None:
            raise AssertionError("basic-colour TUI did not exit within two seconds")
        drain(master, output, 0.2)
    finally:
        os.close(master)
        if proc.poll() is None:
            proc.terminate()
            proc.wait(timeout=2)

    if proc.returncode != 0:
        raise AssertionError(
            f"basic-colour TUI exited with status {proc.returncode}"
        )


def run_led_annotation_smoke():
    # Keep one observation while later requests fail: toggling must use history.
    cases = [
        ({"SGIL1_MOCK_LEDS_RESPONSE": "CPU  A: 0x09\n",
          "SGIL1_MOCK_FAIL_AFTER_COMMANDS": "3"}, [], b"PLED_CKHUBCONFIG"),
        ({"SGIL1_MOCK_LEDS_RESPONSE": "CPU  A: 0x09\n",
          "SGIL1_MOCK_FAIL_AFTER_COMMANDS": "3"}, ["--show-annotations"], b"PLED_CKHUBCONFIG"),
        ({"SGIL1_MOCK_LEDS_RESPONSE": "CPU  A: 0xff: platform status\n",
          "SGIL1_MOCK_VERSION_RESPONSE": "L1 1.48.1 [Altix image]"}, [], b"0xff: platform status"),
        ({"SGIL1_MOCK_LEDS_RESPONSE": "CPU  A: 0xff: platform status\n",
          "SGIL1_MOCK_FAIL_LEDS_ONCE": "1",
          "SGIL1_MOCK_VERSION_AFTER_LED_FAILURE": "L1 1.48.1 [Altix image]"}, [], b"0xff: platform status"),
    ]
    for extra_env, extra_args, expected in cases:
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 160, 0, 0))
        env = os.environ.copy()
        env.update({"LD_PRELOAD": str(MOCK), "SGIL1_MOCK": "1",
                    "TERM": "xterm-256color", **extra_env})
        proc = subprocess.Popen(
            [str(BIN), "--device", "/dev/sgi-l1/l1-0", "--status-device",
             "/dev/sgi-l1/status", "--timeout", "10", "watch", "--tui",
             "--led-interval", "100", *extra_args],
            env=env, stdin=slave, stdout=slave, stderr=slave, close_fds=True,
        )
        os.close(slave)
        output = bytearray()
        try:
            drain(master, output, 1.0)
            initial = plain_text(redraw(proc, master))
            if expected not in initial:
                raise AssertionError(f"retained/qualified LED missing: {initial!r}")
            if b"Altix image" in extra_env.get("SGIL1_MOCK_VERSION_RESPONSE", "").encode():
                if b"Console last active: never" not in initial:
                    raise AssertionError("unqualified firmware affected console activity")
            if extra_args:
                if b"source:" not in initial:
                    raise AssertionError("TUI ignored --show-annotations")
                os.write(master, b"p")
                initial = plain_text(redraw(proc, master))
            if b"source:" in initial:
                raise AssertionError("LED sources unexpectedly enabled")
            os.write(master, b"p")
            annotated = plain_text(redraw(proc, master))
            if b"source:" not in annotated or expected not in annotated:
                raise AssertionError("retained LED source was not shown")
            os.write(master, b"p")
            hidden = plain_text(redraw(proc, master))
            if b"source:" in hidden or expected not in hidden:
                raise AssertionError("retained LED source toggle changed the description")
            os.write(master, b"q")
            proc.wait(timeout=2)
        finally:
            os.close(master)
            if proc.poll() is None:
                proc.terminate()
                proc.wait(timeout=2)
        if proc.returncode:
            raise AssertionError(f"LED annotation TUI exited with {proc.returncode}")


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
        if (
            b"| Filtered | queue pressure" not in initial
            or b"Fuel palette: SGI Fuel (auto)" not in initial
            or b"\x1b[38;5;124m" not in initial
        ):
            raise AssertionError("Fuel palette was not selected automatically")
        if b"\x1b[38;5;54m" in output:
            raise AssertionError("TUI flashed the Indigo palette before detection")
        if plain_text(initial).count(b"Fuel palette") != 1:
            raise AssertionError("palette identity was repeated outside the title bar")
        if plain_text(initial).count(b"100%") < 2:
            raise AssertionError("live pane positions were not shown")

        expected_short_log = (
            b"USB_WQUE Q avail",
            b"05/27/2026 12:38:03 voltage nominal",
            b"previous message repeated 1 additional time: voltage nominal",
        )
        for key in (b"k", b"\x02", b"G", b"j", b"\x06"):
            os.write(master, key)
            short_scrolled = redraw(proc, master)
            output.extend(short_scrolled)
            for line in expected_short_log:
                if line not in short_scrolled:
                    raise AssertionError(
                        "scrolling a short log pane erased visible content"
                    )

        os.write(master, b"\x0c")
        redrawn = redraw(proc, master)
        output.extend(redrawn)
        if b"sgil1ctl watch" not in redrawn or b"L1 log 3/3" not in redrawn:
            raise AssertionError("Ctrl-L did not redraw the TUI")

        os.write(master, b"?")
        help_view = redraw(proc, master)
        output.extend(help_view)
        if (
            b"Help" not in help_view
            or b"Keys" not in help_view
            or b"Observation view" not in help_view
            or b"Filtered" not in help_view
            or b"All" not in help_view
            or b"Auto (Fuel)" not in help_view
        ):
            raise AssertionError("wide TUI Help view was incomplete")
        if not re.search(rb"\x1b\[[0-9;]*7m(?:\x0f)?Filtered", help_view):
            raise AssertionError("active Help setting was not highlighted")
        help_title = help_view.find(b" Help ")
        help_border = help_view[max(0, help_title - 256) : help_title]
        if help_title < 0 or not (
            b"\xe2\x95\x94" in help_border
            or b"\xe2\x94\x8c" in help_border
            or re.search(rb"\x1b\(0[^\r\n]{0,192}lq", help_border)
        ):
            raise AssertionError("Help frame did not use graphical line drawing")

        os.write(master, b"q")
        help_closed = redraw(proc, master)
        output.extend(help_closed)
        if b"Observation view" in help_closed or proc.poll() is not None:
            raise AssertionError("q did not close Help without quitting")

        os.write(master, b"H")
        output.extend(redraw(proc, master))
        os.write(master, b"\x1b")
        escape_started = time.monotonic()
        help_closed = bytearray()
        drain(master, help_closed, 0.4)
        escape_elapsed = time.monotonic() - escape_started
        output.extend(help_closed)
        if not help_closed or escape_elapsed > 0.5:
            raise AssertionError(
                "Escape did not close Help promptly "
                f"({escape_elapsed:.3f}s, output={help_closed[-512:]!r})"
            )
        help_state = redraw(proc, master)
        output.extend(help_state)
        if b"Observation view" in help_state:
            raise AssertionError("Escape left Help visible")

        os.write(master, b"h")
        output.extend(redraw(proc, master))
        os.write(master, b"\r")
        help_closed = redraw(proc, master)
        output.extend(help_closed)
        if b"Observation view" in help_closed:
            raise AssertionError("Return did not close Help")

        os.write(master, b"A")
        all_view = redraw(proc, master)
        output.extend(all_view)
        expected_all = (
            b"All",
            b"0xFE: raw PROM value",
            b"05/27/2026 12:38:04 voltage nominal",
        )
        missing_all = [text for text in expected_all if text not in all_view]
        if missing_all:
            raise AssertionError(
                "All view did not reveal: "
                + ", ".join(text.decode() for text in missing_all)
            )

        os.write(master, b"\t")
        output.extend(redraw(proc, master))
        for key in (b"k", b"K", b"\x02", b"g", b"G", b"j", b"J", b"\x06"):
            os.write(master, key)
            short_scrolled = redraw(proc, master)
            output.extend(short_scrolled)
            if (
                short_scrolled.count(b"0xFE: raw PROM value") < 2
                or short_scrolled.count(b"0xff: Console poll") < 2
            ):
                raise AssertionError(
                    "scrolling a short LED pane erased visible content"
                )
        os.write(master, b"\t")
        output.extend(redraw(proc, master))

        os.write(master, b"p")
        annotated = redraw(proc, master)
        output.extend(annotated)
        if b"source:" not in annotated or b"controller]" not in annotated:
            raise AssertionError("TUI did not show LED description sources")
        os.write(master, b"P")
        unannotated = redraw(proc, master)
        output.extend(unannotated)
        if b"source:" in unannotated:
            raise AssertionError("TUI did not hide LED description sources")

        os.write(master, b"T")
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
        compact_view = redraw(proc, master)
        output.extend(compact_view)
        compact_percentages = re.findall(
            rb"(?:100|[1-9]?[0-9])%", plain_text(compact_view)
        )
        if len(compact_percentages) < 2 or any(
            percentage != b"100%" for percentage in compact_percentages
        ):
            raise AssertionError(
                "live compact panes did not report their final position: "
                f"{compact_percentages!r}"
            )
        os.write(master, b"\x02")
        compact_scrolled = redraw(proc, master)
        output.extend(compact_scrolled)
        scrolled_percentages = re.findall(
            rb"(?:100|[1-9]?[0-9])%", plain_text(compact_scrolled)
        )
        if (
            len(scrolled_percentages) < 2
            or all(percentage == b"100%" for percentage in scrolled_percentages)
        ):
            raise AssertionError(
                "scrollback did not reduce the selected pane position: "
                f"{scrolled_percentages!r}"
            )
        if re.search(rb" (?:(?:[1-9]?[0-9])%)", compact_scrolled):
            raise AssertionError(
                "one- or two-digit pane position was space padded"
            )
        os.write(master, b"g")
        compact_live = redraw(proc, master)
        output.extend(compact_live)
        if plain_text(compact_live).count(b"100%") < 2:
            raise AssertionError("returning live did not restore 100% positions")
        os.write(master, b"h")
        help_view = redraw(proc, master)
        output.extend(help_view)
        if (
            b"a view:All" not in help_view
            or b"c palette:Auto(Fuel)" not in help_view
            or b"h/?/Esc/Ret close" not in help_view
            or b"^L redraw" not in help_view
            or b"p sources:Off" not in help_view
            or b"q quit" in help_view
        ):
            raise AssertionError("minimum-size TUI key guide was incomplete")

        fcntl.ioctl(
            master,
            termios.TIOCSWINSZ,
            struct.pack("HHHH", 24, 120, 0, 0),
        )
        os.write(master, b"\r")
        output.extend(redraw(proc, master))
        os.write(master, b"m")
        monochrome = redraw(proc, master)
        output.extend(monochrome)
        if b"Monochrome" not in monochrome:
            raise AssertionError("monochrome mode was not identified")
        if b"\x1b[38;5;124m" in monochrome:
            raise AssertionError("monochrome mode retained colour output")

        os.write(master, b"M")
        restored = redraw(proc, master)
        output.extend(restored)
        if b"Fuel palette" not in restored or b"\x1b[38;5;124m" not in restored:
            raise AssertionError("monochrome toggle did not restore the palette")

        os.write(master, b"c")
        cycled = redraw(proc, master)
        output.extend(cycled)
        if (
            b"Indigo palette: SGI Indigo" not in cycled
            or b"\x1b[38;5;54m" not in cycled
        ):
            raise AssertionError("colour cycling did not leave automatic mode first")

        check_reverse_palette_cycle(
            proc, master, output, b"Fuel palette: SGI Fuel (auto)",
            b"Personal IRIS palette: SGI Personal IRIS", b"Tezro palette: SGI Tezro",
            b"Indigo palette: SGI Indigo",
        )
        os.write(master, b"c" * 15)
        drain(master, output, 2.0)
        automatic = redraw(proc, master)
        output.extend(automatic)
        if (
            b"Fuel palette: SGI Fuel (auto)" not in automatic
            or b"\x1b[38;5;124m" not in automatic
        ):
            raise AssertionError("palette cycling did not restore cached automatic mode")

        os.kill(proc.pid, signal.SIGTERM)
        deadline = time.monotonic() + 2.0
        while proc.poll() is None and time.monotonic() < deadline:
            drain(master, output, 0.1)
        if proc.poll() is None:
            raise AssertionError("TUI did not exit within two seconds")
        drain(master, output, 0.2)
    finally:
        os.close(master)
        if proc.poll() is None:
            proc.terminate()
            proc.wait(timeout=2)

    if proc.returncode != 0:
        raise AssertionError(f"TUI exited with status {proc.returncode}")
    if b"\x1b[38;5;124m" not in output or b"\x1b[38;5;54m" not in output:
        raise AssertionError("TUI did not render its restrained colour accents")
    run_basic_palette_smoke()
    run_led_annotation_smoke()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, OSError, subprocess.SubprocessError) as error:
        print(f"TUI smoke test failed: {error}", file=sys.stderr)
        sys.exit(1)
