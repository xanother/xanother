#!/bin/sh
# Copyright (c) lezginskiy <xanother-x12@proton.me>
# Copyright (c) fluent1s (akemi) <m1st1kad3ad1y@gmail.com>
# SPDX-License-Identifier: MIT

# builds the world into build/: the server, the c gates and the bridge
# with the x12 ddx on top of the vendored xserver, codegen first.
# warnings are errors, the gate is the gate. the bridge needs the meson
# and ninja toolchain plus the xorg dependency set from bootstrap.sh,
# a build without the bridge is no build at all

set -e
cd "$(dirname "$0")/.."

python3 tools/x12gen.py --lang c --out-dir build/gen

CFLAGS="-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -I src/wire -I src/core -I src/frame -I src/render -I src/backend -I src/input -I src/data -I build/gen"
# system headers hide behind pkg-config on some machines - ask for the
# gles3 set first and fall back to gles2, no pkg-config at all just
# leaves the default include paths
SYS_CFLAGS=$(pkg-config --cflags libdrm libsystemd xkbcommon egl glesv3 2>/dev/null || \
    pkg-config --cflags libdrm libsystemd xkbcommon egl glesv2 2>/dev/null || true)

gcc $CFLAGS $SYS_CFLAGS -o build/x12-server \
    src/server/main.c \
    src/core/conn.c \
    src/wire/x12_net.c \
    src/core/objects.c \
    src/core/output.c \
    src/core/dispatch.c \
    src/core/handlers.c \
    src/core/buffer.c \
    src/frame/frame.c \
    src/frame/windows.c \
    src/frame/compositor.c \
    src/input/input.c \
    src/input/source_file.c \
    src/input/source_evdev.c \
    src/data/selection.c \
    src/render/cpu.c \
    src/render/gpu.c \
    src/backend/backend_headless.c src/backend/backend_kms.c \
    src/backend/seat.c \
    src/wire/x12_wire.c \
    build/gen/x12_tables.c \
    -lxkbcommon -ldl -ldrm -ludev -lsystemd -lssl -lcrypto -lz

gcc $CFLAGS -o build/c_roundtrip \
    tests/c_roundtrip.c \
    src/wire/x12_wire.c \
    build/gen/x12_tables.c

# the gpu canvas contract: cpu and gpu blend paths must agree byte for
# byte, the binary degrades to SKIP on a box without a gl stack
gcc $CFLAGS $SYS_CFLAGS -o build/c_gpu_contract \
    tests/c_gpu_contract.c \
    src/render/cpu.c \
    src/render/gpu.c \
    -ldl -lm

# the gbm present smoke: real render node or nothing, the dma path has
# to land the canvas byte exact on both scanout bos
gcc $CFLAGS $SYS_CFLAGS -o build/c_gbm_present \
    tests/c_gbm_present.c \
    src/render/cpu.c \
    src/render/gpu.c \
    -ldl -lm

# the bridge rides along: one build call produces the server and the
# x12 ddx together
if ! command -v meson >/dev/null 2>&1 || ! command -v ninja >/dev/null 2>&1; then
    echo "build: the bridge needs meson and ninja, run bootstrap.sh first" >&2
    exit 1
fi
sh tools/bridge_build.sh

echo "build ok"
