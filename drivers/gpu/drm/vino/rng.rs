// SPDX-License-Identifier: GPL-2.0

//! Cryptographically-secure randomness for the per-session HDCP nonces/keys
//! (`rtx`, `km`, `rn`, `ks`, `riv`, the OAEP seed).
#![allow(dead_code)] // RNG helpers; some are reached only on the post-engagement CP path

/// Fills `buf` with random bytes from the kernel CSPRNG (`get_random_bytes`).
pub(super) fn fill(buf: &mut [u8]) {
    // SAFETY: `buf` is valid for writes of `buf.len()` bytes; `get_random_bytes`
    // writes exactly that many and never sleeps/faults on a kernel buffer.
    unsafe { kernel::bindings::get_random_bytes(buf.as_mut_ptr().cast(), buf.len()) };
}
