#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later

changelog=${1:-debian/changelog}

if command -v dpkg-parsechangelog >/dev/null 2>&1; then
	version=$(dpkg-parsechangelog -l "$changelog" -S Version 2>/dev/null || true)
	if [ -n "$version" ]; then
		printf '%s\n' "$version"
		exit 0
	fi
fi

version=$(sed -n '1s/^[^(]*(\([^)]*\)).*/\1/p' "$changelog" 2>/dev/null || true)
if [ -n "$version" ]; then
	printf '%s\n' "$version"
	exit 0
fi

printf 'unable to determine package version from %s\n' "$changelog" >&2
exit 1
