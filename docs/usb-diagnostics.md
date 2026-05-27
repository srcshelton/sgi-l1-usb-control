# Software-First USB Diagnostics

Some hosts can fail to enumerate the SGI L1 USB device cleanly, or may
disconnect and re-enumerate the controller after malformed or oversized
transfers. A VID/PID driver cannot bind until the host USB stack has received a
device descriptor, so these checks focus on host-side USB state before the
`sgi_l1_usb` module can do useful work.

Run the passive diagnostic script first:

```sh
sudo ./scripts/usb-diagnostics.sh
```

The script records:

- kernel version and machine model, where available;
- selected `usbcore` parameters;
- `lsusb` output;
- `/sys/kernel/debug/usb/devices`, when mounted and readable;
- recent USB-related kernel log messages;
- attached `ttyUSB` devices, which are useful when comparing USB transport
  behaviour with a separate serial console.

Runtime experiments that do not require a reboot, if the sysfs parameters are
writable on the running kernel:

```sh
echo Y | sudo tee /sys/module/usbcore/parameters/old_scheme_first
echo Y | sudo tee /sys/module/usbcore/parameters/use_both_schemes
echo 10000 | sudo tee /sys/module/usbcore/parameters/initial_descriptor_timeout
```

Then re-enumerate only the affected L1 USB path if possible, either by
unplugging and reconnecting the L1 USB cable or by power-cycling only the
affected managed-hub port.

Experiments which require a reboot:

1. Add `usbcore.old_scheme_first=Y usbcore.initial_descriptor_timeout=10000` to
   the kernel command line;
2. Reboot the host;
3. Check whether `065e:1234` appears in `lsusb` and whether `sgi_l1_usb` binds;
4. If the platform provides selectable USB host-controller modes, test those
   separately and record the resulting controller driver in `dmesg`.
