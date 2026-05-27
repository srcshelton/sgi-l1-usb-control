# Raspberry Pi Software-First USB Checks

The current Pi 500 sees the L1 connection as repeated full-speed setup/address
failures before descriptors are readable.  A normal VID/PID driver cannot bind
until the kernel receives a descriptor, so these checks are about host-side USB
initialisation rather than the `sgi_l1_usb` module.

Run the passive diagnostic script first:

```sh
sudo ./scripts/pi-software-first-diagnostics.sh
```

Runtime experiments that do not require a reboot, if the sysfs parameters are
writable on the running kernel:

```sh
echo Y | sudo tee /sys/module/usbcore/parameters/old_scheme_first
echo Y | sudo tee /sys/module/usbcore/parameters/use_both_schemes
echo 10000 | sudo tee /sys/module/usbcore/parameters/initial_descriptor_timeout
```

Then unplug/replug only the L1 USB connection if physical access is available,
or power-cycle only the affected USB hub port if a managed hub is in use.

Manual reboot experiments for later:

1. Add `usbcore.old_scheme_first=Y usbcore.initial_descriptor_timeout=10000` to
   `/boot/firmware/cmdline.txt`.
2. Reboot manually, resume the session, and check whether `065e:1234` appears.
3. Only after that, experiment with Raspberry Pi USB overlays.  On Pi 5/500 the
   visible external ports are normally xHCI/RP1-backed, so older Pi 4 `otg_mode`
   expectations may not apply.

Do not use `pwr up`, `pwr down`, or reset commands as part of USB enumeration
testing.
