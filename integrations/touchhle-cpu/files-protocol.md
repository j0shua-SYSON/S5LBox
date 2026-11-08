# Live application files

This component is enabled in the touchHLE feature build, not release 1.1.0.
It operates **inside the emulated guest**, never against the physical iPhone's
filesystem. The guest kernel remains the sole HFS writer. No mirror is copied
back into a running disk image.

The authenticated package helper forks a child, closes inherited file
descriptors except its connection, then calls `s5lbox_hle_files`. The integrated
app requires the guest support package for launch coordination. See the
[device verification record](README.md#verified-behavior) for tested paths.
The child reads a 36-byte application-container UUID, takes a single-session
lease, enters that installed application's directory with `chroot`, and drops
all supplementary groups and root privileges. It replies with an empty result
on success. It must exit after returning; it cannot rejoin the privileged
package-service loop. The `H` test channel has only the single-file-session
lease. The app uses `L` instead: it additionally requires an exclusive kernel
launch lock and a live SpringBoard quiescence reply before opening any app
files. There is no fallback from `L` to `H`. Missing hooks, active/pending
native user apps, or a retained launchd job reject the launch. Both locks live
until the broker closes the app files or its process exits.

All integers are big-endian. A request is `u32 length, u8 operation, payload`.
The length includes the operation byte and is limited to 65,536. A response is
`u32 length, u32 guest_errno, payload`; errors have no payload. Paths are
`u32 byte_length, UTF-8 bytes`, absolute within the app container, under 1,024
bytes, without NUL. Handles are opaque monotonically allocated `u32` values;
at most 256 may be open. A broken connection invalidates all handles and locks.
Never automatically replay a mutation after losing its response.

| Op | Request payload | Success payload |
| --- | --- | --- |
| 1 stat | path | u64 size, i64 mtime, u32 mode/uid/gid, u64 inode |
| 2 open | u32 flags, path | u32 handle |
| 3 close | handle | empty |
| 4 read | handle, u32 count | up to count bytes |
| 5 write | handle, bytes | u32 bytes written |
| 6 seek | handle, u32 whence, i64 offset | u64 position |
| 7 sync | handle | empty |
| 8 truncate | handle, u64 length | empty |
| 9 mkdir | path | empty |
| 10 unlink | path | empty |
| 11 rename | source path, destination path | empty |
| 12 readdir | directory handle | u32 count, repeated length-prefixed names |
| 13 lock | handle, u32 kind, u64 start/length | empty |
| 14 opendir | u32 zero, path | u32 directory handle |
| 15 rmdir | path | empty |
| 16 query lock | same as lock | u32 kind, u64 start/length, u32 pid |
| 17 flock | handle, u32 kind | empty |

Open flags: read=1, write=2, append=4, create=8, truncate=16, exclusive=32.
Whence: start=0, current=1, end=2. Lock kind: shared=1, exclusive=2, unlock=3.
Locks are real, nonblocking POSIX byte-range locks, with ordinary process/fd
lifetime semantics, including release on disconnect. A zero lock length means
through EOF. `flock` uses nonblocking whole-file BSD locks; waiting is handled
by the interpreter's cancelable scheduler, never inside the file broker.
Directory batches contain up to 128 names; zero means EOF.

The chroot deliberately prevents symlinks from reaching outside the selected
container. Absolute symlinks using the original full container path are not
remapped. System library resources remain the interpreter's read-only overlay.
Invalid payloads receive errors; oversized or truncated frames close the stream.
An idle authenticated stream stays open during pause; a partial frame times out.
