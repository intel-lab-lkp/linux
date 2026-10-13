// SPDX-License-Identifier: GPL-2.0

//! CRC-CCITT computation.
//!
//! C header: [`include/linux/crc-ccitt.h`](srctree/include/linux/crc-ccitt.h)

/// Computes the CRC-CCITT of `data`, starting from the seed value `crc`.
///
/// Pass the result back in as `crc` to compute the checksum of a buffer
/// incrementally.
///
/// # Examples
///
/// ```
/// use kernel::crc_ccitt::crc_ccitt;
///
/// let one_shot = crc_ccitt(0xffff, b"hello world");
/// let split = crc_ccitt(crc_ccitt(0xffff, b"hello "), b"world");
/// assert_eq!(one_shot, split);
/// ```
#[inline]
pub fn crc_ccitt(crc: u16, data: &[u8]) -> u16 {
    // SAFETY: `data.as_ptr()` is valid for reads of `data.len()` bytes for the
    // duration of the call, since it is derived from a live shared slice.
    unsafe { bindings::crc_ccitt(crc, data.as_ptr(), data.len()) }
}
