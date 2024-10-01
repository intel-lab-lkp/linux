// SPDX-License-Identifier: GPL-2.0

//! Delay and sleep routines.
//!
//! C headers: [`include/linux/delay.h`](srctree/include/linux/delay.h).

use core::{ffi::c_ulong, time::Duration};

/// Sleeps for a given duration.
///
/// Equivalent to the kernel's [`fsleep`] function, internally calls `udelay`,
/// `usleep_range`, or `msleep`.
///
/// This function can only be used in a nonatomic context.
pub fn sleep(duration: Duration) {
    // SAFETY: FFI call.
    unsafe { bindings::fsleep(duration.as_micros() as c_ulong) }
}
