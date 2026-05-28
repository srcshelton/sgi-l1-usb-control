# SGI L1 USB Control

Modern Linux driver and tooling for the Silicon Graphics L1/L3 USB transport
used by SGI L1 system controllers.

This project was developed to make it easier to power on an SGI Fuel
workstation whose dead Dallas and Snaphat clock-chip batteries prevent the
front-panel power button from working. It could also be useful with other
L1-equipped SGI systems, including Origin 300x and Onyx 300x configurations,
subject to the usual differences between L1 firmware revisions and brick types.

> **Raspberry Pi USB compatibility note:** CI publishes a Linux arm64
> `sgil1ctl` Debian package for arm64 hosts, but that does not imply that every
> arm64 USB host can enumerate the SGI L1 USB device. Some Raspberry Pi USB host
> stacks may be incompatible with the SGI L1 controller's USB interface. If
> `065e:1234` does not appear reliably in `lsusb` output, try a different
> type/speed of USB port (if available) or see whether connecting the L1 cable
> through a powered USB hub makes any difference.

The L1 USB port is not a UART. SGI's original Linux software exposed it as a
raw USB bulk transport carrying `IRouter` frames. This repository provides:

- `sgi_l1_usb`: an out-of-tree Linux kernel module with the legacy `/dev/sgil1*`
  ABI as the compatibility target;
- `sgil1ctl`: a small user-space tool for status, clock, power, log, and direct
  L1 command operations;
- Debian DKMS packaging and `udev` rules for easier installation;
- a hardware-free test suite using a mocked L1 transport.

## Scope And Safety

`sgil1ctl` is not a full replacement for SGI L2/L3 controller software. It
focuses on the operations needed for single-system maintenance:

- read firmware, USB, clock, serial/MAC, power, fan, environment, and log
  state;
- set the L1 clock from the host;
- wait for a newly bound L1 USB device, optionally setting time and powering
  on;
- issue `power up`, `power down`, and `reset` with explicit `--force`;
- pass through live help-listed L1 commands with `sgil1ctl l1cmd ...`.

Power and reset commands are deliberately guarded. Users who can open the L1
device can power-cycle attached hardware, so packaged installs restrict device
nodes to group `sgil1` with mode `0660`.

## Device Nodes

The kernel device names are legacy-compatible with the old SGI driver. The
`udev` links are the preferred stable names for local scripts and do not assume
a specific SGI workstation or server model.

- `/dev/sgil1_N`: legacy data device for L1 index `N`;
- `/dev/sgi-l1/l1-N`: stable `udev` alias for `/dev/sgil1_N`;
- `/dev/sgil1_cs`: legacy controller-status device;
- `/dev/sgi-l1/status`: stable `udev` alias for `/dev/sgil1_cs`;
- `/dev/usb/sgil1_N`: compatibility fallback path on systems that place USB
  character devices under `/dev/usb`.

The package creates the `sgil1` system group if needed. Add users who need
remote-management access to SGI L1 controllers to that group:

```sh
sudo usermod -aG sgil1 USERNAME
```

The user must start a new login session before the new group membership will be
visible.

## Debian Or DKMS Installation

Install build dependencies:

```sh
sudo apt-get install build-essential debhelper dkms sparse linux-headers-$(uname -r)
```

Run the tests and build packages:

```sh
make test
make deb
```

Install the resulting packages:

```sh
sudo apt install ./_build/sgi-l1-usb-dkms_*_all.deb ./_build/sgil1ctl_*_*.deb
```

Reload `udev` rules, load the module, and reconnect the L1 USB cable if needed:

```sh
sudo udevadm control --reload
sudo modprobe sgi_l1_usb
```

If a device is already connected and the old permissions are still visible,
trigger `udev` for the existing nodes:

```sh
sudo udevadm trigger --subsystem-match=usbmisc
sudo udevadm trigger --subsystem-match=misc
```

## Manual Installation

Manual installation is supported for non-Debian systems, or on systems where
DKMS is not available.

Build the module and tool:

```sh
make -C module KDIR=/lib/modules/$(uname -r)/build
make -C tools
```

Install the module:

```sh
sudo install -D -m 0644 module/sgi_l1_usb.ko \
  /lib/modules/$(uname -r)/extra/sgi_l1_usb.ko
sudo depmod -a
```

Install the tool:

```sh
sudo install -D -m 0755 tools/sgil1ctl /usr/local/bin/sgil1ctl
sudo install -D -m 0644 include/sgi_l1_ioctl.h \
  /usr/local/include/sgi-l1/sgi_l1_ioctl.h
```

Install `udev` rules and create the access group:

```sh
sudo groupadd --system sgil1 2>/dev/null || true
sudo install -D -m 0644 udev/99-sgi-l1-usb.rules \
  /etc/udev/rules.d/99-sgi-l1-usb.rules
sudo udevadm control --reload
sudo modprobe sgi_l1_usb
```

On non-`udev` systems, create device nodes from `sysfs` after the module is
loaded and an L1 has enumerated. The data-device major/minor can normally be
read from `/sys/class/usbmisc/sgil1_0/dev` and the status device from
`/sys/class/misc/sgil1_cs/dev`.

```sh
sudo mknod /dev/sgil1_0 c MAJOR MINOR
sudo mknod /dev/sgil1_cs c MAJOR MINOR
sudo chgrp sgil1 /dev/sgil1_0 /dev/sgil1_cs
sudo chmod 0660 /dev/sgil1_0 /dev/sgil1_cs
```

## Basic Usage

Confirm that Linux can see the USB device and that the driver is loaded:

```sh
lsusb -d 065e:1234
sgil1ctl probe
```

Run non-destructive status checks:

```sh
sgil1ctl status
sgil1ctl date
sgil1ctl log
```

Set the L1 clock from the host when drift exceeds the default threshold:

```sh
sgil1ctl date --set-time
```

Power control requires `--force`:

```sh
sgil1ctl power up --force
sgil1ctl power down --force
sgil1ctl reset --force
```

Wait for an L1 USB device to bind and then set time and power on if the system
appears off:

```sh
sgil1ctl wait --set-time --power-up --force
```

Leave a process armed for the next future USB bind, ignoring any device that is
already present:

```sh
sgil1ctl wait --deferred --set-time --power-up --force
```

Pass through an L1 command advertised by live `help` output:

```sh
sgil1ctl l1cmd version
sgil1ctl l1cmd flash status
sgil1ctl l1cmd '*' version
```

Use `sgil1ctl --help` for the normal command set and `sgil1ctl --help-all` for
developer and protocol-inspection options.

## Tests

Run the hardware-free test suite:

```sh
make test
```

The tests build the module with warning and sparse checks, validate driver
metadata and source invariants, and run `sgil1ctl` against an `LD_PRELOAD`
mock L1 USB/status transport. To run tests and then build packages:

```sh
make test-deb
```

## USB Diagnostics

If the host cannot enumerate the L1 as `065e:1234`, the kernel driver cannot
bind yet. See [`docs/usb-diagnostics.md`](docs/usb-diagnostics.md) for
host USB checks.

The L1 is sensitive to large USB transfers, the kernel module keeps the
original 4096-byte raw transport limit by default but also exposes a
`max_write_size` parameter. `sgil1ctl` automatically caps direct L1 text
commands before sending them.

```sh
sudo modprobe sgi_l1_usb max_write_size=109
```

The `reset_on_close=1` module parameter restores the original driver's
reset-on-close behaviour for compatibility. It defaults to `0`.

## References

- SGI L1 and L2 Controller Software User's Guide, 007-3938-006:
  <https://irix7.com/techpubs/007-3938-006.pdf>
- SGI Origin 3000 Series Owner's Guide system-control chapter:
  <https://techpubs.jurassic.nl/library/manuals/4000/007-4240-001/sgi_html/ch03.html>
- `flashsc(1M)` manual page, covering SGI L1/L2 firmware update paths:
  <https://help.graphica.com.au/irix-6.5.30/man/1M/flashsc>
- System Controller Software 1.14 Update Guide, 007-4576-015, documenting the
  original SGI system-controller software release and package names:
  <https://www.infania.net/misc1/sgi_techpubs/techpubs/007-4576-015.pdf>
- Notes on running SGI L2/L3 software on old Linux, with a link to archived
  IST/L2/L3 software version 3.24:
  <https://just.graphica.com.au/tips/creaky-old-fedora-core-linux/>
- Direct archive link referenced by the article above:
  <https://www.graphica.com.au/files/cd-ist-3.24.taz>

## License

The source is licensed under GPL-2.0-or-later. See [`COPYING`](COPYING). Source
files carry SPDX license identifiers.
