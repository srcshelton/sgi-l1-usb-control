# SPDX-License-Identifier: GPL-2.0-or-later

CC ?= cc
CFLAGS ?= -O2 -g
CPPFLAGS += -I../include
LDFLAGS ?=

.PHONY: all clean

all: sgil1ctl

sgil1ctl: sgil1ctl.o
	$(CC) $(LDFLAGS) -o $@ $^

sgil1ctl.o: sgil1ctl.c ../include/sgi_l1_ioctl.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -Wpedantic -c -o $@ $<

clean:
	rm -f sgil1ctl sgil1ctl.o
