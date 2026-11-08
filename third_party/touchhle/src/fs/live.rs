// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//! Bounded client for guest-kernel-owned file descriptors. Transport failure is
//! sticky: a lost response must never cause a write or rename to be replayed.
use std::cell::{Cell, RefCell};
use std::fmt;
use std::io::{self, Read, Seek, SeekFrom, Write};
use std::rc::Rc;

const LIMIT: usize = 65_536;

/// One complete request/reply over an already authenticated guest connection.
/// The implementation must have bounded I/O timeouts and must not reconnect.
pub trait Exchange {
    fn exchange(&mut self, request: &[u8], response: &mut [u8]) -> io::Result<usize>;
}

#[derive(Debug)]
pub struct GuestError(pub i32);
impl fmt::Display for GuestError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "guest filesystem error {}", self.0)
    }
}
impl std::error::Error for GuestError {}

pub fn guest_errno(error: &io::Error) -> Option<i32> {
    error.get_ref()?.downcast_ref::<GuestError>().map(|e| e.0)
}

fn malformed() -> io::Error {
    io::Error::new(
        io::ErrorKind::InvalidData,
        "Invalid guest filesystem response",
    )
}
fn u32_at(bytes: &[u8], offset: usize) -> io::Result<u32> {
    let data = bytes.get(offset..offset + 4).ok_or_else(malformed)?;
    Ok(u32::from_be_bytes(data.try_into().unwrap()))
}
fn u64_at(bytes: &[u8], offset: usize) -> io::Result<u64> {
    let data = bytes.get(offset..offset + 8).ok_or_else(malformed)?;
    Ok(u64::from_be_bytes(data.try_into().unwrap()))
}
fn add_path(bytes: &mut Vec<u8>, path: &str) -> io::Result<()> {
    if !path.starts_with('/') || path.len() >= 1024 || path.as_bytes().contains(&0) {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "Invalid guest path",
        ));
    }
    bytes.extend_from_slice(&(path.len() as u32).to_be_bytes());
    bytes.extend_from_slice(path.as_bytes());
    Ok(())
}

struct Connection {
    exchange: RefCell<Box<dyn Exchange>>,
    failed: Cell<bool>,
}
impl Connection {
    fn invalid<T>(&self) -> io::Result<T> {
        self.failed.set(true);
        Err(malformed())
    }
    fn call(&self, op: u8, payload: &[u8]) -> io::Result<Vec<u8>> {
        if self.failed.get() {
            return Err(io::Error::new(
                io::ErrorKind::BrokenPipe,
                "Guest files disconnected",
            ));
        }
        if payload.len() >= LIMIT {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "Guest request is too large",
            ));
        }
        let mut request = Vec::with_capacity(payload.len() + 5);
        request.extend_from_slice(&((payload.len() + 1) as u32).to_be_bytes());
        request.push(op);
        request.extend_from_slice(payload);
        let mut response = vec![0; LIMIT + 8];
        let count = match self.exchange.borrow_mut().exchange(&request, &mut response) {
            Ok(count) => count,
            Err(error) => {
                self.failed.set(true);
                return Err(error);
            }
        };
        if count < 8 || count > response.len() || u32_at(&response, 0)? as usize != count - 4 {
            return self.invalid();
        }
        response.truncate(count);
        let error = u32_at(&response, 4)?;
        if error != 0 {
            if count != 8 || error > 255 {
                return self.invalid();
            }
            // Darwin guest errno values, not the build host's errno constants.
            let kind = match error {
                2 => io::ErrorKind::NotFound,
                1 | 13 => io::ErrorKind::PermissionDenied,
                17 => io::ErrorKind::AlreadyExists,
                22 => io::ErrorKind::InvalidInput,
                35 => io::ErrorKind::WouldBlock,
                _ => io::ErrorKind::Other,
            };
            return Err(io::Error::new(kind, GuestError(error as i32)));
        }
        Ok(response[8..].to_vec())
    }
    fn empty(&self, op: u8, payload: &[u8]) -> io::Result<()> {
        if !self.call(op, payload)?.is_empty() {
            return self.invalid();
        }
        Ok(())
    }
}

#[derive(Clone)]
pub struct LiveFs(Rc<Connection>);
impl fmt::Debug for LiveFs {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("LiveFs")
            .field("disconnected", &self.0.failed.get())
            .finish()
    }
}

#[derive(Debug)]
pub struct Metadata {
    pub size: u64,
    pub modified: i64,
    pub mode: u32,
    pub uid: u32,
    pub gid: u32,
    pub inode: u64,
}
impl Metadata {
    pub fn is_dir(&self) -> bool {
        self.mode & 0o170000 == 0o040000
    }
    pub fn is_file(&self) -> bool {
        self.mode & 0o170000 == 0o100000
    }
}

impl LiveFs {
    pub fn new(exchange: Box<dyn Exchange>) -> Self {
        Self(Rc::new(Connection {
            exchange: RefCell::new(exchange),
            failed: Cell::new(false),
        }))
    }
    pub fn stat(&self, path: &str) -> io::Result<Metadata> {
        let mut request = Vec::new();
        add_path(&mut request, path)?;
        let reply = self.0.call(1, &request)?;
        if reply.len() != 36 {
            return self.0.invalid();
        }
        Ok(Metadata {
            size: u64_at(&reply, 0)?,
            modified: u64_at(&reply, 8)? as i64,
            mode: u32_at(&reply, 16)?,
            uid: u32_at(&reply, 20)?,
            gid: u32_at(&reply, 24)?,
            inode: u64_at(&reply, 28)?,
        })
    }
    fn open_kind(&self, path: &str, flags: u32, directory: bool) -> io::Result<LiveFile> {
        let mut request = flags.to_be_bytes().to_vec();
        add_path(&mut request, path)?;
        let reply = self.0.call(if directory { 14 } else { 2 }, &request)?;
        if reply.len() != 4 || u32_at(&reply, 0)? == 0 {
            return self.0.invalid();
        }
        Ok(LiveFile {
            fs: self.clone(),
            handle: u32_at(&reply, 0)?,
        })
    }
    /// Wire flags: read=1, write=2, append=4, create=8, truncate=16, exclusive=32.
    pub fn open(&self, path: &str, flags: u32) -> io::Result<LiveFile> {
        self.open_kind(path, flags, false)
    }
    pub fn read_dir(&self, path: &str) -> io::Result<Vec<String>> {
        let directory = self.open_kind(path, 0, true)?;
        let mut names = Vec::new();
        loop {
            let reply = self.0.call(12, &directory.handle.to_be_bytes())?;
            let count = u32_at(&reply, 0).inspect_err(|_| self.0.failed.set(true))? as usize;
            if count > 128 || names.len() + count > 100_000 {
                return self.0.invalid();
            }
            let mut offset = 4;
            for _ in 0..count {
                let length =
                    u32_at(&reply, offset).inspect_err(|_| self.0.failed.set(true))? as usize;
                offset += 4;
                let Some(bytes) = reply.get(offset..offset.saturating_add(length)) else {
                    return self.0.invalid();
                };
                let Ok(name) = std::str::from_utf8(bytes) else {
                    return self.0.invalid();
                };
                if length == 0
                    || length > 255
                    || name.contains(['/', '\0'])
                    || name == "."
                    || name == ".."
                {
                    return self.0.invalid();
                }
                names.push(name.to_owned());
                offset += length;
            }
            if offset != reply.len() {
                return self.0.invalid();
            }
            if count == 0 {
                return Ok(names);
            }
        }
    }
    fn path_operation(&self, op: u8, path: &str) -> io::Result<()> {
        let mut request = Vec::new();
        add_path(&mut request, path)?;
        self.0.empty(op, &request)
    }
    pub fn mkdir(&self, path: &str) -> io::Result<()> {
        self.path_operation(9, path)
    }
    pub fn chmod(&self, path: &str, mode: u32) -> io::Result<()> {
        let mut request = Vec::new();
        add_path(&mut request, path)?;
        request.extend_from_slice(&mode.to_be_bytes());
        self.0.empty(18, &request)
    }
    pub fn unlink(&self, path: &str) -> io::Result<()> {
        self.path_operation(10, path)
    }
    pub fn rmdir(&self, path: &str) -> io::Result<()> {
        self.path_operation(15, path)
    }
    pub fn rename(&self, from: &str, to: &str) -> io::Result<()> {
        let mut request = Vec::new();
        add_path(&mut request, from)?;
        add_path(&mut request, to)?;
        self.0.empty(11, &request)
    }
}

#[derive(Debug)]
pub struct LiveFile {
    fs: LiveFs,
    handle: u32,
}
#[derive(Debug)]
pub struct LockConflict {
    pub kind: u32,
    pub start: u64,
    pub length: u64,
    pub pid: i32,
}
impl Drop for LiveFile {
    fn drop(&mut self) {
        // Best effort only. A disconnect closes every guest descriptor. Do not
        // panic here, especially while unwinding an unrelated guest failure.
        let _ = self.fs.0.empty(3, &self.handle.to_be_bytes());
    }
}
impl LiveFile {
    pub fn handle_id(&self) -> u32 {
        self.handle
    }
    pub fn sync_all(&self) -> io::Result<()> {
        self.fs.0.empty(7, &self.handle.to_be_bytes())
    }
    pub fn set_len(&self, len: u64) -> io::Result<()> {
        let mut request = self.handle.to_be_bytes().to_vec();
        request.extend_from_slice(&len.to_be_bytes());
        self.fs.0.empty(8, &request)
    }
    pub fn lock(&self, kind: u32, start: u64, length: u64) -> io::Result<()> {
        let mut request = self.handle.to_be_bytes().to_vec();
        request.extend_from_slice(&kind.to_be_bytes());
        request.extend_from_slice(&start.to_be_bytes());
        request.extend_from_slice(&length.to_be_bytes());
        self.fs.0.empty(13, &request)
    }
    pub fn flock(&self, kind: u32) -> io::Result<()> {
        let mut request = self.handle.to_be_bytes().to_vec();
        request.extend_from_slice(&kind.to_be_bytes());
        self.fs.0.empty(17, &request)
    }
    pub fn query_lock(&self, kind: u32, start: u64, length: u64) -> io::Result<LockConflict> {
        let mut request = self.handle.to_be_bytes().to_vec();
        request.extend_from_slice(&kind.to_be_bytes());
        request.extend_from_slice(&start.to_be_bytes());
        request.extend_from_slice(&length.to_be_bytes());
        let reply = self.fs.0.call(16, &request)?;
        if reply.len() != 24 {
            return self.fs.0.invalid();
        }
        let kind = u32_at(&reply, 0)?;
        if !(1..=3).contains(&kind) {
            return self.fs.0.invalid();
        }
        Ok(LockConflict {
            kind,
            start: u64_at(&reply, 4)?,
            length: u64_at(&reply, 12)?,
            pid: u32_at(&reply, 20)? as i32,
        })
    }
}
impl Read for LiveFile {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        // LIMIT bounds one transport message, not one regular-file read. Apps
        // commonly read an entire asset archive in a single call. Exposing the
        // wire limit leaves their remaining buffer uninitialized.
        let mut total = 0;
        while total < buf.len() {
            crate::host::poll_control();
            let count = (buf.len() - total).min(LIMIT);
            let mut request = self.handle.to_be_bytes().to_vec();
            request.extend_from_slice(&(count as u32).to_be_bytes());
            let reply = match self.fs.0.call(4, &request) {
                Ok(reply) => reply,
                Err(_) if total != 0 => return Ok(total),
                Err(error) => return Err(error),
            };
            if reply.len() > count {
                return self.fs.0.invalid();
            }
            buf[total..total + reply.len()].copy_from_slice(&reply);
            total += reply.len();
            if reply.len() < count {
                break; // Actual short read/EOF from the filesystem.
            }
        }
        Ok(total)
    }
}
impl Write for LiveFile {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        let mut total = 0;
        while total < buf.len() {
            crate::host::poll_control();
            let count = (buf.len() - total).min(LIMIT - 5);
            let mut request = self.handle.to_be_bytes().to_vec();
            request.extend_from_slice(&buf[total..total + count]);
            let reply = match self.fs.0.call(5, &request) {
                Ok(reply) => reply,
                // Never replay a write whose reply was lost. Report only the
                // acknowledged prefix; the disconnected stream stays poisoned.
                Err(_) if total != 0 => return Ok(total),
                Err(error) => return Err(error),
            };
            if reply.len() != 4 {
                return self.fs.0.invalid();
            }
            let written = u32_at(&reply, 0)? as usize;
            if written > count {
                return self.fs.0.invalid();
            }
            total += written;
            if written < count {
                break;
            }
        }
        Ok(total)
    }
    fn flush(&mut self) -> io::Result<()> {
        self.sync_all()
    }
}
impl Seek for LiveFile {
    fn seek(&mut self, pos: SeekFrom) -> io::Result<u64> {
        let (whence, offset): (u32, i64) = match pos {
            SeekFrom::Start(offset) => (
                0,
                offset.try_into().map_err(|_| {
                    io::Error::new(
                        io::ErrorKind::InvalidInput,
                        "Seek offset exceeds signed 64-bit range",
                    )
                })?,
            ),
            SeekFrom::Current(offset) => (1, offset),
            SeekFrom::End(offset) => (2, offset),
        };
        let mut request = self.handle.to_be_bytes().to_vec();
        request.extend_from_slice(&whence.to_be_bytes());
        request.extend_from_slice(&offset.to_be_bytes());
        let reply = self.fs.0.call(6, &request)?;
        if reply.len() != 8 {
            return self.fs.0.invalid();
        }
        u64_at(&reply, 0)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::VecDeque;

    struct Script(VecDeque<(u8, io::Result<Vec<u8>>)>);
    impl Exchange for Script {
        fn exchange(&mut self, request: &[u8], response: &mut [u8]) -> io::Result<usize> {
            let (op, reply) = self
                .0
                .pop_front()
                .expect("unexpected retry or extra request");
            assert_eq!(u32_at(request, 0).unwrap() as usize, request.len() - 4);
            assert_eq!(request[4], op);
            let reply = reply?;
            response[..reply.len()].copy_from_slice(&reply);
            Ok(reply.len())
        }
    }
    fn reply(error: u32, data: &[u8]) -> io::Result<Vec<u8>> {
        let mut bytes = ((data.len() + 4) as u32).to_be_bytes().to_vec();
        bytes.extend_from_slice(&error.to_be_bytes());
        bytes.extend_from_slice(data);
        Ok(bytes)
    }
    fn scripted(script: Vec<(u8, io::Result<Vec<u8>>)>) -> LiveFs {
        LiveFs::new(Box::new(Script(script.into())))
    }

    #[test]
    fn lost_write_is_not_replayed_and_drop_does_not_reconnect() {
        let fs = scripted(vec![
            (2, reply(0, &7u32.to_be_bytes())),
            (
                5,
                Err(io::Error::new(io::ErrorKind::TimedOut, "lost reply")),
            ),
        ]);
        let mut file = fs.open("/Documents/data", 3).unwrap();
        assert_eq!(
            file.write(b"change").unwrap_err().kind(),
            io::ErrorKind::TimedOut
        );
        assert_eq!(
            file.write(b"change").unwrap_err().kind(),
            io::ErrorKind::BrokenPipe
        );
        drop(file);
    }
    #[test]
    fn queried_locks_preserve_ranges_and_reject_unknown_kinds() {
        let mut conflict = 2u32.to_be_bytes().to_vec();
        conflict.extend_from_slice(&8192u64.to_be_bytes());
        conflict.extend_from_slice(&17u64.to_be_bytes());
        conflict.extend_from_slice(&42u32.to_be_bytes());
        let mut invalid = conflict.clone();
        invalid[..4].copy_from_slice(&4u32.to_be_bytes());
        let fs = scripted(vec![
            (2, reply(0, &7u32.to_be_bytes())),
            (16, reply(0, &conflict)),
            (13, reply(35, &[])),
            (17, reply(35, &[])),
            (17, reply(0, &[])),
            (16, reply(0, &invalid)),
        ]);
        let file = fs.open("/Documents/database", 3).unwrap();
        let lock = file.query_lock(2, 0, 0).unwrap();
        assert_eq!(
            (lock.kind, lock.start, lock.length, lock.pid),
            (2, 8192, 17, 42)
        );
        assert_eq!(guest_errno(&file.lock(2, 0, 0).unwrap_err()), Some(35));
        assert_eq!(guest_errno(&file.flock(2).unwrap_err()), Some(35));
        file.flock(3).unwrap();
        assert_eq!(
            file.query_lock(2, 0, 0).unwrap_err().kind(),
            io::ErrorKind::InvalidData
        );
        assert_eq!(
            file.query_lock(2, 0, 0).unwrap_err().kind(),
            io::ErrorKind::BrokenPipe
        );
    }
    #[test]
    fn malformed_reply_poisoning_is_sticky() {
        let fs = scripted(vec![(1, reply(0, &[0; 35]))]);
        assert_eq!(
            fs.stat("/file").unwrap_err().kind(),
            io::ErrorKind::InvalidData
        );
        assert_eq!(
            fs.stat("/file").unwrap_err().kind(),
            io::ErrorKind::BrokenPipe
        );
    }
    #[test]
    fn guest_errors_do_not_disconnect_or_use_host_errno_numbers() {
        let fs = scripted(vec![(9, reply(17, &[])), (9, reply(0, &[]))]);
        let error = fs.mkdir("/Documents").unwrap_err();
        assert_eq!(guest_errno(&error), Some(17));
        assert_eq!(error.kind(), io::ErrorKind::AlreadyExists);
        fs.mkdir("/new").unwrap();
    }
    #[test]
    fn file_limit_error_leaves_the_session_usable() {
        let fs = scripted(vec![
            (2, reply(24, &[])),
            (2, reply(0, &257u32.to_be_bytes())),
            (3, reply(0, &[])),
        ]);
        assert_eq!(guest_errno(&fs.open("/asset", 1).unwrap_err()), Some(24));
        drop(fs.open("/asset", 1).unwrap());
    }
    #[test]
    fn byte_counts_offsets_and_close_are_checked() {
        let fs = scripted(vec![
            (2, reply(0, &3u32.to_be_bytes())),
            (4, reply(0, b"abc")),
            (6, reply(0, &(8u64 * 1024 * 1024 * 1024).to_be_bytes())),
            (3, reply(0, &[])),
        ]);
        let mut file = fs.open("/file", 1).unwrap();
        let mut bytes = [0; 4];
        assert_eq!(file.read(&mut bytes).unwrap(), 3);
        assert_eq!(&bytes[..3], b"abc");
        assert_eq!(
            file.seek(SeekFrom::Start(8 * 1024 * 1024 * 1024)).unwrap(),
            8 * 1024 * 1024 * 1024
        );
    }
    #[test]
    fn regular_file_transfers_span_transport_frames() {
        let first = vec![0xa5; LIMIT];
        let last = vec![0x5a; 19];
        let fs = scripted(vec![
            (2, reply(0, &3u32.to_be_bytes())),
            (4, reply(0, &first)),
            (4, reply(0, &last)),
            (4, reply(0, &[])),
            (5, reply(0, &((LIMIT - 5) as u32).to_be_bytes())),
            (5, reply(0, &24u32.to_be_bytes())),
            (3, reply(0, &[])),
        ]);
        let mut file = fs.open("/Documents/archive", 3).unwrap();
        let mut bytes = vec![0; LIMIT + 19];
        assert_eq!(file.read(&mut bytes).unwrap(), bytes.len());
        assert_eq!(&bytes[..LIMIT], first);
        assert_eq!(&bytes[LIMIT..], last);
        assert_eq!(file.read(&mut bytes).unwrap(), 0);
        assert_eq!(file.write(&bytes).unwrap(), bytes.len());
        // A zero-length operation must not perform an exchange.
        assert_eq!(file.read(&mut []).unwrap(), 0);
        assert_eq!(file.write(&[]).unwrap(), 0);
    }
    #[test]
    fn chunked_write_reports_only_acknowledged_bytes_after_disconnect() {
        let fs = scripted(vec![
            (2, reply(0, &3u32.to_be_bytes())),
            (5, reply(0, &((LIMIT - 5) as u32).to_be_bytes())),
            (
                5,
                Err(io::Error::new(io::ErrorKind::TimedOut, "lost reply")),
            ),
        ]);
        let mut file = fs.open("/Documents/archive", 3).unwrap();
        assert_eq!(file.write(&vec![0x55; LIMIT * 2]).unwrap(), LIMIT - 5);
        assert_eq!(
            file.write(b"retry").unwrap_err().kind(),
            io::ErrorKind::BrokenPipe
        );
    }
    #[test]
    fn directory_names_cannot_escape_the_mount() {
        let mut entries = 1u32.to_be_bytes().to_vec();
        entries.extend_from_slice(&2u32.to_be_bytes());
        entries.extend_from_slice(b"..");
        let fs = scripted(vec![
            (14, reply(0, &1u32.to_be_bytes())),
            (12, reply(0, &entries)),
        ]);
        assert_eq!(
            fs.read_dir("/").unwrap_err().kind(),
            io::ErrorKind::InvalidData
        );
    }

    #[test]
    fn mount_preserves_container_identity_and_path_boundaries() {
        let mut metadata = vec![0; 36];
        metadata[16..20].copy_from_slice(&0o040700u32.to_be_bytes());
        let files = scripted(vec![(1, reply(0, &metadata))]);
        let home = "/var/mobile/Applications/01234567-89AB-CDEF-0123-456789ABCDEF";
        let fs = super::super::Fs::new_live(files, home).unwrap();
        assert_eq!(fs.home_directory().as_str(), home);
        for path in [
            format!("{home}/Documents/save"),
            format!("/private{home}/Documents/save"),
        ] {
            assert_eq!(
                fs.live_path(super::super::GuestPath::new(&path)).unwrap().1,
                "/Documents/save"
            );
        }
        assert!(fs
            .live_path(super::super::GuestPath::new(&format!("{home}/../other")))
            .is_none());
        assert!(fs
            .live_path(super::super::GuestPath::new(&format!("{home}more/file")))
            .is_none());
        assert!(fs
            .live_path(super::super::GuestPath::new("/usr/lib/libz.dylib"))
            .is_none());
    }
}
