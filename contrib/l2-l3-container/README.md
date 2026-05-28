# SGI L2/L3 Legacy Tool Container

This directory provides a minimal container build recipe for testing SGI's
original L2/L3 user-space tools against the `sgi_l1_usb` compatibility driver.

The SGI RPM is not part of this repository. Supply a local copy of
`snxsc_l3-1.62.0-1.i386.rpm`, an extracted `rootfs`, or a local CD-IST archive.
The repository make targets validate the payload and build the image:

```sh
make container-podman SGI_L3_RPM=/path/to/snxsc_l3-1.62.0-1.i386.rpm
make container-docker SGI_L3_ARCHIVE=/path/to/cd-ist-3.24.taz
```

For convenience only, `SGI_L3_FETCH=1` asks the build helper to fetch the public
CD-IST 3.24 archive referenced by Graphica's L2/L3 notes:

```sh
make container-podman SGI_L3_FETCH=1
```

The primary URL is:

```text
https://www.graphica.com.au/files/cd-ist-3.24.taz
```

A fallback URL discussed by IRIX Network users is also encoded in the helper,
but that archive has been reported as possibly truncated.

If you already have the RPM payload extracted, place it under `rootfs`:

```sh
mkdir -p rootfs
cd rootfs
rpm2cpio /path/to/snxsc_l3-1.62.0-1.i386.rpm | cpio -id
cd ..
make container-podman
```

For the closest match to SGI's intended USB workflow, load the driver with the
legacy ioctl and reset-pipes paths, run `l2` as the process which owns
`/dev/sgil1_*`, and use `l2cmd` against the daemon:

```sh
sudo modprobe sgi_l1_usb legacy_status_ioctl=1 legacy_reset_pipes=1
sudo podman run --rm --security-opt seccomp=unconfined \
  --device /dev/sgil1_0 --device /dev/sgil1_cs \
  sgil1-l2-l3-tools 'l2 -usb -nodiscover'
```

From another shell in the same container or host network namespace, use
read-only commands first:

```sh
l2cmd --l2 127.0.0.1 version
l2cmd --l2 127.0.0.1 "l1 version"
l2cmd --l2 127.0.0.1 "l1 power"
l2cmd --l2 127.0.0.1 "l1 env"
l2cmd --l2 127.0.0.1 "l1 usb"
```

Direct `l2cmd --scdev` probes are useful diagnostics but are not the recommended
proof of compatibility for a single attached L1. They may time out even when
the daemon-mediated SGI tool path is working:

```sh
sudo podman run --rm --security-opt seccomp=unconfined \
  --device /dev/sgil1_0 --device /dev/sgil1_cs \
  sgil1-l2-l3-tools 'l2cmd --scdev /dev/sgil1_0 --irtr version'
```

The SGI L3 package examined for this compatibility work contains utilities and
man pages only; no explicit vendor test suite or verifier was present. Use
read-only commands first (`version`, `date`, `env`, `power`) and verify the L1
remains responsive with `sgil1ctl status` after each legacy-tool attempt.
