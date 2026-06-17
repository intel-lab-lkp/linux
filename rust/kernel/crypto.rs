// SPDX-License-Identifier: GPL-2.0

//! Safe wrappers over the kernel's synchronous library crypto.
//!
//! Exposes the one-shot `lib/crypto` primitives — AES-128 single-block ECB,
//! SHA-256 and HMAC-SHA256 — for use from Rust. They run synchronously in the
//! calling context with no allocation; the hashes are infallible.
//!
//! C headers: [`include/crypto/aes.h`](srctree/include/crypto/aes.h),
//! [`include/crypto/sha2.h`](srctree/include/crypto/sha2.h).

use crate::{bindings, error::to_result, prelude::*};

/// Size of a SHA-256 / HMAC-SHA256 digest, in bytes.
pub const SHA256_DIGEST_SIZE: usize = 32;
/// AES-128 block and key size, in bytes.
pub const AES128_BLOCK_SIZE: usize = 16;

/// Returns the SHA-256 digest of `data`.
pub fn sha256(data: &[u8]) -> [u8; SHA256_DIGEST_SIZE] {
    let mut out = [0u8; SHA256_DIGEST_SIZE];
    // SAFETY: `data` is valid for `data.len()` reads and `out` is a valid
    // `SHA256_DIGEST_SIZE`-byte output buffer, as `sha256()` requires.
    unsafe { bindings::sha256(data.as_ptr(), data.len(), out.as_mut_ptr()) };
    out
}

/// Returns `HMAC-SHA256(key, data)`.
pub fn hmac_sha256(key: &[u8], data: &[u8]) -> [u8; SHA256_DIGEST_SIZE] {
    let mut out = [0u8; SHA256_DIGEST_SIZE];
    // SAFETY: `key` and `data` are valid for their respective lengths and `out`
    // is a valid `SHA256_DIGEST_SIZE`-byte output buffer, as required.
    unsafe {
        bindings::hmac_sha256_usingrawkey(
            key.as_ptr(),
            key.len(),
            data.as_ptr(),
            data.len(),
            out.as_mut_ptr(),
        )
    };
    out
}

/// An AES-128 key usable for single-block ECB encryption.
///
/// # Examples
///
/// ```
/// use kernel::crypto::Aes128;
/// let cipher = Aes128::new([0u8; 16]);
/// let _ct = cipher.encrypt_block(&[0u8; 16])?;
/// # Ok::<(), Error>(())
/// ```
pub struct Aes128([u8; AES128_BLOCK_SIZE]);

impl Aes128 {
    /// Creates an AES-128 key from 16 raw key bytes.
    pub fn new(key: [u8; AES128_BLOCK_SIZE]) -> Self {
        Self(key)
    }

    /// Encrypts one 16-byte block: returns `AES-128-ECB(key, block)`.
    pub fn encrypt_block(
        &self,
        block: &[u8; AES128_BLOCK_SIZE],
    ) -> Result<[u8; AES128_BLOCK_SIZE]> {
        let mut out = [0u8; AES128_BLOCK_SIZE];
        // SAFETY: `self.0`, `block` and `out` are all valid 16-byte buffers, as
        // the helper requires.
        let ret = unsafe {
            bindings::aes128_encrypt_block(self.0.as_ptr(), block.as_ptr(), out.as_mut_ptr())
        };
        to_result(ret)?;
        Ok(out)
    }
}
