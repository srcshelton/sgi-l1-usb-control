# Live Fuel USB Validation

Run these checks from the Raspberry Pi physically connected to the Fuel L1.
Keep only one `sgil1ctl` follow/watch process running at a time: the tool's
advisory lock intentionally serializes access to the USB command path.

Store the results somewhere persistent for comparison:

```sh
out="$HOME/sgil1-validation-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out"
```

## Read-Only Baseline

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
```

Compare the LED captures. Raw `l1cmd leds` and `--debug leds` may contain
`0x7f` or `0xff`; normal `leds` output should suppress them while retaining all
other populated current/history slots. `--debug` should name the source used
for every decoded value.

## Bounded Follow Tests

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

`log --follow` and `watch` should report increasing queue-pressure levels after
new `USB_WQUE Q full` messages, avoid treating `Q full`/`Q avail` as burst
activity, and recover levels gradually after ten-second quiet periods. LED-only
follow cannot see log queue messages and should instead back off from failed
responses up to 500 ms. Use `watch` when both streams and shared pressure
control are required.

Do not deliberately flood the L1 to provoke `Q full`. If the messages occur
naturally, retain the complete `Q avail - lost: ... repl: ...` lines. A
non-overlap warning means the circular log advanced farther than snapshot
polling could recover.

## TUI Checks

Install the `sgil1ctl-tui` package in place of `sgil1ctl`, then run:

```sh
sudo sgil1ctl watch --tui
sudo sgil1ctl watch --tui --no-alternate-screen
```

For each mode, verify `q`, `Tab`, Up/Down, Page Up/Page Down, and `End`. Resize
the terminal above and below 100 columns: wide mode should place LEDs to the
right of the larger log pane, while narrow mode should place the smaller LED
pane above the log. If a harmless PROM-console character can be entered from an
existing console, the header should briefly show console input activity without
adding `0x7f` or `0xff` to the LED pane.

## Transport Recovery

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

## Power And Boot Diagnostics

Run these only in a maintenance window. First confirm that the workstation is
already off, then validate the unguarded power-up and LED capture path:

```sh
sudo sgil1ctl power check
sudo sgil1ctl power up --follow 2>&1 | tee "$out/30-power-up-follow.txt"
```

The output should retain `Power-up: confirmed workstation appears on` before
continuing LED follow. End it with Ctrl-C after PROM or IRIX has reached a
stable state. Test `power down`, `power down --force`, or
`power reset --force --follow` only when their shutdown/reset effects and any
possible data loss have been explicitly accepted. Never use a forced action as
a way to manufacture monitor traffic.

## Acceptance Criteria

- Read-only commands consistently return complete framed responses.
- Normal LED output has no `0x7f`/`0xff`; raw/debug evidence retains them.
- Follow output contains no isolated response characters or stale-frame loops.
- Polling slows under observed queue pressure and later recovers gradually.
- No unexplained increase appears in L1 USB errors, stalls, or timeouts.
- Text watch and both TUI screen modes remain responsive during new output.
- Disconnect/reconnect recovery is bounded and resumes without manual pipe
  resets.
