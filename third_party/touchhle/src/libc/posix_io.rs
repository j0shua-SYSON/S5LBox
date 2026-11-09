/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
//! POSIX I/O functions (`fcntl.h`, parts of `unistd.h`, etc)

pub mod stat;
pub mod statvfs;

use crate::abi::DotDotDot;
use crate::dyld::{export_c_func, FunctionExports};
use crate::fs::{FsError, GuestFile, GuestOpenOptions, GuestPath};
use crate::libc::errno::{
    set_errno, EACCES, EBADF, EEXIST, EFAULT, EINTR, EINVAL, EIO, EISDIR, ENOENT, ENOTDIR,
    EOVERFLOW, ESPIPE,
};
use crate::libc::sys::socket::close_socket;
use crate::libc::unistd::pid_t;
use crate::mem::{
    ConstPtr, ConstVoidPtr, GuestISize, GuestUSize, Mem, MutPtr, MutVoidPtr, Ptr, SafeRead,
};
use crate::Environment;
use std::io::{Read, Seek, SeekFrom, Write};

#[derive(Default)]
pub struct State {
    /// File descriptors _other than stdin, stdout, and stderr_
    files: Vec<Option<PosixFileHostObject>>,
}
impl State {
    fn file_for_fd(&mut self, fd: FileDescriptor) -> Option<&mut PosixFileHostObject> {
        let index = usize::try_from(fd.checked_sub(NORMAL_FILENO_BASE)?).ok()?;
        self.files
            .get_mut(index)
            .and_then(|file_or_none| file_or_none.as_mut())
    }

    pub(crate) fn is_fd_open(&mut self, fd: FileDescriptor) -> bool {
        self.file_for_fd(fd).is_some()
    }
}

struct PosixFileHostObject {
    file: GuestFile,
    needs_flush: bool,
    reached_eof: bool,
    access_mode: OpenFlag,
    flags: i32,
}

// TODO: stdin/stdout/stderr handling somehow
fn file_idx_to_fd(idx: usize) -> FileDescriptor {
    FileDescriptor::try_from(idx)
        .unwrap()
        .checked_add(NORMAL_FILENO_BASE)
        .unwrap()
}
fn fd_to_file_idx(fd: FileDescriptor) -> usize {
    fd.checked_sub(NORMAL_FILENO_BASE).unwrap() as usize
}

/// File descriptor type. This alias is for readability, POSIX just uses `int`.
pub type FileDescriptor = i32;
pub const STDIN_FILENO: FileDescriptor = 0;
pub const STDOUT_FILENO: FileDescriptor = 1;
pub const STDERR_FILENO: FileDescriptor = 2;
const NORMAL_FILENO_BASE: FileDescriptor = STDERR_FILENO + 1;

/// Flags bitfield for `open`. This alias is for readability, POSIX just uses
/// `int`.
pub type OpenFlag = i32;
pub const O_RDONLY: OpenFlag = 0x0;
pub const O_WRONLY: OpenFlag = 0x1;
pub const O_RDWR: OpenFlag = 0x2;
pub const O_ACCMODE: OpenFlag = O_RDWR | O_WRONLY | O_RDONLY;

pub const O_NONBLOCK: OpenFlag = 0x4;
pub const O_APPEND: OpenFlag = 0x8;
pub const O_SHLOCK: OpenFlag = 0x10;
pub const O_EXLOCK: OpenFlag = 0x20;
pub const O_NOFOLLOW: OpenFlag = 0x100;
pub const O_CREAT: OpenFlag = 0x200;
pub const O_TRUNC: OpenFlag = 0x400;
pub const O_EXCL: OpenFlag = 0x800;

/// File control command flags.
/// This alias is for readability, POSIX just uses `int`.
pub type FileControlCommand = i32;
const F_GETFD: FileControlCommand = 1;
const F_SETFD: FileControlCommand = 2;
const F_GETLK: FileControlCommand = 7;
const F_SETLK: FileControlCommand = 8;
const F_SETLKW: FileControlCommand = 9;
const F_RDADVISE: FileControlCommand = 44;
const F_NOCACHE: FileControlCommand = 48;

/// File Descriptor flags.
/// This alias is for readability, POSIX just uses `int`.
pub type FDFlag = i32;
pub const FD_CLOEXEC: FDFlag = 1;

/// Record Locking flags.
/// This alias is for readability, POSIX just uses `short`
pub type RecordLockingFlag = i16;
pub const F_RDLCK: RecordLockingFlag = 1;
pub const F_UNLCK: RecordLockingFlag = 2;
pub const F_WRLCK: RecordLockingFlag = 3;

#[repr(C, packed)]
#[derive(Debug)]
#[allow(non_camel_case_types)]
struct flock {
    start: off_t,
    len: off_t,
    pid: pid_t,
    lock_type: i16,
    whence: i16,
}
unsafe impl SafeRead for flock {}

pub type FLockFlag = i32;
pub const LOCK_SH: FLockFlag = 1;
#[allow(dead_code)]
pub const LOCK_EX: FLockFlag = 2;
#[allow(dead_code)]
pub const LOCK_NB: FLockFlag = 4;
#[allow(dead_code)]
pub const LOCK_UN: FLockFlag = 8;

fn open(env: &mut Environment, path: ConstPtr<u8>, flags: i32, _args: DotDotDot) -> FileDescriptor {
    // TODO: handle errno properly
    set_errno(env, 0);

    // TODO: parse variadic arguments and pass them on (file creation mode)
    self::open_direct(env, path, flags)
}

/// Special extension for host code: [open] without the [DotDotDot].
pub fn open_direct(env: &mut Environment, path: ConstPtr<u8>, flags: i32) -> FileDescriptor {
    // TODO: support more flags, this list is not complete
    assert!(
        flags
            & !(O_ACCMODE
                | O_NONBLOCK
                | O_APPEND
                | O_SHLOCK
                | O_EXLOCK
                | O_NOFOLLOW
                | O_CREAT
                | O_TRUNC
                | O_EXCL)
            == 0
    );

    if path.is_null() {
        log_dbg!("open({:?}, {:#x}) => -1", path, flags);
        set_errno(env, EFAULT);
        return -1;
    }

    // TODO: respect the mode (in the variadic arguments) when creating a file
    // Note: NONBLOCK flag is ignored, assumption is all file I/O is fast
    let mut needs_flush = false;
    let mut options = GuestOpenOptions::new();
    match flags & O_ACCMODE {
        O_RDONLY => {
            options.read();
        }
        O_WRONLY => {
            options.write();
            needs_flush = true;
        }
        O_RDWR => {
            options.read().write();
            needs_flush = true;
        }
        _ => panic!(),
    };
    if (flags & O_APPEND) != 0 {
        options.append();
    }
    if (flags & O_CREAT) != 0 {
        options.create();
    }
    if (flags & O_TRUNC) != 0 {
        options.truncate();
    }
    if (flags & O_EXCL) != 0 {
        options.exclusive();
    }

    let path_string = match env.mem.cstr_at_utf8(path) {
        Ok(path_str) => path_str.to_owned(),
        Err(err) => {
            log!(
                "open() error, unable to treat {:?} as utf8 str: {:?}",
                path,
                err
            );
            // TODO: set errno
            return -1;
        }
    };
    // TODO: symlinks don't exist in the FS yet, so we can't "not follow" them.
    if flags & O_NOFOLLOW != 0 {
        log!("Ignoring O_NOFOLLOW when opening {:?}", path_string);
    }
    let res = match env
        .fs
        .open_with_options(GuestPath::new(&path_string), options)
    {
        Ok(file) => {
            let host_object = PosixFileHostObject {
                file,
                needs_flush,
                reached_eof: false,
                access_mode: flags & O_ACCMODE,
                flags: 0,
            };

            find_or_create_fd(env, host_object)
        }
        Err(error) => {
            log!("Warning: open({path:?}, {flags:#x}) failed with: {error:?}, returning -1");
            let errno = match error {
                FsError::AccessDenied => EACCES,
                FsError::AlreadyExist => EEXIST,
                FsError::DoesNotExist => ENOENT,
                FsError::InvalidParentDir => ENOTDIR,
                FsError::IsDirectory => EISDIR,
                FsError::NonexistentParentDir => ENOENT,
                FsError::ReadonlyParentDir => EACCES,
                FsError::IoError(error) => crate::fs::live::guest_errno(&error).unwrap_or(EIO),
                _ => unimplemented!(),
            };
            set_errno(env, errno);
            -1
        }
    };
    if res != -1 && (flags & (O_SHLOCK | O_EXLOCK)) != 0 {
        let kind = if flags & O_EXLOCK != 0 {
            LOCK_EX
        } else {
            LOCK_SH
        };
        let nonblocking = if flags & O_NONBLOCK != 0 { LOCK_NB } else { 0 };
        if flock(env, res, kind | nonblocking) != 0 {
            let error = crate::libc::errno::get_errno(env);
            close(env, res);
            set_errno(env, error);
            return -1;
        }
    }
    log_dbg!(
        "open({:?} {:?}, {:#x}) => {:?}",
        path,
        path_string,
        flags,
        res
    );
    res
}

fn io_errno(error: std::io::Error) -> i32 {
    crate::fs::live::guest_errno(&error).unwrap_or(EIO)
}

/// A regular-file read only touches the bytes available before EOF. Guest
/// buffers may also span adjacent VM regions with separate host allocations.
fn read_into_guest(
    file: &mut (impl Read + Seek),
    mem: &mut Mem,
    buffer: MutVoidPtr,
    size: GuestUSize,
) -> Result<usize, i32> {
    if size > GuestISize::MAX as u32 {
        return Err(EINVAL);
    }
    if size == 0 {
        return Ok(0);
    }
    // Keep ordinary reads direct: no staging allocation or extra seeks.
    if let Some(bytes) = mem.get_bytes_fallible_mut(buffer.cast_const(), size) {
        return file.read(bytes).map_err(io_errno);
    }

    let position = file.stream_position().map_err(io_errno)?;
    let end = file.seek(SeekFrom::End(0));
    file.seek(SeekFrom::Start(position)).map_err(io_errno)?;
    let available = end.map_err(io_errno)?.saturating_sub(position);
    let count = u64::from(size).min(available) as u32;
    if count == 0 {
        return Ok(0);
    }
    if !mem.is_range_mapped(buffer.to_bits(), count as usize) {
        return Err(EFAULT);
    }
    if let Some(bytes) = mem.get_bytes_fallible_mut(buffer.cast_const(), count) {
        return file.read(bytes).map_err(io_errno);
    }

    let mut scratch = vec![0; (count as usize).min(65536)];
    let mut total = 0;
    while total < count as usize {
        let chunk = scratch.len().min(count as usize - total);
        match file.read(&mut scratch[..chunk]) {
            Ok(n) => {
                // The complete destination was checked before advancing the
                // file, and mappings cannot change during this host call.
                assert!(mem.interpreter_write(buffer.to_bits() + total as u32, &scratch[..n]));
                total += n;
                if n < chunk {
                    break;
                }
            }
            Err(error) if total == 0 => return Err(io_errno(error)),
            Err(_) => break,
        }
    }
    Ok(total)
}

pub fn read(
    env: &mut Environment,
    fd: FileDescriptor,
    buffer: MutVoidPtr,
    size: GuestUSize,
) -> GuestISize {
    set_errno(env, 0);

    let Some(file) = env.libc_state.posix_io.file_for_fd(fd) else {
        log!(
            "Warning: read({:?}, {:?}, {:#x}) called with unknown fd, returning -1",
            fd,
            buffer,
            size,
        );
        set_errno(env, EBADF);
        return -1;
    };
    if file.access_mode == O_WRONLY {
        set_errno(env, EBADF);
        return -1;
    }
    // Preserve the existing directory-read result without trying to seek it.
    if matches!(file.file, GuestFile::Directory) {
        set_errno(env, EISDIR);
        return 0;
    }
    match read_into_guest(&mut file.file, &mut env.mem, buffer, size) {
        Ok(bytes_read) => {
            if bytes_read == 0 && size != 0 {
                // need to set EOF
                file.reached_eof = true;
            }
            log_dbg!(
                "read({:?}, {:?}, {:#x}) => {:#x}",
                fd,
                buffer,
                size,
                bytes_read,
            );
            bytes_read.try_into().unwrap()
        }
        Err(error) => {
            set_errno(env, error);
            log!(
                "Warning: read({:?}, {:?}, {:#x}) encountered error {:?}, returning {}",
                fd,
                buffer,
                size,
                error,
                -1,
            );
            -1
        }
    }
}

pub fn pread(
    env: &mut Environment,
    fd: FileDescriptor,
    buffer: MutVoidPtr,
    size: GuestUSize,
    offset: off_t,
) -> GuestISize {
    // Rust doesn't provide a way of reading at a specific offset on windows
    // without affecting the underlying files cursor. Rather than bringing in a
    // library that does this or dealing with the unsafe windows API directly
    // (ReadFile + Overlapped), we can emulate the behavior with a set of seek
    // and read calls.

    // Errno is set by downstream lseek and read calls
    let original_position = lseek(env, fd, 0, SEEK_CUR);
    if original_position == -1 {
        return -1;
    }

    if lseek(env, fd, offset, SEEK_SET) == -1 {
        return -1;
    }

    let bytes_read = read(env, fd, buffer, size);
    let error = crate::libc::errno::get_errno(env);
    if lseek(env, fd, original_position, SEEK_SET) == -1 {
        return -1;
    }
    set_errno(env, error);

    bytes_read
}

/// Helper for C `feof()`.
pub(super) fn eof(env: &mut Environment, fd: FileDescriptor) -> i32 {
    let file = env.libc_state.posix_io.file_for_fd(fd).unwrap();
    if file.reached_eof {
        1
    } else {
        0
    }
}

/// Helper for C `clearerr()`.
pub(super) fn clearerr(env: &mut Environment, fd: FileDescriptor) {
    // TODO: handle errno properly
    set_errno(env, 0);

    let file = env.libc_state.posix_io.file_for_fd(fd).unwrap();
    file.reached_eof = false;
}

/// Helper for C `fflush()`.
pub(super) fn fflush(env: &mut Environment, fd: FileDescriptor) -> i32 {
    // TODO: handle errno properly
    set_errno(env, 0);

    let Some(file) = env.libc_state.posix_io.file_for_fd(fd) else {
        // TODO: set errno to EBADF
        return -1;
    };
    match file.file.flush() {
        Ok(_) => 0,
        Err(_) => -1,
    }
}

fn write_from_guest(
    file: &mut impl Write,
    mem: &Mem,
    buffer: ConstVoidPtr,
    size: GuestUSize,
) -> Result<usize, i32> {
    if size > GuestISize::MAX as u32 {
        return Err(EINVAL);
    }
    if size == 0 {
        return Ok(0);
    }
    if let Some(bytes) = mem.get_bytes_fallible(buffer, size) {
        return file.write(bytes).map_err(io_errno);
    }
    if !mem.is_range_mapped(buffer.to_bits(), size as usize) {
        return Err(EFAULT);
    }
    let mut scratch = vec![0; (size as usize).min(65536)];
    let mut total = 0;
    while total < size as usize {
        let chunk = scratch.len().min(size as usize - total);
        assert!(mem.interpreter_read(buffer.to_bits() + total as u32, &mut scratch[..chunk]));
        match file.write(&scratch[..chunk]) {
            Ok(n) => {
                total += n;
                if n < chunk {
                    break;
                }
            }
            Err(error) if total == 0 => return Err(io_errno(error)),
            Err(_) => break,
        }
    }
    Ok(total)
}

pub fn write(
    env: &mut Environment,
    fd: FileDescriptor,
    buffer: ConstVoidPtr,
    size: GuestUSize,
) -> GuestISize {
    set_errno(env, 0);
    let Some(file) = env.libc_state.posix_io.file_for_fd(fd) else {
        set_errno(env, EBADF);
        return -1;
    };
    if file.access_mode == O_RDONLY {
        set_errno(env, EBADF);
        return -1;
    }
    if matches!(file.file, GuestFile::Directory) {
        set_errno(env, EISDIR);
        return -1;
    }
    match write_from_guest(&mut file.file, &env.mem, buffer, size) {
        Ok(bytes_written) => {
            if bytes_written < size as usize {
                log!(
                    "Warning: write({:?}, {:?}, {:#x}) wrote only {:#x} bytes",
                    fd,
                    buffer,
                    size,
                    bytes_written,
                );
            } else {
                log_dbg!(
                    "write({:?}, {:?}, {:#x}) => {:#x}",
                    fd,
                    buffer,
                    size,
                    bytes_written,
                );
            }
            bytes_written.try_into().unwrap()
        }
        Err(e) => {
            set_errno(env, e);
            log!(
                "Warning: write({:?}, {:?}, {:#x}) encountered error {:?}, returning -1",
                fd,
                buffer,
                size,
                e,
            );
            -1
        }
    }
}

pub fn pwrite(
    env: &mut Environment,
    fd: FileDescriptor,
    buffer: ConstVoidPtr,
    size: GuestUSize,
    offset: off_t,
) -> GuestISize {
    // Rust doesn't provide a way of writing at a specific offset on windows
    // without affecting the underlying files cursor. Rather than bringing in a
    // library that does this or dealing with the unsafe windows API directly
    // (WriteFile + Overlapped), we can emulate the behavior with a set of seek
    // and write calls.

    // Errno is set by downstream lseek and write calls
    let original_position = lseek(env, fd, 0, SEEK_CUR);
    if original_position == -1 {
        return -1;
    }

    if lseek(env, fd, offset, SEEK_SET) == -1 {
        return -1;
    }

    let bytes_written = write(env, fd, buffer, size);
    let error = crate::libc::errno::get_errno(env);
    if lseek(env, fd, original_position, SEEK_SET) == -1 {
        return -1;
    }
    set_errno(env, error);

    bytes_written
}

#[allow(non_camel_case_types)]
pub type off_t = i64;
pub const SEEK_SET: i32 = 0;
pub const SEEK_CUR: i32 = 1;
pub const SEEK_END: i32 = 2;
pub fn lseek(env: &mut Environment, fd: FileDescriptor, offset: off_t, whence: i32) -> off_t {
    let Some(file) = env.libc_state.posix_io.file_for_fd(fd) else {
        log!("lseek({:?}, {:#x}, {}) => {}", fd, offset, whence, -1);
        set_errno(env, EBADF);
        return -1;
    };

    if !file.file.is_seekable() {
        log!(
            "Warning: lseek({:?}, {:#x}, {}) => -1. Called with unseekable fd.",
            fd,
            offset,
            whence
        );
        set_errno(env, ESPIPE);
        return -1;
    }

    let start_position = match whence {
        SEEK_SET => 0,
        SEEK_CUR => match file.file.stream_position() {
            Ok(pos) => pos,
            Err(seek_error) => {
                match seek_error.kind() {
                    std::io::ErrorKind::IsADirectory => set_errno(env, EISDIR),
                    _ => unimplemented!("Unexpected seek error {:?}", seek_error),
                }
                return -1;
            }
        },
        SEEK_END => match file.file.stream_len() {
            Ok(len) => len,
            Err(seek_error) => {
                match seek_error.kind() {
                    std::io::ErrorKind::IsADirectory => set_errno(env, EISDIR),
                    _ => unimplemented!("Unexpected seek error {:?}", seek_error),
                }
                return -1;
            }
        },
        _ => {
            log!(
                "Warning: lseek({:?}, {:#x}, {}) => -1. Called with invalid \"whence\".",
                fd,
                offset,
                whence
            );
            set_errno(env, EINVAL);
            return -1;
        }
    };

    let seek_position = match start_position.checked_add_signed(offset) {
        Some(position) => position,
        None => {
            let (error_msg, errno) = if offset >= 0 {
                ("Seek position does not fit in off_t.", EOVERFLOW)
            } else {
                ("Negative seek position.", EINVAL)
            };
            log!(
                "Warning: lseek({:?}, {:#x}, {}) => -1. {}",
                fd,
                offset,
                whence,
                error_msg
            );
            set_errno(env, errno);
            return -1;
        }
    };

    if seek_position > off_t::MAX as u64 {
        log!(
            "Warning: lseek({:?}, {:#x}, {}) => -1. Seek position does not fit in off_t.",
            fd,
            offset,
            whence
        );
        set_errno(env, EOVERFLOW);
        return -1;
    }

    let res = match file.file.seek(SeekFrom::Start(seek_position)) {
        Ok(new_offset) => {
            // TODO: this side-effect should be tightened to `fseek`
            // "A successful call to the fseek() function clears
            // the end-of-file indicator for the stream..."
            file.reached_eof = false;

            new_offset.try_into().unwrap()
        }
        Err(seek_error) => {
            match seek_error.kind() {
                std::io::ErrorKind::InvalidInput => set_errno(env, EINVAL),
                std::io::ErrorKind::IsADirectory => set_errno(env, EISDIR),
                _ => unimplemented!("Unexpected seek error {:?}", seek_error),
            }
            log!(
                "Warning: lseek({:?}, {:#x}, {}) failed with error: {:?}, returning -1",
                fd,
                offset,
                whence,
                seek_error
            );
            return -1;
        }
    };
    log_dbg!("lseek({:?}, {:#x}, {}) => {}", fd, offset, whence, res);
    res
}

pub fn close(env: &mut Environment, fd: FileDescriptor) -> i32 {
    // TODO: handle errno properly
    set_errno(env, 0);

    if matches!(fd, STDIN_FILENO | STDOUT_FILENO | STDERR_FILENO) {
        log_dbg!("close({:?}) => 0", fd);
        return 0;
    }

    if fd < 0
        || env
            .libc_state
            .posix_io
            .files
            .get(fd_to_file_idx(fd))
            .is_none()
    {
        set_errno(env, EBADF);
        log!("Warning: close({:?}) failed, returning -1", fd);
        return -1;
    }

    let result = match env.libc_state.posix_io.files[fd_to_file_idx(fd)].take() {
        Some(file) => {
            // The actual closing of the file happens implicitly when `file`
            // falls out of scope. The return value is about whether actions
            // performed before closing succeed or not.
            match file.file {
                // Closing directories requires no other actions
                GuestFile::Directory => 0,
                // Socket is a special case
                GuestFile::Socket => {
                    close_socket(env, fd);
                    0
                }
                // Files must be synced if they require flushing
                _ => {
                    if !file.needs_flush {
                        0
                    } else {
                        match file.file.sync_all() {
                            Ok(()) => 0,
                            Err(_) => {
                                // TODO: set errno
                                -1
                            }
                        }
                    }
                }
            }
        }
        None => {
            set_errno(env, EBADF);
            -1
        }
    };

    if result == 0 {
        log_dbg!("close({:?}) => 0", fd);
    } else {
        log!("Warning: close({:?}) failed, returning -1", fd);
    }
    result
}

fn rename(env: &mut Environment, old: ConstPtr<u8>, new: ConstPtr<u8>) -> i32 {
    // TODO: handle errno properly
    set_errno(env, 0);

    let old = env.mem.cstr_at_utf8(old).unwrap();
    let new = env.mem.cstr_at_utf8(new).unwrap();
    let res = match env.fs.rename(GuestPath::new(&old), GuestPath::new(&new)) {
        Ok(_) => 0,
        Err(_) => -1,
    };
    log_dbg!("rename('{}', '{}') => {}", old, new, res);
    res
}

pub fn getcwd(env: &mut Environment, buf_ptr: MutPtr<u8>, buf_size: GuestUSize) -> MutPtr<u8> {
    let working_directory = env.fs.working_directory();
    if !env.fs.is_dir(working_directory) {
        // TODO: set errno to ENOENT
        log!(
            "Warning: getcwd({:?}, {:#x}) failed, returning NULL",
            buf_ptr,
            buf_size
        );
        return Ptr::null();
    }

    let working_directory = env.fs.working_directory().as_str().as_bytes();

    if buf_ptr.is_null() {
        // The buffer size argument is presumably ignored in this mode.
        // This mode is an extension, which might explain the strange API.
        let res = env.mem.alloc_and_write_cstr(working_directory);
        log_dbg!("getcwd(NULL, _) => {:?} ({:?})", res, working_directory);
        return res;
    }

    // Includes space for null terminator
    let res_size: GuestUSize = u32::try_from(working_directory.len()).unwrap() + 1;

    if buf_size < res_size {
        // TODO: set errno to EINVAL or ERANGE as appropriate
        log!(
            "Warning: getcwd({:?}, {:#x}) failed, returning NULL",
            buf_ptr,
            buf_size
        );
        return Ptr::null();
    }

    let buf = env.mem.bytes_at_mut(buf_ptr, res_size);
    buf[..(res_size - 1) as usize].copy_from_slice(working_directory);
    buf[(res_size - 1) as usize] = b'\0';

    log_dbg!(
        "getcwd({:?}, {:#x}) => {:?}, wrote {:?} ({:#x} bytes)",
        buf_ptr,
        buf_size,
        buf_ptr,
        working_directory,
        res_size
    );
    buf_ptr
}

fn chdir(env: &mut Environment, path_ptr: ConstPtr<u8>) -> i32 {
    // TODO: handle errno properly
    set_errno(env, 0);

    let path = GuestPath::new(env.mem.cstr_at_utf8(path_ptr).unwrap());
    match env.fs.change_working_directory(path) {
        Ok(new) => {
            log_dbg!(
                "chdir({:?}) => 0, new working directory: {:?}",
                path_ptr,
                new,
            );
            0
        }
        Err(()) => {
            log!("Warning: chdir({:?}) failed, could not change working directory to {:?}, returning -1", path_ptr, path);
            // TODO: set errno
            -1
        }
    }
}
// TODO: fchdir(), once open() on a directory is supported.

fn fcntl(
    env: &mut Environment,
    fd: FileDescriptor,
    cmd: FileControlCommand,
    args: DotDotDot,
) -> i32 {
    // TODO: handle errno properly
    set_errno(env, 0);

    if fd < NORMAL_FILENO_BASE || env.libc_state.posix_io.file_for_fd(fd).is_none() {
        set_errno(env, EBADF);
        return -1;
    }
    if matches!(cmd, F_GETLK | F_SETLK | F_SETLKW)
        && matches!(
            env.libc_state.posix_io.file_for_fd(fd).unwrap().file,
            GuestFile::Live(_)
        )
    {
        let pointer: MutPtr<flock> = args.start().next(env);
        if pointer.is_null() {
            set_errno(env, EFAULT);
            return -1;
        }
        let mut lock = env.mem.read(pointer);
        let result = live_record_lock(env, fd, cmd, &mut lock);
        return match result {
            Ok(()) => {
                if cmd == F_GETLK {
                    env.mem.write(pointer, lock);
                }
                0
            }
            Err(error) => {
                set_errno(env, error);
                -1
            }
        };
    }

    if fd >= NORMAL_FILENO_BASE
        && env
            .libc_state
            .posix_io
            .files
            .get(fd_to_file_idx(fd))
            .is_none()
    {
        set_errno(env, EBADF);
        return -1;
    }

    match cmd {
        F_GETFD => {
            let file = env.libc_state.posix_io.file_for_fd(fd).unwrap();
            return file.flags;
        }
        F_SETFD => {
            // SET/GETFD are responsible for managing the CLOEXEC flag on an FD.
            // When set, exec* calls automatically close open FDs to prevent
            // them from leaking. Since touchHLE only runs a single process and
            // non-jailbroken apps can not call exec* functions, it's safe to
            // ignore the flag being set.

            // TODO: When exec* is added implement CLOEXEC functionality
            let flags: i32 = args.start().next(env);
            assert!(matches!(flags, FD_CLOEXEC | 0));
            if flags & FD_CLOEXEC == FD_CLOEXEC {
                log!(
                    "TODO: fcntl({}, F_SETFD, {}) called. CLOEXEC currently not supported.",
                    fd,
                    flags
                );
            }
            let file = env.libc_state.posix_io.file_for_fd(fd).unwrap();
            file.flags = flags;
        }
        F_GETLK => {
            let lock_ptr: MutPtr<flock> = args.start().next(env);
            let mut lock = env.mem.read(lock_ptr);

            if let Err(error_code) = validate_lock(env, fd, &lock) {
                set_errno(env, error_code);
                return -1;
            }

            // Since locks are never set, claim no conflict by setting lock_type
            // to F_UNLCK. For more info check F_SETLK match arm.
            // TODO: actually check locks set by other processes and return
            // a conflict if it exists
            log!(
                "TODO: fcntl({}, F_GETLK, {:?}) called. Locking unimplemented, any conflicts will be unreported.",
                fd,
                lock
            );
            lock.lock_type = F_UNLCK;
            env.mem.write(lock_ptr, lock);
        }
        F_SETLK => {
            let lock_ptr: MutPtr<flock> = args.start().next(env);
            let lock = env.mem.read(lock_ptr);

            if let Err(error_code) = validate_lock(env, fd, &lock) {
                set_errno(env, error_code);
                return -1;
            }

            // POSIX locks are process based which means that any threads within
            // a process don't conflict with its own process's locks.
            // For example, setting a lock that conflicts with another lock set
            // by a thread in the same process results in the lock being either:
            // upgraded, extended, split, etc., but it will not conflict.
            // Practically, since touchHLE supports only one process, POSIX
            // locks don't do anything, so they can temporarily be ignored.
            // TODO: Actually set locks when multiproccess support is added and
            // the file system supports it.
            log!(
                "TODO: fcntl({}, F_SETLK, {:?}) called. Locking unimplemented, ignoring lock.",
                fd,
                lock
            );
        }
        F_NOCACHE => {
            let mut args = args.start();
            let arg: i32 = args.next(env);
            assert_eq!(arg, 1);
            log!(
                "TODO: Ignoring enabling F_NOCACHE for file descriptor {}",
                fd
            );
        }
        F_RDADVISE => {
            log_dbg!("TODO: Ignoring F_RDADVISE for file descriptor {}", fd);
        }
        _ => unimplemented!(),
    }
    0 // success
}

fn normalized_lock_range(base: u64, start: i64, length: i64) -> Result<(u64, u64), i32> {
    let base: i64 = base.try_into().map_err(|_| EOVERFLOW)?;
    let start = base.checked_add(start).ok_or(EOVERFLOW)?;
    let (start, length) = if length < 0 {
        (
            start.checked_add(length).ok_or(EOVERFLOW)?,
            length.checked_neg().ok_or(EOVERFLOW)?,
        )
    } else {
        (start, length)
    };
    if start < 0 {
        return Err(EINVAL);
    }
    start.checked_add(length).ok_or(EOVERFLOW)?;
    Ok((start as u64, length as u64))
}

fn live_record_lock(
    env: &mut Environment,
    fd: FileDescriptor,
    cmd: i32,
    lock: &mut flock,
) -> Result<(), i32> {
    let kind = match lock.lock_type {
        F_RDLCK => 1,
        F_WRLCK => 2,
        F_UNLCK => 3,
        _ => return Err(EINVAL),
    };
    let file = env.libc_state.posix_io.file_for_fd(fd).ok_or(EBADF)?;
    let io_errno = |error: std::io::Error| crate::fs::live::guest_errno(&error).unwrap_or(EIO);
    let base = match lock.whence as i32 {
        SEEK_SET => 0,
        SEEK_CUR => file.file.stream_position().map_err(io_errno)?,
        SEEK_END => file.file.stream_len().map_err(io_errno)?,
        _ => return Err(EINVAL),
    };
    let (start, length) = normalized_lock_range(base, lock.start, lock.len)?;
    let GuestFile::Live(file) = &file.file else {
        return Err(EBADF);
    };
    let identity = file.handle_id();
    if cmd == F_GETLK {
        let conflict = file.query_lock(kind, start, length).map_err(io_errno)?;
        lock.lock_type = match conflict.kind {
            1 => F_RDLCK,
            2 => F_WRLCK,
            _ => F_UNLCK,
        };
        if conflict.kind != 3 {
            lock.start = conflict.start.try_into().map_err(|_| EOVERFLOW)?;
            lock.len = conflict.length.try_into().map_err(|_| EOVERFLOW)?;
            lock.whence = SEEK_SET as i16;
            lock.pid = conflict.pid;
        }
        return Ok(());
    }
    loop {
        let file = env.libc_state.posix_io.file_for_fd(fd).ok_or(EBADF)?;
        let GuestFile::Live(file) = &file.file else {
            return Err(EBADF);
        };
        // Another guest thread may close/reuse the fd while this one waits.
        if file.handle_id() != identity {
            return Err(EBADF);
        }
        match file.lock(kind, start, length).map_err(io_errno) {
            Err(error)
                if cmd == F_SETLKW && matches!(error, EACCES | crate::libc::errno::EAGAIN) =>
            {
                // Yield the guest thread, not the host UI or the guest broker.
                // Stop controls remain effective through the scheduler.
                env.sleep(std::time::Duration::from_millis(10));
            }
            result => return result,
        }
    }
}

#[cfg(test)]
mod live_lock_tests {
    use super::*;
    #[test]
    fn lock_ranges_normalize_without_signed_overflow() {
        assert_eq!(normalized_lock_range(10, -5, 3), Ok((5, 3)));
        assert_eq!(normalized_lock_range(10, 0, -4), Ok((6, 4)));
        assert_eq!(normalized_lock_range(0, 1, 0), Ok((1, 0)));
        assert_eq!(normalized_lock_range(0, -1, 1), Err(EINVAL));
        assert_eq!(normalized_lock_range(i64::MAX as u64, 1, 0), Err(EOVERFLOW));
        assert_eq!(normalized_lock_range(0, 0, i64::MIN), Err(EOVERFLOW));
    }
}

fn flock(env: &mut Environment, fd: FileDescriptor, operation: FLockFlag) -> i32 {
    let result = (|| -> Result<(), i32> {
        if fd < NORMAL_FILENO_BASE {
            return Err(EBADF);
        }
        let kind = match operation & !LOCK_NB {
            LOCK_SH => 1,
            LOCK_EX => 2,
            LOCK_UN => 3,
            _ => return Err(EINVAL),
        };
        let file = env.libc_state.posix_io.file_for_fd(fd).ok_or(EBADF)?;
        let GuestFile::Live(file) = &file.file else {
            log!("TODO: flock on a host-backed file");
            return Ok(());
        };
        let identity = file.handle_id();
        loop {
            let file = env.libc_state.posix_io.file_for_fd(fd).ok_or(EBADF)?;
            let GuestFile::Live(file) = &file.file else {
                return Err(EBADF);
            };
            if file.handle_id() != identity {
                return Err(EBADF);
            }
            match file
                .flock(kind)
                .map_err(|error| crate::fs::live::guest_errno(&error).unwrap_or(EIO))
            {
                Err(error)
                    if operation & LOCK_NB == 0
                        && matches!(error, EACCES | crate::libc::errno::EAGAIN) =>
                {
                    env.sleep(std::time::Duration::from_millis(10));
                }
                result => return result,
            }
        }
    })();
    match result {
        Ok(()) => 0,
        Err(error) => {
            set_errno(env, error);
            -1
        }
    }
}

fn fsync(env: &mut Environment, fd: FileDescriptor) -> i32 {
    let Some(file) = env.libc_state.posix_io.file_for_fd(fd) else {
        log!(
            "Warning: fsync({:?}) called with unknown fd, returning -1",
            fd,
        );
        set_errno(env, EBADF);
        return -1;
    };

    match file.file.sync_all() {
        Ok(()) => 0,
        Err(error) => {
            match error.kind() {
                std::io::ErrorKind::PermissionDenied => {
                    log!(
                        "Warning: fsync({:?}) sync failed with error: {:?}, returning 0 to match expected behavior",
                        fd,
                        error
                    );
                    return 0;
                }
                std::io::ErrorKind::Unsupported => set_errno(env, EINVAL),
                std::io::ErrorKind::Interrupted => set_errno(env, EINTR),
                _ => set_errno(env, EIO),
            }

            log!(
                "Warning: fsync({:?}) sync failed with error: {:?}, returning -1",
                fd,
                error
            );
            -1
        }
    }
}

fn ftruncate(env: &mut Environment, fd: FileDescriptor, len: off_t) -> i32 {
    // TODO: handle errno properly
    set_errno(env, 0);

    let file = env.libc_state.posix_io.file_for_fd(fd).unwrap();
    match file.file.set_len(len as u64) {
        Ok(()) => 0,
        Err(_) => -1, // TODO: set errno
    }
}

fn truncate(env: &mut Environment, path_ptr: ConstPtr<u8>, len: off_t) -> i32 {
    // TODO: handle errno properly
    set_errno(env, 0);

    let path_string = match env.mem.cstr_at_utf8(path_ptr) {
        Ok(s) => s.to_owned(),
        Err(_) => {
            return -1; // TODO: set errno
        }
    };

    let fd = open_direct(env, path_ptr, O_WRONLY);
    if fd < 0 {
        log_dbg!("truncate('{}', {}) => -1", path_string, len);
        return -1;
    }

    let res = ftruncate(env, fd, len);

    close(env, fd);

    log_dbg!("truncate('{}', {}) => {}", path_string, len, res);
    res
}

pub const FUNCTIONS: FunctionExports = &[
    export_c_func!(open(_, _, _)),
    export_c_func!(read(_, _, _)),
    export_c_func!(pread(_, _, _, _)),
    export_c_func!(write(_, _, _)),
    export_c_func!(pwrite(_, _, _, _)),
    export_c_func!(lseek(_, _, _)),
    export_c_func!(close(_)),
    export_c_func!(rename(_, _)),
    export_c_func!(getcwd(_, _)),
    export_c_func!(chdir(_)),
    export_c_func!(fcntl(_, _, _)),
    export_c_func!(flock(_, _)),
    export_c_func!(fsync(_)),
    export_c_func!(ftruncate(_, _)),
    export_c_func!(truncate(_, _)),
];

#[cfg(test)]
mod file_io_tests {
    use super::*;
    use std::io::Cursor;

    #[test]
    fn oversized_read_only_requires_the_bytes_before_eof() {
        let mut mem = Mem::new();
        mem.set_null_segment_size(4096);
        mem.reserve(0x10000, 4096);
        let buffer = Ptr::from_bits(0x10ff0);
        let mut file = Cursor::new(b"short resource");
        assert_eq!(read_into_guest(&mut file, &mut mem, buffer, 0x100000), Ok(14));
        assert_eq!(mem.bytes_at(buffer.cast(), 14), b"short resource");
        assert_eq!(file.position(), 14);
        assert_eq!(read_into_guest(&mut file, &mut mem, buffer, 0x100000), Ok(0));
        assert_eq!(read_into_guest(&mut file, &mut mem, Ptr::null(), 0), Ok(0));
    }

    #[test]
    fn reads_cross_separate_host_allocations_and_stop_at_eof() {
        let mut mem = Mem::new();
        mem.set_null_segment_size(4096);
        mem.reserve(0x10000, 0x10000);
        mem.reserve(0x20000, 0x10000);
        let buffer = Ptr::from_bits(0x10013);
        let data: Vec<u8> = (0..80001).map(|n| (n % 251) as u8).collect();
        let mut file = Cursor::new(&data);
        assert_eq!(read_into_guest(&mut file, &mut mem, buffer, 0x20000), Ok(data.len()));
        let mut actual = vec![0; data.len()];
        assert!(mem.interpreter_read(buffer.to_bits(), &mut actual));
        assert_eq!(actual, data);
        assert_eq!(file.position(), data.len() as u64);
    }

    #[test]
    fn invalid_destinations_fail_without_consuming_or_writing_data() {
        let mut mem = Mem::new();
        mem.set_null_segment_size(4096);
        mem.reserve(0x10000, 4096);
        let buffer = Ptr::from_bits(0x10ffc);
        let mut file = Cursor::new(b"0123456789");
        file.set_position(1);
        assert_eq!(read_into_guest(&mut file, &mut mem, buffer, 9), Err(EFAULT));
        assert_eq!(file.position(), 1);
        assert_eq!(mem.bytes_at(buffer.cast(), 4), &[0; 4]);
        for address in [0, 0x60000000, 0xfffffffc] {
            assert_eq!(read_into_guest(&mut file, &mut mem, Ptr::from_bits(address), 9), Err(EFAULT));
            assert_eq!(file.position(), 1);
        }
        assert_eq!(read_into_guest(&mut file, &mut mem, buffer, u32::MAX), Err(EINVAL));
        assert_eq!(file.position(), 1);
    }

    #[test]
    fn invalid_descriptors_are_rejected_without_panicking() {
        let mut state = State::default();
        for fd in [i32::MIN, -1, 0, 1, 2, 3, i32::MAX] {
            assert!(!state.is_fd_open(fd));
        }
        state.files.push(None);
        assert!(!state.is_fd_open(3));
    }

    #[test]
    fn writes_cross_regions_and_reject_holes_before_changing_the_file() {
        let mut mem = Mem::new();
        mem.set_null_segment_size(4096);
        mem.reserve(0x10000, 4096);
        mem.reserve(0x11000, 4096);
        assert!(mem.interpreter_write(0x10ffd, b"resource"));
        let mut file = Cursor::new(Vec::new());
        assert_eq!(write_from_guest(&mut file, &mem, Ptr::from_bits(0x10ffd), 8), Ok(8));
        assert_eq!(file.get_ref(), b"resource");
        assert_eq!(write_from_guest(&mut file, &mem, Ptr::from_bits(0x11ffd), 8), Err(EFAULT));
        assert_eq!(file.position(), 8);
        assert_eq!(file.get_ref(), b"resource");
        assert_eq!(write_from_guest(&mut file, &mem, Ptr::null(), 1), Err(EFAULT));
        assert_eq!(write_from_guest(&mut file, &mem, Ptr::null(), 0), Ok(0));
    }
}

/// Helper function, not part of API
fn find_or_create_fd(env: &mut Environment, host_object: PosixFileHostObject) -> FileDescriptor {
    let idx = if let Some(free_idx) = env
        .libc_state
        .posix_io
        .files
        .iter()
        .position(|f| f.is_none())
    {
        env.libc_state.posix_io.files[free_idx] = Some(host_object);
        free_idx
    } else {
        let idx = env.libc_state.posix_io.files.len();
        env.libc_state.posix_io.files.push(Some(host_object));
        idx
    };
    file_idx_to_fd(idx)
}

/// Helper function for socket creation, not part of API
pub fn find_or_create_socket(env: &mut Environment) -> FileDescriptor {
    let host_object = PosixFileHostObject {
        file: GuestFile::Socket,
        needs_flush: false,
        reached_eof: false,
        access_mode: O_RDWR,
        flags: 0,
    };
    find_or_create_fd(env, host_object)
}

/// Helper function for socket check, not part of API
pub fn is_socket(env: &mut Environment, fd: FileDescriptor) -> bool {
    let guest_file = &env
        .libc_state
        .posix_io
        .files
        .get(fd_to_file_idx(fd))
        .unwrap()
        .as_ref()
        .unwrap()
        .file;
    matches!(guest_file, GuestFile::Socket)
}

/// Helper function to validate lock, not part of API. Assumes fd is a valid
/// file descriptor
fn validate_lock(env: &mut Environment, fd: FileDescriptor, lock: &flock) -> Result<(), i32> {
    let lock_type = lock.lock_type;
    if !matches!(lock_type, F_RDLCK | F_UNLCK | F_WRLCK) {
        return Err(EINVAL);
    }

    let whence = lock.whence as i32;
    let lock_start = match whence {
        SEEK_SET => lock.start,
        SEEK_CUR => {
            let file = env.libc_state.posix_io.file_for_fd(fd).unwrap();
            let file_position = file.file.stream_position().unwrap();
            file_position as i64 + lock.start
        }
        SEEK_END => {
            let file = env.libc_state.posix_io.file_for_fd(fd).unwrap();
            let size: i64 = file.file.stream_len().unwrap().try_into().unwrap();
            size + lock.start
        }
        _ => {
            return Err(EINVAL);
        }
    };

    if lock_start < 0 {
        return Err(EINVAL);
    }

    Ok(())
}
