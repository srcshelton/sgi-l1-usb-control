# SGI L1 USB Control

This project lets you manage SGI systems with an L1 system controller from a
modern Linux computer. It provides a USB driver and the `sgil1ctl` command-line
tool, which can read system information, set the controller's clock and operate
the system's power controls. You can also monitor the L1 log and diagnostic LED
states, either as text or in an interactive terminal display.

The Linux computer connects to the SGI system's L1 USB port. The L1 runs from
standby power, so its status information and controls remain available while
the main system is switched off.

## Compatible hardware

The project targets the L1 USB interface found on SGI Fuel and the
Chimera-generation systems: Tezro workstations in both tower and rack-mount
forms, Origin 350, Onyx 350 and Onyx4. These machines share the L1 system-control
interface, with commands appropriate to each model. Hardware testing of this
project has so far used a Fuel.

SGI also documents L1 USB connections on Itanium-based Altix 350 and rack-mount
Prism systems. The [hardware compatibility notes](docs/hardware.md) describe
the individual models, firmware families and supporting SGI documentation.

## Installation

The [latest release](https://github.com/srcshelton/sgi-l1-usb-control/releases/latest)
provides packages for Debian and Raspberry Pi OS. Install the
`sgi-l1-usb-dkms` driver package together with one of these tool packages:

- **`sgil1ctl`** provides the command-line interface, including text-based log
  and LED monitoring.
- **`sgil1ctl-tui`** provides the same commands and adds an interactive terminal
  display for combined log and LED monitoring.

Both editions install the `sgil1ctl` command, and installing one replaces the
other. The driver package is marked `all`; choose the tool package that matches
your Linux computer's architecture. You can find that with:

```sh
dpkg --print-architecture
```

Download the driver and your chosen tool package into a directory of their own.
From that directory, install the packages and headers for your running kernel:

```sh
sudo apt update
sudo apt install build-essential "linux-headers-$(uname -r)" ./*.deb
```

DKMS builds the driver for the installed kernel and rebuilds it during kernel
upgrades. Load the driver and reload the device rules:

```sh
sudo udevadm control --reload
sudo modprobe sgi_l1_usb
```

The packages create an `sgil1` group for access to the controller. Membership
includes permission to operate the power and reset controls. To give your
account access, replace `USERNAME` with your login name:

```sh
sudo usermod -aG sgil1 USERNAME
```

Log out and back in to apply the new group membership. Connect the L1 USB
cable, or reconnect it if it was already attached during installation.

For other Linux distributions, or to build your own packages, see the
[source build and manual installation guide](docs/installation.md).

## Basic usage

You can check the connection and read an overview of the SGI system with:

```sh
sgil1ctl probe
sgil1ctl status
```

`probe` shows the available controller devices. `status` gathers the firmware
version, system identity, clock, power and environmental information into one
report. Individual commands provide more focused output:

```sh
sgil1ctl date
sgil1ctl power check
sgil1ctl env
sgil1ctl log
sgil1ctl leds
```

`sgil1ctl` finds the controller automatically. If you have more than one
connected, you can select a device by placing `--device PATH` before the
command:

```sh
sgil1ctl --device /dev/sgi-l1/l1-0 status
```

Run `sgil1ctl --help` for a command summary, or add `--help` to a command for
its options, for example `sgil1ctl log --help`.

### Monitoring logs and LEDs

The `log` command displays the L1 log, while `leds` shows the current and recent
diagnostic LED states with descriptions. Add `--follow` (or `-w`) to either
command to keep displaying new information:

```sh
sgil1ctl log --follow
sgil1ctl leds --follow
```

The `watch` command combines both displays:

```sh
sgil1ctl watch
```

Press **Ctrl-C** to stop monitoring. Repeated log messages are summarised by
default; `--no-repeat-summary` displays them individually in `log` and `watch`.

With the `sgil1ctl-tui` edition installed, you can open the interactive display:

```sh
sgil1ctl watch --tui
```

The log and LED panes appear side by side in a wide terminal and one above the
other in a narrower window. Press **Tab** to select a pane, then use the arrow
keys or **PgUp/PgDn** to scroll through its history. **End** returns to live
output, **h** opens help, and **q** quits.

Some LED status lines are hidden by default. Press **a** to switch between
the **Filtered** and **All** views. To see where LED descriptions come from,
press **p**, or add `--show-annotations` to `leds` or `watch`.
The [monitoring reference](docs/usage.md#interactive-terminal-display)
covers the remaining keys, colour palettes, history sizes and polling options.

### Setting the clock

The L1 clock can be set from the Linux computer:

```sh
sgil1ctl date --set-time
```

By default, this updates the clock when the difference is at least 60 seconds
and uses the Linux computer's timezone. `--drift-seconds SEC` changes the
threshold, and `--timezone TZ` selects a timezone explicitly.

### Power and reset controls

`power up` turns on the SGI system. `power down` sends a single power-down
signal, while `power down --force` sends the second signal used to force the
system off:

```sh
sgil1ctl power up
sgil1ctl power down
sgil1ctl power down --force
```

There are separate commands to reset the SGI system and its L1 controller.
Both require `--force`:

```sh
sgil1ctl power reset --force
sgil1ctl reset --force
```

The first issues a soft reset to the SGI system; the second restarts the L1
controller itself. Adding `--follow` to a power or reset command displays LED
changes during the operation.

### Waiting for a controller

`sgil1ctl wait` waits for an L1 USB device to become available and then reads
its status. With `--background`, it waits for the next connection event.
Options are also available to set the clock, operate the power controls and
monitor LEDs after connection. These are described in
`sgil1ctl wait --help` and the [command reference](docs/usage.md#actions-on-connection).

### Debug settings and direct L1 commands

`sgil1ctl debug` displays and decodes the virtual debug switches, together
with the L1's current debug settings. `sgil1ctl debug --list-switches` lists
the available switch names, diagnostic modes and boot-stop points. Changes
use explicit options and `--force`; see `sgil1ctl debug --help` for details.

You can send commands directly to the L1 with `l1cmd`. For example:

```sh
sgil1ctl l1cmd version
sgil1ctl l1cmd flash status
```

`sgil1ctl l1cmd help` lists the commands supplied by the connected controller's
firmware. The [command reference](docs/usage.md#direct-l1-commands) also covers
command arguments and the SGI broadcast prefix.

## Troubleshooting and further documentation

For USB connection problems, `lsusb -d 065e:1234` checks whether Linux has
detected the controller. The [USB troubleshooting guide](docs/usb-diagnostics.md)
contains further checks, including host-controller and Raspberry Pi advice.
For package upgrade errors, see [DKMS recovery](docs/dkms-recovery.md).

The [installation reference](docs/installation.md) covers device permissions,
manual installation and use with the original SGI L2/L3 tools. The
[command reference](docs/usage.md) contains the full monitoring options and
links to SGI's controller manuals.

`sgil1ctl --version` reports the installed tool version. `sgil1ctl --help-all`
includes the additional device and protocol diagnostics, and `--debug` adds
raw communication diagnostics when placed before a command.

## License

The source is licensed under GPL-2.0-or-later. See [COPYING](COPYING).
