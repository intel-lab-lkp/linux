// SPDX-License-Identifier: GPL-2.0

//! Delay and sleep primitives.
//!
//! This module contains the kernel APIs related to delay and sleep that
//! have been ported or wrapped for usage by Rust code in the kernel.
//!
//! C header: [`include/linux/delay.h`](srctree/include/linux/delay.h).

use crate::time;
use core::ffi::c_ulong;

/// Sleeps for a given duration at least.
///
/// Equivalent to the kernel's [`fsleep`], flexible sleep function,
/// which automatically chooses the best sleep method based on a duration.
///
/// `Delta` must be longer than one microsecond.
///
/// This function can only be used in a nonatomic context.
pub fn fsleep(delta: time::Delta) {
    // SAFETY: FFI call.
    unsafe {
        // Convert the duration to microseconds and round up to preserve
        // the guarantee; fsleep sleeps for at least the provided duration,
        // but that it may sleep for longer under some circumstances.
        bindings::fsleep(
            ((delta.as_nanos() + time::NSEC_PER_USEC - 1) / time::NSEC_PER_USEC) as c_ulong,
        )
    }
}
