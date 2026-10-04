#!/bin/sh
# Copyright (c) 2026 Daniel Kos
#
# This file is part of hipe-pdf, licensed under the GNU General Public License
# version 3 or later. See COPYING and LICENSE.md.

# Writes hipe-pdf-<version>-src.tar.gz: the complete source of a hipe-pdf binary built from
# the current commit -- this repository plus MuPDF and every library MuPDF bundles, each at
# the exact commit recorded for it. Attach it to a release next to the binary: it is the
# Corresponding Source the AGPL asks for (see LICENSE.md), with no dependency on anyone
# else's servers staying up. libhipe isn't included; it is published with Hipe.
#
# usage: scripts/source-bundle.sh [output-directory]    (default: build/)
set -eu

cd "$(dirname "$0")/.."
out_dir=${1:-build}

# The bundle describes a commit. A binary built from uncommitted changes wouldn't match it.
if [ -n "$(git status --porcelain --ignore-submodules=none)" ]; then
	echo "source-bundle: uncommitted changes (here or in third_party/mupdf); commit or stash them first" >&2
	exit 1
fi
if [ ! -e third_party/mupdf/.git ]; then
	echo "source-bundle: third_party/mupdf is not checked out (git submodule update --init --recursive)" >&2
	exit 1
fi

version=$(git describe --tags --always --dirty)
name="hipe-pdf-$version"
mkdir -p "$out_dir"
out_dir=$(cd "$out_dir" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM

# This repository at HEAD.
git archive --format=tar --prefix="$name/" HEAD | tar -x -C "$work"

# Each submodule, recursively, at the commit its parent records (git submodule foreach
# provides that as $sha1, and the path from the top as $displaypath).
git submodule foreach --quiet --recursive '
	git archive --format=tar --prefix="'"$name"'/$displaypath/" "$sha1" | tar -x -C "'"$work"'"
'

tar -C "$work" -czf "$out_dir/$name-src.tar.gz" "$name"
echo "$out_dir/$name-src.tar.gz ($(du -h "$out_dir/$name-src.tar.gz" | cut -f1))"
