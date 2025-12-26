// SPDX-License-Identifier: GPL-2.0

//! Randomness.
//!
//! C header: [`include/linux/random.h`](srctree/include/linux/random.h)

use crate::prelude::*;

/// Adds the given buffer to the entropy pool, but does not credit any entropy.
///
/// This is intended for use mixing in data that is likely to differ between devices or boots, but
/// may otherwise be predictable. Examples include MAC addresses or RTC values. This slightly
/// improves randomness in entropy-constrained environments (especially common for embedded
/// devices).
pub fn add_device_randomness(buf: &[u8]) {
    // SAFETY: We just need the pointer to be valid for the length, which a slice provides.
    unsafe { bindings::add_device_randomness(buf.as_ptr().cast::<c_void>(), buf.len()) };
}
