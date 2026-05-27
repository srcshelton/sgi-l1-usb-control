# SPDX-License-Identifier: GPL-2.0-or-later

ifneq ($(KERNELRELEASE),)

obj-m := sgi_l1_usb.o
ccflags-y += -I$(src)/../include -Wall

else

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

endif
