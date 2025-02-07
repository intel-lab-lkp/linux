// SPDX-License-Identifier: GPL-2.0

//! IO polling.
//!
//! C header: [`include/linux/iopoll.h`](srctree/include/linux/iopoll.h).

use crate::{
    cpu::cpu_relax,
    error::{code::*, Result},
    time::{delay::fsleep, Delta, Instant},
};

use core::panic::Location;

/// Polls periodically until a condition is met or a timeout is reached.
///
/// ```rust
/// use kernel::io::poll::read_poll_timeout;
/// use kernel::time::Delta;
/// use kernel::sync::{SpinLock, new_spinlock};
///
/// let lock = KBox::pin_init(new_spinlock!(()), kernel::alloc::flags::GFP_KERNEL)?;
/// let g = lock.lock();
/// read_poll_timeout(|| Ok(()), |()| true, Delta::from_micros(42), Some(Delta::from_micros(42)));
/// drop(g);
///
/// # Ok::<(), Error>(())
/// ```
#[track_caller]
pub fn read_poll_timeout<Op, Cond, T>(
    mut op: Op,
    mut cond: Cond,
    sleep_delta: Delta,
    timeout_delta: Option<Delta>,
) -> Result<T>
where
    Op: FnMut() -> Result<T>,
    Cond: FnMut(&T) -> bool,
{
    let start = Instant::now();
    let sleep = !sleep_delta.is_zero();

    if sleep {
        might_sleep(Location::caller());
    }

    loop {
        let val = op()?;
        if cond(&val) {
            // Unlike the C version, we immediately return.
            // We know the condition is met so we don't need to check again.
            return Ok(val);
        }
        if let Some(timeout_delta) = timeout_delta {
            if start.elapsed() > timeout_delta {
                // Unlike the C version, we immediately return.
                // We have just called `op()` so we don't need to call it again.
                return Err(ETIMEDOUT);
            }
        }
        if sleep {
            fsleep(sleep_delta);
        }
        // fsleep() could be busy-wait loop so we always call cpu_relax().
        cpu_relax();
    }
}

fn might_sleep(loc: &Location<'_>) {
    // SAFETY: FFI call.
    unsafe {
        crate::bindings::__might_sleep_precision(
            loc.file().as_ptr().cast(),
            loc.file().len() as i32,
            loc.line() as i32,
        )
    }
}
