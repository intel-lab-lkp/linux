// SPDX-License-Identifier: GPL-2.0

//! IO polling.
//!
//! C header: [`include/linux/iopoll.h`](srctree/include/linux/iopoll.h).

use crate::{
    cpu::cpu_relax,
    error::{code::*, Result},
    time::{delay::fsleep, Delta, Instant},
};

/// Polls periodically until a condition is met or a timeout is reached.
///
/// The function repeatedly executes the given operation `op` closure and
/// checks its result using the condition closure `cond`.
/// If `cond` returns `true`, the function returns successfully with the result of `op`.
/// Otherwise, it waits for a duration specified by `sleep_delta`
/// before executing `op` again.
/// This process continues until either `cond` returns `true` or the timeout,
/// specified by `timeout_delta`, is reached. If `timeout_delta` is `None`,
/// polling continues indefinitely until `cond` evaluates to `true` or an error occurs.
///
/// # Examples
///
/// ```rust,ignore
/// fn wait_for_hardware(dev: &mut Device) -> Result<()> {
///     // The `op` closure reads the value of a specific status register.
///     let op = || -> Result<u16> { dev.read_ready_register() };
///
///     // The `cond` closure takes a reference to the value returned by `op`
///     // and checks whether the hardware is ready.
///     let cond = |val: &u16| *val == HW_READY;
///
///     match read_poll_timeout(op, cond, Delta::from_millis(50), Some(Delta::from_secs(3))) {
///         Ok(_) => {
///             // The hardware is ready. The returned value of the `op`` closure isn't used.
///             Ok(())
///         }
///         Err(e) => Err(e),
///     }
/// }
/// ```
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
        might_sleep();
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

/// Annotation for functions that can sleep.
///
/// Equivalent to the C side [`might_sleep()`], this function serves as
/// a debugging aid and a potential scheduling point.
///
/// This function can only be used in a nonatomic context.
#[track_caller]
fn might_sleep() {
    #[cfg(CONFIG_DEBUG_ATOMIC_SLEEP)]
    {
        let loc = core::panic::Location::caller();
        // SAFETY: FFI call.
        unsafe {
            crate::bindings::__might_sleep_precision(
                loc.file().as_ptr().cast(),
                loc.file().len() as i32,
                loc.line() as i32,
            )
        }
    }

    // SAFETY: FFI call.
    unsafe { crate::bindings::might_resched() }
}
