#!/bin/sh

# Copyright (c) lezginskiy <xanother-x12@proton.me>
# SPDX-License-Identifier: MIT

# assembles the bridge build tree from the in-repo vendored sources:
# copies third-party/xserver into build/, applies the vendor patch stack
# and injects the bridge sources; no network, the repository carries
# everything; the build tree is ephemeral and rebuilt from the pristine
# in-repo tree on every sync

set -e

cd "$(dirname "$0")/.."
VENDOR=build/vendor/xserver

if [ ! -f third-party/xserver.pin ] || [ ! -d third-party/xserver ]; then
    echo "vendor: third-party/xserver missing, restore from git history" >&2
    exit 1
fi
rm -rf "$VENDOR"
mkdir -p build/vendor
cp -a third-party/xserver "$VENDOR"
# the distro xorgproto lags what master requires, the wrap subproject
# fills the gap at configure time
mkdir -p "$VENDOR/subprojects"
cat > "$VENDOR/subprojects/xorgproto.wrap" <<'WRAP'
[wrap-git]
url = https://gitlab.freedesktop.org/xorg/proto/xorgproto.git
revision = master
depth = 1
WRAP
for p in third-party/patches/*.patch; do
    git -C "$VENDOR" apply --whitespace=nowarn "../../../$p"
done
mkdir -p "$VENDOR/hw/x12"
cp src/bridge/hw-x12/*.c src/bridge/hw-x12/*.h \
   src/bridge/hw-x12/meson.build "$VENDOR/hw/x12/"
cp client/libx12.c client/libx12.h src/wire/x12_wire.c src/wire/x12_wire.h \
   build/gen/x12_protocol.h build/gen/x12_tables.c "$VENDOR/hw/x12/"
echo "vendor: bridge sources synced"
