# DKMS upgrade recovery

Versions before 0.1.57 could leave old `sgi-l1-usb` registrations in
`/var/lib/dkms` after an upgrade removed their sources from `/usr/src`.
Kernel and header package hooks then reported the old version as broken.
Purging a newer driver package did not remove those older registrations.

Version 0.1.57 uses Debian's `dh_dkms` maintainer scripts to unregister the
correct version on upgrade and removal, and to build for the kernels selected
by the installed DKMS framework. Removal failures are no longer suppressed.
The package also checks that the registration was actually removed, because
older DKMS versions can print an error while returning a successful exit status.
Driver and user-space behavior are unchanged.

On an upgrade from an older package, the new pre-installation script removes
its registration before unpacking replaces its source files. A removal failure
stops the upgrade with the old sources still present. Resolve the reported
DKMS failure and retry the package installation.

During configuration, registrations older than 0.1.57 with missing source
links are repaired when their original `/usr/src/sgi-l1-usb-VERSION` directory
still exists. If both the usable link and that directory are absent, the
registration is moved out of DKMS's registry into a unique directory under:

```text
/var/lib/sgi-l1-usb-dkms/recovery/
```

The package prints the missing paths and the exact backup location. Each backup
contains a `README` and the original `registration/` directory. These backups
remain after removal or purge so that recovery evidence is not lost.
Only obsolete registrations belonging to `sgi-l1-usb` are migrated; registrations
with usable source links and other drivers are preserved.

Moving a registration does not unload a running driver or remove previously
installed `.ko` files. The normal installation of the new driver follows the
migration. To identify any remaining copies for older kernels:

```sh
sudo find /lib/modules -type f -name 'sgi_l1_usb.ko*' -print
```

Keep the backup if those old installations still need investigation. For a
complete DKMS-managed removal of an old version, restore its matching original
source tree and saved registration, then use `dkms remove -m sgi-l1-usb -v
VERSION --all`. Do not point an old version's source link at a different release.

After installing the fixed package, resume unfinished package configuration:

```sh
sudo dpkg --configure --pending
sudo dpkg --audit
sudo apt-get check
sudo dkms status
```

An empty `dpkg --audit` result means no package consistency problems were found.
The original missing-source diagnostic by itself does not establish that the
kernel package failed configuration: DKMS can skip a broken entry and continue.

To retry building registered drivers for a particular installed kernel, use
`sudo dkms autoinstall -k KERNEL_VERSION`, replacing `KERNEL_VERSION` with the
full target release, for example `6.18.50+rpt-rpi-v8`. Use the target kernel,
which may differ from the currently running kernel reported by `uname -r`.
Verify that the expected driver version is shown as `installed` for that
kernel in `dkms status`. If another error occurs, resolve it before continuing.
