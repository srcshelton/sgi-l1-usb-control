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

    def test_version_uses_mock_irouter_transport_without_debug_noise(self):
        proc, log = self.run_with_log(["version"])

        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("L1 1.24.11", proc.stdout)
        self.assertNotIn("sent L1 command", proc.stdout)
        self.assertIn("CMD version", log)

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

        self.assertIn("LEDs: power-off standby", stdout)
        self.assertIn("LEDs: power-on diagnostics", stdout)
        self.assertEqual(stdout.count("LEDs: power-on diagnostics"), 1, stdout)
        self.assertNotIn("LEDs follow: leds command failed", stderr)

    def test_reset_follow_starts_leds_follow(self):
        stdout, stderr, _returncode = self.run_follow_for(
            ["reset", "--force", "--follow"],
            {"SGIL1_MOCK_LEDS_FOLLOW": "1"},
            seconds=0.5,
        )

        self.assertIn("reset issued", stdout)
        self.assertIn("LEDs: power-off standby", stdout)
        self.assertIn("LEDs: power-on diagnostics", stdout)
        self.assertNotIn("LEDs follow: leds command failed", stderr)

    def test_wait_follow_requires_power_up_or_reset(self):
        proc = self.run_ctl(["wait", "--follow"])

        self.assertEqual(proc.returncode, 2)
        self.assertIn("wait --follow requires --power-up or --reset", proc.stderr)

    def test_wait_power_up_follow_starts_leds_follow(self):
        stdout, stderr, _returncode = self.run_follow_for(
            ["wait", "--power-up", "--force", "--follow"],
            {"SGIL1_MOCK_POWER": "off", "SGIL1_MOCK_LEDS_FOLLOW": "1"},
            seconds=2.2,
        )

        self.assertIn("Power-up: workstation appears off; issuing power up", stdout)
        self.assertIn("LEDs: power-off standby", stdout)
        self.assertIn("LEDs: power-on diagnostics", stdout)
        self.assertNotIn("LEDs follow: leds command failed", stderr)

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
            "Environment",
            "USB Transport",
        ]:
            self.assertIn(heading, proc.stdout)
        self.assertIn("Environmental monitoring is enabled", proc.stdout)
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

    def test_power_up_requires_force_then_confirms_state_after_timeout(self):
        denied = self.run_ctl(["power", "up"], {"SGIL1_MOCK_POWER": "off"})
        self.assertEqual(denied.returncode, 2)
        self.assertIn("add --force", denied.stderr)

        proc, log = self.run_with_log(
            ["--force", "power", "up"],
            {"SGIL1_MOCK_POWER": "off", "SGIL1_MOCK_POWER_UP_TIMEOUT": "1"},
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("confirmed workstation appears on", proc.stdout)
        self.assertIn("CMD power up", log)
        self.assertIn("CMD power check", log)

    def test_power_down_confirmation_prompt_triggers_second_command(self):
        proc, log = self.run_with_log(
            ["power", "down", "--force"],
            {"SGIL1_MOCK_POWER": "on", "SGIL1_MOCK_POWER_DOWN_CONFIRM": "1"},
        )

        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("requested a second power down command", proc.stdout)
        self.assertEqual(log.count("CMD power down"), 2, log)
        self.assertIn("confirmed workstation appears off", proc.stdout)

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
