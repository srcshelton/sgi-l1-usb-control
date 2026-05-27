# SPDX-License-Identifier: GPL-2.0-or-later

import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MODULE = ROOT / "module" / "sgi_l1_usb.c"
UDEV = ROOT / "udev" / "99-sgi-l1-usb.rules"
POSTINST = ROOT / "debian" / "sgi-l1-usb-dkms.postinst"
CONTROL = ROOT / "debian" / "control"


class KernelDriverStaticTests(unittest.TestCase):
    def test_module_builds_with_warning_and_sparse_checks(self):
        subprocess.run(["make", "-C", str(ROOT / "module"), "clean"], check=True)
        subprocess.run(["make", "-C", str(ROOT / "module"), "W=1"], check=True)
        subprocess.run(
            ["make", "-C", str(ROOT / "module"), "C=2", "CF=-D__CHECK_ENDIAN__"],
            check=True,
        )

    def test_modinfo_exposes_expected_metadata_and_parameters(self):
        subprocess.run(["make", "-C", str(ROOT / "module")], check=True)
        info = subprocess.check_output(
            ["modinfo", str(ROOT / "module" / "sgi_l1_usb.ko")],
            text=True,
        )

        self.assertIn("author:         Stuart Shelton <stuart@shelton.me>", info)
        self.assertIn("description:    USB transport driver for SGI L1 system controllers", info)
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
        self.assertRegex(src, r"if \(count > sgi_l1_max_write_size\(dev\)\)\s+return -EMSGSIZE;")

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


if __name__ == "__main__":
    unittest.main()
