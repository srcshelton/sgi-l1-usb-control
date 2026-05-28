# SPDX-License-Identifier: GPL-2.0-or-later

VERSION_SCRIPT := ../scripts/package-version.sh
CHANGELOG := ../debian/changelog

SGI_L1_VERSION ?= $(shell \
	if [ -r $(VERSION_SCRIPT) ] && [ -r $(CHANGELOG) ]; then \
		sh $(VERSION_SCRIPT) $(CHANGELOG) 2>/dev/null; \
	else \
		echo unknown; \
	fi)

ifneq ($(KERNELRELEASE),)

obj-m := sgi_l1_usb.o
ccflags-y += -I$(src)/../include -Wall
ccflags-y += -DSGI_L1_VERSION=\"$(SGI_L1_VERSION)\"

else

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(PWD) SGI_L1_VERSION=$(SGI_L1_VERSION) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) SGI_L1_VERSION=$(SGI_L1_VERSION) clean

endif
