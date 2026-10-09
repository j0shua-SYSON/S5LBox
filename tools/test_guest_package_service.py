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
        def connect(op, capability=token, modern=False):
            sock = socket.create_connection(("127.0.0.1", 64321), timeout=5)
            sock.sendall((b"SPM2" if modern else b"SPM1") + capability + op)
            return sock
        def finish_status():
            with connect(b"S", modern=True) as sock:
                kind, finish = frame(sock)
                assert kind == b"F"
                assert frame(sock) == (b"S", status)
                return finish
        def change(script, hint=0, disconnect=False):
            (root / "dpkg").write_text('#!/bin/sh\n' + script)
            with connect(b"R", modern=True) as sock:
                sock.sendall(hashlib.sha256(status).digest() + bytes([hint]) + struct.pack("!I", 7) + b"fixture")
                if disconnect:
                    return
                frames = []
                while True:
                    kind, data = frame(sock)
                    if kind != b"L":
                        frames.append((kind, data))
                    if kind in (b"D", b"E"):
                        return frames
        def apply(action, digest=None):
            with connect(b"F", modern=True) as sock:
                sock.sendall(bytes([action]) + (digest or hashlib.sha256(status).digest()))
                results = []
                while True:
                    kind, data = frame(sock)
                    if kind != b"L":
                        results.append((kind, data))
                    if kind in (b"C", b"E"):
                        return results
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
            assert finish_status() == b"\x00\x00"  # failed legacy dpkg did not invent an action
            checks += 1
            (root / "finish").write_text('#!/bin/sh\necho "$@" >> fixture/finish-calls\n[ ! -f fixture/finish-fails ]\n')
            (root / "finish").chmod(0o700)
            # Ordinary stdout must not masquerade as Cydia's dedicated pipe.
            assert change('echo finish:reboot\n')[-1] == (b"D", status)
            assert finish_status() == b"\x00\x01"
            checks += 2
            # Split writes, priority, malformed/overlong lines and unknown actions.
            script = '''fd=${CYDIA%% *}
eval "printf 'finish:restart\\nfinish:return\\nfinish:bogus\\n' >&$fd"
eval "printf 'finish:re' >&$fd"
eval "printf 'load\\n' >&$fd"
eval "printf '%090d' 0 >&$fd"
eval "printf 'finish:reboot\\nfinish:restart\\n' >&$fd"
'''
            assert change(script)[-2:] == [(b"F", b"\x03\x01"), (b"D", status)]
            assert finish_status() == b"\x03\x01"
            checks += 2
            assert apply(2)[0][0] == b"E"  # stale priority
            assert apply(3, b"x" * 32)[0][0] == b"E"  # stale installed database
            assert not (root / "finish-calls").exists()
            checks += 3
            # Service restart preserves a completed-but-not-applied request.
            process.terminate(); process.wait(timeout=10)
            process = subprocess.Popen([binary], cwd=work)
            time.sleep(0.15)
            assert finish_status() == b"\x03\x01"
            assert apply(3) == [(b"A", b"\x03"), (b"C", b"\x03")]
            assert (root / "finish-calls").read_text() == "reload\n"
            assert finish_status() == b"\x00\x00"
            checks += 4
            # Failed actions are retryable; a failed dpkg is never finish-ready.
            assert change('exit 0\n', hint=2)[-2] == (b"F", b"\x02\x01")
            (root / "finish-fails").touch()
            assert apply(2)[-1][0] == b"E"
            assert finish_status() == b"\x02\x01"
            (root / "finish-fails").unlink()
            assert apply(2)[-1] == (b"C", b"\x02")
            checks += 4
            assert change('fd=${CYDIA%% *}\neval "echo finish:reboot >&$fd"\nexit 1\n')[-2][1] == b"\x04\x00"
            assert apply(4)[0][0] == b"E"
            assert change('exit 0\n')[-2][1] == b"\x04\x01"
            assert apply(4)[-1] == (b"C", b"\x04")
            checks += 4
            # A real reboot kills its parent helper before execute() returns.
            # Emulate that and relaunch with the SAME boot timestamp: the
            # consumed reboot must not reappear. Never call a host reboot.
            assert change('fd=${CYDIA%% *}\neval "echo finish:reboot >&$fd"\n')[-2][1] == b"\x04\x01"
            (root / "finish").write_text('#!/bin/sh\nkill -TERM "$PPID"\n')
            with connect(b"F", modern=True) as sock:
                sock.sendall(b"\x04" + hashlib.sha256(status).digest())
                assert frame(sock) == (b"A", b"\x04")
            process.wait(timeout=10)
            process = subprocess.Popen([binary], cwd=work)
            time.sleep(0.15)
            assert finish_status() == b"\x00\x00"
            (root / "finish").write_text('#!/bin/sh\necho "$@" >> fixture/finish-calls\n[ ! -f fixture/finish-fails ]\n')
            assert change('exit 0\n', hint=2)[-2][1] == b"\x02\x01"
            assert apply(2)[-1][0] == b"C"
            checks += 5
            # Cydia's configuration-file fallbacks include creation/removal.
            assert change('echo changed > fixture/SpringBoard.plist\n')[-2][1] == b"\x03\x01"
            assert apply(3)[-1][0] == b"C"
            assert change('echo changed > fixture/notify.conf\n')[-2][1] == b"\x04\x01"
            assert apply(4)[-1][0] == b"C"
            assert change('rm fixture/notify.conf\n')[-2][1] == b"\x04\x01"
            assert apply(4)[-1][0] == b"C"
            checks += 6
            # Completion survives a detached host. A fresh boot clears it;
            # relaunching the service in the same boot does not (tested above).
            change('fd=${CYDIA%% *}\neval "echo finish:restart >&$fd"\n', disconnect=True)
            assert finish_status() == b"\x02\x01"
            process.terminate(); process.wait(timeout=10)
            process = subprocess.Popen([binary], cwd=work, env={**os.environ, "S5LBOX_TEST_BOOT": "2"})
            time.sleep(0.15)
            assert finish_status() == b"\x00\x00"
            checks += 2
            assert change('fd=${CYDIA%% *}\neval "echo finish:reopen >&$fd"\n')[-2][1] == b"\x01\x01"
            before = (root / "finish-calls").read_text()
            assert apply(1)[-1] == (b"C", b"\x01")
            assert (root / "finish-calls").read_text() == before  # reopen is not a reboot
            checks += 3
            with connect(b"R", modern=True) as sock:
                sock.sendall(hashlib.sha256(status).digest() + b"\x04")
                assert frame(sock)[0] == b"E"  # hints cannot fabricate reboot policy
            checks += 1
            (root / "pending-finish").write_bytes(b"invalid")
            with connect(b"S", modern=True) as sock:
                assert frame(sock)[0] == b"E"
            checks += 1
            print(f"guest package service: {checks} checks passed (synthetic dpkg, no firmware)")
        finally:
            process.terminate()
            process.wait(timeout=10)


if __name__ == "__main__":
    main()
