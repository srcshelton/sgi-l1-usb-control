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
  emits IRouter responses for discovery, status, date, power, reset, help, and
  pass-through commands so guarded destructive paths can be tested without a
  connected workstation.

Run the normal suite and then build Debian packages:

```sh
make test-deb
```
