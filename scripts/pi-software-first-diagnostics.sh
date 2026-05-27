#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later

set -eu

echo "== kernel =="
uname -a

echo
echo "== model =="
if [ -r /proc/device-tree/model ]; then
	tr '\0' '\n' < /proc/device-tree/model
fi

echo
echo "== usbcore parameters =="
for p in old_scheme_first use_both_schemes initial_descriptor_timeout autosuspend; do
	if [ -r "/sys/module/usbcore/parameters/$p" ]; then
		printf "%s=" "$p"
		cat "/sys/module/usbcore/parameters/$p"
	fi
done

echo
echo "== lsusb =="
if command -v lsusb >/dev/null 2>&1; then
	lsusb || true
else
	echo "lsusb not installed"
fi

echo
echo "== debugfs USB devices =="
if [ -r /sys/kernel/debug/usb/devices ]; then
	cat /sys/kernel/debug/usb/devices
else
	echo "/sys/kernel/debug/usb/devices is not readable"
fi

echo
echo "== dmesg USB tail =="
dmesg | grep -i -E 'usb|xhci|dwc|065e|1234|sgil1|sgi_l1' | tail -200 || true

echo
echo "== ttyUSB sysfs =="
for t in /sys/class/tty/ttyUSB*; do
	[ -e "$t" ] || continue
	printf "%s " "$t"
	readlink -f "$t"
	if [ -r "$t/dev" ]; then
		printf "  dev="
		cat "$t/dev"
	fi
done
