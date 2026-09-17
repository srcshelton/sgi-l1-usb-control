# USB troubleshooting

If Linux does not detect the L1, or the connection repeatedly drops, first
check whether the controller appears on the USB bus:

```sh
lsusb -d 065e:1234
```

A detected controller appears with the ID `065e:1234`. If detection is
unreliable, compare another USB port or cable, or a connection through a
powered USB hub. The choice of host USB controller can matter, particularly
on Raspberry Pi systems.

## Collect connection details

From the source directory, run:

```sh
sudo ./scripts/usb-diagnostics.sh
```

The script reads the host's USB state and prints a report containing:

- the Linux kernel version and machine model, where available;
- USB settings and the devices reported by `lsusb`;
- detailed USB device information, when available through debugfs;
- recent USB-related kernel messages;
- attached USB serial devices, including any separate serial console.

Save the output when comparing connections or reporting a problem.

## Try alternative USB detection settings

Linux provides settings for older USB devices that need a different detection
sequence or more time to respond. On kernels that expose writable settings,
the following changes take effect for subsequent USB connections:

```sh
echo Y | sudo tee /sys/module/usbcore/parameters/old_scheme_first
echo Y | sudo tee /sys/module/usbcore/parameters/use_both_schemes
echo 10000 | sudo tee /sys/module/usbcore/parameters/initial_descriptor_timeout
```

These settings apply to the Linux host's USB subsystem. Record their original
values in the diagnostic report before changing them. Reconnect the L1 USB
cable, then repeat `lsusb -d 065e:1234` and `sgil1ctl probe`. The runtime changes
last until reboot; writing the original values restores them sooner.

The corresponding persistent settings go on the Linux kernel command line:

```text
usbcore.old_scheme_first=Y usbcore.use_both_schemes=Y usbcore.initial_descriptor_timeout=10000
```

After rebooting, check detection again with `lsusb` and `sgil1ctl probe`.
Remove the added kernel parameters and reboot to restore the previous
settings.
