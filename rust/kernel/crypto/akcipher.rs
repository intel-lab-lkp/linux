// SPDX-License-Identifier: GPL-2.0

//! Safe wrappers for the kernel public-key cipher API.
//!
//! C header: [`include/crypto/akcipher.h`](srctree/include/crypto/akcipher.h)

use core::ptr::NonNull;

use crate::{
    alloc::{Flags, KVec},
    bindings, c_str,
    crypto::{sha256, SHA256_DIGEST_SIZE},
    error::{from_err_ptr, to_result},
    prelude::*,
};

/// A configured RSA public key.
///
/// The key is backed by the existing kernel `"rsa"` akcipher implementation.
/// Creating it converts unsigned big-endian modulus and exponent components
/// into the PKCS#1 DER form consumed by the crypto API.
pub struct RsaPublicKey {
    tfm: NonNull<bindings::crypto_akcipher>,
    size: usize,
}

impl RsaPublicKey {
    /// Create an RSA public key from unsigned big-endian components.
    pub fn new(modulus: &[u8], exponent: &[u8], flags: Flags) -> Result<Self> {
        let modulus = trim_unsigned(modulus).ok_or(EINVAL)?;
        let exponent = trim_unsigned(exponent).ok_or(EINVAL)?;
        let der = encode_rsa_public_key(modulus, exponent, flags)?;

        // SAFETY: The name is NUL-terminated and remains live for the call.
        let tfm = from_err_ptr(unsafe {
            bindings::crypto_alloc_akcipher(c_str!("rsa").as_char_ptr(), 0, 0)
        })?;
        let tfm = NonNull::new(tfm).ok_or(ENOMEM)?;

        // SAFETY: `tfm` is a live akcipher transform and `der` contains
        // `der.len()` initialized bytes.
        let result = to_result(unsafe {
            bindings::crypto_akcipher_set_pub_key(
                tfm.as_ptr(),
                der.as_ptr().cast(),
                der.len().try_into()?,
            )
        });
        if let Err(err) = result {
            // SAFETY: `tfm` was returned by `crypto_alloc_akcipher()` and has
            // not been freed.
            unsafe { bindings::crypto_free_akcipher(tfm.as_ptr()) };
            return Err(err);
        }

        Ok(Self {
            tfm,
            size: modulus.len(),
        })
    }

    /// Return the RSA modulus size in bytes.
    pub fn size(&self) -> usize {
        self.size
    }

    /// Apply the raw RSA public-key operation to one already-encoded message.
    ///
    /// Both buffers must have the modulus size. The output is fixed-width,
    /// unsigned, and big-endian. Prefer a padded scheme such as
    /// [`oaep_sha256_encrypt`](Self::oaep_sha256_encrypt).
    pub fn encrypt(&mut self, encoded: &[u8], out: &mut [u8]) -> Result {
        if encoded.len() != self.size || out.len() != self.size {
            return Err(EINVAL);
        }

        out.fill(0);
        // SAFETY: `self.tfm` remains live and exclusively borrowed for the
        // synchronous operation; both buffers are valid for their lengths.
        to_result(unsafe {
            bindings::crypto_akcipher_sync_encrypt(
                self.tfm.as_ptr(),
                encoded.as_ptr().cast(),
                encoded.len().try_into()?,
                out.as_mut_ptr().cast(),
                out.len().try_into()?,
            )
        })
    }

    /// Encrypt a message using RSAES-OAEP with SHA-256 and an empty label.
    ///
    /// `seed` is a caller-provided random OAEP seed. It is explicit so callers
    /// can use the kernel CSPRNG while tests can use published deterministic
    /// vectors.
    #[cfg(CONFIG_RUST_CRYPTO_LIB_SHA256)]
    pub fn oaep_sha256_encrypt(
        &mut self,
        message: &[u8],
        seed: &[u8; SHA256_DIGEST_SIZE],
        out: &mut [u8],
        flags: Flags,
    ) -> Result {
        let overhead = 2 * SHA256_DIGEST_SIZE + 2;
        if out.len() != self.size || self.size < overhead || message.len() > self.size - overhead {
            return Err(EINVAL);
        }

        let mut encoded = KVec::from_elem(0u8, self.size, flags)?;
        let result = (|| {
            encoded[1..1 + SHA256_DIGEST_SIZE].copy_from_slice(seed);
            let db = &mut encoded[1 + SHA256_DIGEST_SIZE..];
            db[..SHA256_DIGEST_SIZE].copy_from_slice(&sha256(&[]));
            let separator = db.len() - message.len() - 1;
            db[separator] = 1;
            db[separator + 1..].copy_from_slice(message);

            mgf1_sha256_xor(seed, db, flags)?;
            let (seed_block, masked_db) = encoded[1..].split_at_mut(SHA256_DIGEST_SIZE);
            mgf1_sha256_xor(masked_db, seed_block, flags)?;

            self.encrypt(&encoded, out)
        })();
        encoded.fill(0);
        result
    }
}

impl Drop for RsaPublicKey {
    fn drop(&mut self) {
        // SAFETY: `self.tfm` was returned by `crypto_alloc_akcipher()` and is
        // owned by this object.
        unsafe { bindings::crypto_free_akcipher(self.tfm.as_ptr()) };
    }
}

#[cfg(CONFIG_RUST_CRYPTO_LIB_SHA256)]
fn mgf1_sha256_xor(seed: &[u8], output: &mut [u8], flags: Flags) -> Result {
    let mut input = KVec::with_capacity(seed.len().checked_add(4).ok_or(EOVERFLOW)?, flags)?;
    let result = (|| {
        let mut counter = 0u32;
        for chunk in output.chunks_mut(SHA256_DIGEST_SIZE) {
            input.clear();
            input.extend_from_slice(seed, flags)?;
            input.extend_from_slice(&counter.to_be_bytes(), flags)?;
            let digest = sha256(&input);
            for (byte, mask) in chunk.iter_mut().zip(digest) {
                *byte ^= mask;
            }
            counter = counter.checked_add(1).ok_or(EOVERFLOW)?;
        }
        Ok(())
    })();
    input.fill(0);
    result
}

fn trim_unsigned(value: &[u8]) -> Option<&[u8]> {
    let value = value
        .iter()
        .position(|byte| *byte != 0)
        .map(|i| &value[i..])?;
    Some(value)
}

fn der_length_size(length: usize) -> Result<usize> {
    if length < 128 {
        return Ok(1);
    }

    let bytes = (usize::BITS - length.leading_zeros()).div_ceil(8) as usize;
    if bytes > 126 {
        return Err(EOVERFLOW);
    }
    Ok(1 + bytes)
}

fn push_der_length(out: &mut KVec<u8>, length: usize, flags: Flags) -> Result {
    if length < 128 {
        out.push(length as u8, flags)?;
        return Ok(());
    }

    let bytes = der_length_size(length)? - 1;
    out.push(0x80 | bytes as u8, flags)?;
    for shift in (0..bytes).rev() {
        out.push((length >> (shift * 8)) as u8, flags)?;
    }
    Ok(())
}

fn der_integer_size(value: &[u8]) -> Result<usize> {
    let leading_zero = usize::from(value[0] & 0x80 != 0);
    1usize
        .checked_add(der_length_size(value.len() + leading_zero)?)
        .and_then(|size| size.checked_add(value.len() + leading_zero))
        .ok_or(EOVERFLOW)
}

fn push_der_integer(out: &mut KVec<u8>, value: &[u8], flags: Flags) -> Result {
    let leading_zero = value[0] & 0x80 != 0;
    out.push(0x02, flags)?;
    push_der_length(out, value.len() + usize::from(leading_zero), flags)?;
    if leading_zero {
        out.push(0, flags)?;
    }
    out.extend_from_slice(value, flags)?;
    Ok(())
}

fn encode_rsa_public_key(modulus: &[u8], exponent: &[u8], flags: Flags) -> Result<KVec<u8>> {
    let content_len = der_integer_size(modulus)?
        .checked_add(der_integer_size(exponent)?)
        .ok_or(EOVERFLOW)?;
    let total_len = 1usize
        .checked_add(der_length_size(content_len)?)
        .and_then(|size| size.checked_add(content_len))
        .ok_or(EOVERFLOW)?;
    let mut der = KVec::with_capacity(total_len, flags)?;
    der.push(0x30, flags)?;
    push_der_length(&mut der, content_len, flags)?;
    push_der_integer(&mut der, modulus, flags)?;
    push_der_integer(&mut der, exponent, flags)?;
    Ok(der)
}

#[cfg(CONFIG_RUST_CRYPTO_KUNIT_TEST)]
#[crate::macros::kunit_tests(rust_kernel_crypto_akcipher)]
mod tests {
    use super::*;
    use crate::alloc::flags::GFP_KERNEL;

    // Wycheproof rsa_oaep_2048_sha256_mgf1sha256_test.json, test case 3.
    const OAEP_MODULUS: [u8; 256] = [
        0xa2, 0xb4, 0x51, 0xa0, 0x7d, 0x0a, 0xa5, 0xf9, 0x6e, 0x45, 0x56, 0x71, 0x51, 0x35, 0x50,
        0x51, 0x4a, 0x8a, 0x5b, 0x46, 0x2e, 0xbe, 0xf7, 0x17, 0x09, 0x4f, 0xa1, 0xfe, 0xe8, 0x22,
        0x24, 0xe6, 0x37, 0xf9, 0x74, 0x6d, 0x3f, 0x7c, 0xaf, 0xd3, 0x18, 0x78, 0xd8, 0x03, 0x25,
        0xb6, 0xef, 0x5a, 0x17, 0x00, 0xf6, 0x59, 0x03, 0xb4, 0x69, 0x42, 0x9e, 0x89, 0xd6, 0xea,
        0xc8, 0x84, 0x50, 0x97, 0xb5, 0xab, 0x39, 0x31, 0x89, 0xdb, 0x92, 0x51, 0x2e, 0xd8, 0xa7,
        0x71, 0x1a, 0x12, 0x53, 0xfa, 0xcd, 0x20, 0xf7, 0x9c, 0x15, 0xe8, 0x24, 0x7f, 0x3d, 0x3e,
        0x42, 0xe4, 0x6e, 0x48, 0xc9, 0x8e, 0x25, 0x4a, 0x2f, 0xe9, 0x76, 0x53, 0x13, 0xa0, 0x3e,
        0xff, 0x8f, 0x17, 0xe1, 0xa0, 0x29, 0x39, 0x7a, 0x1f, 0xa2, 0x6a, 0x8d, 0xce, 0x26, 0xf4,
        0x90, 0xed, 0x81, 0x29, 0x96, 0x15, 0xd9, 0x81, 0x4c, 0x22, 0xda, 0x61, 0x04, 0x28, 0xe0,
        0x9c, 0x7d, 0x96, 0x58, 0x59, 0x42, 0x66, 0xf5, 0xc0, 0x21, 0xd0, 0xfc, 0xec, 0xa0, 0x8d,
        0x94, 0x5a, 0x12, 0xbe, 0x82, 0xde, 0x4d, 0x1e, 0xce, 0x6b, 0x4c, 0x03, 0x14, 0x5b, 0x5d,
        0x34, 0x95, 0xd4, 0xed, 0x54, 0x11, 0xeb, 0x87, 0x8d, 0xaf, 0x05, 0xfd, 0x7a, 0xfc, 0x3e,
        0x09, 0xad, 0xa0, 0xf1, 0x12, 0x64, 0x22, 0xf5, 0x90, 0x97, 0x5a, 0x19, 0x69, 0x81, 0x6f,
        0x48, 0x69, 0x8b, 0xcb, 0xba, 0x1b, 0x4d, 0x9c, 0xae, 0x79, 0xd4, 0x60, 0xd8, 0xf9, 0xf8,
        0x5e, 0x79, 0x75, 0x00, 0x5d, 0x9b, 0xc2, 0x2c, 0x4e, 0x5a, 0xc0, 0xf7, 0xc1, 0xa4, 0x5d,
        0x12, 0x56, 0x9a, 0x62, 0x80, 0x7d, 0x3b, 0x9a, 0x02, 0xe5, 0xa5, 0x30, 0xe7, 0x73, 0x06,
        0x6f, 0x45, 0x3d, 0x1f, 0x5b, 0x4c, 0x2e, 0x9c, 0xf7, 0x82, 0x02, 0x83, 0xf7, 0x42, 0xb9,
        0xd5,
    ];
    const OAEP_SEED: [u8; 32] = [
        0x70, 0x97, 0x14, 0xb0, 0x48, 0xc3, 0x69, 0x73, 0x22, 0x69, 0xa3, 0xd8, 0xf9, 0x23, 0x02,
        0x50, 0x87, 0x70, 0xa4, 0x43, 0x68, 0x01, 0x4b, 0x3a, 0x5c, 0xb1, 0x85, 0xc0, 0xc9, 0x1d,
        0x97, 0x2c,
    ];
    const OAEP_CIPHERTEXT: [u8; 256] = [
        0x5e, 0xab, 0x3f, 0x07, 0x41, 0xe6, 0x39, 0x86, 0xed, 0x64, 0x7d, 0x53, 0xe1, 0xcd, 0x71,
        0xdf, 0x04, 0x19, 0x86, 0x90, 0x08, 0x03, 0xd0, 0xf9, 0x9c, 0x68, 0x35, 0x5d, 0x24, 0x9a,
        0x15, 0xa4, 0x7d, 0xc5, 0xb4, 0xf7, 0x0a, 0x19, 0x14, 0x77, 0x65, 0x42, 0x99, 0xe5, 0xa2,
        0x73, 0x1f, 0x3b, 0x4e, 0xec, 0x76, 0xde, 0xa1, 0x82, 0x62, 0xfc, 0x69, 0x6a, 0xc7, 0x94,
        0xe5, 0xf6, 0x6c, 0xbf, 0xcd, 0xda, 0xc4, 0x47, 0x2c, 0x57, 0x8e, 0x24, 0x6c, 0x26, 0x70,
        0x75, 0x98, 0x05, 0x55, 0x84, 0x54, 0x0b, 0x83, 0x98, 0x36, 0xb1, 0x40, 0x4c, 0x56, 0x11,
        0xae, 0x55, 0x8a, 0x98, 0x4c, 0xee, 0x8f, 0xd0, 0x36, 0xce, 0xa9, 0x24, 0xe0, 0xbe, 0x24,
        0x74, 0xa9, 0x40, 0xf6, 0x1e, 0x0a, 0xcc, 0x14, 0xfc, 0xae, 0x95, 0xeb, 0xdc, 0x59, 0x94,
        0x2a, 0x9c, 0xe9, 0xaf, 0x9a, 0x9c, 0x81, 0x99, 0x9f, 0x7f, 0x68, 0x15, 0xf0, 0x57, 0xff,
        0xdc, 0x25, 0x33, 0xcb, 0x15, 0xd6, 0x39, 0x1d, 0x1e, 0x2d, 0x95, 0xf1, 0x6f, 0x9c, 0x04,
        0x20, 0x9c, 0x88, 0x9a, 0x4c, 0x35, 0x9c, 0x7d, 0x29, 0x26, 0xd2, 0x8a, 0x66, 0xe2, 0xb0,
        0x30, 0xa4, 0x16, 0xb9, 0x28, 0xd2, 0x82, 0x56, 0x27, 0x99, 0x8e, 0x51, 0x91, 0xfb, 0x49,
        0x83, 0xa6, 0xe6, 0x50, 0x24, 0x26, 0x2d, 0x94, 0xfc, 0x09, 0x18, 0x7a, 0x2d, 0x78, 0x16,
        0x21, 0x22, 0x43, 0x32, 0x51, 0xd1, 0xbf, 0xcc, 0x8e, 0x50, 0x7d, 0x06, 0xeb, 0xa2, 0xd2,
        0x29, 0xc1, 0x00, 0x31, 0x26, 0x1d, 0xa3, 0x2a, 0xb8, 0xcc, 0xd1, 0x5f, 0x1c, 0x5f, 0x9f,
        0xbf, 0x07, 0xed, 0x15, 0x84, 0x83, 0xd7, 0x36, 0xa1, 0x10, 0xaf, 0x4b, 0x44, 0xd6, 0xa4,
        0xda, 0x60, 0xd6, 0xcb, 0x51, 0x9b, 0x44, 0x54, 0x21, 0x3c, 0xf9, 0xf0, 0xdc, 0x56, 0x0f,
        0x2b,
    ];

    #[test]
    fn rsa_der_encoding() -> Result {
        let der = encode_rsa_public_key(&[0x80, 0x01], &[0x01, 0x00, 0x01], GFP_KERNEL)?;
        assert_eq!(
            der.as_slice(),
            &[0x30, 0x0a, 0x02, 0x03, 0x00, 0x80, 0x01, 0x02, 0x03, 0x01, 0x00, 0x01]
        );
        Ok(())
    }

    #[test]
    fn rsa_oaep_sha256_wycheproof() -> Result {
        let mut key = RsaPublicKey::new(&OAEP_MODULUS, &[0x01, 0x00, 0x01], GFP_KERNEL)?;
        let mut out = [0u8; 256];
        key.oaep_sha256_encrypt(b"Test", &OAEP_SEED, &mut out, GFP_KERNEL)?;
        assert_eq!(out, OAEP_CIPHERTEXT);
        Ok(())
    }
}
