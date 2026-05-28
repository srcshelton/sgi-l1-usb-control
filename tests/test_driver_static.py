# SPDX-License-Identifier: GPL-2.0-or-later

import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MODULE = ROOT / "module" / "sgi_l1_usb.c"
MODULE_MAKEFILE = ROOT / "module" / "Makefile"
DKMS_TEMPLATE = ROOT / "dkms.conf.in"
DEBIAN_RULES = ROOT / "debian" / "rules"
CHANGELOG = ROOT / "debian" / "changelog"
MAKEFILE = ROOT / "Makefile"
VERSION_SCRIPT = ROOT / "scripts" / "package-version.sh"
UDEV = ROOT / "udev" / "99-sgi-l1-usb.rules"
POSTINST = ROOT / "debian" / "sgi-l1-usb-dkms.postinst"
CONTROL = ROOT / "debian" / "control"
CI_WORKFLOW = ROOT / ".github" / "workflows" / "ci.yml"


class KernelDriverStaticTests(unittest.TestCase):
    def current_version(self):
        return subprocess.check_output(
            ["sh", str(VERSION_SCRIPT), str(CHANGELOG)],
            cwd=ROOT,
            text=True,
        ).strip()

    def make_print_version(self, env=None):
        return subprocess.check_output(
            ["make", "--no-print-directory", "-s", "print-version"],
            cwd=ROOT,
            env=env,
            text=True,
        ).strip()

    def test_module_builds_with_warning_and_sparse_checks(self):
        subprocess.run(["make", "-C", str(ROOT / "module"), "clean"], check=True)
        subprocess.run(["make", "-C", str(ROOT / "module"), "W=1"], check=True)
        subprocess.run(
            ["make", "-C", str(ROOT / "module"), "C=2", "CF=-D__CHECK_ENDIAN__"],
            check=True,
        )

    def test_modinfo_exposes_expected_metadata_and_parameters(self):
        version = self.current_version()

        subprocess.run(["make", "-C", str(ROOT / "module")], check=True)
        info = subprocess.check_output(
            ["modinfo", str(ROOT / "module" / "sgi_l1_usb.ko")],
            text=True,
        )

        self.assertIn("author:         Stuart Shelton <stuart@shelton.me>", info)
        self.assertIn("description:    USB transport driver for SGI L1 system controllers", info)
        self.assertRegex(info, rf"(?m)^version:\s+{re.escape(version)}$")
        self.assertNotRegex(info, r"(?m)^version:\s+unknown$")
        self.assertIn("parm:           reset_on_close:", info)
        self.assertIn("parm:           max_write_size:", info)
        self.assertIn("ic00isc00ipFF", info)
        self.assertIn("icFFiscFFip00", info)

    def test_kernel_source_keeps_lifetime_and_recovery_invariants(self):
        src = MODULE.read_text()

        self.assertIn("kref_get_unless_zero", src)
        self.assertIn("usb_queue_reset_device", src)
        self.assertNotIn("usb_reset_device(dev->udev)", src)
        self.assertIn("mutex_trylock(&dev->io_mutex)", src)
        self.assertIn("GFP_ATOMIC", src)
        self.assertIn("usb_clear_halt(dev->udev, pipe)", src)
        self.assertNotIn("usb_get_std_status", src)
        self.assertIn("module_param(max_write_size", src)
        self.assertIn('#define SGI_L1_VERSION\t\t"unknown"', src)
        self.assertIn('MODULE_VERSION(SGI_L1_VERSION)', src)
        self.assertNotIn("sgi-l1-usb 0.", src)
        self.assertRegex(src, r"if \(count > sgi_l1_max_write_size\(dev\)\)\s+return -EMSGSIZE;")

    def test_versions_derive_from_debian_changelog(self):
        version = self.current_version()
        make_version = self.make_print_version()
        module_make = MODULE_MAKEFILE.read_text()
        dkms_template = DKMS_TEMPLATE.read_text()
        debian_rules = DEBIAN_RULES.read_text()
        makefile = MAKEFILE.read_text()
        version_script = VERSION_SCRIPT.read_text()
        changelog = CHANGELOG.read_text()
        generated_dkms = dkms_template.replace("@VERSION@", version)

        self.assertEqual(make_version, version)
        self.assertTrue(changelog.startswith(f"sgi-l1-usb-control ({version}) "))
        self.assertIn("scripts/package-version.sh", makefile)
        self.assertIn("../scripts/package-version.sh", module_make)
        self.assertIn("dpkg-parsechangelog", version_script)
        self.assertIn("sed -n", version_script)
        self.assertIn('ccflags-y += -DSGI_L1_VERSION=', module_make)
        self.assertIn('SGI_L1_VERSION=$(SGI_L1_VERSION)', module_make)
        self.assertIn('PACKAGE_VERSION="@VERSION@"', dkms_template)
        self.assertIn("SGI_L1_VERSION=@VERSION@", dkms_template)
        self.assertNotIn(version, dkms_template)
        self.assertIn(f'PACKAGE_VERSION="{version}"', generated_dkms)
        self.assertIn(f"SGI_L1_VERSION={version}", generated_dkms)
        self.assertIn("dkms.conf.in", debian_rules)
        self.assertIn("DEB_VERSION", debian_rules)
        self.assertNotIn("DEB_VERSION_UPSTREAM", debian_rules)

    def test_non_debian_source_build_uses_changelog_version_without_dpkg_tools(self):
        version = self.current_version()

        with tempfile.TemporaryDirectory() as tmpdir:
            fake_dpkg = Path(tmpdir) / "dpkg-parsechangelog"
            fake_dpkg.write_text("#!/bin/sh\nexit 127\n")
            fake_dpkg.chmod(0o755)
            env = os.environ.copy()
            env["PATH"] = f"{tmpdir}:{env.get('PATH', '')}"

            make_version = self.make_print_version(env=env)
            self.assertEqual(make_version, version)

            subprocess.run(["make", "-C", str(ROOT / "module"), "clean"], env=env, check=True)
            subprocess.run(["make", "-C", str(ROOT / "module")], env=env, check=True)

        info = subprocess.check_output(
            ["modinfo", str(ROOT / "module" / "sgi_l1_usb.ko")],
            text=True,
        )
        self.assertRegex(info, rf"(?m)^version:\s+{re.escape(version)}$")
        self.assertNotRegex(info, r"(?m)^version:\s+unknown$")

    def test_kernel_source_keeps_legacy_abi_names_and_interface_matches(self):
        src = MODULE.read_text()

        self.assertIn('.name = "sgil1_%d"', src)
        self.assertIn('.name = "sgil1_cs"', src)
        self.assertIn("SGI_L1_MINOR_BASE\t208", src)
        self.assertIn("SGI_L1_STATUS_MINOR\t249", src)
        self.assertIn("USB_DEVICE_AND_INTERFACE_INFO", src)
        self.assertIn("USB_CLASS_VENDOR_SPEC", src)
        self.assertIn("0, 0, 0xff", src)

    def test_udev_rules_are_generic_and_group_restricted(self):
        rules = UDEV.read_text()

        self.assertIn(
            'KERNEL=="sgil1_cs", GROUP="sgil1", MODE="0660", SYMLINK+="sgi-l1/status"',
            rules,
        )
        self.assertIn(
            'KERNEL=="sgil1_[0-9]*", GROUP="sgil1", MODE="0660", SYMLINK+="sgi-l1/l1-%n"',
            rules,
        )
        self.assertNotIn("fuel", rules.lower())
        self.assertNotIn("uaccess", rules.lower())

    def test_package_creates_sgil1_group(self):
        postinst = POSTINST.read_text()
        control = CONTROL.read_text()

        self.assertIn("addgroup --system sgil1", postinst)
        self.assertIn("getent group sgil1", postinst)
        self.assertIn("Depends: adduser, dkms, udev", control)

    def test_ci_release_gate_backfills_missing_release_assets(self):
        workflow = CI_WORKFLOW.read_text()

        self.assertIn('tag="v${current}"', workflow)
        self.assertIn("gh release view \"$tag\" --json assets", workflow)
        self.assertIn('"sgi-l1-usb-dkms_${current}_all.deb"', workflow)
        self.assertIn('"sgil1ctl_${current}_amd64.deb"', workflow)
        self.assertIn('"sgil1ctl_${current}_arm64.deb"', workflow)
        self.assertIn("[ -z \"$missing_assets\" ]", workflow)
        self.assertIn("Release $tag is missing; publishing current version", workflow)
        self.assertIn("Release $tag is missing expected asset(s)", workflow)


if __name__ == "__main__":
    unittest.main()
