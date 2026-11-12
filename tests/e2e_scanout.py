#!/usr/bin/env python3
# Copyright (c) lezginskiy <xanother-x12@proton.me>
# SPDX-License-Identifier: MIT

"""end to end for dmabuf buffers and direct scanout.

a real dma buffer comes from a dumb buffer on a drm node through prime,
python drives the ioctls itself. three things get proven: a fake dmabuf
(memfd) is refused loudly, a real one imports and shows its pixels
through the composite fallback, and on a kms node the fullscreen frame
flips to the screen directly with kind=flip present feedback while its
release credit waits for the replacing flip. without any drm node the
smoke skips like every hardware test here."""

import fcntl
import mmap
import os
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.x12gen import load_idl  # noqa: E402
from tests import core_client as cc  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
SOCK = ROOT / "build" / "x12-e2e-scanout.sock"
PPM = ROOT / "build" / "x12-e2e-scanout.ppm"

# drm ioctls, python knows no drm headers
_IOC_WRITE = 1
_IOC_READ = 2


def _ioc(dir_, type_, nr, size):
    return (dir_ << 30) | (size << 16) | (type_ << 8) | nr


DRM_IOCTL_BASE = ord("d")
DRM_IOCTL_MODE_CREATE_DUMB = _ioc(_IOC_READ | _IOC_WRITE, DRM_IOCTL_BASE, 0xB2, 32)
DRM_IOCTL_MODE_MAP_DUMB = _ioc(_IOC_READ | _IOC_WRITE, DRM_IOCTL_BASE, 0xB3, 16)
DRM_IOCTL_PRIME_HANDLE_TO_FD = _ioc(_IOC_READ | _IOC_WRITE, DRM_IOCTL_BASE, 0x2D, 12)
DRM_IOCTL_MODE_DESTROY_DUMB = _ioc(_IOC_READ | _IOC_WRITE, DRM_IOCTL_BASE, 0xB4, 4)

XR24 = 0x34325258


def stage(msg):
    print(f"  - {msg}")


def find_node():
    # a node only counts when dumb buffers and prime work on it, some
    # render nodes refuse mode ioctls outright
    for p in sorted(Path("/dev/dri").glob("card*")) + sorted(
            Path("/dev/dri").glob("renderD*")):
        fd = -1
        try:
            fd = os.open(str(p), os.O_RDWR)
            buf = bytearray(struct.pack("=IIIIIIQ", 8, 8, 32, 0, 0, 0, 0))
            fcntl.ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, buf)
            handle = struct.unpack("=IIIIIIQ", buf)[4]
            fcntl.ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB,
                        bytearray(struct.pack("=I", handle)))
            os.close(fd)
            return str(p)
        except OSError:
            if fd >= 0:
                os.close(fd)
            continue
    return None


def make_dmabuf(node, w, h, fill):
    """a dumb buffer full of `fill` bytes exported as a dma-buf fd"""
    dev = os.open(node, os.O_RDWR)
    try:
        buf = bytearray(struct.pack("=IIIIIIQ", h, w, 32, 0, 0, 0, 0))
        fcntl.ioctl(dev, DRM_IOCTL_MODE_CREATE_DUMB, buf)
        _, _, _, _, handle, pitch, size = struct.unpack("=IIIIIIQ", buf)
        mbuf = bytearray(struct.pack("=IIQ", handle, 0, 0))
        fcntl.ioctl(dev, DRM_IOCTL_MODE_MAP_DUMB, mbuf)
        offset = struct.unpack("=IIQ", mbuf)[2]
        mm = mmap.mmap(dev, size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE,
                       offset=offset)
        row = (fill * (w * 4))[:pitch]
        for y in range(h):
            mm[y * pitch:y * pitch + w * 4] = row[:w * 4]
        # drm mappings take the writes as they go, msync knows them not
        pbuf = bytearray(struct.pack("=IIi", handle, 0, 0))
        fcntl.ioctl(dev, DRM_IOCTL_PRIME_HANDLE_TO_FD, pbuf)
        dmabuf_fd = struct.unpack("=IIi", pbuf)[2]
        mm.close()
        return dmabuf_fd
    finally:
        os.close(dev)


def handshake(s):
    s.request("x12_connection", "hello", 1, idl_major=1, idl_minor=0)
    s.wait_for("welcome")
    for _ in range(5):
        s.wait_for("global")


def bind(s, name, version, id):
    s.request("x12_registry", "bind", 2, name=name, version=version, id=id,
              instance=0)
    s.register(name, id)


def make_window(s, surface, pool_id, buf_id, w, h, memfd=None, dmabuf=None,
                modifier=0):
    s.request("x12_compositor", "create_surface", 10, id=surface)
    s.register("x12_surface", surface)
    if dmabuf is not None:
        s.request("x12_buffer_manager", "import_dmabuf", 12, fds=[dmabuf],
                  fd=0, width=w, height=h, format=XR24, modifier=modifier,
                  stride=w * 4, id=buf_id)
    else:
        os.ftruncate(memfd, w * h * 4)
        s.request("x12_buffer_manager", "create_shm_pool", 12,
                  fds=[memfd], fd=0, size=w * h * 4, id=pool_id)
        os.close(memfd)
        s.register("x12_shm_pool", pool_id)
        s.request("x12_shm_pool", "create_buffer", pool_id, id=buf_id,
                  offset=0, width=w, height=h, stride=w * 4, format=XR24)
    s.register("x12_buffer", buf_id)
    s.request("x12_surface", "set_buffer", surface, buffer=buf_id)


def map_fullscreen(s, surface, role):
    s.request("x12_compositor", "create_toplevel", 10, id=role, surface=surface)
    s.register("x12_toplevel", role)
    ev = s.wait_for("configure")
    s.request("x12_toplevel", "ack_configure", role,
              serial=ev["args"]["serial"])
    s.request("x12_toplevel", "set_fullscreen", role, state=1)
    ev = s.wait_for("configure")
    s.request("x12_toplevel", "ack_configure", role,
              serial=ev["args"]["serial"])


def main():
    idl = load_idl(ROOT / "spec" / "x12-core.yaml")
    node = find_node()
    kms_node = os.environ.get("X12_KMS_NODE", "")
    print(f"e2e scanout (dmabuf node: {node or 'none'}):")

    log = open(ROOT / "build" / "x12-e2e-scanout.log", "wb")
    proc = cc.spawn_server(ROOT / "build" / "x12-server", SOCK,
                           ["--width", "320", "--height", "240",
                            "--ppm", str(PPM)])
    try:
        s = cc.Session(SOCK, idl)
        handshake(s)
        bind(s, "x12_compositor", 2, 10)
        bind(s, "x12_buffer_manager", 1, 12)

        stage("a fake dmabuf is refused loudly")
        fake = os.memfd_create("x12-not-a-dmabuf", os.MFD_ALLOW_SEALING)
        os.ftruncate(fake, 64 * 64 * 4)
        s.request("x12_buffer_manager", "import_dmabuf", 12, fds=[fake],
                  fd=0, width=64, height=64, format=XR24, modifier=0,
                  stride=64 * 4, id=20)
        os.close(fake)
        err = s.wait_for("error")
        assert err["args"]["code"] == "bad-value", err
        print("  - memfd refused with bad-value")

        if node:
            stage("a real dmabuf imports and composites through its map")
            dmabuf_fd = make_dmabuf(node, 64, 64,
                                    struct.pack("<I", 0xFF112233))
            make_window(s, 30, 21, 22, 64, 64, dmabuf=dmabuf_fd)
            os.close(dmabuf_fd)
            s.request("x12_compositor", "create_toplevel", 10,
                      id=23, surface=30)
            s.register("x12_toplevel", 23)
            ev = s.wait_for("configure")
            s.request("x12_toplevel", "ack_configure", 23,
                      serial=ev["args"]["serial"])
            s.request("x12_surface", "damage", 30, x=0, y=0, width=64,
                      height=64)
            s.request("x12_surface", "commit", 30)
            s.wait_for("committed")
            time.sleep(0.3)

            def read_body():
                data = PPM.read_bytes()
                parts = data.split(b"255\n", 1)
                if len(parts) != 2:
                    return None
                head, body = parts
                w = int(head.split()[1])
                return w, body

            got = read_body()
            assert got, "ppm dump missing"
            w, body = got
            o = ((32 + 4) * w + (32 + 4)) * 3
            assert body[o:o + 3] == b"\x11\x22\x33", body[o:o + 3]
            print("  - dmabuf pixels landed through the composite path")
        else:
            stage("no drm node, composite fallback check skipped")

        s.close()
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()

    if node and kms_node:
        print(f"e2e scanout on kms {kms_node}:")
        sock2 = ROOT / "build" / "x12-e2e-scanout2.sock"
        log2 = open(ROOT / "build" / "x12-e2e-scanout-kms.log", "wb")
        proc = cc.spawn_server(ROOT / "build" / "x12-server", sock2,
                               ["--kms", kms_node], stderr=log2)
        try:
            s = cc.Session(sock2, idl)
            handshake(s)
            bind(s, "x12_output", 2, 3)
            mode = s.wait_for("mode")
            w, h = mode["args"]["width"], mode["args"]["height"]
            s.wait_for("geometry")
            s.wait_for("scale")
            s.wait_for("name")
            bind(s, "x12_compositor", 2, 10)
            bind(s, "x12_buffer_manager", 1, 12)
            print("  - screen %ux%u" % (w, h))

            stage("the fullscreen dma frame flips straight to the screen")
            dmabuf_fd = make_dmabuf(kms_node, w, h,
                                    struct.pack("<I", 0xFF334455))
            make_window(s, 30, 21, 22, w, h, dmabuf=dmabuf_fd)
            os.close(dmabuf_fd)
            map_fullscreen(s, 30, 23)
            s.request("x12_surface", "damage", 30, x=0, y=0, width=w,
                      height=h)
            s.request("x12_surface", "commit", 30)
            t0 = s.wait_for("committed")["args"]["token"]
            pr = s.wait_for("present")
            assert pr["args"]["commit_token"] == t0, (pr, t0)
            assert pr["args"]["kind"] == "flip", pr
            assert pr["timestamp_ns"] > 0
            print("  - present token %d kind flip at ns %d" % (
                pr["args"]["commit_token"], pr["timestamp_ns"]))

            stage("the release credit waits for the replacing flip")
            # the buffer is still on screen: no release may exist yet
            time.sleep(0.2)
            early = [e for e in s.drain() if e["name"] == "release"]
            assert early == [], early
            print("  - no release while the buffer is in scanout")

            # unmap the window, the black repaint replaces the buffer on
            # screen and only then the release credit goes home
            s.request("x12_surface", "set_buffer", 30, buffer=0)
            s.request("x12_surface", "damage", 30, x=0, y=0, width=w,
                      height=h)
            s.request("x12_surface", "commit", 30)
            s.wait_for("committed")
            deadline = time.time() + 5
            rel = None
            while time.time() < deadline:
                for e in s.drain():
                    if e["name"] == "release" and e["object"] == 22:
                        rel = e
                if rel:
                    break
                time.sleep(0.05)
            assert rel, "release never came after the replacing flip"
            print("  - release credit landed after the replacement")

            s.close()
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except Exception:
                proc.kill()
    else:
        print("e2e scanout: kms stage skipped (needs X12_KMS_NODE and a node)")

    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
