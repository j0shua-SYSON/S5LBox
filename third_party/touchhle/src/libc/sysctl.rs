/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
//! `sys/sysctl.h`

use std::collections::HashMap;
use std::sync::LazyLock;

use crate::dyld::{export_c_func, FunctionExports};
use crate::libc::errno::{set_errno, EFAULT, EINVAL, ENAMETOOLONG, ENOENT, ENOMEM, EPERM, ERANGE};
use crate::mem::{ConstPtr, GuestUSize, MutPtr, MutVoidPtr, PAGE_SIZE};
use crate::Environment;

// Top level constants
const CTL_KERN: i32 = 1;
const CTL_HW: i32 = 6;

// CTL_KERN
const KERN_OSTYPE: i32 = 1;
const KERN_OSRELEASE: i32 = 2;
const KERN_OSREV: i32 = 3;
const KERN_VERSION: i32 = 4;
const KERN_HOSTNAME: i32 = 10;
const KERN_PROC: i32 = 14;
const KERN_OSVERSION: i32 = 65;

// KERN_PROC
const KERN_PROC_ALL: i32 = 0;

// CTL_HW
const HW_MACHINE: i32 = 1;
const HW_MODEL: i32 = 2;
const HW_NCPU: i32 = 3;
const HW_PHYSMEM: i32 = 5;
const HW_USERMEM: i32 = 6;
const HW_PAGESIZE: i32 = 7;
const HW_BUS_FREQ: i32 = 14;
const HW_CPU_FREQ: i32 = 15;
const HW_MEMSIZE: i32 = 24;

/// There is probably more idiomatic way to express "a variable sized array of
/// integers, up to 12 max", but this enum would do for now.
#[derive(Copy, Clone, PartialEq, Eq, Hash, Debug)]
enum SysCtlNamePath {
    Undefined,
    Length2(i32, i32),
    Length4(i32, i32, i32, i32),
}

// Clippy complains about the type.
// Below values corresponds to the original iPhone.
// Reference https://www.mail-archive.com/misc@openbsd.org/msg80988.html
static SYSCTL_VALUES: [(SysCtlNamePath, &str, SysInfoType); 18] = [
    // Generic CPU, I/O
    (SysCtlNamePath::Length2(CTL_HW, HW_MACHINE), "hw.machine", SysInfoType::String(b"iPhone1,1")),
    (SysCtlNamePath::Length2(CTL_HW, HW_MODEL), "hw.model", SysInfoType::String(b"M68AP")),
    (SysCtlNamePath::Length2(CTL_HW, HW_NCPU), "hw.ncpu", SysInfoType::Int32(1)),
    (SysCtlNamePath::Undefined, "hw.cputype", SysInfoType::Int32(12)),
    (SysCtlNamePath::Undefined, "hw.cpusubtype", SysInfoType::Int32(6)),
    (SysCtlNamePath::Length2(CTL_HW, HW_CPU_FREQ), "hw.cpufrequency", SysInfoType::Int64(412000000)),
    (SysCtlNamePath::Length2(CTL_HW, HW_BUS_FREQ), "hw.busfrequency", SysInfoType::Int64(103000000)),
    (SysCtlNamePath::Length2(CTL_HW, HW_PHYSMEM), "hw.physmem", SysInfoType::Int32(121634816)),
    (SysCtlNamePath::Length2(CTL_HW, HW_USERMEM), "hw.usermem", SysInfoType::Int32(93564928)),
    (SysCtlNamePath::Length2(CTL_HW, HW_MEMSIZE), "hw.memsize", SysInfoType::Int64(121634816)),
    (SysCtlNamePath::Length2(CTL_HW, HW_PAGESIZE), "hw.pagesize", SysInfoType::Int64(PAGE_SIZE as i64)),
    // High kernel limits
    (SysCtlNamePath::Length2(CTL_KERN, KERN_OSTYPE), "kern.ostype", SysInfoType::String(b"Darwin")),
    (SysCtlNamePath::Length2(CTL_KERN, KERN_OSRELEASE), "kern.osrelease", SysInfoType::String(b"10.0.0d3")),
    (SysCtlNamePath::Length2(CTL_KERN, KERN_OSREV), "kern.osrevision", SysInfoType::Int32(199506)),
    (SysCtlNamePath::Length2(CTL_KERN, KERN_HOSTNAME), "kern.hostname", SysInfoType::String(b"touchHLE")), // this is arbitrary
    (SysCtlNamePath::Length2(CTL_KERN, KERN_VERSION), "kern.version", SysInfoType::String(b"Darwin Kernel Version 10.0.0d3: Wed May 13 22:11:58 PDT 2009; root:xnu-1357.2.89~4/RELEASE_ARM_S5L8900X")),
    (SysCtlNamePath::Length2(CTL_KERN, KERN_OSVERSION), "kern.osversion", SysInfoType::String(b"7A341")),
    // Last 0 here is an unnamed placeholder
    (SysCtlNamePath::Length4(CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0), "kern.proc.all", SysInfoType::Struct),
];

static STRING_MAP: LazyLock<HashMap<&str, SysInfoType>> = LazyLock::new(|| {
    // Can't use from_iter because the closure erases the lifetime
    let mut hashmap = HashMap::new();
    for (_, str, value) in SYSCTL_VALUES.iter() {
        hashmap.insert(*str, value.clone());
    }
    hashmap
});

#[allow(clippy::type_complexity)]
static INT_MAP: LazyLock<HashMap<SysCtlNamePath, (&str, SysInfoType)>> = LazyLock::new(|| {
    // Can't use from_iter because the closure erases the lifetime
    let mut hashmap = HashMap::new();
    for (ints, str, value) in SYSCTL_VALUES.iter() {
        if *ints == SysCtlNamePath::Undefined {
            // skip entries which do not have integer name paths
            continue;
        }
        // Darwin's legacy numeric OIDs use int-sized values for these
        // properties; their named counterparts are 64-bit nodes.
        let value = match ints {
            SysCtlNamePath::Length2(CTL_HW, HW_PAGESIZE | HW_CPU_FREQ | HW_BUS_FREQ) => {
                let SysInfoType::Int64(value) = value else {
                    unreachable!()
                };
                SysInfoType::Int32((*value).try_into().unwrap())
            }
            _ => value.clone(),
        };
        hashmap.insert(*ints, (*str, value));
    }
    hashmap
});

#[derive(Clone)]
enum SysInfoType {
    String(&'static [u8]),
    Int32(i32),
    Int64(i64),
    Struct,
}

fn error(env: &mut Environment, errno: i32) -> i32 {
    set_errno(env, errno);
    -1
}

fn sysctl(
    env: &mut Environment,
    name: MutPtr<i32>,
    name_len: u32,
    oldp: MutVoidPtr,
    oldlenp: MutPtr<GuestUSize>,
    newp: MutVoidPtr,
    newlen: GuestUSize,
) -> i32 {
    if name_len == 0 || name_len > 12 {
        return error(env, EINVAL);
    }
    let Some(bytes) = env
        .mem
        .get_bytes_fallible(name.cast_const().cast(), name_len * 4)
    else {
        return error(env, EFAULT);
    };
    let names: Vec<i32> = bytes
        .chunks_exact(4)
        .map(|bytes| i32::from_le_bytes(bytes.try_into().unwrap()))
        .collect();
    let key = match names.as_slice() {
        &[a, b] => SysCtlNamePath::Length2(a, b),
        &[a, b, c, d] => SysCtlNamePath::Length4(a, b, c, d),
        _ => SysCtlNamePath::Undefined,
    };
    sysctl_generic(env, INT_MAP.get(&key).cloned(), oldp, oldlenp, newp, newlen)
}

fn sysctlbyname(
    env: &mut Environment,
    name: ConstPtr<u8>,
    oldp: MutVoidPtr,
    oldlenp: MutPtr<GuestUSize>,
    newp: MutVoidPtr,
    newlen: GuestUSize,
) -> i32 {
    let mut bytes = Vec::new();
    for offset in 0..1024 {
        let Some(address) = name.to_bits().checked_add(offset) else {
            return error(env, EFAULT);
        };
        let Some(byte) = env
            .mem
            .get_bytes_fallible(ConstPtr::<u8>::from_bits(address).cast(), 1)
        else {
            return error(env, EFAULT);
        };
        if byte[0] == 0 {
            break;
        }
        bytes.push(byte[0]);
    }
    if bytes.len() == 1024 {
        return error(env, ENAMETOOLONG);
    }
    let Ok(name) = std::str::from_utf8(&bytes) else {
        return error(env, ENOENT);
    };
    let value = STRING_MAP
        .get_key_value(name)
        .map(|(&name, value)| (name, value.clone()));
    sysctl_generic(env, value, oldp, oldlenp, newp, newlen)
}

fn sysctl_generic(
    env: &mut Environment,
    value: Option<(&'static str, SysInfoType)>,
    oldp: MutVoidPtr,
    oldlenp: MutPtr<GuestUSize>,
    newp: MutVoidPtr,
    newlen: GuestUSize,
) -> i32 {
    let Some((_name, value)) = value else {
        return error(env, ENOENT);
    };
    // Process enumeration is not supplied by this runtime. Never enumerate
    // the host's processes or turn an unavailable query into a host panic.
    if matches!(value, SysInfoType::Struct) {
        return error(env, ENOENT);
    }
    if !newp.is_null() {
        return error(env, EPERM);
    }
    if newlen != 0 {
        return error(env, EINVAL);
    }
    if oldlenp.is_null() {
        return if oldp.is_null() {
            0
        } else {
            error(env, EFAULT)
        };
    }
    if env
        .mem
        .get_bytes_fallible(oldlenp.cast_const().cast(), 4)
        .is_none()
    {
        return error(env, EFAULT);
    }
    let oldlen = env.mem.read(oldlenp);
    let bytes = match value {
        SysInfoType::String(string) => {
            let mut bytes = string.to_vec();
            bytes.push(0);
            bytes
        }
        SysInfoType::Int32(number) => number.to_le_bytes().to_vec(),
        // Darwin's sysctl_io_number accepts a four-byte destination for a
        // quad-sized property when the value fits in a signed int. A size
        // probe still reports the node's full eight-byte representation.
        SysInfoType::Int64(number) if !oldp.is_null() && oldlen == 4 => {
            let Ok(number) = i32::try_from(number) else {
                return error(env, ERANGE);
            };
            number.to_le_bytes().to_vec()
        }
        SysInfoType::Int64(number) => number.to_le_bytes().to_vec(),
        SysInfoType::Struct => unreachable!(),
    };
    let size = bytes.len() as GuestUSize;
    if oldp.is_null() {
        env.mem.write(oldlenp, size);
        return 0;
    }
    if oldlen < size {
        // iPhone OS 3's sysctl_old_user rejects an undersized scalar/string
        // transfer before copying it; oldlen records zero bytes transferred.
        env.mem.write(oldlenp, 0);
        return error(env, ENOMEM);
    }
    let Some(destination) = env.mem.get_bytes_fallible_mut(oldp.cast_const(), size) else {
        return error(env, EFAULT);
    };
    destination.copy_from_slice(&bytes);
    env.mem.write(oldlenp, size);
    0
}

pub const FUNCTIONS: FunctionExports = &[
    export_c_func!(sysctl(_, _, _, _, _, _)),
    export_c_func!(sysctlbyname(_, _, _, _, _)),
];
