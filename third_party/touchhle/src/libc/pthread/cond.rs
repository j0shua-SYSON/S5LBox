/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
//! Conditional variables.

use super::mutex::pthread_mutex_t;
use crate::dyld::FunctionExports;
use crate::libc::errno::{EINVAL, ETIMEDOUT};
use crate::libc::pthread::mutex::pthread_mutex_unlock;
use crate::mem::{ConstPtr, MutPtr, Ptr, SafeRead};
use crate::{export_c_func, Environment};
use std::collections::{HashMap, HashSet, VecDeque};
use std::time::{Duration, Instant, SystemTime};

use crate::environment::{MutexId, ThreadBlock, ThreadId};
use crate::libc::time::timespec;

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum CondDeadline {
    Absolute(SystemTime),
    Relative(Instant),
}

impl CondDeadline {
    pub fn remaining(self) -> Duration {
        match self {
            Self::Absolute(time) => time.duration_since(SystemTime::now()).unwrap_or_default(),
            Self::Relative(time) => time.saturating_duration_since(Instant::now()),
        }
    }
}

fn read_timeout(env: &Environment, time: ConstPtr<timespec>) -> Result<Duration, i32> {
    if time.is_null() {
        return Err(EINVAL);
    }
    let time = env.mem.read(time);
    let seconds = time.tv_sec;
    let nanos = time.tv_nsec;
    if seconds < 0 || !(0..1_000_000_000).contains(&nanos) {
        return Err(EINVAL);
    }
    Ok(Duration::new(seconds as u64, nanos as u32))
}

#[repr(C, packed)]
pub struct pthread_condattr_t {}
unsafe impl SafeRead for pthread_condattr_t {}

/// Arbitrarily-chosen magic number for `pthread_cond_t` (not Apple's).
const MAGIC_COND: u32 = u32::from_be_bytes(*b"COND");
/// Magic number used by `PTHREAD_COND_INITIALIZER`. This is part of the ABI!
const MAGIC_COND_STATIC: u32 = 0x3CB0B1BB;

/// Apple's implementation is a 4-byte magic number followed by an 24-byte
/// opaque region. We only have to match the size theirs has.
#[repr(C, packed)]
pub struct pthread_cond_t {
    /// Magic number (must be [MAGIC_COND])
    magic: u32,
    _unused: [u32; 6],
}
unsafe impl SafeRead for pthread_cond_t {}

#[derive(Default)]
pub struct State {
    pub condition_variables: HashMap<MutPtr<pthread_cond_t>, CondHostObject>,
}
impl State {
    fn get(env: &Environment) -> &Self {
        &env.libc_state.pthread.cond
    }
    fn get_mut(env: &mut Environment) -> &mut Self {
        &mut env.libc_state.pthread.cond
    }
}

pub struct CondHostObject {
    pub(crate) waiting: VecDeque<ThreadId>,
    pub(crate) waking: VecDeque<ThreadId>,
    pub(crate) curr_mutex: Option<MutexId>,
    pub(crate) timed_out: HashSet<ThreadId>,
}

pub fn pthread_cond_init(
    env: &mut Environment,
    cond: MutPtr<pthread_cond_t>,
    attr: ConstPtr<pthread_condattr_t>,
) -> i32 {
    assert!(attr.is_null());
    let opaque = pthread_cond_t {
        magic: MAGIC_COND,
        _unused: [0; 6],
    };
    env.mem.write(cond, opaque);

    assert!(!State::get(env).condition_variables.contains_key(&cond));
    State::get_mut(env).condition_variables.insert(
        cond,
        CondHostObject {
            waiting: VecDeque::new(),
            waking: VecDeque::new(),
            curr_mutex: None,
            timed_out: Default::default(),
        },
    );
    0 // success
}

fn check_or_register_cond(env: &mut Environment, cond: MutPtr<pthread_cond_t>) -> Result<(), i32> {
    let magic: u32 = env.mem.read(cond.cast());
    // This is a statically-initialized cond, we need to register it, and
    // change the magic number in the process.
    if magic == MAGIC_COND_STATIC {
        log_dbg!(
            "Detected statically-initialized cond at {:?}, registering.",
            cond
        );
        pthread_cond_init(env, cond, Ptr::null());
        Ok(())
    } else if magic == MAGIC_COND {
        Ok(())
    } else {
        Err(EINVAL)
    }
}

fn pthread_cond_timedwait(
    env: &mut Environment,
    cond: MutPtr<pthread_cond_t>,
    mutex: MutPtr<pthread_mutex_t>,
    abs_time: ConstPtr<timespec>,
) -> i32 {
    let deadline = match read_timeout(env, abs_time)
        .ok()
        .and_then(|time| SystemTime::UNIX_EPOCH.checked_add(time))
    {
        Some(time) => CondDeadline::Absolute(time),
        None => return EINVAL,
    };
    wait(env, cond, mutex, Some(deadline))
}

fn pthread_cond_timedwait_relative_np(
    env: &mut Environment,
    cond: MutPtr<pthread_cond_t>,
    mutex: MutPtr<pthread_mutex_t>,
    relative_time: ConstPtr<timespec>,
) -> i32 {
    let deadline = match read_timeout(env, relative_time)
        .ok()
        .and_then(|time| Instant::now().checked_add(time))
    {
        Some(time) => CondDeadline::Relative(time),
        None => return EINVAL,
    };
    wait(env, cond, mutex, Some(deadline))
}

fn wait(
    env: &mut Environment,
    cond: MutPtr<pthread_cond_t>,
    mutex: MutPtr<pthread_mutex_t>,
    deadline: Option<CondDeadline>,
) -> i32 {
    match check_or_register_cond(env, cond) {
        Ok(_) => {}
        Err(e) => {
            return e;
        }
    };
    let res = pthread_mutex_unlock(env, mutex);
    if res != 0 {
        return res;
    }
    log_dbg!(
        "Thread {} is blocking on condition variable {:?} with deadline {:?}",
        env.current_thread,
        cond,
        deadline
    );

    let current_thread = env.current_thread;
    let mutex = env.mem.read(mutex).mutex_id;
    let host_object = State::get_mut(env)
        .condition_variables
        .get_mut(&cond)
        .unwrap();
    // The mutex used must be the same as the currently waiting mutex, or there
    // must be no other waiters.
    assert!(
        host_object.curr_mutex == Some(mutex)
            || host_object.waking.is_empty() && host_object.waiting.is_empty()
    );
    host_object.curr_mutex = Some(mutex);
    host_object.waiting.push_back(current_thread);

    env.yield_thread(ThreadBlock::Condition(cond, deadline));

    let host_object = State::get_mut(env)
        .condition_variables
        .get_mut(&cond)
        .unwrap();
    if host_object.timed_out.remove(&current_thread) {
        ETIMEDOUT
    } else {
        0 // success
    }
}

pub fn pthread_cond_wait(
    env: &mut Environment,
    cond: MutPtr<pthread_cond_t>,
    mutex: MutPtr<pthread_mutex_t>,
) -> i32 {
    wait(env, cond, mutex, None)
}

pub fn pthread_cond_signal(env: &mut Environment, cond: MutPtr<pthread_cond_t>) -> i32 {
    match check_or_register_cond(env, cond) {
        Ok(_) => {}
        Err(e) => {
            return e;
        }
    };
    let host_object = State::get_mut(env)
        .condition_variables
        .get_mut(&cond)
        .unwrap();
    if let Some(tid) = host_object.waiting.pop_front() {
        host_object.waking.push_back(tid);
        log_dbg!(
            "Thread {} unblocks one thread ({}) waiting on condition variable {:?}",
            env.current_thread,
            tid,
            cond
        );
    } else {
        log_dbg!(
            "Thread {} signals condition variable {:?}, no waiters",
            env.current_thread,
            cond
        );
    }
    0 // success
}

pub fn pthread_cond_broadcast(env: &mut Environment, cond: MutPtr<pthread_cond_t>) -> i32 {
    match check_or_register_cond(env, cond) {
        Ok(_) => {}
        Err(e) => {
            return e;
        }
    };
    log_dbg!(
        "Thread {} unblocks all threads waiting on condition variable {:?}",
        env.current_thread,
        cond
    );
    let host_object = State::get_mut(env)
        .condition_variables
        .get_mut(&cond)
        .unwrap();
    host_object.waking.extend(host_object.waiting.drain(..));
    0 // success
}

pub fn pthread_cond_destroy(env: &mut Environment, cond: MutPtr<pthread_cond_t>) -> i32 {
    match check_or_register_cond(env, cond) {
        Ok(_) => {}
        Err(e) => {
            return e;
        }
    };
    let old_object = State::get_mut(env)
        .condition_variables
        .remove(&cond)
        .unwrap();
    assert!(old_object.waiting.is_empty() && old_object.waking.is_empty());
    0 // success
}

pub const FUNCTIONS: FunctionExports = &[
    export_c_func!(pthread_cond_init(_, _)),
    export_c_func!(pthread_cond_wait(_, _)),
    export_c_func!(pthread_cond_timedwait(_, _, _)),
    export_c_func!(pthread_cond_timedwait_relative_np(_, _, _)),
    export_c_func!(pthread_cond_signal(_)),
    export_c_func!(pthread_cond_broadcast(_)),
    export_c_func!(pthread_cond_destroy(_)),
];
