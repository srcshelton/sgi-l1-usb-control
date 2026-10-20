# Command reference

See the [main guide](../README.md) for installation and basic commands.
`sgil1ctl COMMAND --help` lists the options for each command.

## Device selection and diagnostics

Global options go before the command:

```sh
sgil1ctl --device /dev/sgi-l1/l1-0 status
sgil1ctl --debug l1cmd leds
```

| Option | Purpose |
| --- | --- |
| `--device PATH` | Select the L1 data device; default: automatic |
| `--status-device PATH` | Select the status device; default: automatic |
| `--timeout MS` | Set the read timeout; default: 3000 milliseconds |
| `--debug` | Show raw USB protocol diagnostics |
| `--show-annotations` | Identify the sources of LED descriptions, including during power/reset follow |

The `debug` command displays SGI debug settings. The global `--debug` option
adds communication diagnostics to command output.

`sgil1ctl probe` shows the selected devices. `sgil1ctl --help-all` lists
additional device and protocol diagnostics.

## Logs and LEDs

```sh
sgil1ctl log --follow
sgil1ctl leds --follow
sgil1ctl watch
```

Each command runs until **Ctrl-C**. `-w` is an alias for `--follow`.

| Command option | Meaning | Default |
| --- | --- | --- |
| `log --poll-interval MS` | Log polling interval; minimum 100 ms | 1000 ms |
| `leds --poll-interval MS` | LED polling interval; minimum 50 ms | 100 ms |
| `watch --log-interval MS` | Log polling interval when quiet; minimum 100 ms | 1000 ms |
| `watch --led-interval MS` | LED polling interval when active; minimum 100 ms | 100 ms |
| `log --no-repeat-summary` | Print repeated messages individually | Summarise repeats |
| `watch --no-repeat-summary` | Print repeated messages individually | Summarise repeats |

The LED display includes descriptions of current and recent states. The default
filter hides input-wait, raw PROM value and console-character-read entries
on recognised MIPS firmware.
`sgil1ctl l1cmd leds` displays the raw L1 response; the terminal display's
**All** view includes the hidden entries.

Add `--show-annotations` to `leds` or `watch` to identify descriptions supplied
by the controller and those added from a reference. The
[diagnostic reference guide](diagnostic-references.md#how-led-descriptions-are-selected)
explains the sources and their firmware scope.

## Interactive terminal display

The `sgil1ctl-tui` package provides the interactive display:

```sh
sgil1ctl watch --tui
```

The log and LED panes appear side by side in wide terminals and stacked in
narrow terminals. Each pane has its own scroll position. In side-by-side
layouts, **Left/Right** or **<**/**>** move the divider one column to the left
or right, whichever pane is selected. In stacked layouts, select a pane with
**Tab**, then use these keys or **^**/**v** to shrink or grow its height by one
row. The display keeps your width and height settings for the current session.

| Key | Action |
| --- | --- |
| **Tab** | Select the log or LED pane |
| **Left/Right**, **<**/**>** | Move the divider left/right; shrink/grow the selected pane when stacked |
| **^**/**v** | Shrink or grow the selected pane when stacked |
| **Up/Down**, **k/j** | Scroll one line |
| **PgUp/PgDn**, **Ctrl-B/F** | Scroll one page |
| **End**, **g** | Return to live output |
| **a** | Switch between Filtered and All views |
| **t** | Show or hide LED timestamps |
| **p** | Show or hide LED description sources, including in retained history |
| **c** | Cycle colour palettes |
| **m** | Toggle monochrome |
| **h**, **?** | Show help |
| **Esc**, **Return** | Close help |
| **\*** | Open operator power/reset/clock controls |
| **Ctrl-L** | Redraw the screen |
| **q** | Quit |

The **Filtered** view summarises repeated log messages and LED states.
The **All** view shows every retained observation, including hidden LED entries.
Repeated filtered LED states show a sample count even when timestamps are off.
With **t** enabled, the timestamp above the state is its first retained
observation, and the repeat summary gives its latest observation. Counts and
times cover retained samples, so they decrease or advance as history expires.
These are observed snapshots, not a count of distinct firmware events.

Press **\*** (also the first Help entry) to open **Operator controls**.
The menu reads the host power state and the L1 date and timezone on entry and
on return. It checks again when an action is selected, before asking for
confirmation, and rechecks host power state immediately before sending a
power/reset command. Unavailable host actions remain visible, dimmed or italic
and marked with `*`: **u** (power up) requires an off host, while **d** (power
down) and **r** (host soft reset/restart) require an on host. If host state is
unknown, those actions are unavailable. **l** resets the L1 controller and can
interrupt USB; **s** sets the L1 clock and timezone from the machine running
`sgil1ctl`. The menu shows the L1 date and its approximate difference from
that machine's time, using years/days or smaller units as appropriate.

Type `YES` and press **Return** to send the selected command once. Blank or
other input followed by **Return** aborts the action and returns to the menu.
**Esc** or **\*** closes Operator controls; **q** still quits the application.
If the L1 requests a second power-down signal, that signal needs a separate
`YES` confirmation. Commands and responses appear in both log views. An
acknowledgement does not mean a power transition or reboot has finished.
Timed-out commands are reported as having an unknown outcome and are not
retried. Power actions resume LED/log monitoring without waiting for a
power-state confirmation, so early boot diagnostics can be captured.

The status bar shows the L1 clock immediately before console activity. Clock
readings refresh in idle polling slots, approximately every five seconds;
the displayed time advances between readings. Automatic clock reads pause
for five seconds after power/reset requests to preserve early LED capture,
and use a short response timeout. `[out of sync]` means the
estimated difference from the `sgil1ctl` host exceeds five seconds. Controller
timezone is used for comparison. Date replies with whole seconds cannot
provide a precise subsecond measurement, so differences are approximate.
Unavailable readings are labelled accordingly. Any non-empty LED response,
including hidden entries, and each newly received log line refreshes
**Console last active**; folding or hiding messages does not suppress activity.

If USB is absent at startup or disappears while monitoring, the TUI opens a
mandatory **Waiting for USB** dialog. **t** toggles clock sync on connection;
**u**, **d** and **r** select power up, power down or host reset, each requiring
`YES` plus **Return** to arm. **n** clears the armed power action; selecting a replacement disarms the
previous action until the new choice is confirmed. **Esc** and
**Return** cannot dismiss the waiting dialog; **q** quits the application.
Unconfirmed input is discarded if USB appears. Confirmed actions run once,
with inapplicable host actions skipped, then normal LED/log following resumes.
A later disconnect starts a fresh wait with no actions armed. The controller
lock is held only while connected, and queued power actions are never retried.

Operator controls are a guard against accidental input, not an authentication
boundary. They use the same Linux device access and L1 protocol authority as
the command-line controls.

| Option | Meaning | Default |
| --- | --- | --- |
| `--log-history ENTRIES` | Number of log messages retained for scrolling | 4096 |
| `--led-history ENTRIES` | Number of LED observations retained for scrolling | 512 |
| `--palette NAME` | Colour palette | `auto` |
| `--no-alternate-screen` | Draw on the terminal's primary screen | Use alternate screen |

History limits accept up to 1,000,000 entries each. Histories last for the
current session.

Available palettes are `auto`, `monochrome`, `indigo`, `crimson`, `indy`,
`indigo2`, `onyx`, `challenge`, `impact`, `o2`, `octane`, `onyx2`, `octane2`,
`o2plus`, `fuel`, `tezro` and `personal-iris`. Automatic selection uses the
firmware image family reported by the L1. For example, `--palette fuel` selects the
Fuel palette explicitly.

## Clock

`sgil1ctl date` displays the L1 clock. `sgil1ctl date --set-time` sets it from
the Linux host when the difference reaches the threshold.

| Option | Meaning | Default |
| --- | --- | --- |
| `--timezone TZ` | Timezone to apply when setting the clock | Linux host's timezone |
| `--drift-seconds SEC` | Minimum clock difference before setting the time | 60 seconds |

`sgil1ctl set-date` is an alias for `sgil1ctl date --set-time`.

## Power and reset

| Command | Action |
| --- | --- |
| `sgil1ctl power` | Read L1 power data |
| `sgil1ctl power check` | Read the system power state |
| `sgil1ctl power up` | Power on the SGI system |
| `sgil1ctl power down` | Send one power-down signal |
| `sgil1ctl power down --force` | Send a second signal to force power off |
| `sgil1ctl power reset --force` | Issue a soft reset to the SGI system |
| `sgil1ctl reset --force` | Reset the L1 controller |

`--follow` or `-w` monitors LEDs during power and reset operations.

## Actions on connection

`sgil1ctl wait` waits for an L1 device and then reads its status. These options
select further actions:

| Option | Action |
| --- | --- |
| `--background` | Wait for the next USB connection event |
| `--wait-timeout SEC` | Limit the wait to this number of seconds |
| `--set-time` | Set the L1 clock from the Linux host |
| `--timezone TZ` | Select the timezone when setting the clock |
| `--drift-seconds SEC` | Set the clock difference threshold; default: 60 seconds |
| `--power-up --force` | Power on if the SGI system reports that it is off |
| `--power-down --force` | Force the SGI system to power off |
| `--reset --force` | Reset the SGI system |
| `--follow` | Monitor LEDs after the selected power action |
| `--keepalive SEC` | Continue checking the connection at this interval and wait again after disconnection |

The default wait has no time limit. `--keepalive` defaults to `0`, which ends
the command after the requested work completes.

With the TUI edition, `sgil1ctl wait --tui` opens the same mandatory waiting
dialog and then follows logs and LEDs. Existing `--set-time`, `--timezone`,
`--drift-seconds` and guarded power options can preselect connection actions:

```sh
sgil1ctl wait --tui --set-time --power-up --force
```

`--background` waits for a disconnect/reconnect even if USB is already present.
TUI waiting has no timeout and automatically returns to waiting on disconnect;
`--wait-timeout` and `--keepalive` apply only to text mode. No `--follow` flag is
needed in TUI mode.

## Debug settings

`sgil1ctl debug` displays the virtual debug switches and the L1 `l1dbg`
settings. `sgil1ctl debug --list-switches` lists switch names, diagnostic modes
and boot-stop points.

The following options change the virtual debug switches:

| Option | Action |
| --- | --- |
| `--enable SWITCH... --force` | Enable named switches |
| `--disable SWITCH... --force` | Disable named switches |
| `--set SWITCHES --force` | Replace the switch value |
| `--test MODE --force` | Select a diagnostic testing mode |
| `--boot-stop POINT --force` | Select a PROM boot-stop point; `none` clears it |

For example, `sgil1ctl debug --boot-stop none --force` clears the boot-stop
setting.

## Direct L1 commands

`sgil1ctl l1cmd help` lists the commands available from the connected L1.
Pass command arguments after `l1cmd`:

```sh
sgil1ctl l1cmd version
sgil1ctl l1cmd flash status
sgil1ctl l1cmd '*' version
```

Quote `'*'` to send the SGI broadcast prefix. `--force` confirms guarded power
and reset commands, and permits commands absent from the L1 help list.

## SGI manuals

- [L1 and L2 Controller Software User's Guide, 007-3938-001](https://irix7.com/techpubs/007-3938-001.pdf), including the MIPS LED table
- [L1 and L2 Controller Software User's Guide, 007-3938-006](https://irix7.com/techpubs/007-3938-006.pdf), for later Altix systems
- [Fuel Visual Workstation Diagnostic Reference Manual, 108-0350-002](https://techpubs.jurassic.nl/manuals/hdwr/service/108-0350-002.pdf), the main source of supplementary LED descriptions
- [Origin 3000 system-control chapter](https://techpubs.jurassic.nl/library/manuals/4000/007-4240-001/sgi_html/ch03.html)
- [flashsc manual page](https://help.graphica.com.au/irix-6.5.30/man/1M/flashsc)
- [System Controller Software 1.5 Update Guide](https://www.infania.net/misc1/sgi_techpubs/techpubs/007-4576-006.pdf)
- [System Controller Software 1.14 Update Guide](https://www.infania.net/misc1/sgi_techpubs/techpubs/007-4576-015.pdf)

The [diagnostic reference comparison](diagnostic-references.md) explains the
scope of these sources and links to the Tezro, Origin 350 and Onyx manuals.
