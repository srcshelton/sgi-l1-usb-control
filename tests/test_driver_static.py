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
L2_L3_CONTAINER = ROOT / "contrib" / "l2-l3-container" / "Containerfile"
L2_L3_CONTAINER_README = ROOT / "contrib" / "l2-l3-container" / "README.md"
L2_L3_CONTAINER_SCRIPT = ROOT / "scripts" / "build-l2-l3-container.sh"


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
        self.assertIn("parm:           legacy_status_ioctl:", info)
        self.assertIn("parm:           legacy_reset_pipes:", info)
        self.assertIn("ic00isc00ipFF", info)
        self.assertIn("icFFiscFFip00", info)

    def test_kernel_source_keeps_lifetime_and_recovery_invariants(self):
        src = MODULE.read_text()

        self.assertIn("kref_get_unless_zero", src)
        self.assertIn("usb_queue_reset_device", src)
        self.assertNotIn("usb_reset_device(dev->udev)", src)
        self.assertIn("mutex_trylock(&dev->io_mutex)", src)
        self.assertIn("GFP_ATOMIC", src)
        self.assertIn("init_waitqueue_head(&dev->read_wait)", src)
        self.assertLess(
            src.index("init_waitqueue_head(&dev->read_wait)"),
            src.index("usb_set_intfdata(interface, dev)"),
        )
        self.assertIn("usb_clear_halt(dev->udev, pipe)", src)
        self.assertIn("usb_control_msg(dev->udev, usb_sndctrlpipe(dev->udev, 0)", src)
        self.assertIn("USB_REQ_SET_FEATURE", src)
        self.assertIn("USB_ENDPOINT_HALT", src)
        self.assertIn("msleep(20)", src)
        self.assertNotIn("usb_get_std_status", src)
        self.assertIn("module_param(max_write_size", src)
        self.assertIn("module_param(legacy_status_ioctl", src)
        self.assertIn("module_param(legacy_reset_pipes", src)
        self.assertIn("static bool legacy_status_ioctl;", src)
        self.assertIn("static bool legacy_reset_pipes;", src)
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

    def test_kernel_source_accepts_original_status_revision_ioctl(self):
        src = MODULE.read_text()
        header = (ROOT / "include" / "sgi_l1_ioctl.h").read_text()

        self.assertIn("#define SGIL1_ST_READ_REV_LEGACY\t_IOR(SGIL1_IOCTL_BASE, 6, int)", header)
        self.assertIn("case SGIL1_ST_READ_REV_LEGACY:", src)
        self.assertIn("if (!legacy_status_ioctl)", src)
        self.assertIn("copy_to_user((void __user *)arg, rev, sizeof(int))", src)
        self.assertNotIn("static bool legacy_status_ioctl = true;", src)
        self.assertIn("disabled by default", src)

    def test_l2_l3_container_targets_are_documented_and_scripted(self):
        makefile = MAKEFILE.read_text()
        readme = (ROOT / "README.md").read_text()
        container_readme = L2_L3_CONTAINER_README.read_text()
        containerfile = L2_L3_CONTAINER.read_text()
        script = L2_L3_CONTAINER_SCRIPT.read_text()

        subprocess.run(["sh", "-n", str(L2_L3_CONTAINER_SCRIPT)], check=True)
        self.assertIn("help:", makefile)
        self.assertIn("L2_L3_CONTAINER_SCRIPT := scripts/build-l2-l3-container.sh", makefile)
        self.assertIn("container-docker:", makefile)
        self.assertIn("container-podman:", makefile)
        self.assertIn("container-apple:", makefile)
        self.assertIn("$(L2_L3_CONTAINER_SCRIPT) docker", makefile)
        self.assertIn("$(L2_L3_CONTAINER_SCRIPT) podman", makefile)
        self.assertIn("$(L2_L3_CONTAINER_SCRIPT) container", makefile)
        self.assertIn("legacy_status_ioctl=1 legacy_reset_pipes=1", readme)
        self.assertIn("legacy_status_ioctl=1 legacy_reset_pipes=1", container_readme)
        self.assertIn("https://www.graphica.com.au/files/cd-ist-3.24.taz", script)
        self.assertIn("https://usftp.irixnet.org/sgi-tools/l3-emulator-linux.tar.gz", script)
        self.assertIn("SGI_L3_FETCH=1", script)
        self.assertIn("snxsc_l3-1.62.0-1.i386.rpm", script)
        self.assertIn("ARG TARGETPLATFORM=linux/386", containerfile)
        self.assertIn("FROM --platform=${TARGETPLATFORM}", containerfile)
        self.assertIn("test -x /opt/snxsc_l3/stand/sysco/bin/l2", containerfile)
        self.assertIn("test -x /opt/snxsc_l3/stand/sysco/bin/l2cmd", containerfile)
        self.assertIn("[ \"$(uname -s)\" = Darwin ]", script)
        self.assertIn("[ \"$(uname -m)\" = arm64 ]", script)
        self.assertIn("container build", script)
        self.assertIn("--arch \"${SGI_L3_CONTAINER_ARCH:-amd64}\"", script)

    def test_l2_l3_container_script_builds_with_existing_rootfs(self):
        containerfile = L2_L3_CONTAINER.read_text()

        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = Path(tmpdir)
            container_dir = tmp / "container"
            fakebin = tmp / "bin"
            log = tmp / "runtime.log"
            (container_dir / "rootfs" / "stand" / "sysco" / "bin").mkdir(parents=True)
            (container_dir / "Containerfile").write_text(containerfile)
            for tool in ("l2", "l2cmd"):
                path = container_dir / "rootfs" / "stand" / "sysco" / "bin" / tool
                path.write_text("#!/bin/sh\nexit 0\n")
                path.chmod(0o755)

            fakebin.mkdir()
            fake_podman = fakebin / "podman"
            fake_podman.write_text(
                "#!/bin/sh\n"
                f"printf '%s\\n' \"$@\" > {log}\n"
                "exit 0\n"
            )
            fake_podman.chmod(0o755)

            env = os.environ.copy()
            env["PATH"] = f"{fakebin}:{env.get('PATH', '')}"
            env["SGI_L3_CONTAINER_DIR"] = str(container_dir)
            env["SGI_L3_IMAGE"] = "sgil1-l2-l3-tools:test"

            subprocess.run(
                [str(L2_L3_CONTAINER_SCRIPT), "podman"],
                cwd=ROOT,
                env=env,
                check=True,
            )

            args = log.read_text()
            self.assertIn("build\n", args)
            self.assertIn("--platform\nlinux/386\n", args)
            self.assertIn("--build-arg\nTARGETPLATFORM=linux/386\n", args)
            self.assertIn("-t\nsgil1-l2-l3-tools:test\n", args)
            self.assertIn(f"-f\n{container_dir / 'Containerfile'}\n", args)

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
        self.assertIn("stale_notes=false", workflow)
        self.assertIn("sgi-l1-usb-control (${current})", workflow)
        self.assertIn("Release $tag notes contain Debian changelog metadata", workflow)
        self.assertIn("Changes in %s", workflow)
        self.assertIn("awk '", workflow)
        self.assertIn('gh release edit "$TAG" --notes-file release-notes.txt', workflow)


if __name__ == "__main__":
    unittest.main()
