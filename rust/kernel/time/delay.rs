// SPDX-License-Identifier: GPL-2.0

//! Delay and sleep primitives.
//!
//! This module contains the kernel APIs related to delay and sleep that
//! have been ported or wrapped for usage by Rust code in the kernel.
//!
//! C header: [`include/linux/delay.h`](srctree/include/linux/delay.h).

use super::Delta;
use core::ffi::c_ulong;

/// Sleeps for a given duration at least.
///
/// Equivalent to the kernel's [`fsleep`], flexible sleep function,
/// which automatically chooses the best sleep method based on a duration.
///
/// `delta` must be 0 or greater and no more than `u32::MAX / 2` microseconds.
/// If a value outside the range is given, the function will sleep
/// for `u32::MAX / 2` microseconds (= ~2147 seconds or ~36 minutes) at least.
///
/// This function can only be used in a nonatomic context.
pub fn fsleep(delta: Delta) {
    // The argument of fsleep is an unsigned long, 32-bit on 32-bit architectures.
    // Considering that fsleep rounds up the duration to the nearest millisecond,
    // set the maximum value to u32::MAX / 2 microseconds.
    const MAX_DURATION: Delta = Delta::from_micros(u32::MAX as i64 >> 1);

    let duration = if delta > MAX_DURATION || delta.is_negative() {
        // TODO: add WARN_ONCE() when it's supported.
        MAX_DURATION
    } else {
        delta
    };

    // SAFETY: FFI call.
    unsafe {
        // Convert the duration to microseconds and round up to preserve
        // the guarantee; fsleep sleeps for at least the provided duration,
        // but that it may sleep for longer under some circumstances.
        bindings::fsleep(duration.as_micros_ceil() as c_ulong)
    }
}
