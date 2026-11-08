#!/bin/sh
# Copyright (c) lezginskiy <xanother-x12@proton.me>
# SPDX-License-Identifier: MIT

# builds the server and the c roundtrip gate into build/, codegen first.
# warnings are errors, the gate is the gate

set -e
cd "$(dirname "$0")/.."

python3 tools/x12gen.py --lang c --out-dir build/gen

CFLAGS="-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -I src/wire -I src/core -I src/frame -I src/render -I src/backend -I src/input -I src/data -I build/gen"
DRM_CFLAGS=$(pkg-config --cflags libdrm 2>/dev/null)

gcc $CFLAGS $DRM_CFLAGS -o build/x12-server \
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
    src/wire/x12_wire.c \
    build/gen/x12_tables.c \
    -lxkbcommon -ldl -ldrm -ludev -lssl -lcrypto -lz

gcc $CFLAGS -o build/c_roundtrip \
    tests/c_roundtrip.c \
    src/wire/x12_wire.c \
    build/gen/x12_tables.c

# the gpu canvas contract: cpu and gpu blend paths must agree byte for
# byte, the binary degrades to SKIP on a box without a gl stack
gcc $CFLAGS -o build/c_gpu_contract \
    tests/c_gpu_contract.c \
    src/render/cpu.c \
    src/render/gpu.c \
    -ldl -lm

# the gbm present smoke: real render node or nothing, the dma path has
# to land the canvas byte exact on both scanout bos
gcc $CFLAGS -o build/c_gbm_present \
    tests/c_gbm_present.c \
    src/render/cpu.c \
    src/render/gpu.c \
    -ldl -lm

# client side: libx12 links the same wire engine the server speaks
CLIENT="$CFLAGS -I client -I client/terminal"

gcc $CLIENT -o build/welcome \
    client/welcome.c \
    client/libx12.c \
    src/wire/x12_wire.c \
    src/wire/x12_net.c \
    build/gen/x12_tables.c -lssl -lcrypto -lz

gcc $CLIENT -o build/x12-term \
    client/terminal/x12-term.c \
    client/terminal/term_grid.c \
    client/terminal/term_vt.c \
    client/libx12.c \
    src/wire/x12_wire.c \
    src/wire/x12_net.c \
    build/gen/x12_tables.c \
    -lxkbcommon -lutil -lssl -lcrypto -lz

echo "build ok"
