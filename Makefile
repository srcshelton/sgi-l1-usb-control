# SPDX-License-Identifier: GPL-2.0-or-later

PACKAGE := sgi-l1-usb-control
VERSION := 0.1.0
BUILD_ROOT := _build
BUILD_DIR := $(BUILD_ROOT)/$(PACKAGE)-$(VERSION)

.PHONY: all module tools clean deb

all: module tools

module:
	$(MAKE) -C module

tools:
	$(MAKE) -C tools

clean:
	$(MAKE) -C module clean
	$(MAKE) -C tools clean
	rm -rf $(BUILD_ROOT)
	@if command -v dh_clean >/dev/null 2>&1; then dh_clean; fi

deb:
	rm -rf $(BUILD_ROOT)
	mkdir -p $(BUILD_DIR)
	tar --exclude='./$(BUILD_ROOT)' --exclude='./.git' --exclude='./repo.git' -cf - . | tar -C $(BUILD_DIR) -xf -
	cd $(BUILD_DIR) && dpkg-buildpackage -us -uc -b
	@echo "Debian packages are in $(BUILD_ROOT)"
