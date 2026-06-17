// SPDX-License-Identifier: GPL-2.0

//! Thin adapters onto the shared [`kernel::crypto`] library-crypto bindings, so the
//! protocol code keeps its `crypto::aes128_ecb` / `crypto::hmac_sha256` call sites.
#![allow(dead_code)] // exercised by the AES-CTR seal + HDCP AKE

use super::*;

/// `AES_ECB(key, block)` -- one 16-byte AES-128 block.
pub(super) fn aes128_ecb(key: &[u8; 16], block: &[u8; 16]) -> Result<[u8; 16]> {
    kernel::crypto::Aes128::new(*key).encrypt_block(block)
}

/// `HMAC-SHA256(key, data)`.
pub(super) fn hmac_sha256(key: &[u8], data: &[u8]) -> [u8; 32] {
    kernel::crypto::hmac_sha256(key, data)
}

/// `AES-CMAC-128(key, data)` (RFC 4493), built on the one-block ECB above.
/// This is DisplayLink's "Dl3Cmac" core -- the CP per-message integrity tag is
/// `AES_CMAC(ks, nonce8 || BE64(counter) || content)` (see `cp::dl3cmac_tag`);
/// verified byte-exact against live DLM data (canonical guide sec 8.6.7).
pub(super) fn aes_cmac(key: &[u8; 16], data: &[u8]) -> Result<[u8; 16]> {
    // dbl: left-shift the 128-bit value by 1, XOR 0x87 if the MSB was set.
    fn dbl(b: &[u8; 16]) -> [u8; 16] {
        let mut o = [0u8; 16];
        for i in 0..15 {
            o[i] = (b[i] << 1) | (b[i + 1] >> 7);
        }
        o[15] = b[15] << 1;
        if b[0] & 0x80 != 0 {
            o[15] ^= 0x87;
        }
        o
    }
    let l = aes128_ecb(key, &[0u8; 16])?;
    let k1 = dbl(&l);
    let k2 = dbl(&k1);
    let n = if data.is_empty() { 1 } else { data.len().div_ceil(16) };
    let complete = !data.is_empty() && data.len() % 16 == 0;
    let mut c = [0u8; 16];
    for i in 0..n {
        let mut blk = [0u8; 16];
        let start = i * 16;
        let end = core::cmp::min(start + 16, data.len());
        blk[..end - start].copy_from_slice(&data[start..end]);
        if i == n - 1 {
            if complete {
                for j in 0..16 {
                    blk[j] ^= k1[j];
                }
            } else {
                blk[end - start] = 0x80; // 10* padding
                for j in 0..16 {
                    blk[j] ^= k2[j];
                }
            }
        }
        for j in 0..16 {
            blk[j] ^= c[j];
        }
        c = aes128_ecb(key, &blk)?;
    }
    Ok(c)
}

/// `SHA256(data)`.
pub(super) fn sha256(data: &[u8]) -> [u8; 32] {
    kernel::crypto::sha256(data)
}

/// Raw RSA public-key op `out = input^exponent mod modulus`, big-endian,
/// `out` written fixed-width (caller applies OAEP padding to `input`).
pub(super) fn rsa_pubkey_encrypt(
    modulus: &[u8],
    exponent: &[u8],
    input: &[u8],
    out: &mut [u8],
) -> Result {
    kernel::crypto::rsa_pubkey_encrypt(modulus, exponent, input, out)
}
