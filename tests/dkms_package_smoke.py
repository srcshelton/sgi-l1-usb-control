#!/usr/bin/env python3
"""Exercise real dpkg/DKMS transitions in a disposable root environment."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess


NAME = "sgi-l1-usb"
PACKAGE = NAME + "-dkms"
REGISTRY = Path("/var/lib/dkms") / NAME
RECOVERY = Path("/var/lib/sgi-l1-usb-dkms/recovery")


def run(*args, success=True):
    result = subprocess.run(args, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    print("+", " ".join(map(str, args)), flush=True)
    print(result.stdout, end="", flush=True)
    if success and result.returncode:
        raise RuntimeError(f"command exited {result.returncode}: {args}")
    if not success and not result.returncode:
        raise RuntimeError(f"command unexpectedly succeeded: {args}")
    return result.stdout


def source(version):
    return Path("/usr/src") / f"{NAME}-{version}"


def check_installed(version, kernel):
    status = run("dkms", "status", "-m", NAME, "-v", version, "-k", kernel)
    assert ": installed" in status, status
    assert run("modinfo", "-k", kernel, "-F", "version", "sgi_l1_usb").strip() == version


def package_fixture(package, work, current, version, legacy=False):
    root = work / ("fixture-" + version)
    run("dpkg-deb", "-R", str(package), str(root))
    control = root / "DEBIAN/control"
    control.write_text(control.read_text().replace(f"Version: {current}\n",
                                                   f"Version: {version}\n"))
    old_source = root / f"usr/src/{NAME}-{current}"
    new_source = root / f"usr/src/{NAME}-{version}"
    old_source.rename(new_source)
    config = new_source / "dkms.conf"
    config.write_text(config.read_text().replace(current, version))
    # Payload checksums changed when constructing the fixture.
    (root / "DEBIAN/md5sums").unlink(missing_ok=True)
    if legacy:
        # Reproduce the old package's upgrade omission and swallowed failures.
        (root / "DEBIAN/preinst").unlink(missing_ok=True)
        (root / "DEBIAN/postinst").write_text(
            f'#!/bin/sh\nset -e\nif [ "$1" = configure ]; then\n'
            f'    dkms add -m {NAME} -v {version}\nfi\n')
        (root / "DEBIAN/prerm").write_text(
            '#!/bin/sh\nset -e\ncase "$1" in\nremove|deconfigure)\n'
            f'    dkms remove -m {NAME} -v {version} --all >/dev/null 2>&1 || true\n'
            ';;\nesac\n')
    else:
        # Model the next release using the generated, version-bound scripts.
        for script in ("postinst", "prerm"):
            path = root / "DEBIAN" / script
            text = path.read_text()
            assert f"DKMS_VERSION={current}\n" in text
            path.write_text(text.replace(f"DKMS_VERSION={current}\n",
                                         f"DKMS_VERSION={version}\n"))
    for script in ("postinst", "prerm"):
        (root / "DEBIAN" / script).chmod(0o755)
    result = work / f"{PACKAGE}_{version}_all.deb"
    run("dpkg-deb", "--build", str(root), str(result))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--kernel-version", required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--allow-system-changes", action="store_true")
    args = parser.parse_args()
    if os.geteuid() != 0 or not args.allow_system_changes:
        parser.error("requires root and --allow-system-changes in a disposable container/CI runner")
    os.environ["DEBIAN_FRONTEND"] = "noninteractive"
    if REGISTRY.exists() or RECOVERY.exists() or list(Path("/usr/src").glob(NAME + "-*")):
        parser.error("requires a clean environment without existing SGI DKMS state")
    package = args.package.resolve()
    current = run("dpkg-deb", "-f", str(package), "Version").strip()
    kernel = args.kernel_version
    assert Path(f"/lib/modules/{kernel}/build/Makefile").is_file()
    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=False)
    # Keep even transient compiler/test files beneath the caller's work directory.
    (work / "tmp").mkdir()
    os.environ["TMPDIR"] = str(work / "tmp")

    legacy = package_fixture(package, work, current, "0.1.56", legacy=True)
    run("dpkg", "-i", str(legacy))
    run("dkms", "install", "-m", NAME, "-v", "0.1.56", "-k", kernel)
    check_installed("0.1.56", kernel)

    # A failed legacy cleanup must stop before dpkg replaces its source tree.
    legacy_config = source("0.1.56") / "dkms.conf"
    saved_config = work / "legacy-dkms.conf"
    shutil.copy2(legacy_config, saved_config)
    legacy_config.unlink()
    try:
        run("dpkg", "-i", str(package), success=False)
        assert source("0.1.56").is_dir()
        assert not source(current).exists()
    finally:
        shutil.copy2(saved_config, legacy_config)
    # Also exercise repair by the preinst, before unregistering that version.
    (REGISTRY / "0.1.56/source").unlink()

    for version in ("0.1.1", "0.1.2"):
        shutil.copytree(source("0.1.56"), source(version))
        config = source(version) / "dkms.conf"
        config.write_text(config.read_text().replace("0.1.56", version))
        run("dkms", "add", "-m", NAME, "-v", version)
    # The exact reported failure: a registration outlives its source directory.
    shutil.rmtree(source("0.1.1"))
    (REGISTRY / "0.1.1/sentinel").write_text("preserve orphan evidence\n")
    run("dkms", "remove", "-m", NAME, "-v", "0.1.1", "--all", success=False)
    # A missing source link with surviving sources should be repaired in place.
    (REGISTRY / "0.1.2/source").unlink()

    # Another driver's registration must survive migration unchanged.
    other_source = Path("/usr/src/sgi-l1-unrelated-test-1")
    other_source.mkdir()
    (other_source / "dkms.conf").write_text(
        'PACKAGE_NAME="sgi-l1-unrelated-test"\nPACKAGE_VERSION="1"\n'
        'BUILT_MODULE_NAME[0]="unrelated"\nDEST_MODULE_LOCATION[0]="/updates/dkms"\n')
    run("dkms", "add", "-m", "sgi-l1-unrelated-test", "-v", "1")

    output = run("dpkg", "-i", str(package))
    assert "unregistering legacy sgi-l1-usb/0.1.56 before unpacking" in output
    assert "recovered obsolete sgi-l1-usb/0.1.1 registration" in output
    assert not (REGISTRY / "0.1.56").exists()
    assert not source("0.1.56").exists()
    assert not (REGISTRY / "0.1.1").exists()
    backups = list(RECOVERY.glob("sgi-l1-usb-0.1.1.*"))
    assert len(backups) == 1
    backup = backups[0]
    assert (backup / "registration/sentinel").read_text() == "preserve orphan evidence\n"
    assert (backup / "README").is_file()
    assert (REGISTRY / "0.1.2/source/dkms.conf").is_file()
    assert ": added" in run("dkms", "status", "-m", "sgi-l1-unrelated-test", "-v", "1")
    check_installed(current, kernel)

    run("dpkg-reconfigure", PACKAGE)
    check_installed(current, kernel)
    run("dpkg", "-i", str(package))
    check_installed(current, kernel)
    assert list(RECOVERY.glob("sgi-l1-usb-0.1.1.*")) == backups

    future = package_fixture(package, work, current, "99.0")
    run("dpkg", "-i", str(future))
    assert not (REGISTRY / current).exists()
    assert not source(current).exists()
    check_installed("99.0", kernel)
    run("dpkg", "-i", str(package))
    assert not (REGISTRY / "99.0").exists()
    check_installed(current, kernel)

    # Removal failure must be visible and must not let dpkg erase the sources.
    (REGISTRY / current / "source").unlink()
    run("dpkg", "--remove", PACKAGE, success=False)
    assert (source(current) / "dkms.conf").is_file()
    run("dkms", "add", "-m", NAME, "-v", current)
    run("dpkg", "--purge", PACKAGE)
    assert not (REGISTRY / current).exists()
    assert not source(current).exists()
    run("modinfo", "-k", kernel, "sgi_l1_usb", success=False)
    assert backup.is_dir()

    # Fresh install after purge also repairs an orphan left by a purged package.
    shutil.rmtree(source("0.1.2"))
    run("dpkg", "-i", str(package))
    assert not (REGISTRY / "0.1.2").exists()
    assert len(list(RECOVERY.glob("sgi-l1-usb-0.1.2.*"))) == 1
    check_installed(current, kernel)
    run("dkms", "autoinstall", "-k", kernel)
    assert "broken" not in run("dkms", "status")
    run("dpkg", "--purge", PACKAGE)
    run("dkms", "remove", "-m", "sgi-l1-unrelated-test", "-v", "1", "--all")
    shutil.rmtree(other_source)
    assert not run("dpkg", "--audit").strip()
    print("PASS: failed/healthy legacy upgrade, orphan recovery, link repair, reconfigure, reinstall, "
          "future upgrade, downgrade, failed removal, purge, and fresh install", flush=True)


if __name__ == "__main__":
    main()
