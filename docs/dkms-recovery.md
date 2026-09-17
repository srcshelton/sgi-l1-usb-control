# Recovering from a DKMS upgrade error

An error such as this means that DKMS still has a driver registration whose
source directory is missing:

```text
Error! sgi-l1-usb/0.1.1 is broken! Missing the source directory or the symbolic link pointing to it.
Manual intervention is required!
```

Packages before version 0.1.57 could leave these registrations behind during
upgrades. Install the current `sgi-l1-usb-dkms` package from the
[releases page](https://github.com/srcshelton/sgi-l1-usb-control/releases),
following the [installation guide](../README.md#installation). It repairs or
moves aside affected obsolete registrations during configuration.

## Complete the upgrade

After installing the current driver package, resume any unfinished package
configuration and check the result:

```sh
sudo dpkg --configure --pending
sudo dpkg --audit
sudo apt-get check
sudo dkms status
```

An empty `dpkg --audit` result indicates that no package consistency problems
were found. `apt-get check` checks package dependencies. In `dkms status`, the
expected `sgi-l1-usb` version should be listed as `installed` for each target
kernel.

To retry a build for a particular installed kernel, replace `KERNEL_VERSION`
with its full release string, for example `6.18.50+rpt-rpi-v8`:

```sh
sudo dkms autoinstall -k KERNEL_VERSION
```

The target kernel may be newer than the running kernel reported by `uname -r`.
Check `dkms status` again after the build. If it reports another error, follow
its diagnostic and check the build log at the path it gives before retrying.

## Recovered registrations

The package restores a missing source link when the original source directory
still exists. When both are missing for an affected obsolete registration,
it saves the registration beneath:

```text
/var/lib/sgi-l1-usb-dkms/recovery/
```

The installation output gives the missing paths and the exact recovery
location. Each saved entry contains a `README` and the original
`registration/` directory. These entries remain available after package
removal or purge.

A recovered registration may have installed driver files for older kernels.
List those files with:

```sh
sudo find /lib/modules -type f -name 'sgi_l1_usb.ko*' -print
```

Keep the saved entry while investigating any such installation. To remove an
old version through DKMS, restore its matching original source tree and saved
registration, then run:

```sh
sudo dkms remove -m sgi-l1-usb -v VERSION --all
```

Replace `VERSION` with that registration's version. The restored sources must
belong to the same release. The recovery directory's `README` identifies the
original paths.
