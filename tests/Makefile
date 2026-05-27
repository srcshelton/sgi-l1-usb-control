# SPDX-License-Identifier: GPL-2.0-or-later

CC ?= cc
CFLAGS ?= -O2 -g
CPPFLAGS += -I../include
LDFLAGS ?=

.PHONY: all clean

all: mock_l1.so
	python3 -m unittest discover -s . -v

mock_l1.so: mock_l1.c ../include/sgi_l1_ioctl.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -Wpedantic -fPIC -shared -o $@ $< -ldl

clean:
	rm -f mock_l1.so
