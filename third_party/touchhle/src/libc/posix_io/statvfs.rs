/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
//! POSIX `sys/statvfs.h`

use crate::dyld::{export_c_func, FunctionExports};
use crate::libc::errno::{set_errno, EFAULT, EOVERFLOW};
use crate::libc::posix_io::FileDescriptor;
use crate::libc::sys::mount::{fstatfs_inner, statfs, statfs_inner};
use crate::mem::{guest_size_of, ConstPtr, MutPtr, SafeRead};
use crate::Environment;

#[allow(non_camel_case_types)]
pub type fsblkcnt_t = u32;
#[allow(non_camel_case_types)]
pub type fsfilcnt_t = u32;

pub const ST_RDONLY: u32 = 1;
pub const ST_NOSUID: u32 = 2;

#[allow(non_camel_case_types)]
#[derive(Default)]
#[repr(C, packed)]
pub struct statvfs {
    f_bsize: u32,
    f_frsize: u32,
    f_blocks: fsblkcnt_t,
    f_bfree: fsblkcnt_t,
    f_bavail: fsblkcnt_t,
    f_files: fsfilcnt_t,
    f_ffree: fsfilcnt_t,
    f_favail: fsfilcnt_t,
    f_fsid: u32,
    f_flag: u32,
    f_namemax: u32,
}
unsafe impl SafeRead for statvfs {}

impl TryFrom<statfs> for statvfs {
    type Error = i32;
    fn try_from(statfs: statfs) -> Result<Self, i32> {
        Ok(Self {
            // From the manpage:
            // "Corresponds to the f_iosize member of struct statfs."
            f_bsize: statfs.f_iosize.try_into().map_err(|_| EOVERFLOW)?,
            // From the manpage:
            // "This corresponds to the f_bsize member of struct statfs."
            f_frsize: statfs.f_bsize,
            f_blocks: statfs.f_blocks.try_into().map_err(|_| EOVERFLOW)?,
            f_bfree: statfs.f_bfree.try_into().map_err(|_| EOVERFLOW)?,
            f_bavail: statfs.f_bavail.try_into().map_err(|_| EOVERFLOW)?,
            f_files: statfs.f_files.try_into().map_err(|_| EOVERFLOW)?,
            f_ffree: statfs.f_ffree.try_into().map_err(|_| EOVERFLOW)?,
            f_favail: statfs.f_ffree.try_into().map_err(|_| EOVERFLOW)?,
            // From the manpage: "Not meaningful in this implementation"
            f_fsid: 0,
            // According to the manpage:
            // "There are two flags defined for the f_flag member"
            // ST_RDONLY and ST_NOSUID
            // Darwin MNT_NOSUID is 8; POSIX ST_NOSUID is 2.
            f_flag: (statfs.f_flags & ST_RDONLY)
                | if statfs.f_flags & 8 != 0 {
                    ST_NOSUID
                } else {
                    0
                },
            f_namemax: 255,
        })
    }
}

fn write_statvfs(env: &mut Environment, buf: MutPtr<statvfs>, result: Result<statfs, i32>) -> i32 {
    if env
        .mem
        .get_bytes_fallible_mut(buf.cast_const().cast(), guest_size_of::<statvfs>())
        .is_none()
    {
        set_errno(env, EFAULT);
        return -1;
    }
    match result.and_then(statvfs::try_from) {
        Ok(statvfs) => {
            env.mem.write(buf, statvfs);
            0
        }
        Err(error) => {
            set_errno(env, error);
            -1
        }
    }
}

fn statvfs(env: &mut Environment, path: ConstPtr<u8>, buf: MutPtr<statvfs>) -> i32 {
    let result = statfs_inner(env, path);
    write_statvfs(env, buf, result)
}

fn fstatvfs(env: &mut Environment, fd: FileDescriptor, buf: MutPtr<statvfs>) -> i32 {
    let result = fstatfs_inner(env, fd);
    write_statvfs(env, buf, result)
}

pub const FUNCTIONS: FunctionExports = &[
    export_c_func!(statvfs(_, _)),
    export_c_func!(fstatvfs(_, _)),
];

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn converts_mount_flags_and_reports_count_overflow() {
        // The C structures contain only integers and byte arrays.
        let mut input: statfs = unsafe { std::mem::zeroed() };
        input.f_flags = 1 | 8 | 0x1000;
        input.f_bsize = 4096;
        input.f_iosize = 65536;
        input.f_blocks = u32::MAX.into();
        let result = statvfs::try_from(input).unwrap();
        let (flags, blocks) = (result.f_flag, result.f_blocks);
        assert_eq!(flags, ST_RDONLY | ST_NOSUID);
        assert_eq!(blocks, u32::MAX);
        let mut input: statfs = unsafe { std::mem::zeroed() };
        input.f_blocks = u32::MAX as u64 + 1;
        assert!(matches!(statvfs::try_from(input), Err(EOVERFLOW)));
    }
}
