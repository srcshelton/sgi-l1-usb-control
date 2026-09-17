# Installation reference

See the [main guide](../README.md#installation) for release packages on Debian
and Raspberry Pi OS.

Source archives are available with each
[release](https://github.com/srcshelton/sgi-l1-usb-control/releases).
Extract an archive before using the build instructions below.

## Build Debian packages

Run these commands from the repository root on the target Linux host.

1. Install the build dependencies:

   ```sh
   sudo apt update
   sudo apt install build-essential debhelper dh-dkms dkms libncurses-dev pkgconf sparse "linux-headers-$(uname -r)"
   ```

2. Build the packages:

   ```sh
   make deb
   ```

3. Install the driver and one tool edition. For the command-line edition:

   ```sh
   sudo apt install ./_build/sgi-l1-usb-dkms_*_all.deb ./_build/sgil1ctl_*_*.deb
   ```

   For the edition with the interactive terminal display:

   ```sh
   sudo apt install ./_build/sgi-l1-usb-dkms_*_all.deb ./_build/sgil1ctl-tui_*_*.deb
   ```

4. Complete the module loading, group membership and connection steps in the
   [main guide](../README.md#installation).

Both tool editions provide `sgil1ctl`; installing one replaces the other.

`make test` runs the test suite. `make test-deb` runs the tests and then builds
packages. See the [test guide](../tests/README.md) and
[live hardware validation guide](live-hardware-validation.md) for details.

## Manual installation

The source build needs a C compiler, Make and headers for the running Linux
kernel. The interactive terminal display also needs the wide-character ncurses
development library and `pkg-config`.

Run these commands from the repository root.

1. Build the kernel module and command-line tool:

   ```sh
   make -C module KDIR=/lib/modules/$(uname -r)/build
   make -C tools
   ```

   To build the tool with the interactive terminal display, use:

   ```sh
   make -C tools WITH_TUI=1
   ```

2. Install the module and tool:

   ```sh
   sudo install -D -m 0644 module/sgi_l1_usb.ko \
     /lib/modules/$(uname -r)/extra/sgi_l1_usb.ko
   sudo depmod -a
   sudo install -D -m 0755 tools/sgil1ctl /usr/local/bin/sgil1ctl
   ```

3. Create the access group and install the udev rules:

   ```sh
   getent group sgil1 >/dev/null || sudo groupadd --system sgil1
   sudo install -D -m 0644 udev/99-sgi-l1-usb.rules \
     /etc/udev/rules.d/99-sgi-l1-usb.rules
   sudo udevadm control --reload
   sudo modprobe sgi_l1_usb
   ```

4. Complete the group membership and connection steps in the
   [main guide](../README.md#installation).

Rebuild and install the module for each new kernel. Debian DKMS packages
perform this step automatically during kernel upgrades.

`NCURSES_CFLAGS` and `NCURSES_LIBS` can override the flags from
`pkg-config ncursesw` for terminal builds.

## Device access

The udev rules give the `sgil1` group read and write access to the devices
(mode `0660`). This access includes power and reset operations.

| Path | Purpose |
| --- | --- |
| `/dev/sgi-l1/l1-N` | L1 data device with index `N` |
| `/dev/sgi-l1/status` | Controller status device |
| `/dev/sgil1_N` | Original name of the data device |
| `/dev/sgil1_cs` | Original name of the status device |
| `/dev/usb/sgil1_N` | Alternative data device location on some systems |

`sgil1ctl probe` displays the selected paths. `sgil1ctl` automatically finds
available devices; `--device PATH` and `--status-device PATH` select them
explicitly.

To apply updated udev permissions to connected devices:

```sh
sudo udevadm control --reload
sudo udevadm trigger --subsystem-match=usbmisc
sudo udevadm trigger --subsystem-match=misc
```

### Systems with manual device management

After loading the module and connecting the L1, read the device numbers from
sysfs. For data device index `0`, the paths are:

```sh
cat /sys/class/usbmisc/sgil1_0/dev
cat /sys/class/misc/sgil1_cs/dev
```

Each output is `MAJOR:MINOR`. Substitute the corresponding numbers below,
and use the connected data device's index in its filename:

```sh
sudo mknod /dev/sgil1_0 c MAJOR MINOR
sudo mknod /dev/sgil1_cs c MAJOR MINOR
sudo chgrp sgil1 /dev/sgil1_0 /dev/sgil1_cs
sudo chmod 0660 /dev/sgil1_0 /dev/sgil1_cs
```

## Original SGI L2/L3 software

The driver supports the original SGI device names and compatibility options
for SGI's Linux L2/L3 tools. Load the module with these options before starting
the SGI daemon:

```sh
sudo modprobe sgi_l1_usb legacy_status_ioctl=1 legacy_reset_pipes=1
l2 -usb -nodiscover
l2cmd --l2 127.0.0.1 "l1 version"
```

`legacy_status_ioctl` enables SGI's status-revision query.
`legacy_reset_pipes` enables SGI's USB endpoint reset sequence.
Both default to `0`. Module options passed to `modprobe` take effect when
it loads the module.

The [L2/L3 container guide](../contrib/l2-l3-container/README.md) covers SGI
software archives and the `container-podman`, `container-docker` and
`container-apple` build targets.

## Further help

- [USB troubleshooting](usb-diagnostics.md)
- [DKMS package recovery](dkms-recovery.md)
