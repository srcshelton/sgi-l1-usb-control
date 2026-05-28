# SPDX-License-Identifier: GPL-2.0-or-later

PACKAGE := sgi-l1-usb-control
VERSION := $(shell sh scripts/package-version.sh debian/changelog 2>/dev/null)
BUILD_ROOT := _build
BUILD_DIR := $(BUILD_ROOT)/$(PACKAGE)-$(VERSION)
L2_L3_CONTAINER_SCRIPT := scripts/build-l2-l3-container.sh

ifeq ($(strip $(VERSION)),)
$(error unable to determine package version from debian/changelog)
endif

.PHONY: all help module tools clean deb dkms-conf print-version test test-deb \
	container-docker container-podman container-apple

all: module tools

help:
	@printf '%s\n' 'Targets:'
	@printf '  %-18s %s\n' 'all' 'build the kernel module and sgil1ctl'
	@printf '  %-18s %s\n' 'module' 'build module/sgi_l1_usb.ko'
	@printf '  %-18s %s\n' 'tools' 'build tools/sgil1ctl'
	@printf '  %-18s %s\n' 'test' 'run kernel static checks and sgil1ctl mock tests'
	@printf '  %-18s %s\n' 'deb' 'build Debian binary packages under _build/'
	@printf '  %-18s %s\n' 'test-deb' 'run tests, then build Debian packages'
	@printf '  %-18s %s\n' 'dkms-conf' 'generate dkms.conf from dkms.conf.in'
	@printf '  %-18s %s\n' 'clean' 'remove local build outputs'
	@printf '  %-18s %s\n' 'print-version' 'print the package version'
	@printf '  %-18s %s\n' 'container-docker' 'build the optional SGI L2/L3 image with Docker'
	@printf '  %-18s %s\n' 'container-podman' 'build the optional SGI L2/L3 image with Podman'
	@printf '  %-18s %s\n' 'container-apple' 'build the optional SGI L2/L3 image with Apple container on macOS arm64'
	@printf '%s\n' ''
	@printf '%s\n' 'Container targets expect contrib/l2-l3-container/rootfs or SGI_L3_RPM=/path/to/snxsc_l3-*.rpm.'
	@printf '%s\n' 'Set SGI_L3_FETCH=1 to fetch the referenced public CD-IST archive before extracting.'

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

container-docker:
	$(L2_L3_CONTAINER_SCRIPT) docker

container-podman:
	$(L2_L3_CONTAINER_SCRIPT) podman

container-apple:
	$(L2_L3_CONTAINER_SCRIPT) container

deb:
	rm -rf $(BUILD_ROOT)
	mkdir -p $(BUILD_DIR)
	tar --exclude='./$(BUILD_ROOT)' --exclude='./.git' --exclude='./repo.git' -cf - . | tar -C $(BUILD_DIR) -xf -
	cd $(BUILD_DIR) && dpkg-buildpackage -us -uc -b
	@echo "Debian packages are in $(BUILD_ROOT)"
