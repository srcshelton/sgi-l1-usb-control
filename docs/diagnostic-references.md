# Diagnostic references and terminology

The main source of supplementary LED descriptions in `sgil1ctl` is the
**Fuel Visual Workstation Diagnostic Reference Manual, 108-0350-002**,
Table 3-6, printed pages 3-14 to 3-20. This is a Fuel-specific service manual.
Its table associates numeric LED values with diagnostic routines, functional
units, failure messages and components. The descriptions in the tool combine
information from these columns. See the [original table](https://techpubs.jurassic.nl/manuals/hdwr/service/108-0350-002.pdf#page=78).

The other manual source is the **L1 and L2 Controller Software User's Guide,
007-3938-001**, Table 3-6, printed pages 61-62. Its shorter table covers
`0x00` through `0x0a`. The overlapping entries describe the same early boot
operations as the Fuel table; it also supplies `0x08` and `0x09`, which the
Fuel table omits. It does not give replacement-component advice. See the
[controller guide's LED table](https://irix7.com/techpubs/007-3938-001.pdf#page=77).

The edition matters: **007-3938-006** is an Altix-focused revision and does
not contain that LED table. Its introduction directs readers of older
systems to earlier editions. Its Table 3-2 is the source of the tool's full
virtual debug-switch table. The project also uses IP35 PROM and L1 1.48.1
firmware tables, separately identified in the LED decoder's source labels.
See the [later guide's scope](https://irix7.com/techpubs/007-3938-006.pdf#page=3)
and [debug-switch table](https://irix7.com/techpubs/007-3938-006.pdf#page=88).

## Manuals for the other systems

SGI published separate hardware guides for Tezro, Origin 350, Onyx 350 and
Onyx4. Their relevant diagnostic sections are listed below. These are user
guides with a different scope from the Fuel service manual.

| System and manual | Relevant contents |
| --- | --- |
| [Tezro tower, 007-4564-001, chapter 4](https://techpubs.jurassic.nl/library/manuals/4000/007-4564-001/sgi_html/ch04.html) | Table 4-1 describes physical bezel LED colours. The chapter also describes environmental monitoring and power-on, offline and online diagnostics. |
| [Tezro rack-mount, 007-4643-002, pages 81-82](https://irix7.com/techpubs/007-4643-002.pdf#page=99) | Table 4-2 explains L1 voltage, fan, temperature and power-off messages. |
| [Origin 350, 007-4566-001, chapter 7](https://techpubs.jurassic.nl/library/manuals/4000/007-4566-001/sgi_html/ch07.html) | Table 7-2 explains the same categories of L1 environmental messages. |
| [Onyx 350, 007-4632-001, pages 144-145](https://irix7.com/techpubs/007-4632-001.pdf#page=166) | Table 8-2 closely follows the Origin 350 environmental-message table. |
| [Onyx4, 007-4634-002, pages 83-84](https://irix7.com/techpubs/007-4634-002.pdf#page=103) | Table A-2 explains L1 environmental messages for its bricks. |

The Fuel service manual remains the source for the full numeric LED table
used by this project. The guides above describe the diagnostics and
environmental messages for their respective systems; component-level advice
from the Fuel table applies specifically to Fuel.

## Differences that matter

The environmental-message tables have much in common, including voltage
warnings at a 10% deviation and faults at 20%. However, their differences go
beyond replacing the system name:

- **Temperature limits:** Origin 350 and Onyx 350 give advisory, critical
  and fault inlet temperatures of 30/35/40 °C at low altitude and 27/31/35 °C
  at high altitude. Tezro rack-mount describes enclosure temperature relative
  to a target: advisory above the target, critical at target +6 °C and fault
  at target +10 °C, with corresponding fan-speed settings. These are the
  descriptions in their respective tables, not interchangeable thresholds.
- **Onyx4 temperatures:** its table uses named limits and refers to `env`
  for details. The explanatory text repeats the advisory-limit wording for
  all three severities, so that text does not establish their actual values.
- **Fan faults:** Tezro rack-mount describes a fan falling below its minimum
  speed. Origin 350, Onyx 350 and Onyx4 instead describe maximum speed in the
  corresponding row. This is a disagreement between the documents, not
  evidence sufficient to choose one interpretation for all firmware.
- **Physical indicators:** the Tezro tower's bezel colours are a separate
  diagnostic display from the numeric virtual LED values read by `leds`.
- **Hardware coverage:** the Fuel manual's Table 3-1 explicitly excludes
  some IP35 router and hub tests from IP34 diagnostics. Its component advice
  therefore needs to remain associated with Fuel.

The first three comparisons use the environmental-message tables linked
above. The last two follow the [Tezro diagnostics chapter](https://techpubs.jurassic.nl/library/manuals/4000/007-4564-001/sgi_html/ch04.html)
and [Fuel power-on diagnostics chapter](https://techpubs.jurassic.nl/manuals/hdwr/service/108-0350-002.pdf#page=67).

## Generic terms for the project's own text

The project uses these terms in its own explanations, help and status
messages. Descriptions taken from SGI documentation retain the original
terminology so that they can be compared with their source.

| Source-specific wording | General term | Where the distinction matters |
| --- | --- | --- |
| Fuel or workstation | **SGI system**; **L1 controller** when referring to the controller itself | Power/reset help and status messages, and general documentation. |
| IP34 motherboard | **system board**, or **node board** when describing a compute node | The Fuel-derived failure descriptions, principally `0x91`-`0xb5`, name IP34. Tezro and Onyx 350 documentation describes IP53 node boards. This is a hardware difference, not just a product-name change. |
| PIMM | **processor hardware** | Fuel entries `0x81`-`0x87` identify PIMM as the failing component. Other models integrate processors differently; a generic explanation must not imply that the same replaceable module exists. |
| IO7 PROM | **I/O PROM** | Fuel entry `0x82`. This describes the role without asserting an equivalent board or replacement part on another model. |
| CPU A/B | **local processors** | Fuel entry `0x24` describes local arbitration. The controller guide also documents CPU C and D; actual CPU labels in controller output should be retained. |
| Brick, base module or workstation enclosure | **module**, or **enclosure** for airflow and temperature | The environmental-message explanations vary by chassis. Keep **system** for the whole machine, which can contain several modules. |

The board and processor distinctions are documented in the
[Tezro tower system overview](https://techpubs.jurassic.nl/library/manuals/4000/007-4564-001/sgi_html/ch02.html)
and the [Onyx 350 compute-module chapter](https://techpubs.jurassic.nl/library/manuals/4000/007-4632-001/sgi_html/ch03.html).
In particular, the latter describes processors soldered to the IP53 board
and an IO9 base I/O card. It does not establish a replacement diagnosis for
Fuel's `0x82` entry.

Terms such as DIMM, cache, hub and PROM already describe the relevant
components without selecting a particular chassis. `Fuel/PE` and
`Fuel/PE/O300` are literal firmware image labels and remain unchanged.
The diagnostic descriptions retain their source terminology; any future
model-specific component advice will need evidence for that model.

## How LED descriptions are selected

`sgil1ctl` displays descriptions supplied by the controller and fills in
missing descriptions for recognised MIPS firmware. This includes replies
that contain only a byte value, such as `CPU A: 0x02`. CPU identifiers,
module headings and absent-CPU messages remain as reported by the controller.

Use `sgil1ctl leds --show-annotations` or `sgil1ctl watch --show-annotations`
to show the source of each displayed description. In the TUI, **p** toggles
these labels for both live output and retained history. A **controller** label
identifies text supplied by the connected L1; other labels name the reference
used to add or correct a description. Fuel component names remain attached
to their original diagnostic reference.

The MIPS descriptions and the input-wait, raw PROM value and console-read
filter apply to firmware reporting the `Fuel/PE 1MB image` or
`Fuel/PE/O300 1MB image` family with an L1 1.x version. Other image families,
and replies received when firmware identification is unavailable, retain
the controller's text and all LED entries. **All** in the TUI and `--debug`
make the filtered entries visible on recognised firmware as well.

Corrections to existing text have a narrower scope: the recognised image must
report version **1.48.1**, and its text must match the known table entry.
That table repeats `PLED_JUMPRAMUOK` at both `0x3d` and `0x3e`; the Fuel manual
distinguishes the completed jump to UALIAS from the next jump to cached space.
The tool also expands the table's empty descriptions for `PLED_INITDCACHE`,
`PLED_JUMPRAMUOK` and `PLED_UARTBASE`, and its module-ID arbitration message.
Other nonempty descriptions remain the controller's own text.

The tool reads the firmware identity when monitoring starts and refreshes it
after transport failures. The TUI keeps the interpretation and source of each
retained observation, even if the controller later reports different firmware.

The distinction matters for Itanium systems. The later controller guide gives
`0x00` and `0x01` as kernel-idle states, whereas the MIPS table
describes early boot operations. Its examples also include blade prefixes
before CPU labels. See the
[Altix LED examples](https://irix7.com/techpubs/007-3938-006.pdf#page=100).
