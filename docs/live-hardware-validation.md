# Live hardware validation on Fuel

This procedure checks a new build on the project's tested hardware: a Fuel
connected by L1 USB to a Raspberry Pi. It covers command output, monitoring,
terminal behaviour and recovery after a disconnected cable. Run the checks
from that Pi, ending each monitor before starting the next.

Store the results somewhere persistent for comparison:

```sh
out="$HOME/sgil1-validation-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out"
```

## Read-only baseline

These commands must complete without stale one-character responses, repeated
32-frame drain warnings, or unexpected pipe resets:

```sh
sudo sgil1ctl probe 2>&1 | tee "$out/01-probe.txt"
sudo sgil1ctl version 2>&1 | tee "$out/02-version.txt"
sudo sgil1ctl status 2>&1 | tee "$out/03-status.txt"
sudo sgil1ctl l1cmd usb 2>&1 | tee "$out/04-usb-before.txt"
sudo sgil1ctl l1cmd leds 2>&1 | tee "$out/05-leds-raw.txt"
sudo sgil1ctl leds 2>&1 | tee "$out/06-leds-decoded.txt"
sudo sgil1ctl --debug leds 2>&1 | tee "$out/07-leds-debug.txt"
sudo sgil1ctl log 2>&1 | tee "$out/08-log.txt"
sudo sgil1ctl leds --show-annotations 2>&1 | tee "$out/09-leds-sources.txt"
```

Compare the LED captures. Raw `l1cmd leds` and `--debug leds` may contain
`0x7f`, `0xfe` or `0xff`; normal `leds` output should hide these entries on
recognised Fuel/PE firmware while retaining the other current/history slots.
`--show-annotations` should identify descriptions supplied by the controller
and those added from a reference. CPU identifiers and absent-CPU messages
should agree with the raw response.

## Monitoring

Run each test separately for five minutes. GNU `timeout` normally returns 124
when it ends the otherwise successful follow process.

```sh
sudo timeout --signal=INT --kill-after=5s 300 \
  sgil1ctl log --follow 2>&1 | tee "$out/10-log-follow.txt"

sudo timeout --signal=INT --kill-after=5s 300 \
  sgil1ctl leds --follow 2>&1 | tee "$out/11-leds-follow.txt"

sudo timeout --signal=INT --kill-after=5s 300 \
  sgil1ctl watch 2>&1 | tee "$out/12-watch.txt"
```

`log --follow` and `watch` should report slower polling when new
`USB_WQUE Q full` messages appear, then recover their polling rate after quiet
periods. LED follow should report failed responses and retry after a delay.

Do not deliberately flood the L1 to provoke `Q full`. If the messages occur
naturally, retain the complete `Q avail - lost: ... repl: ...` lines. A
non-overlap warning means the circular log advanced farther than snapshot
polling could recover.

## Interactive display

Install the `sgil1ctl-tui` package in place of `sgil1ctl`, then run:

```sh
sudo sgil1ctl watch --tui
sudo sgil1ctl watch --tui --no-alternate-screen
```

For each mode, verify `q`, `Tab`, Up/Down, Page Up/Page Down, and `End`.
Use `a` to compare Filtered and All views, `t` to toggle LED timestamps, and
`p` to show and hide description sources. Check that `p` also updates retained
observations when scrolling back. Open help with `h` and check the key list
at both a normal terminal size and the minimum size of 40 columns by 10 rows. Resize
the terminal above and below 100 columns: wide mode should place LEDs to the
right of the larger log pane, while narrow mode should place the smaller LED
pane above the log. If a harmless PROM-console character can be entered from an
existing console, the bottom-right `Console last active` age should reset
while the Filtered view continues to hide `0x7f` and `0xff`.

## Connection recovery

With the workstation otherwise stable, run `sgil1ctl watch`, disconnect only
the Raspberry Pi-to-L1 USB management cable, and reconnect it. The monitor
should use bounded failure delays, rediscover after repeated failures, and
resume both streams without emitting isolated response characters. Stop if the
L1 becomes unresponsive or USB bus-reset/error counters rise continuously.

After the follow and recovery checks, capture transport and host-kernel state:

```sh
sudo sgil1ctl l1cmd usb 2>&1 | tee "$out/20-usb-after.txt"
sudo sgil1ctl status 2>&1 | tee "$out/21-status-after.txt"
sudo dmesg | tail -n 250 | tee "$out/22-kernel-tail.txt"
diff -u "$out/04-usb-before.txt" "$out/20-usb-after.txt" \
  | tee "$out/23-usb-diff.txt" || true
```

## Power and boot diagnostics

Run these only in a maintenance window. First confirm that the workstation is
already off, then validate the unguarded power-up and LED capture path:

```sh
sudo sgil1ctl power check
sudo sgil1ctl power up --follow 2>&1 | tee "$out/30-power-up-follow.txt"
```

The output should retain `Power-up: confirmed system appears on` before
continuing LED follow. End it with Ctrl-C after PROM or IRIX has reached a
stable state. Test `power down`, `power down --force`, or
`power reset --force --follow` only when their shutdown/reset effects and any
possible data loss have been explicitly accepted. Never use a forced action as
a way to manufacture monitor traffic.

## Acceptance criteria

- Read-only commands consistently return complete framed responses.
- The default LED filter hides `0x7f`, `0xfe` and `0xff` on recognised firmware;
  raw/debug captures and the TUI All view retain them.
- Source labels appear when requested and describe the text actually shown.
- Follow output contains no isolated response characters or stale-frame loops.
- Polling slows under observed queue pressure and later recovers gradually.
- No unexplained increase appears in L1 USB errors, stalls, or timeouts.
- Text watch and both TUI screen modes remain responsive during new output.
- Disconnect/reconnect recovery is bounded and resumes without manual pipe
  resets.
