#!/bin/sh

# Copyright (c) lezginskiy <xanother-x12@proton.me>
# SPDX-License-Identifier: MIT

# the pinned upstream xserver tree lives in-repo under third-party/xserver
# as plain sources, the exact upstream commit is recorded in
# third-party/xserver.pin; this script guards that provenance:
#
#   vendor_fetch.sh            verify the in-repo tree against the pin:
#                              shallow-clone the pinned commit, diff, report
#   vendor_fetch.sh --import   pin bump: fetch upstream master (or $2),
#                              replace the in-repo tree, refresh the pin
#
# the pin discipline is deliberate: master, updated in named steps

set -e
cd "$(dirname "$0")/.."

TREE=third-party/xserver
PIN_FILE=third-party/xserver.pin
UPSTREAM=https://gitlab.freedesktop.org/xorg/xserver.git
TMP=build/vendor/upstream

if [ "${1:-}" = "--import" ]; then
    rm -rf "$TMP"
    mkdir -p build/vendor
    git clone --depth 1 "$UPSTREAM" "$TMP/xserver"
    if [ -n "${2:-}" ]; then
        git -C "$TMP/xserver" fetch --depth 1 origin "$2"
        git -C "$TMP/xserver" checkout --detach "$2"
    fi
    NEWPIN=$(git -C "$TMP/xserver" rev-parse HEAD)
    echo "vendor: importing upstream master commit $NEWPIN"
    rm -rf "$TREE"
    cp -a "$TMP/xserver" "$TREE"
    rm -rf "$TREE/.git"
    echo "$NEWPIN" > "$PIN_FILE"
    git add "$TREE" "$PIN_FILE"
    echo "vendor: tree replaced and staged, review and commit"
    exit 0
fi

PIN=$(cat "$PIN_FILE")
if [ -z "$PIN" ]; then
    echo "vendor: $PIN_FILE is empty" >&2
    exit 1
fi
if [ ! -d "$TREE" ] || [ -z "$(ls -A "$TREE")" ]; then
    echo "vendor: $TREE is empty, restore it from git history" >&2
    exit 1
fi

rm -rf "$TMP"
mkdir -p build/vendor
git clone --depth 1 "$UPSTREAM" "$TMP/xserver" >/dev/null 2>&1
if [ "$(git -C "$TMP/xserver" rev-parse HEAD)" != "$PIN" ]; then
    git -C "$TMP/xserver" fetch --depth 1 origin "$PIN" >/dev/null 2>&1
    git -C "$TMP/xserver" checkout --detach "$PIN" >/dev/null 2>&1
fi
if diff -rq --exclude=.git "$TMP/xserver" "$TREE" >/dev/null; then
    echo "vendor: $TREE verified against upstream $PIN"
else
    echo "vendor: MISMATCH against upstream $PIN:" >&2
    diff -rq --exclude=.git "$TMP/xserver" "$TREE" | head -20 >&2
    exit 1
fi
