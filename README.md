# SGI L1 USB Control

Modern Linux driver and tooling for the Silicon Graphics L1/L3 USB transport
used by early-2000s SGI system controllers.

The L1 USB device is not a UART.  The original Linux driver exposed a raw bulk
transport for SGI's IRouter protocol.  This project keeps the legacy transport
ABI as a first target and adds a small `sgil1ctl` utility for diagnostics and
future safe power/status commands.

## Current Scope

- Kernel module: `sgi_l1_usb`
- USB ID: `065e:1234`
- Legacy data devices: `/dev/sgil1_0` through `/dev/sgil1_39`
- Legacy status device: `/dev/sgil1_cs`
- Stable udev links: `/dev/sgi-l1/status`, `/dev/sgi-l1/l1-0`, and
  `/dev/sgi-l1/fuel-l1`

The driver preserves the old write framing rule: user space writes a complete
frame with at least two bytes, and the driver overwrites bytes 0 and 1 with the
big-endian total frame length before sending the frame on the bulk OUT endpoint.

Protocol commands such as `power up`, `power down`, `reset`, `env`, and `power`
are intentionally not sent by `sgil1ctl` yet.  They need confirmed IRouter
packet formats from captures or further reverse engineering.

## Build

```sh
make
```

Build only the kernel module:

```sh
make -C module
```

Build only the user-space tool:

```sh
make -C tools
```

Build Debian packages:

```sh
make deb
```

## Manual Test Flow

1. Load the module on a host that can enumerate the L1:

   ```sh
   sudo insmod module/sgi_l1_usb.ko
   ```

2. Confirm the device registered:

   ```sh
   lsusb -d 065e:1234
   ./tools/sgil1ctl probe
   ```

3. Try non-destructive reads first:

   ```sh
   ./tools/sgil1ctl status
   ./tools/sgil1ctl read-cfg
   ./tools/sgil1ctl raw-recv
   ```

4. Use `raw-send` only with known-good complete frames.

The module parameter `reset_on_close=1` restores the original driver's
reset-on-close behavior for compatibility experiments.  It defaults to `0`.
