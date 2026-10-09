"""Loopback executor contract tests. Uses only a fake dpkg in a private cwd."""
import hashlib
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time


def exact(sock, size):
    out = b""
    while len(out) < size:
        chunk = sock.recv(size - len(out))
        if not chunk:
            raise EOFError("service closed")
        out += chunk
    return out


def frame(sock):
    size, = struct.unpack("!I", exact(sock, 4))
    assert 0 < size <= 4 * 1024 * 1024 + 1
    data = exact(sock, size)
    return data[:1], data[1:]


def main():
    binary = str(Path(sys.argv[1]).resolve())
    token = bytes(range(32))
    status = b"Package: fixture\nVersion: 1\nStatus: install ok installed\n\n"
    checks = 0
    with tempfile.TemporaryDirectory(prefix="package-service-", dir=os.getcwd()) as work:
        root = Path(work) / "fixture"
        root.mkdir(mode=0o700)
        (root / "capability").write_bytes(token)
        (root / "capability").chmod(0o600)
        (root / "status").write_bytes(status)
        (root / "dpkg").write_text('#!/bin/sh\necho "$@" >> fixture/calls\necho "fixture dpkg output"\n[ ! -f fixture/fail ]\n')
        (root / "dpkg").chmod(0o700)
        process = subprocess.Popen([binary], cwd=work)
        def connect(op, capability=token):
            sock = socket.create_connection(("127.0.0.1", 64321), timeout=5)
            sock.sendall(b"SPM1" + capability + op)
            return sock
        try:
            for _ in range(100):
                if process.poll() is not None:
                    raise RuntimeError("test service exited")
                try:
                    with connect(b"S") as sock:
                        assert frame(sock) == (b"S", status)
                    break
                except ConnectionRefusedError:
                    time.sleep(0.02)
            else:
                raise RuntimeError("service did not listen")
            checks += 1
            with connect(b"S", b"x" * 32) as sock:
                assert sock.recv(1) == b""
            checks += 1
            with connect(b"I") as sock:
                sock.sendall(b"x" * 32)
                assert frame(sock)[0] == b"E"
            checks += 1
            assert not (root / "calls").exists()
            checks += 1
            with connect(b"I") as sock:
                sock.sendall(hashlib.sha256(status).digest() + struct.pack("!II", 1, 3) + b"x" * 32 + b"deb")
                assert frame(sock)[0] == b"E"
            assert not (root / "calls").exists()
            checks += 2
            for name in (b"--root", b"a;id", b"../x", b"a\x00b"):
                with connect(b"R") as sock:
                    sock.sendall(hashlib.sha256(status).digest() + struct.pack("!I", len(name)) + name)
                    assert sock.recv(1) == b""
                checks += 1
            payload = b"test-deb" * 8193
            with connect(b"I") as sock:
                sock.sendall(hashlib.sha256(status).digest() + struct.pack("!I", 2))
                part = struct.pack("!I", len(payload)) + hashlib.sha256(payload).digest() + payload
                sock.sendall(part)
                assert not (root / "calls").exists()  # no half-upload execution
                checks += 1
                sock.sendall(part)
                while True:
                    kind, data = frame(sock)
                    if kind != b"L":
                        assert (kind, data) == (b"D", status)
                        break
            assert len((root / "calls").read_text().splitlines()) == 2
            assert not list(root.glob("transaction-*"))
            checks += 3
            (root / "fail").touch()
            with connect(b"R") as sock:
                sock.sendall(hashlib.sha256(status).digest() + struct.pack("!I", 7) + b"fixture")
                while True:
                    kind, data = frame(sock)
                    if kind != b"L":
                        assert kind == b"E"
                        break
            checks += 1
            print(f"guest package service: {checks} checks passed (synthetic dpkg, no firmware)")
        finally:
            process.terminate()
            process.wait(timeout=10)


if __name__ == "__main__":
    main()
