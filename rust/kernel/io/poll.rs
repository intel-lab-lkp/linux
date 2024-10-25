// SPDX-License-Identifier: GPL-2.0

//! IO polling.
//!
//! C header: [`include/linux/iopoll.h`](srctree/include/linux/iopoll.h).

use crate::{
    error::{code::*, Result},
    processor::cpu_relax,
    time::{delay::fsleep, Delta, Instant},
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
) -> Result<T>
where
    Op: FnMut() -> Result<T>,
    Cond: Fn(T) -> bool,
{
    let start = Instant::now();
    let sleep = !sleep_delta.is_zero();
    let timeout = !timeout_delta.is_zero();

    let val = loop {
        let val = op()?;
        if cond(val) {
            // Unlike the C version, we immediately return.
            // We know a condition is met so we don't need to check again.
            return Ok(val);
        }
        if timeout && start.elapsed() > timeout_delta {
            // Should we return Err(ETIMEDOUT) here instead of call op() again
            // wihout a sleep between? But we follow the C version. op() could
            // take some time so might be worth checking again.
            break op()?;
        }
        if sleep {
            fsleep(sleep_delta);
        }
        // fsleep() could be busy-wait loop so we always call cpu_relax().
        cpu_relax();
    };

    if cond(val) {
        Ok(val)
    } else {
        Err(ETIMEDOUT)
    }
}

/// Print debug information if it's called inside atomic sections.
#[macro_export]
macro_rules! __might_sleep {
    () => {
        #[cfg(CONFIG_DEBUG_ATOMIC_SLEEP)]
        // SAFETY: FFI call.
        unsafe {
            $crate::bindings::__might_sleep(
                c_str!(::core::file!()).as_char_ptr(),
                ::core::line!() as i32,
            )
        }
    };
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
        if !$sleep_delta.is_zero() {
            $crate::__might_sleep!();
        }

        $crate::io::poll::read_poll_timeout($op, $cond, $sleep_delta, $timeout_delta)
    }};
}
