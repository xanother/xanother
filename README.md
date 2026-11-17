# Xanother

## What xanother (X12) is and why it, and not the others

Xanother (X12) is a display server - not a fork of Xorg and not a wrapper around it, but a new implementation with different priorities. Networking, isolation and explicit sync are the foundation here, not add-ons.

Key decisions:

- client-side rendering - clients draw into their own buffers (shared memory, dmabuf or network upload), the server only composites and puts them on screen. nothing is drawn on the client's behalf;
- explicit sync - dma-buf with explicit fence points instead of X11 implicit sync, which drags on modern drivers;
- window isolation - every window sees only itself and its own child processes. this applies to X11 applications through the bridge too, not just to our clients;
- one wire over unix, tcp and tls - all three are first-class. remote sessions and migration work out of the box: a client can drop the connection, come back on a new socket and get its windows and objects back;

Why not X11: the X11 security model lets any client snoop on someone else's input; implicit sync and an architecture from the 80s still drag along to this day. Compatibility with X11 applications is kept through the bridge (Xx12 is a full X server on top of X12), but we are not dragging its architecture along.

Why not Wayland: it has no networking out of the box - remote access and session migration live in separate projects outside the protocol; the compositor, server and window manager are fused into a single process; the protocol is generated from xml and reads poorly by hand. X12 puts networking and portable sessions in the foundation. and on top of that there is no real X11 window compatibility either (XWayland does not isolate them from each other at all).

The project is young and alive: the core, the bridge and the network work and are covered by tests, but the API will still change.

## How to build

Dependencies (Debian/Ubuntu):

```
build-essential pkg-config python3 python3-yaml
libdrm-dev libgbm-dev libxkbcommon-dev libudev-dev
libssl-dev zlib1g-dev
meson ninja-build
libpixman-1-dev x11proto-dev libxkbfile-dev libxfont-dev libxcvt-dev
libepoxy-dev libglvnd-dev mesa-common-dev libgl1-mesa-dev libxshmfence-dev
```

Build:

```
python3 tools/x12gen.py --lang check
sh tools/build.sh                      # everything builds into build/
```

Output: `x12-server` (the server), `Xx12` (the bridge for X11 applications), `c_roundtrip`, `c_gpu_contract`, `c_gbm_present` (test utilities).

The bridge is built from the vendored xorg xserver in `third-party/` - this is deliberate, not duplication: that is how X11 applications get a real DIX with nothing substituted. you do not build it separately, but `sh tools/bridge_build.sh` stays around for rebuilding just the bridge.

## How to run

One command, in the spirit of startx:

```
sh tools/x12start fvwm3
```

`x12start` starts the server and the bridge and hands the display to a session, just like xorg hands it to `~/.xinitrc`. with no command it runs `~/.x12initrc`, falling back to `~/.xinitrc`; with no config at all it writes a starter one with recipes for fvwm, i3 and plasma. the session is a plain shell script: helpers go to the background with `&`, the window manager comes last with `exec` and ends the session when it exits.

```
sh tools/x12start                     # the session from ~/.x12initrc
sh tools/x12start -s 1600x900 i3      # bigger canvas
sh tools/x12start -kms fvwm3          # output to real drm
sh tools/x12start -ppm frame.ppm      # keep the frame on disk
```

Options: `-d` display for the bridge, `-s` canvas size, `-sock` listen address, `-ppm`, `-kms`, `-gpu`, `-grants` (comma-separated process names the isolation lets see the window tree).

The long form, piece by piece - server first. The first argument is the address, in three forms:

- `/path` or `unix:/path` - a unix socket, the only transport that can pass file descriptors;
- `tcp:HOST:PORT` - plain tcp;
- `tls:HOST:PORT` - tls over tcp.

```
./build/x12-server /tmp/x12.sock --width 1280 --height 720 --ppm frame.ppm
```

Flags: `--ppm PATH` (headless mode, the server writes the frame after every draw), `--width` / `--height`, `--heads N` (multiple heads), `--gpu` and `--gpu-node /dev/dri/renderD128` (GL canvas), `--kms` (output to real DRM), `--input FIFO` (input as lines: `motion X Y`, `button N press|release`, `key K press|release`), `--evdev NODE` (multiple allowed).

TLS: the server needs `X12_TLS_CERT` and `X12_TLS_KEY`; the client verifies the certificate against `X12_TLS_CA` or turns verification off with `X12_TLS_INSECURE=1`. how long a disconnected session stays alive is `X12_SESSION_TTL_MS` (30 seconds by default).

The bridge connects to the server as an ordinary client:

```
X12_BRIDGE_SOCKET=/tmp/x12.sock build/vendor/xserver/build/hw/x12/Xx12 :90 -ac -noreset
DISPLAY=:90 fvwm
```

Bridge environment: `X12_BRIDGE_WIDTH` / `X12_BRIDGE_HEIGHT` (canvas size), `X12_BRIDGE_FULLSCREEN`, `X12_BRIDGE_GLAMOR` (GL acceleration), `X12_BRIDGE_GRANTS` (comma-separated process names the isolation lets see the window tree - `xwininfo`, `xrandr` and so on).

## License

MIT. Copyright (c) lezginskiy [xanother-x12@proton.me](mailto:xanother-x12@proton.me).
