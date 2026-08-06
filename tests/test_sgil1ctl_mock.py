# SPDX-License-Identifier: GPL-2.0-or-later

import os
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "tools" / "sgil1ctl"
MOCK = ROOT / "tests" / "mock_l1.so"


class Sgil1CtlMockTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(["make", "-C", str(ROOT / "tools")], check=True)
        subprocess.run(["make", "-C", str(ROOT / "tests"), "mock_l1.so"], check=True)

    def run_ctl(self, args, extra_env=None, check=False):
        env = os.environ.copy()
        env.update(
            {
                "LD_PRELOAD": str(MOCK),
                "SGIL1_MOCK": "1",
                "SGIL1_MOCK_NO_SLEEP": "1",
                "TZ": "Europe/London",
            }
        )
        if extra_env:
            env.update(extra_env)

        cmd = [
            str(BIN),
            "--device",
            "/dev/sgi-l1/l1-0",
            "--status-device",
            "/dev/sgi-l1/status",
            "--timeout",
            "10",
        ] + args
        return subprocess.run(
            cmd,
            cwd=ROOT,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=check,
            timeout=10,
        )

    def run_ctl_auto_data(self, args, extra_env=None, check=False):
        env = os.environ.copy()
        env.update(
            {
                "LD_PRELOAD": str(MOCK),
                "SGIL1_MOCK": "1",
                "SGIL1_MOCK_NO_SLEEP": "1",
                "TZ": "Europe/London",
            }
        )
        if extra_env:
            env.update(extra_env)

        cmd = [
            str(BIN),
            "--status-device",
            "/dev/sgi-l1/status",
            "--timeout",
            "10",
        ] + args
        return subprocess.run(
            cmd,
            cwd=ROOT,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=check,
            timeout=10,
        )

    def run_follow_for(self, args, extra_env=None, seconds=0.7):
        env = os.environ.copy()
        env.update(
            {
                "LD_PRELOAD": str(MOCK),
                "SGIL1_MOCK": "1",
                "TZ": "Europe/London",
            }
        )
        if extra_env:
            env.update(extra_env)

        cmd = [
            str(BIN),
            "--device",
            "/dev/sgi-l1/l1-0",
            "--status-device",
            "/dev/sgi-l1/status",
            "--timeout",
            "10",
        ] + args
        proc = subprocess.Popen(
            cmd,
            cwd=ROOT,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        try:
            return proc.communicate(timeout=seconds) + (proc.returncode,)
        except subprocess.TimeoutExpired:
            proc.terminate()
            stdout, stderr = proc.communicate(timeout=5)
            return stdout, stderr, proc.returncode

    def run_with_log(self, args, extra_env=None):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "mock.log"
            env = {"SGIL1_MOCK_LOG": str(log)}
            if extra_env:
                env.update(extra_env)
            proc = self.run_ctl(args, env)
            return proc, log.read_text() if log.exists() else ""

    def test_probe_reads_status_and_cfg_devices(self):
        proc = self.run_ctl(["probe"])

        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("driver=sgi-l1-usb mock", proc.stdout)
        self.assertIn("sgil1_0 present", proc.stdout)
        self.assertIn("data device: /dev/sgi-l1/l1-0", proc.stdout)
        self.assertIn("bus=2 dev=10 level=2 path=1.1", proc.stdout)

    def test_help_documents_follow_modes(self):
        proc = self.run_ctl(["--help"])

        self.assertEqual(proc.returncode, 0)
        self.assertIn("wait [-w|--follow] [OPTIONS]", proc.stdout)
        self.assertIn("-w|--follow after --power-up", proc.stdout)
        self.assertIn("--power-down, or --reset", proc.stdout)
        self.assertIn("reset --force [-w|--follow]", proc.stdout)
        self.assertIn("power up [-w|--follow]", proc.stdout)
        self.assertIn("power down [-w|--follow]", proc.stdout)
        self.assertIn("power reset --force [-w|--follow]", proc.stdout)
        self.assertIn("log [-w|--follow]", proc.stdout)
        self.assertIn("leds [-w|--follow]", proc.stdout)
        self.assertIn("debug [OPTIONS]", proc.stdout)
        self.assertIn("sgil1ctl COMMAND --help", proc.stdout)

    def test_command_specific_help_documents_wait_options(self):
        proc = self.run_ctl(["wait", "--help"])

        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(proc.stderr, "")
        self.assertIn("Usage: sgil1ctl [GLOBAL OPTIONS] wait [OPTIONS]", proc.stdout)
        self.assertIn("--power-up", proc.stdout)
        self.assertIn("--power-down", proc.stdout)
        self.assertIn("--reset", proc.stdout)
        self.assertIn("--wait-timeout SEC", proc.stdout)
        self.assertIn("-w, --follow", proc.stdout)
        self.assertNotIn("unknown wait option", proc.stderr)

    def test_command_specific_help_documents_option_commands(self):
        cases = [
            (
                ["date", "--help"],
                [
                    "Usage: sgil1ctl [GLOBAL OPTIONS] date [OPTIONS]",
                    "sgil1ctl [GLOBAL OPTIONS] set-date [OPTIONS]",
                    "--set-time",
                    "--timezone TZ",
                    "--drift-seconds SEC",
                ],
            ),
            (
                ["log", "--help"],
                [
                    "Usage: sgil1ctl [GLOBAL OPTIONS] log [OPTIONS]",
                    "--poll-interval MS",
                    "--no-repeat-summary",
                ],
            ),
            (
                ["leds", "-h"],
                [
                    "Usage: sgil1ctl [GLOBAL OPTIONS] leds [OPTIONS]",
                    "--poll-interval MS",
                    "-w, --follow",
                ],
            ),
            (
                ["power", "--help"],
                [
                    "Usage: sgil1ctl [GLOBAL OPTIONS] power [SUBCOMMAND] [OPTIONS]",
                    "(none)",
                    "check",
                    "reset|softreset|softrst",
                    "send one power-down signal",
                    "send second power-down signal",
                    "follow LEDs after power up, down, or reset",
                    "up/down buffer LEDs until confirmation",
                ],
            ),
            (
                ["debug", "--help"],
                [
                    "Usage: sgil1ctl [GLOBAL OPTIONS] debug [OPTIONS]",
                    "--enable SWITCH",
                    "--boot-stop POINT",
                    "--list-switches",
                ],
            ),
            (
                ["reset", "--help"],
                [
                    "Usage: sgil1ctl [GLOBAL OPTIONS] reset --force",
                    "L1 controller reset",
                    "power reset --force",
                ],
            ),
            (
                ["l1cmd", "--help"],
                [
                    "Usage: sgil1ctl [GLOBAL OPTIONS] l1cmd <command>",
                    "Quote '*'",
                    "l1cmd help",
                ],
            ),
        ]

        for args, expected in cases:
            with self.subTest(args=args):
                proc = self.run_ctl(args)

                self.assertEqual(proc.returncode, 0, proc.stderr)
                self.assertEqual(proc.stderr, "")
                for text in expected:
                    self.assertIn(text, proc.stdout)

    def test_help_output_uses_current_terms_and_wraps_lines(self):
        help_commands = [
            ["--help"],
            ["--help-all"],
            ["date", "--help"],
            ["set-date", "--help"],
            ["wait", "--help"],
            ["log", "--help"],
            ["logs", "--help"],
            ["leds", "--help"],
            ["power", "--help"],
            ["power-up", "--help"],
            ["power-down", "--help"],
            ["reset", "--help"],
            ["debug", "--help"],
            ["l1cmd", "--help"],
        ]

        for args in help_commands:
            with self.subTest(args=args):
                proc = self.run_ctl(args)

                self.assertEqual(proc.returncode, 0, proc.stderr)
                self.assertNotIn("FEATURE", proc.stdout)
                self.assertNotIn("Aliases:", proc.stdout)
                self.assertNotIn("alias for --drift-seconds", proc.stdout)
                self.assertNotIn("aliases for --background", proc.stdout)
                self.assertNotIn("alias for --keepalive", proc.stdout)
                self.assertNotIn("alias for --list-switches", proc.stdout)
                for line in proc.stdout.splitlines():
                    self.assertLessEqual(len(line), 79, line)

    def test_command_specific_help_does_not_send_l1_command(self):
        proc, log = self.run_with_log(["version", "--help"])

        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("Usage: sgil1ctl [GLOBAL OPTIONS] version", proc.stdout)
        self.assertNotIn("L1 1.24.11", proc.stdout)
        self.assertEqual(log, "")

    def test_version_uses_mock_irouter_transport_without_debug_noise(self):
        proc, log = self.run_with_log(["version"])

        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("L1 1.24.11", proc.stdout)
        self.assertNotIn("sent L1 command", proc.stdout)
        self.assertIn("CMD version", log)

    def test_logs_alias_matches_log_command(self):
        proc, log = self.run_with_log(["logs"])

        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("05/27/2026 12:38:00 L1 booted", proc.stdout)
        self.assertIn("CMD log", log)

    def test_debug_shows_virtual_switch_decode_and_l1dbg_state(self):
        proc, log = self.run_with_log(
            ["debug"],
            {"SGIL1_MOCK_DEBUG_SWITCHES": "0x0084"},
        )

        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertNotIn("debug switches set to 0x0084", proc.stdout)
        self.assertIn("L1 virtual debug switches are 0x0084", proc.stdout)
        self.assertIn("L1 virtual diagnostic testing is normal testing", proc.stdout)
        self.assertIn("L1 virtual diagnostic output level is verbose", proc.stdout)
        self.assertIn("L1 virtual do-not-clear-errors flag is on", proc.stdout)
        self.assertIn("L1 irouter debugging is off", proc.stdout)
        self.assertIn("CMD debug", log)
        self.assertIn("CMD l1dbg", log)

    def test_l1cmd_debug_decodes_virtual_switches(self):
        proc = self.run_ctl(
            ["l1cmd", "debug"],
            {"SGIL1_MOCK_DEBUG_SWITCHES": "0x4018"},
        )

        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("debug switches set to 0x4018", proc.stdout)
        self.assertIn("L1 virtual boot stop point is memoryless POD", proc.stdout)
        self.assertIn("L1 virtual I/O discovery disable is on", proc.stdout)

    def test_debug_updates_require_force_and_accept_named_switches(self):
        denied = self.run_ctl(["debug", "--enable", "verbose"])
        self.assertEqual(denied.returncode, 2)
        self.assertIn("debug update requires --force", denied.stderr)

        proc, log = self.run_with_log(
            [
                "debug",
                "--enable",
                "verbose",
                "do-not-clear-errors",
                "--test",
                "heavy",
                "--force",
            ],
            {"SGIL1_MOCK_DEBUG_SWITCHES": "0x0000"},
        )

        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertNotIn("debug switches set to 0x0086", proc.stdout)
        self.assertIn("L1 virtual debug switches are 0x0086", proc.stdout)
        self.assertIn("L1 virtual diagnostic testing is heavy testing", proc.stdout)
        self.assertIn("L1 virtual diagnostic output level is verbose", proc.stdout)
        self.assertIn("L1 virtual do-not-clear-errors flag is on", proc.stdout)
        self.assertIn("CMD debug", log)
        self.assertIn("CMD debug 0x0086", log)
        self.assertIn("CMD l1dbg", log)

    def test_debug_set_and_list_switches_do_not_touch_l1dbg_expert_controls(self):
        switches = self.run_ctl(["debug", "--list-switches"])
        self.assertEqual(switches.returncode, 0, switches.stderr)
        self.assertIn("0x0004  verbose", switches.stdout)
        self.assertIn("0x0018  memoryless-pod", switches.stdout)
        self.assertIn("--boot-stop none --force", switches.stdout)

        short_list = self.run_ctl(["debug", "--list"])
        self.assertEqual(short_list.returncode, 0, short_list.stderr)
        self.assertIn("Switch flags for --enable/--disable", short_list.stdout)

        removed_alias = self.run_ctl(["debug", "--list-features"])
        self.assertEqual(removed_alias.returncode, 2)
        self.assertIn("unknown debug option: --list-features", removed_alias.stderr)

        proc, log = self.run_with_log(["debug", "--set", "0x0018", "--force"])

        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertNotIn("debug switches set to 0x0018", proc.stdout)
        self.assertIn("L1 virtual debug switches are 0x0018", proc.stdout)
        self.assertIn("L1 virtual boot stop point is memoryless POD", proc.stdout)
        self.assertIn("CMD debug 0x0018", log)
        self.assertIn("CMD l1dbg", log)
        self.assertNotIn("CMD l1dbg env", log)

    def test_log_follow_prints_new_lines_and_repeat_summary(self):
        stdout, stderr, _returncode = self.run_follow_for(
            ["log", "--follow", "--poll-interval", "100"],
            {"SGIL1_MOCK_LOG_FOLLOW": "1"},
        )

        self.assertIn("05/27/2026 12:38:00 L1 booted", stdout)
        self.assertEqual(stdout.count("L1 booted"), 1, stdout)
        self.assertIn("05/27/2026 12:38:01 USB ready", stdout)
        self.assertIn("05/27/2026 12:38:02 fan stable", stdout)
        self.assertIn("message repeated 2 times: fan stable", stdout)
        self.assertIn("05/27/2026 12:38:05 voltage nominal", stdout)
        self.assertNotIn("advanced without overlap", stderr)

    def test_stale_single_character_response_is_ignored(self):
        proc = self.run_ctl(["log"], {"SGIL1_MOCK_STALE_BEFORE_RESPONSE": "1"})

        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(proc.stdout, "05/27/2026 12:38:00 L1 booted\n")

    def test_debug_dumps_drained_stale_frames(self):
        proc = self.run_ctl(
            ["--debug", "--no-discover", "log"],
            {"SGIL1_MOCK_STALE_ON_OPEN": "1"},
        )

        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("drained stale IRouter frame", proc.stdout)
        self.assertIn("arg0 text:\nstale drain", proc.stdout)

    def test_leds_follow_prints_changes_without_repeats(self):
        stdout, stderr, _returncode = self.run_follow_for(
            ["leds", "--follow", "--poll-interval", "50"],
            {"SGIL1_MOCK_LEDS_FOLLOW": "1"},
            seconds=0.4,
        )

        self.assertIn("0x55: Global master in PROM", stdout)
        self.assertIn("0x70: Running BIST on bank 0", stdout)
        self.assertEqual(stdout.count("Running BIST on bank 0"), 1, stdout)
        self.assertNotIn("unknown LED status", stdout)
        self.assertNotIn("LEDs follow: leds command failed", stderr)

    def test_leds_decodes_documented_unknown_statuses(self):
        proc = self.run_ctl(["leds"], {"SGIL1_MOCK_LEDS_UNKNOWN": "1"})

        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("0x55: Global master in PROM", proc.stdout)
        self.assertIn("0x81: CP1 failed", proc.stdout)
        self.assertIn("0xB5: Error calculating backplane frequency", proc.stdout)
        self.assertIn("0x00: In slave loop", proc.stdout)
        self.assertIn("0xff: Console poll found data for reading", proc.stdout)
        self.assertNotIn("unknown LED status", proc.stdout)

    def test_l1cmd_leds_decodes_documented_unknown_statuses(self):
        proc = self.run_ctl(
            ["l1cmd", "leds"],
            {"SGIL1_MOCK_LEDS_UNKNOWN": "1"},
        )

        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("0x55: Global master in PROM", proc.stdout)
        self.assertIn("0x81: CP1 failed", proc.stdout)
        self.assertNotIn("unknown LED status", proc.stdout)

    def test_reset_follow_starts_leds_follow(self):
        stdout, stderr, _returncode = self.run_follow_for(
            ["reset", "--force", "--follow"],
            {"SGIL1_MOCK_LEDS_FOLLOW": "1"},
            seconds=0.5,
        )

        self.assertIn("reset issued", stdout)
        self.assertIn("0x55: Global master in PROM", stdout)
        self.assertIn("0x70: Running BIST on bank 0", stdout)
        self.assertNotIn("LEDs follow: leds command failed", stderr)

    def test_wait_follow_requires_power_action(self):
        proc = self.run_ctl(["wait", "--follow"])

        self.assertEqual(proc.returncode, 2)
        self.assertIn(
            "wait --follow requires --power-up, --power-down, or --reset",
            proc.stderr,
        )

    def test_wait_power_up_follow_starts_leds_follow(self):
        stdout, stderr, _returncode = self.run_follow_for(
            ["wait", "--power-up", "--force", "--follow"],
            {"SGIL1_MOCK_POWER": "off", "SGIL1_MOCK_LEDS_FOLLOW": "1"},
            seconds=2.2,
        )

        self.assertIn("Power-up: workstation appears off; issuing power up", stdout)
        self.assertIn("Power-up: entering LED follow before power-state confirmation", stdout)
        self.assertIn("Power-up: confirmed workstation appears on", stdout)
        self.assertIn("0x55: Global master in PROM", stdout)
        self.assertIn("0x70: Running BIST on bank 0", stdout)
        self.assertLess(
            stdout.index("Power-up: confirmed workstation appears on"),
            stdout.index("0x55: Global master in PROM"),
        )
        self.assertNotIn("LEDs follow: leds command failed", stderr)

    def test_wait_power_down_follow_starts_leds_follow(self):
        stdout, stderr, _returncode = self.run_follow_for(
            ["wait", "--power-down", "--force", "--follow"],
            {"SGIL1_MOCK_POWER": "on", "SGIL1_MOCK_LEDS_FOLLOW": "1"},
            seconds=2.2,
        )

        self.assertIn("Power-down: issuing power down from wait mode", stdout)
        self.assertIn("Power-down: entering LED follow before power-state confirmation", stdout)
        self.assertIn("Power-down: confirmed workstation appears off", stdout)
        self.assertIn("0x55: Global master in PROM", stdout)
        self.assertIn("0x70: Running BIST on bank 0", stdout)
        self.assertLess(
            stdout.index("Power-down: confirmed workstation appears off"),
            stdout.index("0x55: Global master in PROM"),
        )
        self.assertNotIn("LEDs follow: leds command failed", stderr)

    def test_wait_reset_sends_host_softreset(self):
        proc, log = self.run_with_log(["wait", "--reset", "--force"])

        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("Power-reset: issuing host soft reset", proc.stdout)
        self.assertIn("soft reset issued", proc.stdout)
        self.assertIn("CMD softreset", log)
        self.assertNotIn("CMD reset\n", log)

    def test_auto_device_scans_nonzero_data_nodes(self):
        proc = self.run_ctl_auto_data(
            ["version"],
            {"SGIL1_MOCK_DATA_INDEX": "2"},
        )

        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("L1 1.24.11", proc.stdout)

    def test_status_consolidates_expected_sections(self):
        proc = self.run_ctl(["status"])

        self.assertEqual(proc.returncode, 0, proc.stderr)
        for heading in [
            "Firmware",
            "Identity",
            "Clock",
            "Power State",
            "Debug",
            "Environment",
            "USB Transport",
        ]:
            self.assertIn(heading, proc.stdout)
        self.assertIn("Environmental monitoring is enabled", proc.stdout)
        self.assertIn("L1 virtual debug switches are", proc.stdout)
        self.assertIn("L1 irouter debugging is off", proc.stdout)
        self.assertNotIn("MAC classification", proc.stdout)

    def test_date_set_time_sends_timezone_and_date_commands(self):
        proc, log = self.run_with_log(
            ["date", "--set-time", "--timezone", "GMT0BST", "--drift-seconds", "0"],
            {"SGIL1_MOCK_DATE": "01/01/2000 00:00:00 GMT\n"},
        )

        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("Clock: set L1 timezone", proc.stdout)
        self.assertIn("Clock: set L1 time from host", proc.stdout)
        self.assertIn("CMD date tz", log)
        self.assertRegex(log, r"CMD date [0-9]{12}\.[0-9]{2}")

    def test_set_date_and_silent_date_aliases_set_time(self):
        for command in ["set-date", "set-clock", "set-time"]:
            with self.subTest(command=command):
                proc, log = self.run_with_log(
                    [command, "--timezone", "GMT0BST", "--drift-seconds", "0"],
                    {"SGIL1_MOCK_DATE": "01/01/2000 00:00:00 GMT\n"},
                )

                self.assertEqual(proc.returncode, 0, proc.stderr)
                self.assertIn("Clock: set L1 timezone", proc.stdout)
                self.assertIn("CMD date tz", log)
                self.assertRegex(log, r"CMD date [0-9]{12}\.[0-9]{2}")

        for command in ["clock", "time"]:
            with self.subTest(command=command):
                proc, log = self.run_with_log([command])

                self.assertEqual(proc.returncode, 0, proc.stderr)
                self.assertIn("05/27/2026 12:38:03 BST", proc.stdout)
                self.assertIn("CMD date", log)
                self.assertNotRegex(log, r"CMD date [0-9]{12}\.[0-9]{2}")

    def test_power_up_confirms_state_after_timeout_without_force(self):
        proc, log = self.run_with_log(
            ["power", "up"],
            {"SGIL1_MOCK_POWER": "off", "SGIL1_MOCK_POWER_UP_TIMEOUT": "1"},
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("confirmed workstation appears on", proc.stdout)
        self.assertIn("CMD power up", log)
        self.assertIn("CMD power check", log)

    def test_power_up_still_accepts_force_for_compatibility(self):
        proc, log = self.run_with_log(
            ["--force", "power", "up"],
            {"SGIL1_MOCK_POWER": "off"},
        )

        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("confirmed workstation appears on", proc.stdout)
        self.assertIn("CMD power up", log)

    def test_power_up_follow_starts_leds_follow(self):
        stdout, stderr, _returncode = self.run_follow_for(
            ["power", "up", "--follow"],
            {
                "SGIL1_MOCK_POWER": "off",
                "SGIL1_MOCK_LEDS_FOLLOW": "1",
                "SGIL1_MOCK_POWER_UP_TIMEOUT": "1",
            },
            seconds=0.5,
        )

        self.assertIn("Power-up: entering LED follow before power-state confirmation", stdout)
        self.assertIn("Power-up: confirmed workstation appears on", stdout)
        self.assertIn("0x55: Global master in PROM", stdout)
        self.assertIn("0x70: Running BIST on bank 0", stdout)
        self.assertLess(
            stdout.index("Power-up: confirmed workstation appears on"),
            stdout.index("0x55: Global master in PROM"),
        )
        self.assertNotIn("LEDs follow: leds command failed", stderr)

    def test_power_down_without_force_sends_single_signal(self):
        proc, log = self.run_with_log(
            ["power", "down"],
            {"SGIL1_MOCK_POWER": "on"},
        )

        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(log.count("CMD power down"), 1, log)
        self.assertIn("Power-down: confirmed workstation appears off", proc.stdout)

    def test_power_down_force_sends_two_signals_without_prompt(self):
        proc, log = self.run_with_log(
            ["power", "down", "--force"],
            {"SGIL1_MOCK_POWER": "on"},
        )

        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(log.count("CMD power down"), 2, log)
        self.assertIn("--force set; issuing second power-down signal", proc.stdout)
        self.assertIn("Power-down: confirmed workstation appears off", proc.stdout)

    def test_power_down_follow_starts_leds_follow(self):
        stdout, stderr, _returncode = self.run_follow_for(
            ["power", "down", "--force", "--follow"],
            {"SGIL1_MOCK_POWER": "on", "SGIL1_MOCK_LEDS_FOLLOW": "1"},
            seconds=0.5,
        )

        self.assertIn("Power-down: entering LED follow before power-state confirmation", stdout)
        self.assertIn("Power-down: confirmed workstation appears off", stdout)
        self.assertIn("0x55: Global master in PROM", stdout)
        self.assertIn("0x70: Running BIST on bank 0", stdout)
        self.assertLess(
            stdout.index("Power-down: confirmed workstation appears off"),
            stdout.index("0x55: Global master in PROM"),
        )
        self.assertNotIn("LEDs follow: leds command failed", stderr)

    def test_power_down_confirmation_prompt_triggers_second_command(self):
        proc, log = self.run_with_log(
            ["power", "down", "--force"],
            {"SGIL1_MOCK_POWER": "on", "SGIL1_MOCK_POWER_DOWN_CONFIRM": "1"},
        )

        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("requested second signal", proc.stdout)
        self.assertEqual(log.count("CMD power down"), 2, log)
        self.assertIn("confirmed workstation appears off", proc.stdout)

    def test_power_reset_requires_force_and_sends_softreset(self):
        denied = self.run_ctl(["power", "reset"])
        self.assertEqual(denied.returncode, 2)
        self.assertIn("requires --force", denied.stderr)

        proc, log = self.run_with_log(["power", "reset", "--force"])

        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("soft reset issued", proc.stdout)
        self.assertIn("CMD softreset", log)

    def test_power_reset_follow_starts_leds_follow(self):
        stdout, stderr, _returncode = self.run_follow_for(
            ["power", "reset", "--force", "--follow"],
            {"SGIL1_MOCK_LEDS_FOLLOW": "1"},
            seconds=0.5,
        )

        self.assertIn("soft reset issued", stdout)
        self.assertIn("0x55: Global master in PROM", stdout)
        self.assertIn("0x70: Running BIST on bank 0", stdout)
        self.assertNotIn("LEDs follow: leds command failed", stderr)

    def test_power_softreset_aliases_are_accepted(self):
        proc, log = self.run_with_log(["power", "softrst", "--force"])

        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("soft reset issued", proc.stdout)
        self.assertIn("CMD softrst", log)

    def test_l1cmd_destructive_commands_require_force(self):
        denied = self.run_ctl(["l1cmd", "softreset"])

        self.assertEqual(denied.returncode, 2)
        self.assertIn("add --force", denied.stderr)

        proc, log = self.run_with_log(["l1cmd", "softreset", "--force"])

        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("soft reset issued", proc.stdout)
        self.assertIn("CMD softreset", log)

    def test_l1cmd_parent_help_replaces_namespace_error(self):
        proc = self.run_ctl(["l1cmd", "flash"])

        self.assertEqual(proc.returncode, 2)
        self.assertIn("Help for 'flash':", proc.stdout)
        self.assertIn("flash status", proc.stdout)
        self.assertNotIn("ERROR: command not found.", proc.stdout)

    def test_l1cmd_invalid_child_keeps_l1_error(self):
        proc = self.run_ctl(["l1cmd", "date", "help"])

        self.assertEqual(proc.returncode, 2)
        self.assertIn("ERROR: command not found.", proc.stdout)
        self.assertNotIn("Help for 'date help':", proc.stdout)

    def test_broadcast_prefix_validation_and_success_path(self):
        bare = self.run_ctl(["l1cmd", "*"])
        self.assertEqual(bare.returncode, 2)
        self.assertIn("broadcast prefix", bare.stderr)

        proc, log = self.run_with_log(["l1cmd", "*", "version"])
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("L1 1.24.11", proc.stdout)
        self.assertIn("CMD version", log)

    def test_l1_command_length_guard_runs_before_usb_write(self):
        long_command = "A" * 73
        proc, log = self.run_with_log(["--force", "l1cmd", long_command])

        self.assertEqual(proc.returncode, 2)
        self.assertIn("maximum safe L1 USB command text is 72 bytes", proc.stderr)
        self.assertNotIn("CMD " + long_command, log)

    def test_timeout_parser_rejects_bad_values(self):
        bad_text = subprocess.run(
            [str(BIN), "--timeout", "banana", "version"],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=10,
        )
        self.assertEqual(bad_text.returncode, 2)
        self.assertIn("invalid timeout", bad_text.stderr)

        bad_range = subprocess.run(
            [str(BIN), "--timeout", "-2", "version"],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=10,
        )
        self.assertEqual(bad_range.returncode, 2)
        self.assertIn("invalid timeout", bad_range.stderr)

    def test_raw_send_reports_mock_kernel_size_rejection(self):
        proc = self.run_ctl(
            ["raw-send", "00", "01", "02"],
            {"SGIL1_MOCK_MAX_WRITE": "2"},
        )

        self.assertEqual(proc.returncode, 1)
        self.assertIn("write failed", proc.stderr)

    def test_reset_device_is_guarded_and_uses_ioctl_when_forced(self):
        denied = self.run_ctl(["reset-device"])
        self.assertEqual(denied.returncode, 2)
        self.assertIn("requires --force", denied.stderr)

        proc, log = self.run_with_log(["--force", "reset-device"])
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("reset-device ok", proc.stdout)
        self.assertIn("IOCTL reset-device", log)


if __name__ == "__main__":
    unittest.main()
