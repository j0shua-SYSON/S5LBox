"""Exercise the real chroot/uid-drop and kernel locking, in disposable CI data.

Requires root only to enter a private test chroot. Never uses device firmware.
"""
import errno
import fcntl
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile


def u32(value):
    return struct.pack("!I", value)


def u64(value):
    return struct.pack("!Q", value & ((1 << 64) - 1))


def path(value):
    data = value.encode()
    return u32(len(data)) + data


def exact(sock, size):
    result = b""
    while len(result) < size:
        part = sock.recv(size - len(result))
        if not part:
            raise EOFError("file service disconnected")
        result += part
    return result


def response(sock, expected=0):
    length, = struct.unpack("!I", exact(sock, 4))
    assert 4 <= length <= 65540
    error, = struct.unpack("!I", exact(sock, 4))
    data = exact(sock, length - 4)
    assert error == expected, (error, expected)
    assert not error or not data
    return data


def request(sock, op, data=b"", expected=0):
    sock.sendall(u32(1 + len(data)) + bytes([op]) + data)
    return response(sock, expected)


def main():
    assert os.geteuid() == 0, "Run this isolated fixture with sudo in CI"
    binary = str(Path(sys.argv[1]).resolve())
    uuid = "01234567-89AB-CDEF-0123-456789ABCDEF"
    with tempfile.TemporaryDirectory(prefix="hle-files-", dir=os.getcwd()) as scratch:
        root = Path(scratch)
        state = root / "state"
        apps = root / "apps"
        home = apps / uuid
        state.mkdir(mode=0o700)
        home.mkdir(parents=True)
        os.chown(home, 65534, 65534)
        (root / "outside").write_text("must not be visible")
        (home / "escape").symlink_to("../../outside")
        (home / "absolute").symlink_to(str(root / "outside"))
        (home / "root-owned").write_text("owned by the service parent")
        processes = []

        def connect(name=uuid, expected=0):
            client, server = socket.socketpair()
            client.settimeout(5)
            process = subprocess.Popen([binary, str(state), str(apps)], stdin=server,
                                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            server.close()
            processes.append(process)
            client.sendall(name.encode())
            response(client, expected)
            return client, process

        try:
            bad, p = connect("../" + "X" * 33, errno.EINVAL)
            bad.close()
            assert p.wait(timeout=5) == 1
            sock, first = connect()
            busy, p = connect(expected=errno.EBUSY)
            busy.close()
            assert p.wait(timeout=5) == 1
            for target in ("/escape", "/absolute", "/etc/passwd"):
                request(sock, 1, path(target), errno.ENOENT)
            request(sock, 9, path("/Documents"))
            assert (home / "Documents").stat().st_uid == 65534
            request(sock, 18, path("/Documents") + u32(0o750))
            assert (home / "Documents").stat().st_mode & 0o777 == 0o750
            request(sock, 18, path("/Documents") + u32(0o4755), errno.EINVAL)
            request(sock, 18, path("/Documents") + u32(0o700) + b"extra", errno.EINVAL)
            assert (home / "Documents").stat().st_mode & 0o777 == 0o750
            request(sock, 18, path("/missing") + u32(0o600), errno.ENOENT)
            request(sock, 18, path("/root-owned") + u32(0o600), errno.EPERM)
            opened = request(sock, 2, u32(1 | 2 | 8 | 32) + path("/Documents/save"))
            handle, = struct.unpack("!I", opened)
            request(sock, 2, u32(2 | 8 | 32) + path("/Documents/save"), errno.EEXIST)
            assert request(sock, 5, opened + b"hello") == u32(5)
            assert request(sock, 6, opened + u32(0) + u64(0)) == u64(0)
            assert request(sock, 4, opened + u32(10)) == b"hello"
            request(sock, 7, opened)
            assert (home / "Documents/save").read_bytes() == b"hello"
            metadata = request(sock, 1, path("/Documents/save"))
            assert struct.unpack("!Q", metadata[:8])[0] == 5
            modified=(home / "Documents/save").stat().st_mtime_ns
            request(sock, 18, path("/Documents/save") + u32(0o640))
            metadata = request(sock, 1, path("/Documents/save"))
            assert struct.unpack("!I", metadata[16:20])[0] & 0o777 == 0o640
            assert (home / "Documents/save").stat().st_mtime_ns == modified
            # Native guest and interpreter writes observe the same inode.
            (home / "Documents/save").write_bytes(b"guest")
            request(sock, 6, opened + u32(0) + u64(0))
            assert request(sock, 4, opened + u32(5)) == b"guest"
            # A second process really conflicts with POSIX locks, not a stub.
            lock = opened + u32(2) + u64(1) + u64(2)
            request(sock, 13, lock)
            with (home / "Documents/save").open("r+b") as native:
                try:
                    fcntl.lockf(native, fcntl.LOCK_EX | fcntl.LOCK_NB, 2, 1)
                    raise AssertionError("interpreter lock was not held")
                except OSError as error:
                    assert error.errno in (errno.EACCES, errno.EAGAIN)
                request(sock, 13, opened + u32(3) + u64(1) + u64(2))
                fcntl.lockf(native, fcntl.LOCK_EX | fcntl.LOCK_NB, 2, 1)
                request(sock, 13, lock, errno.EAGAIN)
                query = request(sock, 16, lock)
                assert struct.unpack("!I", query[:4])[0] == 2
                assert struct.unpack("!I", query[20:])[0] == os.getpid()
            # 64-bit offsets remain intact; this is a sparse file, not 8 GiB I/O.
            request(sock, 17, opened + u32(2))
            with (home / "Documents/save").open("r+b") as native:
                try:
                    fcntl.flock(native, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    raise AssertionError("flock was not held")
                except OSError as error:
                    assert error.errno in (errno.EACCES, errno.EAGAIN)
                request(sock, 17, opened + u32(3))
                fcntl.flock(native, fcntl.LOCK_EX | fcntl.LOCK_NB)
                request(sock, 17, opened + u32(1), errno.EAGAIN)
            request(sock, 17, opened + u32(4), errno.EINVAL)
            position = 8 * 1024**3
            assert request(sock, 6, opened + u32(0) + u64(position)) == u64(position)
            assert request(sock, 5, opened + b"end") == u32(3)
            request(sock, 8, opened + u64(3))
            assert (home / "Documents/save").stat().st_size == 3
            request(sock, 11, path("/Documents/save") + path("/Documents/moved"))
            request(sock, 6, opened + u32(0) + u64(0))
            assert request(sock, 4, opened + u32(3)) == b"gue"
            directory = request(sock, 14, u32(0) + path("/Documents"))
            assert request(sock, 12, directory) == u32(1) + path("moved")
            assert request(sock, 12, directory) == u32(0)
            request(sock, 3, directory)
            request(sock, 3, opened)
            request(sock, 4, opened + u32(1), errno.EBADF)
            request(sock, 15, path("/Documents"), errno.ENOTEMPTY)
            request(sock, 10, path("/Documents/moved"))
            request(sock, 15, path("/Documents"))
            request(sock, 1, u32(4) + b"/\x00xx", errno.EINVAL)
            request(sock, 2, u32(16) + path("/invalid"), errno.EINVAL)
            handles = []
            for index in range(256):
                item = request(sock, 2, u32(3 | 8) + path(f"/file{index}"))
                assert struct.unpack("!I", item)[0] > handle
                handles.append(item)
            request(sock, 2, u32(3 | 8) + path("/overflow"), errno.EMFILE)
            # Closing releases capacity; stale ids cannot alias the new stream.
            for old in handles[128:]:
                request(sock, 3, old)
            recycled = []
            for index in range(128):
                recycled.append(request(sock, 2, u32(1) + path(f"/file{index}")))
            assert struct.unpack("!I", recycled[0])[0] > struct.unpack("!I", handles[-1])[0]
            request(sock, 4, handles[-1] + u32(1), errno.EBADF)
            request(sock, 2, u32(3 | 8) + path("/overflow"), errno.EMFILE)
            # EOF releases every fd and the session lease, even without closes.
            request(sock, 13, handles[0] + u32(2) + u64(0) + u64(0))
            sock.close()
            assert first.wait(timeout=5) == 0
            with (home / "file0").open("r+b") as native:
                fcntl.lockf(native, fcntl.LOCK_EX | fcntl.LOCK_NB)
            again, p = connect()
            again.sendall(u32(65537))
            assert not again.recv(1)
            again.close()
            assert p.wait(timeout=5) == 0
            print("S5LBOX-HLE-FILES PASS: live writes, jail, locks, offsets, limits, disconnect")
        finally:
            for process in processes:
                if process.poll() is None:
                    process.kill()
                process.communicate(timeout=5)


if __name__ == "__main__":
    main()
