# SPDX-License-Identifier: GPL-2.0-or-later

CC ?= cc
CFLAGS ?= -O2 -g
CPPFLAGS += -I../include
LDFLAGS ?=
LDLIBS ?=
WITH_TUI ?= 0
PKG_CONFIG ?= pkg-config
SGIL1CTL_VERSION ?= $(shell sh ../scripts/package-version.sh \
	../debian/changelog 2>/dev/null)
VERSION_INPUTS := ../debian/changelog ../scripts/package-version.sh

ifneq ($(strip $(SGIL1CTL_VERSION)),)
CPPFLAGS += -DSGIL1CTL_VERSION='"$(SGIL1CTL_VERSION)"'
endif

ifeq ($(WITH_TUI),1)
NCURSES_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags ncursesw 2>/dev/null)
NCURSES_LIBS ?= $(shell $(PKG_CONFIG) --libs ncursesw 2>/dev/null)
ifeq ($(strip $(NCURSES_LIBS)),)
$(error WITH_TUI=1 requires ncursesw development files or NCURSES_LIBS)
endif
CPPFLAGS += -DSGIL1_WITH_TUI $(NCURSES_CFLAGS)
LDLIBS += $(NCURSES_LIBS)
SGIL1CTL_OBJECT := sgil1ctl-tui.o
CONFIG_STAMP := .sgil1ctl-config-tui
else
SGIL1CTL_OBJECT := sgil1ctl.o
CONFIG_STAMP := .sgil1ctl-config-minimal
endif

.PHONY: all clean

all: sgil1ctl

sgil1ctl: $(SGIL1CTL_OBJECT) $(CONFIG_STAMP)
	$(CC) $(LDFLAGS) -o $@ $(SGIL1CTL_OBJECT) $(LDLIBS)

sgil1ctl.o: sgil1ctl.c ../include/sgi_l1_ioctl.h $(VERSION_INPUTS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -Wpedantic -c -o $@ $<

sgil1ctl-tui.o: sgil1ctl.c ../include/sgi_l1_ioctl.h $(VERSION_INPUTS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -Wpedantic -c -o $@ $<

$(CONFIG_STAMP):
	rm -f .sgil1ctl-config-minimal .sgil1ctl-config-tui
	touch $@

clean:
	rm -f sgil1ctl sgil1ctl-tui sgil1ctl.o sgil1ctl-tui.o
	rm -f .sgil1ctl-config-minimal .sgil1ctl-config-tui
