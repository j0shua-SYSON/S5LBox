"""Execute the generated first-boot finalizer with simulated guest services.

No firmware, root privileges, network or host SpringBoard is used. Only package
unpacking/cache generation and OS services are stubbed; registration, ordering,
bounded waits, checkpoints and completion execute from the real generated job.
"""
import pathlib
import subprocess
import sys
import tempfile


MOCKS = r'''
mock_chown() { return 0; }
mock_chmod() { return 0; }
mock_killall() {
    if [ "$1" = -0 ]; then
        [ ! -e no-springboard ]
    else
        echo respring >> events
        [ ! -e fail-respring ] || return 1
        : > resprung
    fi
}
mock_su() {
    echo uicache >> events
    [ ! -e fail-uicache ] || return 1
    /bin/rm -f ./private/var/mobile/Library/Caches/SpringBoardIconCache/com.saurik.Cydia
    [ ! -e fail-registration ] || return 0
    echo com.saurik.Cydia > ./private/var/mobile/Library/Caches/com.apple.mobile.installation.plist
}
mock_sleep() {
    echo tick >> ticks
    if [ -e resprung ] && [ ! -e delay-icon ]; then
        echo icon > ./private/var/mobile/Library/Caches/SpringBoardIconCache/com.saurik.Cydia
    fi
}
'''


def main():
    executable, bash, build_dir = sys.argv[1:]
    # Explicit build-local staging: never use the system temporary directory.
    with tempfile.TemporaryDirectory(prefix="first-boot-", dir=build_dir) as tmp:
        root = pathlib.Path(tmp)
        emitted = root / "generated.sh"
        subprocess.run([executable, "--write-install-script", str(emitted)],
                       check=True, capture_output=True)
        script = emitted.read_text()
        registration = script.index("cydia=/Applications/Cydia.app/Cydia_\n")
        apt = script.index("cache_archive=\"$packages/")
        completion = script.index(': >"$state/complete.partial"')
        assert registration < apt < completion, "APT must not delay registration"
        header = script[:script.index("install_one() {")]
        # Keep the actual durable completion code and shortcut from production.
        body = (header + script[registration:apt] +
                'echo apt-cache >> events\n[ ! -e fail-cache ] || exit 1\n' +
                script[completion:])
        body = body.replace("/private/", "./private/")
        body = body.replace("/Applications/", "./Applications/")
        body = body.replace("/usr/bin/uicache", "./uicache")
        for original, mock in [("/bin/chown", "mock_chown"),
                               ("/bin/chmod", "mock_chmod"),
                               ("/usr/bin/killall", "mock_killall"),
                               ("/bin/su", "mock_su"),
                               ("/bin/sync", ":"),
                               ("/bin/sleep", "mock_sleep")]:
            body = body.replace(original, mock)
        # NTFS has no Unix setuid/setgid bits. Those remain checked by the C
        # plan test; the shell harness tests the publication state machine.
        body = body.replace('[ -u "$cydia" ] && [ -g "$cydia" ]',
                            '[ -f "$cydia" ]')
        harness = MOCKS + body

        def fixture(name):
            path = root / name
            for directory in ["private/var/lib/s5lbox/packages", "private/var/log",
                              "private/var/mobile/Library/Caches/SpringBoardIconCache",
                              "Applications/Cydia.app"]:
                (path / directory).mkdir(parents=True, exist_ok=True)
            (path / "Applications/Cydia.app/Cydia_").touch()
            (path / "uicache").write_text("#!/bin/sh\n", newline="\n")
            (path / "uicache").chmod(0o755)
            (path / "run.sh").write_text(harness, newline="\n")
            return path

        def run(path, expected):
            result = subprocess.run([bash, "run.sh"], cwd=path,
                                    capture_output=True, text=True, timeout=15)
            assert result.returncode == expected, (path.name, result.stderr,
                (path / "private/var/log/s5lbox-guest-install.log").read_text())

        def events(path):
            event_file = path / "events"
            return event_file.read_text().splitlines() if event_file.exists() else []

        def complete(path):
            return (path / "private/var/lib/s5lbox/complete").exists()

        fresh = fixture("fresh")
        run(fresh, 0)
        assert complete(fresh)
        assert events(fresh) == ["uicache", "respring", "apt-cache"]
        run(fresh, 0)
        assert events(fresh) == ["uicache", "respring", "apt-cache"]

        interrupted = fixture("interrupted-cache")
        (interrupted / "fail-cache").touch()
        run(interrupted, 1)
        assert not complete(interrupted)
        (interrupted / "fail-cache").unlink()
        run(interrupted, 0)
        assert complete(interrupted)
        assert events(interrupted) == ["uicache", "respring", "apt-cache", "apt-cache"]

        pending = fixture("delayed-icon")
        (pending / "delay-icon").touch()
        run(pending, 1)
        assert not complete(pending)
        assert events(pending) == ["uicache", "respring"]
        assert len((pending / "ticks").read_text().splitlines()) == 60
        run(pending, 1)
        assert events(pending) == ["uicache", "respring"]
        (pending / "delay-icon").unlink()
        run(pending, 0)
        assert complete(pending)
        assert events(pending) == ["uicache", "respring", "apt-cache"]

        absent = fixture("no-springboard")
        (absent / "no-springboard").touch()
        run(absent, 1)
        assert not complete(absent) and events(absent) == []
        assert len((absent / "ticks").read_text().splitlines()) == 60
        (absent / "no-springboard").unlink()
        run(absent, 0)
        assert complete(absent)

        for failure in ["fail-uicache", "fail-registration", "fail-respring"]:
            path = fixture(failure)
            (path / failure).touch()
            run(path, 1)
            assert not complete(path)
            assert "apt-cache" not in events(path)
            assert not (path / "private/var/lib/s5lbox/springboard-refreshed").exists()
            (path / failure).unlink()
            run(path, 0)
            assert complete(path)
        print("first-boot finalizer: fresh install, retry, timeout, failures and idempotence passed")


if __name__ == "__main__":
    main()
