# Hardware compatibility

`sgi_l1_usb` and `sgil1ctl` connect a Linux host to an SGI system through its
L1 USB management port. Hardware testing for this project has used a Fuel.
The tables below distinguish that tested configuration from systems with a
corresponding interface documented by SGI.

## MIPS systems

The intended hardware includes Fuel and the Chimera-generation Tezro,
Origin 350, Onyx 350 and Onyx4 systems. SGI's documentation places them in
the L1 controller family and describes the following connections:

| System | L1 connection documented by SGI |
| --- | --- |
| Fuel | A USB-B maintenance port; see the [Fuel hardware guide](https://techpubs.jurassic.nl/library/manuals/4000/007-4480-001/sgi_html/apa.html) |
| Tezro tower | An L1 diagnostic USB-B port on the I/O daughtercard; see the [Tezro system overview](https://techpubs.jurassic.nl/library/manuals/4000/007-4564-001/sgi_html/ch02.html) |
| Tezro rack-mount | A rear L1 USB-B port; see the [rack-mount system overview](https://techpubs.jurassic.nl/library/manuals/4000/007-4643-002/sgi_html/ch02.html) |
| Origin 350 | An L1 USB-B port for connection to an L2 controller; see the [compute module guide](https://techpubs.jurassic.nl/library/manuals/4000/007-4566-001/sgi_html/ch03.html) |
| Onyx 350 | An L1 USB-B port on the compute module; see the [compute module guide](https://techpubs.jurassic.nl/library/manuals/4000/007-4632-001/sgi_html/ch03.html) |
| Onyx4 | L1 system control for the compute modules and an L1 USB-B port on the graphics bricks; see the [quick-start guide](https://techpubs.jurassic.nl/library/manuals/4000/007-4667-001/sgi_html/ch01.html) and [system overview](https://techpubs.jurassic.nl/library/manuals/4000/007-4634-002/sgi_html/ch01.html) |

The Fuel guide describes USB-B as a maintenance connection, and the Tezro
rack-mount guide marks its L1 USB port as unused in the documented setup.
Compatibility with the other listed MIPS systems is expected from their shared
L1 interface and remains subject to testing on each model.

SGI's [L1/L2 controller guide](https://techpubs.jurassic.nl/library/manuals/3000/007-3938-005/sgi_html/ch01.html)
also describes the earlier Origin 300/3000 and Onyx 300/3000 controller family.
The shared command interface provides model-specific power, environmental,
configuration and diagnostic facilities.

## Itanium systems

SGI documents the same type of upstream L1 USB connection on the
[Altix 350 base compute module](https://techpubs.jurassic.nl/library/manuals/4000/007-4660-001/sgi_html/ch03.html)
and the [rack-mount Prism compute and graphics modules](https://techpubs.jurassic.nl/library/manuals/4000/007-4701-004/sgi_html/ch01.html).
These are candidates for compatibility testing. Their firmware images and
processor diagnostics need separate qualification from the MIPS systems.

These connection details are specific to the listed models. The
[Prism Deskside guide](https://techpubs.jurassic.nl/library/manuals/4000/007-4772-001/sgi_html/ch01.html)
describes a serial L1 console, while the
[Altix 4700 controller description](https://techpubs.jurassic.nl/library/manuals/3000/007-3938-005/sgi_html/ch01.html)
covers system controllers communicating over NUMAlink and Ethernet.

## Firmware and diagnostic descriptions

SGI firmware identifies images with labels such as `Fuel/PE` and
`Fuel/PE/O300`. These are firmware families, not a restriction to a Fuel
chassis. For example, these [Chimera system notes](https://just.graphica.com.au/tips/sgi-o350-chimera-notes/)
include controller version output with the `Fuel/PE/O300` label. SGI's
[system-controller release notes](https://techpubs.jurassic.nl/library/manuals/4000/007-4576-009/pdf/007-4576-009.pdf)
cover several hardware families within one software release.

The controller supplies system identity, sensor readings, environmental limits
and log messages. Supplementary LED descriptions come from SGI's Fuel
diagnostic manual, the L1/L2 controller guide, and IP35 PROM and L1 firmware
tables. `--show-annotations` identifies the source of each LED description;
**p** toggles these labels in the TUI.

Descriptions retain the terminology of their sources, including Fuel-specific
names such as IP34 and PIMM. The [diagnostic reference guide](diagnostic-references.md)
explains their scope, the firmware families used for LED interpretation, and
the differences between the model-specific manuals.
