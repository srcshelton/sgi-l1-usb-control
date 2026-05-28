# SPDX-License-Identifier: GPL-2.0-or-later

PACKAGE := sgi-l1-usb-control
VERSION := $(shell sh scripts/package-version.sh debian/changelog 2>/dev/null)
BUILD_ROOT := _build
BUILD_DIR := $(BUILD_ROOT)/$(PACKAGE)-$(VERSION)

ifeq ($(strip $(VERSION)),)
$(error unable to determine package version from debian/changelog)
endif

.PHONY: all module tools clean deb dkms-conf print-version test test-deb

all: module tools

module:
	$(MAKE) -C module

tools:
	$(MAKE) -C tools

clean:
	$(MAKE) -C module clean
	$(MAKE) -C tools clean
	$(MAKE) -C tests clean
	rm -f dkms.conf
	rm -rf $(BUILD_ROOT)
	@if command -v dh_clean >/dev/null 2>&1; then dh_clean; fi

dkms.conf: dkms.conf.in debian/changelog scripts/package-version.sh
	sed 's/@VERSION@/$(VERSION)/g' $< > $@

dkms-conf: dkms.conf

print-version:
	@printf '%s\n' '$(VERSION)'

test: all
	$(MAKE) -C tests

test-deb: test deb

deb:
	rm -rf $(BUILD_ROOT)
	mkdir -p $(BUILD_DIR)
	tar --exclude='./$(BUILD_ROOT)' --exclude='./.git' --exclude='./repo.git' -cf - . | tar -C $(BUILD_DIR) -xf -
	cd $(BUILD_DIR) && dpkg-buildpackage -us -uc -b
	@echo "Debian packages are in $(BUILD_ROOT)"
