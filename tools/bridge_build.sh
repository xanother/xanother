#!/bin/sh

# Copyright (c) lezginskiy <xanother-x12@proton.me>
# SPDX-License-Identifier: MIT

# builds the vendored xserver with the x12 bridge DDX on top; distro
# packages provide meson, ninja and the pixman, xkbfile, xfont2,
# xtrans, xcvt, glproto, libgl, mesa-common (dri.pc), epoxy dependency
# set; the software glx path needs the dri.pc provider for the swrast
# glx backend

set -e

cd "$(dirname "$0")/.."
python3 tools/x12gen.py --lang c --out-dir build/gen >/dev/null
sh tools/vendor_sync.sh

cd build/vendor/xserver
if [ ! -f build/build.ninja ]; then
    rm -rf build
    # the xkb rules data lives in the distro tree, the meson prefix
    # fallback would point the bridge at /usr/local
    meson setup build -Dxorg=false -Dxephyr=false \
        -Dxnest=false -Dxvfb=false -Dxwayland=false -Dxquartz=false \
        -Dxwin=false -Dx12=true -Dglx=true -Dglamor=true -Ddocs=false \
        -Dxdmcp=false -Dipv6=false -Dudev=false -Dudev_kms=false \
        -Dinput_thread=false -Dxkb_dir=/usr/share/X11/xkb
fi
ninja -C build
echo "bridge: Xx12 at build/vendor/xserver/build/hw/x12/Xx12"
