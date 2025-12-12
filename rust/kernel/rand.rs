// SPDX-License-Identifier: GPL-2.0

//! Randomness.
//!
//! C header: [`include/linux/random.h`](../../../../include/linux/random.h)

use crate::bindings;
use crate::ffi::c_void;

/// Adds the given buffer to the entropy pool.
pub fn add_device_randomness(buf: &[u8]) {
    // SAFETY: We just need the pointer to be valid for the length, which a slice provides.
    unsafe { bindings::add_device_randomness(buf.as_ptr().cast::<c_void>(), buf.len()) };
}
