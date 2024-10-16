// SPDX-License-Identifier: GPL-2.0

//! IO polling.
//!
//! C header: [`include/linux/iopoll.h`](srctree/include/linux/iopoll.h).

use crate::{
    bindings,
    error::{code::*, Result},
    time::{delay::fsleep, Delta, Ktime},
};

/// Polls periodically until a condition is met or a timeout is reached.
///
/// Public but hidden since it should only be used from public macros.
#[doc(hidden)]
pub fn read_poll_timeout<Op, Cond, T: Copy>(
    mut op: Op,
    cond: Cond,
    sleep_delta: Delta,
    timeout_delta: Delta,
    sleep_before_read: bool,
) -> Result<T>
where
    Op: FnMut() -> Result<T>,
    Cond: Fn(T) -> bool,
{
    let timeout = Ktime::ktime_get() + timeout_delta;
    let sleep = !sleep_delta.is_zero();

    if sleep_before_read && sleep {
        fsleep(sleep_delta);
    }

    let val = loop {
        let val = op()?;
        if cond(val) {
            break val;
        }
        if !timeout_delta.is_zero() && Ktime::ktime_get() > timeout {
            break op()?;
        }
        if sleep {
            fsleep(sleep_delta);
        }
        // SAFETY: FFI call.
        unsafe { bindings::cpu_relax() }
    };

    if cond(val) {
        Ok(val)
    } else {
        Err(ETIMEDOUT)
    }
}

/// Polls periodically until a condition is met or a timeout is reached.
///
/// `op` is called repeatedly until `cond` returns `true` or the timeout is
///  reached. The return value of `op` is passed to `cond`.
///
/// `sleep_delta` is the duration to sleep between calls to `op`.
/// If `sleep_delta` is less than one microsecond, the function will busy-wait.
///
/// `timeout_delta` is the maximum time to wait for `cond` to return `true`.
///
/// This macro can only be used in a nonatomic context.
#[macro_export]
macro_rules! readx_poll_timeout {
    ($op:expr, $cond:expr, $sleep_delta:expr, $timeout_delta:expr) => {{
        #[cfg(CONFIG_DEBUG_ATOMIC_SLEEP)]
        if !$sleep_delta.is_zero() {
            // SAFETY: FFI call.
            unsafe {
                $crate::bindings::__might_sleep(
                    ::core::file!().as_ptr() as *const i8,
                    ::core::line!() as i32,
                )
            }
        }

        $crate::io::poll::read_poll_timeout($op, $cond, $sleep_delta, $timeout_delta, false)
    }};
}
