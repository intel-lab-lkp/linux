// SPDX-License-Identifier: GPL-2.0

//! Randomness.
//!
//! C header: [`include/linux/random.h`](srctree/include/linux/random.h)

use crate::prelude::*;

/// Adds the given buffer to the entropy pool, but does not credit any entropy.
///
/// [`add_device_randomness`] adds data to the input pool that
/// is likely to differ between two devices (or possibly even per boot).
/// This would be things like MAC addresses or serial numbers, or the
/// read-out of the RTC. This does *not* credit any actual entropy to
/// the pool, but it initializes the pool to different values for devices
/// that might otherwise be identical and have very little entropy
/// available to them (particularly common in the embedded world).
pub fn add_device_randomness(buf: &[u8]) {
    // SAFETY: We just need the pointer to be valid for the length, which a slice provides.
    unsafe { bindings::add_device_randomness(buf.as_ptr().cast::<c_void>(), buf.len()) };
}
