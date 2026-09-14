# Test Suite

Run the hardware-free suite:

```sh
make test
```

The suite has two layers:

- `test_driver_static.py` builds the DKMS module, runs kernel warning and sparse
  checks, validates `modinfo`, and checks source invariants for lifetime,
  endpoint recovery, write-size limiting, and legacy SGI device naming.
- `test_sgil1ctl_mock.py` runs the real `tools/sgil1ctl` binary against
  `mock_l1.so`, an `LD_PRELOAD` mock of the L1 USB/status devices. The mock
  emits IRouter responses for discovery, status, date, power, reset, help,
  pass-through commands, and combined log/LED monitoring so guarded destructive
  paths and queue-aware back-off can be tested without a connected workstation.

The default build remains free of ncurses dependencies. To compile-check the
optional TUI configuration locally:

```sh
make -C tools clean
make -C tools WITH_TUI=1
make -C tests mock_l1.so
python3 tests/tui_smoke.py
python3 tests/tui_terminal_smoke.py
```

The smoke test runs the TUI in a 120x24 pseudo-terminal, forces small log and
LED histories to wrap, and verifies the empty-filtered-state/sample-count
distinction. It also checks filtered/All views, timestamp and Help toggles,
scroll bounds, alternate navigation and redraw keys, automatic and cycled
hardware palettes, restoration of cached automatic selection, reversible
monochrome mode, context-sensitive `q`, zero-delay Escape handling, lower-case
`g` navigation, explicit automatic-palette labels, unpadded rendered-history
position markers, and targeted primary-screen teardown. The second test
requires GNU Screen; it runs the default and explicit primary-screen modes with
`TERM=screen-256color`, alternate-screen support both disabled and enabled, and
a reserved hardstatus row. It exits each combination with `q`, `SIGINT`, and
`SIGTERM`, then checks Screen's rendered state to ensure the footer is erased
and the returning shell prompt has a clean row.

Run the normal suite and then build Debian packages:

```sh
make test-deb
```
