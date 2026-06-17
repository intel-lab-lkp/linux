// SPDX-License-Identifier: GPL-2.0

//! Safe wrappers over the kernel's synchronous library crypto.
//!
//! Exposes the one-shot `lib/crypto` primitives — AES-128 single-block ECB,
//! SHA-256 and HMAC-SHA256 — for use from Rust. They run synchronously in the
//! calling context with no allocation; the hashes are infallible.
//!
//! Also exposes the asynchronous public-key API ([`Akcipher`]) over
//! `crypto_akcipher`, driven synchronously, and a convenience RSA public-key
//! primitive built on it for callers that do their own padding (see
//! [`rsa_pubkey_encrypt`]).
//!
//! C headers: [`include/crypto/aes.h`](srctree/include/crypto/aes.h),
//! [`include/crypto/akcipher.h`](srctree/include/crypto/akcipher.h),
//! [`include/crypto/sha2.h`](srctree/include/crypto/sha2.h).

use crate::{
    bindings,
    error::{from_err_ptr, to_result},
    prelude::*,
};
use core::ptr::NonNull;

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

/// An asynchronous public-key cipher transform (`struct crypto_akcipher`),
/// driven synchronously.
///
/// Wraps a tfm allocated with `crypto_alloc_akcipher()`; the underlying
/// algorithm (e.g. `"rsa"`) is selected by name in [`Akcipher::new`]. After a
/// key is installed with [`set_pub_key`](Akcipher::set_pub_key), [`encrypt`]
/// performs one public-key operation, blocking until the request completes.
///
/// [`encrypt`]: Akcipher::encrypt
///
/// # Examples
///
/// ```
/// use kernel::crypto::Akcipher;
/// // `der` is a DER-encoded RSAPublicKey; `pt` is already padded to key size.
/// # fn f(der: &[u8], pt: &[u8]) -> Result {
/// let mut tfm = Akcipher::new(c"rsa")?;
/// tfm.set_pub_key(der)?;
/// let mut ct = [0u8; 256];
/// let n = tfm.encrypt(pt, &mut ct)?;
/// # let _ = n; Ok(())
/// # }
/// ```
pub struct Akcipher(NonNull<bindings::crypto_akcipher>);

// SAFETY: a tfm is a self-contained kernel object with no thread affinity; the
// synchronous request path takes its own per-call state, so the handle may be
// moved and used from any thread.
unsafe impl Send for Akcipher {}

impl Akcipher {
    /// Allocates a transform for the named akcipher algorithm (e.g. `c"rsa"`).
    pub fn new(alg_name: &core::ffi::CStr) -> Result<Self> {
        // SAFETY: `alg_name` is a valid NUL-terminated C string; the call
        // returns a valid tfm pointer or an `ERR_PTR`.
        let tfm = from_err_ptr(unsafe {
            bindings::crypto_alloc_akcipher(alg_name.as_ptr().cast(), 0, 0)
        })?;
        Ok(Self(NonNull::new(tfm).ok_or(ENOMEM)?))
    }

    /// Installs the public key, encoded in the algorithm's expected wire format
    /// (for `"rsa"`, a DER-encoded `RSAPublicKey`).
    pub fn set_pub_key(&mut self, key: &[u8]) -> Result {
        // SAFETY: `self.0` is a live tfm; `key` is valid for `key.len()` reads.
        to_result(unsafe {
            bindings::crypto_akcipher_set_pub_key(
                self.0.as_ptr(),
                key.as_ptr().cast(),
                key.len() as u32,
            )
        })
    }

    /// Encrypts `src` into `dst`, returning the ciphertext length. Blocks until
    /// the operation completes. `dst` must be at least the algorithm's maximum
    /// output size (the key/modulus size for RSA).
    pub fn encrypt(&self, src: &[u8], dst: &mut [u8]) -> Result<usize> {
        // SAFETY: `self.0` is a live tfm; `src`/`dst` are valid for their
        // lengths. The helper bounces them through kmalloc'd buffers, runs one
        // synchronous akcipher encrypt, and returns the length or a negative
        // errno.
        let ret = unsafe {
            bindings::akcipher_encrypt_oneshot(
                self.0.as_ptr(),
                src.as_ptr(),
                src.len() as u32,
                dst.as_mut_ptr(),
                dst.len() as u32,
            )
        };
        to_result(ret)?;
        Ok(ret as usize)
    }
}

impl Drop for Akcipher {
    fn drop(&mut self) {
        // SAFETY: `self.0` was allocated by `crypto_alloc_akcipher()` and is
        // freed exactly once here.
        unsafe { bindings::crypto_free_akcipher(self.0.as_ptr()) };
    }
}

/// Appends a DER definite length to `out`.
fn der_len(out: &mut KVec<u8>, len: usize) -> Result {
    if len < 0x80 {
        out.push(len as u8, GFP_KERNEL)?;
        return Ok(());
    }
    let mut bytes = [0u8; core::mem::size_of::<usize>()];
    let mut n = 0;
    let mut rest = len;
    while rest > 0 {
        bytes[n] = rest as u8;
        rest >>= 8;
        n += 1;
    }
    out.push(0x80 | n as u8, GFP_KERNEL)?;
    for i in (0..n).rev() {
        out.push(bytes[i], GFP_KERNEL)?;
    }
    Ok(())
}

/// Appends a DER `INTEGER` carrying the unsigned big-endian magnitude `bytes`.
fn der_integer(out: &mut KVec<u8>, bytes: &[u8]) -> Result {
    // Canonicalise: drop leading zero octets, keeping at least one.
    let mut mag = bytes;
    while mag.len() > 1 && mag[0] == 0 {
        mag = &mag[1..];
    }
    // A leading zero is needed to keep the value positive if the top bit is set
    // (or to represent zero when the magnitude is empty).
    let pad = mag.is_empty() || mag[0] & 0x80 != 0;
    out.push(0x02, GFP_KERNEL)?;
    der_len(out, mag.len() + pad as usize)?;
    if pad {
        out.push(0x00, GFP_KERNEL)?;
    }
    out.extend_from_slice(mag, GFP_KERNEL)?;
    Ok(())
}

/// DER-encodes `RSAPublicKey ::= SEQUENCE { modulus INTEGER, publicExponent
/// INTEGER }` from big-endian `modulus`/`exponent`, as `rsa`'s `set_pub_key`
/// expects.
fn der_rsa_pubkey(modulus: &[u8], exponent: &[u8]) -> Result<KVec<u8>> {
    let mut body = KVec::new();
    der_integer(&mut body, modulus)?;
    der_integer(&mut body, exponent)?;
    let mut der = KVec::new();
    der.push(0x30, GFP_KERNEL)?;
    der_len(&mut der, body.len())?;
    der.extend_from_slice(&body, GFP_KERNEL)?;
    Ok(der)
}

/// Computes the RSA public-key operation `out = (input ^ exponent) mod modulus`
/// through the `crypto_akcipher` `"rsa"` transform.
///
/// All buffers are unsigned big-endian. `out` is written fixed-width to exactly
/// `out.len()` bytes (left zero-padded by the cipher); pass `out.len()` equal to
/// the modulus size (e.g. 128 for RSA-1024). This is the bare primitive: the
/// caller applies any padding (PKCS#1 v1.5, EME-OAEP, …) to `input` first.
///
/// `input` interpreted as an integer must be less than `modulus`, as RSA
/// requires; otherwise an error is returned. On any error `out` is zeroed, so
/// it never retains data from a partial computation.
pub fn rsa_pubkey_encrypt(
    modulus: &[u8],
    exponent: &[u8],
    input: &[u8],
    out: &mut [u8],
) -> Result {
    let der = der_rsa_pubkey(modulus, exponent)?;
    let mut tfm = Akcipher::new(c"rsa")?;
    tfm.set_pub_key(&der)?;
    match tfm.encrypt(input, out) {
        Ok(_) => Ok(()),
        Err(e) => {
            out.fill(0);
            Err(e)
        }
    }
}
