#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later

set -eu

usage()
{
	cat <<EOF
Usage: $0 docker|podman|container

Build a local container image containing SGI's legacy L2/L3 tools.

Inputs, in order of preference:
  contrib/l2-l3-container/rootfs/         already extracted RPM payload
  SGI_L3_RPM=/path/to/snxsc_l3-*.rpm      local SGI L3 RPM
  SGI_L3_ARCHIVE=/path/to/archive         local CD-IST or L3 archive
  SGI_L3_FETCH=1                          fetch the public archive URL below

Default public archive URL:
  ${SGI_L3_ARCHIVE_URL:-https://www.graphica.com.au/files/cd-ist-3.24.taz}

The SGI software is not redistributed by this repository.
EOF
}

fail()
{
	printf 'ERROR: %s\n' "$*" >&2
	exit 1
}

need_cmd()
{
	command -v "$1" >/dev/null 2>&1 || fail "$1 is required"
}

root_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
container_dir=${SGI_L3_CONTAINER_DIR:-$root_dir/contrib/l2-l3-container}
runtime=${1:-}
image=${SGI_L3_IMAGE:-sgil1-l2-l3-tools:latest}
rpm_name=${SGI_L3_RPM_NAME:-snxsc_l3-1.62.0-1.i386.rpm}
archive_url=${SGI_L3_ARCHIVE_URL:-https://www.graphica.com.au/files/cd-ist-3.24.taz}
fallback_archive_url=${SGI_L3_FALLBACK_ARCHIVE_URL:-https://usftp.irixnet.org/sgi-tools/l3-emulator-linux.tar.gz}
rootfs=$container_dir/rootfs
work_dir=$container_dir/_work
downloads_dir=$container_dir/downloads

case "$runtime" in
docker|podman|container)
	;;
""|-h|--help)
	usage
	exit 0
	;;
*)
	usage >&2
	exit 2
	;;
esac

extract_archive()
{
	archive=$1
	out=$2

	mkdir -p "$out"
	case "$archive" in
	*.tar|*.tar.gz|*.tgz|*.taz|*.tar.Z|*.tar.xz|*.txz)
		tar -C "$out" -xf "$archive"
		;;
	*)
		fail "unsupported archive format: $archive"
		;;
	esac
}

find_rpm()
{
	search_root=$1
	find "$search_root" -type f -name "$rpm_name" -print | sort | sed -n '1p'
}

fetch_archive()
{
	mkdir -p "$downloads_dir"
	archive=$downloads_dir/cd-ist-3.24.taz
	if [ -f "$archive" ]; then
		printf '%s\n' "$archive"
		return 0
	fi

	if command -v curl >/dev/null 2>&1; then
		curl -fL "$archive_url" -o "$archive" ||
			curl -fL "$fallback_archive_url" -o "$archive"
	elif command -v wget >/dev/null 2>&1; then
		wget -O "$archive" "$archive_url" ||
			wget -O "$archive" "$fallback_archive_url"
	else
		fail "curl or wget is required when SGI_L3_FETCH=1"
	fi
	printf '%s\n' "$archive"
}

ensure_rootfs()
{
	rpm=${SGI_L3_RPM:-}
	archive=${SGI_L3_ARCHIVE:-}

	if [ -x "$rootfs/stand/sysco/bin/l2" ] &&
	   [ -x "$rootfs/stand/sysco/bin/l2cmd" ]; then
		return 0
	fi

	if [ -z "$rpm" ]; then
		for candidate in "$container_dir/$rpm_name" "$root_dir/$rpm_name"; do
			if [ -f "$candidate" ]; then
				rpm=$candidate
				break
			fi
		done
	fi

	if [ -z "$rpm" ] && [ -n "$archive" ]; then
		rm -rf "$work_dir/archive"
		extract_archive "$archive" "$work_dir/archive"
		rpm=$(find_rpm "$work_dir/archive" || true)
	fi

	if [ -z "$rpm" ] && [ "${SGI_L3_FETCH:-0}" = 1 ]; then
		archive=$(fetch_archive)
		rm -rf "$work_dir/archive"
		extract_archive "$archive" "$work_dir/archive"
		rpm=$(find_rpm "$work_dir/archive" || true)
	fi

	if [ -z "$rpm" ]; then
		cat >&2 <<EOF
ERROR: no SGI L3 payload found.

Provide one of:
  $rootfs/
  SGI_L3_RPM=/path/to/$rpm_name
  SGI_L3_ARCHIVE=/path/to/cd-ist-3.24.taz

For convenience only, you can also run with SGI_L3_FETCH=1 to fetch:
  $archive_url

Fallback URL, if the primary source is unavailable:
  $fallback_archive_url
EOF
		exit 1
	fi

	need_cmd rpm2cpio
	need_cmd cpio
	rm -rf "$rootfs"
	mkdir -p "$rootfs"
	( cd "$rootfs" && rpm2cpio "$rpm" | cpio -idmu )

	[ -x "$rootfs/stand/sysco/bin/l2" ] ||
		fail "extracted payload does not contain stand/sysco/bin/l2"
	[ -x "$rootfs/stand/sysco/bin/l2cmd" ] ||
		fail "extracted payload does not contain stand/sysco/bin/l2cmd"
}

build_image()
{
	case "$runtime" in
	docker|podman)
		need_cmd "$runtime"
		"$runtime" build \
			--platform "${SGI_L3_PLATFORM:-linux/386}" \
			--build-arg "TARGETPLATFORM=${SGI_L3_PLATFORM:-linux/386}" \
			-t "$image" \
			-f "$container_dir/Containerfile" \
			"$container_dir"
		;;
	container)
		[ "$(uname -s)" = Darwin ] ||
			fail "Apple container target is only supported on macOS"
		[ "$(uname -m)" = arm64 ] ||
			fail "Apple container target requires an Apple Silicon arm64 host"
		need_cmd container
		cat >&2 <<EOF
NOTICE: Apple container officially documents arm64 and amd64 image builds.
This target builds an amd64 image containing SGI i386 binaries; actually
running those binaries depends on the host's x86 compatibility support.
Docker or Podman with linux/386 support remains the reference path.
EOF
		container build \
			--arch "${SGI_L3_CONTAINER_ARCH:-amd64}" \
			--build-arg "TARGETPLATFORM=${SGI_L3_PLATFORM:-linux/amd64}" \
			--tag "$image" \
			--file "$container_dir/Containerfile" \
			"$container_dir"
		;;
	esac
}

ensure_rootfs
build_image
