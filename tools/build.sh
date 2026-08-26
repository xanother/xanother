#!/bin/sh
# Copyright (c) lezginskiy <xanother-x12@proton.me>
# SPDX-License-Identifier: MIT

# builds the server and the c roundtrip gate into build/, codegen first.
# warnings are errors, the gate is the gate

set -e
cd "$(dirname "$0")/.."

python3 tools/x12gen.py --lang c --out-dir build/gen

CFLAGS="-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -I src/wire -I src/core -I build/gen"

gcc $CFLAGS -o build/x12-server \
    src/server/main.c \
    src/core/conn.c \
    src/core/objects.c \
    src/core/dispatch.c \
    src/core/handlers.c \
    src/wire/x12_wire.c \
    build/gen/x12_tables.c

gcc $CFLAGS -o build/c_roundtrip \
    tests/c_roundtrip.c \
    src/wire/x12_wire.c \
    build/gen/x12_tables.c

echo "build ok"
