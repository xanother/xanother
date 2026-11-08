# Copyright (c) lezginskiy <xanother-x12@proton.me>
# SPDX-License-Identifier: MIT

# the x12 bridge DDX, built from the same xserver libraries as the
# other ddx targets; the sources here are synced from the parent
# repository by tools/vendor_sync.sh and live outside the vendor git

srcs = [
    'x12-init.c',
    'x12-input.c',
    'x12-client.c',
    'x12-glamor.c',
    'x12-dri3.c',
    'x12-isolate.c',
    'libx12.c',
    'x12_wire.c',
    'x12_net.c',
    'x12_tables.c',
    '../../mi/miinitext.c',
]

x12_server = executable(
    'Xx12',
    srcs,
    include_directories: [inc, include_directories('../../dri3')],
    dependencies: [common_dep, dependency('libdrm'), dependency('gbm'),
                   dependency('openssl'),
                   dependency('zlib')],
    link_with: [
        libxserver_main,
        libxserver_fb,
        libxserver,
        libxserver_xkb_stubs,
        libxserver_xi_stubs,
        libxserver_miext_shadow,
        libxserver_glx,
        libglxvnd,
        # the ddx calls into glamor unconditionally, the runtime
        # engine choice is the X12_BRIDGE_GLAMOR env, not the build
        glamor,
    ],
    install: false,
)
