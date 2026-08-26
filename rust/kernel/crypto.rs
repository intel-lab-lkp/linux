// SPDX-License-Identifier: GPL-2.0

//! Safe wrappers over the kernel's synchronous library crypto.
//!
//! Exposes the one-shot `lib/crypto` primitives — AES-128 (an [`Aes128`] key
//! prepared once for single-block encryption, the building block for modes the
//! library does not yet provide such as AES-CTR), the in-tree AES-CMAC
//! ([`aes_cmac`]), SHA-256 and HMAC-SHA256 — for use from Rust. They run
//! synchronously in the calling context with no allocation; the hashes and the
//! MAC are infallible.
//!
//! Public-key ciphers are available through [`akcipher`] when
//! `CONFIG_RUST_CRYPTO_AKCIPHER` is enabled.
//!
//! C headers: [`include/crypto/akcipher.h`](srctree/include/crypto/akcipher.h),
//! [`include/crypto/aes.h`](srctree/include/crypto/aes.h),
//! [`include/crypto/aes-cbc-macs.h`](srctree/include/crypto/aes-cbc-macs.h),
//! [`include/crypto/sha2.h`](srctree/include/crypto/sha2.h).

use core::ops::{Deref, DerefMut};

use crate::bindings;
#[cfg(any(
    CONFIG_RUST_CRYPTO_AKCIPHER,
    CONFIG_RUST_CRYPTO_LIB_AES,
    CONFIG_RUST_CRYPTO_LIB_SHA256
))]
use crate::{error::to_result, prelude::*};

#[cfg(CONFIG_RUST_CRYPTO_AKCIPHER)]
pub mod akcipher;

/// Size of a SHA-256 / HMAC-SHA256 digest, in bytes.
pub const SHA256_DIGEST_SIZE: usize = 32;
/// AES-128 block and key size, in bytes.
pub const AES128_BLOCK_SIZE: usize = 16;

/// A fixed-size byte string which is wiped when dropped.
///
/// This is intended for cryptographic keys and other sensitive intermediate
/// values. Borrowing the contained bytes can still create copies which this
/// type cannot track; callers should avoid copying them unnecessarily.
pub struct Secret<const N: usize>([u8; N]);

impl<const N: usize> Secret<N> {
    /// Wraps bytes which should be wiped when their owner is dropped.
    pub const fn new(bytes: [u8; N]) -> Self {
        Self(bytes)
    }

    /// Creates an all-zero byte string.
    pub const fn zeroed() -> Self {
        Self([0; N])
    }
}

impl<const N: usize> Deref for Secret<N> {
    type Target = [u8; N];

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

impl<const N: usize> DerefMut for Secret<N> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.0
    }
}

impl<const N: usize> Drop for Secret<N> {
    fn drop(&mut self) {
        // SAFETY: `self.0` is valid for exactly `N` writable bytes. The helper
        // uses `memzero_explicit()`, so the wipe is not optimised away.
        unsafe { bindings::memzero_explicit(self.0.as_mut_ptr().cast(), N) };
    }
}

/// Returns the SHA-256 digest of `data`.
#[cfg(CONFIG_RUST_CRYPTO_LIB_SHA256)]
pub fn sha256(data: &[u8]) -> [u8; SHA256_DIGEST_SIZE] {
    let mut out = [0u8; SHA256_DIGEST_SIZE];
    // SAFETY: `data` is valid for `data.len()` reads and `out` is a valid
    // `SHA256_DIGEST_SIZE`-byte output buffer, as `sha256()` requires.
    unsafe { bindings::sha256(data.as_ptr(), data.len(), out.as_mut_ptr()) };
    out
}

/// Returns `HMAC-SHA256(key, data)`.
#[cfg(CONFIG_RUST_CRYPTO_LIB_SHA256)]
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

/// Returns `AES-CMAC-128(key, data)` (RFC 4493), computed by the in-tree
/// AES-CMAC library ([`include/crypto/aes-cbc-macs.h`]). The 128-bit key is
/// prepared and wiped internally; the call is infallible.
///
/// [`include/crypto/aes-cbc-macs.h`]: srctree/include/crypto/aes-cbc-macs.h
#[cfg(CONFIG_RUST_CRYPTO_LIB_AES)]
pub fn aes_cmac(key: &[u8; AES128_BLOCK_SIZE], data: &[u8]) -> [u8; AES128_BLOCK_SIZE] {
    let mut out = [0u8; AES128_BLOCK_SIZE];
    // SAFETY: `key` is a valid 16-byte key, `data` is valid for `data.len()`
    // reads, and `out` is a valid `AES128_BLOCK_SIZE`-byte output buffer, as the
    // helper requires.
    unsafe { bindings::aes_cmac(key.as_ptr(), data.as_ptr(), data.len(), out.as_mut_ptr()) };
    out
}

/// An AES-128 key, expanded once for single-block encryption.
///
/// The key schedule is computed in [`Aes128::new`] and reused across every
/// [`encrypt_block`](Aes128::encrypt_block) call, so encrypting a stream of
/// blocks (e.g. an AES-CTR keystream) does not re-expand the key per block. This
/// is a low-level building block: prefer a full mode of operation where the
/// library provides one (see [`aes_cmac`]); the bare block cipher is here only
/// for modes `lib/crypto` does not yet expose, such as AES-CTR.
///
/// # Examples
///
/// ```
/// use kernel::crypto::Aes128;
/// let cipher = Aes128::new(&[0u8; 16])?;
/// let _ct = cipher.encrypt_block(&[0u8; 16]);
/// # Ok::<(), Error>(())
/// ```
#[cfg(CONFIG_RUST_CRYPTO_LIB_AES)]
pub struct Aes128(bindings::aes_enckey);

#[cfg(CONFIG_RUST_CRYPTO_LIB_AES)]
impl Aes128 {
    /// Expands an AES-128 key from 16 raw key bytes.
    pub fn new(key: &[u8; AES128_BLOCK_SIZE]) -> Result<Self> {
        // SAFETY: `aes_enckey` is a plain-old-data key schedule (integer arrays
        // in a union of integer arrays); an all-zero bit pattern is a valid,
        // inert initial value, fully overwritten by `aes_prepareenckey()` below.
        let mut enckey: bindings::aes_enckey = unsafe { core::mem::zeroed() };
        // SAFETY: `enckey` is a valid, owned `aes_enckey`; `key` is a valid
        // 16-byte buffer; `AES128_BLOCK_SIZE` (16) is a supported key length.
        let ret =
            unsafe { bindings::aes_prepareenckey(&mut enckey, key.as_ptr(), AES128_BLOCK_SIZE) };
        to_result(ret)?;
        Ok(Self(enckey))
    }

    /// Encrypts one 16-byte block with the prepared key: returns
    /// `AES-128-ECB(key, block)`.
    pub fn encrypt_block(&self, block: &[u8; AES128_BLOCK_SIZE]) -> [u8; AES128_BLOCK_SIZE] {
        let mut out = [0u8; AES128_BLOCK_SIZE];
        // SAFETY: `self.0` is a prepared encryption key; `block` and `out` are
        // valid 16-byte buffers, as the helper requires.
        unsafe { bindings::aes_enckey_encrypt_block(&self.0, out.as_mut_ptr(), block.as_ptr()) };
        out
    }
}

#[cfg(CONFIG_RUST_CRYPTO_LIB_AES)]
impl Drop for Aes128 {
    fn drop(&mut self) {
        // SAFETY: `self.0` is a valid, owned `aes_enckey`.
        unsafe { bindings::aes_enckey_zero(&mut self.0) };
    }
}
