# Copyright (c) lezginskiy <xanother-x12@proton.me>
# SPDX-License-Identifier: MIT

# shared client for the core skeleton e2e tests, one implementation of the
# socket dance so every scenario file stays focused on its own assertions

import os
import socket
import struct
import subprocess
import time
import zlib
from pathlib import Path

from tests import wire_model as wm

# frame envelope for the network transports, mirrors x12_net_wrap and
# x12_net_peel in the server: [u32 hdr][u32 raw_len][data], the high
# bit of hdr marks raw data, clear means a zlib stream
_NET_RAW_FLAG = 0x80000000
_NET_COMPRESS_MIN = 512


def net_wrap(frame):
    hdr = _NET_RAW_FLAG | len(frame)
    data = frame
    if len(frame) >= _NET_COMPRESS_MIN:
        comp = zlib.compress(frame, 6)
        if len(comp) < len(frame):
            hdr, data = len(comp), comp
    return struct.pack("<II", hdr, len(frame)) + data


def net_peel(buf):
    # pulls every complete envelope out of buf, returns (frames, rest)
    frames = []
    off = 0
    while len(buf) - off >= 8:
        hdr, raw_len = struct.unpack_from("<II", buf, off)
        data_len = hdr & ~_NET_RAW_FLAG
        if 8 + data_len > len(buf) - off:
            break
        chunk = buf[off + 8:off + 8 + data_len]
        frames.append(chunk if hdr & _NET_RAW_FLAG else zlib.decompress(chunk))
        off += 8 + data_len
    return frames, buf[off:]


def spawn_server(binary, sock_path, extra_args=(), stderr=subprocess.PIPE):
    # stderr lands in a pipe the caller can read, tests that expect
    # messages pass an open file to watch them live
    argv = [str(binary), str(sock_path)] + [str(a) for a in extra_args]
    Path(sock_path).unlink(missing_ok=True)
    proc = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=stderr)
    deadline = time.time() + 10
    while not Path(sock_path).exists():
        if time.time() > deadline or proc.poll() is not None:
            err = proc.stderr.read().decode(errors="replace") if proc.poll() is not None else ""
            raise AssertionError(f"server did not start: {err}")
        time.sleep(0.05)
    return proc


class Session:
    # one connected client with its object id registry, ids must be
    # registered before events for them arrive, unknown ids are a test bug
    def __init__(self, sock_path, idl, sock=None):
        self.idl = idl
        self.ifaces = {1: "x12_connection", 2: "x12_registry"}
        if sock is not None:
            # a network transport dials its own socket and hands it over
            self.sock = sock
            self.net = sock.family != socket.AF_UNIX
            self.netbuf = b""
        else:
            self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.sock.connect(str(sock_path))
        self.sock.settimeout(5)
        self.buf = b""
        self.events = []
        # descriptors the server shipped in scm rights, in arrival
        # order; a message names its fd by index into this list
        self.recv_fds = []

    def close(self):
        self.sock.close()

    def send_raw(self, data, fds=()):
        if getattr(self, "net", False):
            # envelopes carry every frame on the network transports
            self.sock.sendall(net_wrap(data))
            return
        if not fds:
            self.sock.sendall(data)
            return
        # descriptors ride along in scm rights, the wire slot in the
        # message names them by their index in the received list
        anc = [(socket.SOL_SOCKET, socket.SCM_RIGHTS,
                struct.pack(f"{len(fds)}i", *fds))]
        self.sock.sendmsg([data], anc)

    def request(self, iface, msg, object_id, fds=(), **args):
        self.send_raw(wm.encode_request(self.idl, iface, msg,
                                        object_id=object_id, **args), fds)

    def register(self, iface, id):
        self.ifaces[id] = iface

    def _read_frame(self):
        while len(self.buf) < 12:
            self._recv_more()
        (size,) = struct.unpack_from("<I", self.buf, 0)
        while len(self.buf) < size:
            self._recv_more()
        frame, self.buf = self.buf[:size], self.buf[size:]
        (object_id,) = struct.unpack_from("<I", frame, 4)
        iface = self.ifaces.get(object_id)
        assert iface, f"event for unknown object {object_id}"
        return wm.decode(self.idl, iface, "event", frame)

    def _recv_more(self):
        # recvmsg so scm rights fds land in recv_fds instead of dying
        # unread; plain recv silently drops them on linux. tls sockets
        # have no recvmsg at all, they never carry fds
        try:
            chunk, anc, _, _ = self.sock.recvmsg(65536, 256)
        except (AttributeError, NotImplementedError):
            chunk, anc = self.sock.recv(65536), ()
        if not chunk:
            raise AssertionError("server closed the connection")
        if getattr(self, "net", False):
            # envelopes peel into whole frames for the parser
            self.netbuf += chunk
            frames, self.netbuf = net_peel(self.netbuf)
            self.buf += b"".join(frames)
            return
        self.buf += chunk
        for level, typ, data in anc:
            if level == socket.SOL_SOCKET and typ == socket.SCM_RIGHTS:
                for off in range(0, len(data), 4):
                    (fd,) = struct.unpack_from("<i", data, off)
                    self.recv_fds.append(fd)

    def next_event(self):
        # returns the next event in arrival order
        if self.events:
            return self.events.pop(0)
        return self._read_frame()

    def wait_for(self, name):
        # scans events until one with the given name shows up
        while True:
            out = self.next_event()
            if out["name"] == name:
                return out

    def drain(self):
        # collects everything the server queued so far
        self.sock.settimeout(0.2)
        try:
            while True:
                self.events.append(self._read_frame())
        except (socket.timeout, AssertionError):
            pass
        self.sock.settimeout(5)
        out, self.events = self.events, []
        return out

    def closed_by_server(self):
        # a loud disconnect ends with the peer closing the socket
        self.sock.settimeout(5)
        try:
            while True:
                chunk = self.sock.recv(4096)
                if not chunk:
                    return True
        except socket.timeout:
            return False
