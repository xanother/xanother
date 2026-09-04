# Copyright (c) lezginskiy <xanother-x12@proton.me>
# SPDX-License-Identifier: MIT

# shared client for the core skeleton e2e tests, one implementation of the
# socket dance so every scenario file stays focused on its own assertions

import os
import socket
import struct
import subprocess
import time
from pathlib import Path

from tests import wire_model as wm


def spawn_server(binary, sock_path, extra_args=()):
    argv = [str(binary), str(sock_path)] + [str(a) for a in extra_args]
    Path(sock_path).unlink(missing_ok=True)
    proc = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
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
    def __init__(self, sock_path, idl):
        self.idl = idl
        self.ifaces = {1: "x12_connection", 2: "x12_registry"}
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(str(sock_path))
        self.sock.settimeout(5)
        self.buf = b""
        self.events = []

    def close(self):
        self.sock.close()

    def send_raw(self, data, fds=()):
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
            chunk = self.sock.recv(65536)
            if not chunk:
                raise AssertionError("server closed the connection")
            self.buf += chunk
        (size,) = struct.unpack_from("<I", self.buf, 0)
        while len(self.buf) < size:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise AssertionError("server closed the connection")
            self.buf += chunk
        frame, self.buf = self.buf[:size], self.buf[size:]
        (object_id,) = struct.unpack_from("<I", frame, 4)
        iface = self.ifaces.get(object_id)
        assert iface, f"event for unknown object {object_id}"
        return wm.decode(self.idl, iface, "event", frame)

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
