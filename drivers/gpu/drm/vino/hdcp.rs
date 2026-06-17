// SPDX-License-Identifier: GPL-2.0

//! HDCP 2.2 key derivation and verifier computation (sec 5.6), built on [`crypto`].
//! Lets the driver run a clean-room AKE without DisplayLink's binary; the byte-exact
//! formulas are verified against the live dock in the guide.
#![allow(dead_code)] // some HDCP builders/handlers are reached only after CP engagement

use super::*;

/// `dkey_n = AES_ECB(km with low-8-bytes XOR rn, rtx || (rrx with byte15 XOR n))`
/// (HDCP 2.2 IIA sec 2.7, sec 5.6). The counter `n` XORs into byte 15 (LSB of the rrx
/// half) of the IV; `rn` XORs into the low 8 bytes (km[8..16]) of the key -- zero
/// for the `kd` derivation, the SKE nonce for `dkey_2`.
fn derive_dkey(
    km: &[u8; 16],
    rn: &[u8; 8],
    rtx: &[u8; 8],
    rrx: &[u8; 8],
    n: u8,
) -> Result<[u8; 16]> {
    let mut iv = [0u8; 16];
    iv[..8].copy_from_slice(rtx);
    iv[8..].copy_from_slice(rrx);
    iv[15] ^= n;
    let mut key = *km;
    for i in 0..8 {
        key[8 + i] ^= rn[i];
    }
    crypto::aes128_ecb(&key, &iv)
}

/// `kd = dkey_0 || dkey_1` with `rn = 0` (sec 5.6) -- the 256-bit derived key.
pub(super) fn derive_kd(km: &[u8; 16], rtx: &[u8; 8], rrx: &[u8; 8]) -> Result<[u8; 32]> {
    let rn = [0u8; 8];
    let dkey0 = derive_dkey(km, &rn, rtx, rrx, 0)?;
    let dkey1 = derive_dkey(km, &rn, rtx, rrx, 1)?;
    let mut kd = [0u8; 32];
    kd[..16].copy_from_slice(&dkey0);
    kd[16..].copy_from_slice(&dkey1);
    Ok(kd)
}

/// `H' = HMAC-SHA256(kd, rtx with byte7 ^= repeater)` (sec 5.6).
pub(super) fn compute_h(kd: &[u8; 32], rtx: &[u8; 8], repeater: bool) -> [u8; 32] {
    let mut msg = *rtx;
    msg[7] ^= repeater as u8;
    crypto::hmac_sha256(kd, &msg)
}

/// `L' = HMAC-SHA256(kd with low-8-bytes XOR rrx, rn)` (sec 5.6).
///
/// "low-8-bytes" is the *least-significant* 64 bits of the 256-bit `kd`, i.e.
/// `kd[24..32]` -- verified byte-exact against the live dock by the userspace
/// oracle (`vino-hdcp::kdf::compute_l`). XOR-ing into `kd[0..8]` does not verify.
pub(super) fn compute_l(kd: &[u8; 32], rrx: &[u8; 8], rn: &[u8; 8]) -> [u8; 32] {
    let mut key = *kd;
    for i in 0..8 {
        key[24 + i] ^= rrx[i];
    }
    crypto::hmac_sha256(&key, rn)
}

/// Full `V = HMAC-SHA256(kd, list_header)` (256 bits) for RepeaterAuth (sec 2.3).
/// The **MSB-128** (`[..16]`) is `V'` -- verified against the repeater's
/// `RepeaterAuth_Send_ReceiverID_List` trailer. The **LSB-128** (`[16..]`) is the
/// value the transmitter returns in `RepeaterAuth_Send_Ack`. vino had been sending
/// the MSB (i.e. echoing the dock's own `V'`) as the Ack -- so the dock rejected the
/// repeater authentication, never acknowledged Stream_Manage, and never engaged CP
/// (proven 2026-06-11: vino's ctr6 == the dock's `id=0x21` list trailer; DLM's ctr6
/// is a computed value present in no dock push). H'/L'/V' still pass because V'
/// verification uses the MSB.
pub(super) fn compute_v_full(kd: &[u8; 32], list_header: &[u8]) -> [u8; 32] {
    crypto::hmac_sha256(kd, list_header)
}

/// MGF1 mask generation (RFC 8017 sec B.2.1) with SHA-256: returns `mask_len`
/// bytes of `T = SHA256(seed || I2OSP(0,4)) || SHA256(seed || I2OSP(1,4)) || ...`.
fn mgf1_sha256(seed: &[u8], mask_len: usize) -> Result<KVec<u8>> {
    let mut mask = KVec::with_capacity(mask_len, GFP_KERNEL)?;
    let mut counter: u32 = 0;
    let mut block = KVec::with_capacity(seed.len() + 4, GFP_KERNEL)?;
    while mask.len() < mask_len {
        block.clear();
        block.extend_from_slice(seed, GFP_KERNEL)?;
        block.extend_from_slice(&counter.to_be_bytes(), GFP_KERNEL)?;
        let digest = crypto::sha256(&block);
        let take = core::cmp::min(digest.len(), mask_len - mask.len());
        mask.extend_from_slice(&digest[..take], GFP_KERNEL)?;
        counter += 1;
    }
    Ok(mask)
}

/// EME-OAEP encode (RFC 8017 sec 7.1.1) with SHA-256 and an empty label, for a
/// `k`-byte modulus. Returns the `k`-byte encoded message `EM` ready for the
/// raw RSA op. `seed` is `hLen` (32) random bytes. HDCP 2.2 uses SHA-256 here
/// (SHA-1 makes the dock stop responding -- guide sec 5.4).
fn eme_oaep_encode(k: usize, msg: &[u8], seed: &[u8; 32]) -> Result<KVec<u8>> {
    const HLEN: usize = 32;
    // DB = lHash || PS(zeros) || 0x01 || M, length k - hLen - 1.
    let l_hash = crypto::sha256(&[]);
    let db_len = k - HLEN - 1;
    let mut db = KVec::with_capacity(db_len, GFP_KERNEL)?;
    db.extend_from_slice(&l_hash, GFP_KERNEL)?;
    let ps_len = db_len - HLEN - 1 - msg.len(); // k - mLen - 2*hLen - 2
    for _ in 0..ps_len {
        db.push(0, GFP_KERNEL)?;
    }
    db.push(0x01, GFP_KERNEL)?;
    db.extend_from_slice(msg, GFP_KERNEL)?;
    // maskedDB = DB ^ MGF1(seed, db_len).
    let db_mask = mgf1_sha256(seed, db_len)?;
    for i in 0..db_len {
        db[i] ^= db_mask[i];
    }
    // maskedSeed = seed ^ MGF1(maskedDB, hLen).
    let seed_mask = mgf1_sha256(&db, HLEN)?;
    let mut masked_seed = [0u8; HLEN];
    for i in 0..HLEN {
        masked_seed[i] = seed[i] ^ seed_mask[i];
    }
    // EM = 0x00 || maskedSeed || maskedDB.
    let mut em = KVec::with_capacity(k, GFP_KERNEL)?;
    em.push(0x00, GFP_KERNEL)?;
    em.extend_from_slice(&masked_seed, GFP_KERNEL)?;
    em.extend_from_slice(&db, GFP_KERNEL)?;
    Ok(em)
}

/// RSA-OAEP-SHA256 encrypt the 16-byte master key `km` under the dock's
/// RSA-1024 public key (`modulus[128]`, `exponent`), giving the 128-byte
/// `Ekpub(km)` for `AKE_No_Stored_km` (sec 5.4). Generates a fresh OAEP seed.
pub(super) fn oaep_encrypt_km(
    modulus: &[u8; 128],
    exponent: &[u8],
    km: &[u8; 16],
) -> Result<[u8; 128]> {
    let mut seed = [0u8; 32];
    super::rng::fill(&mut seed);
    let em = eme_oaep_encode(128, km, &seed)?;
    let mut out = [0u8; 128];
    crypto::rsa_pubkey_encrypt(modulus, exponent, &em, &mut out)?;
    Ok(out)
}

/// SKE: `Edkey(ks) = ks XOR (dkey_2 with low-8-bytes XOR rrx)` (sec 5.6).
///
/// `dkey_2` is derived with the SKE nonce `rn` mixed into the key; `rrx` then
/// XORs into the low 8 bytes (`dkey_2[8..16]`) of the mask. The result is the
/// 16-byte `Edkey_ks` carried by `SKE_Send_Eks` (msg_id 0x0b).
pub(super) fn compute_eks(
    km: &[u8; 16],
    rtx: &[u8; 8],
    rrx: &[u8; 8],
    rn: &[u8; 8],
    ks: &[u8; 16],
) -> Result<[u8; 16]> {
    let mut mask = derive_dkey(km, rn, rtx, rrx, 2)?;
    for i in 0..8 {
        mask[8 + i] ^= rrx[i];
    }
    let mut edkey_ks = [0u8; 16];
    for i in 0..16 {
        edkey_ks[i] = ks[i] ^ mask[i];
    }
    Ok(edkey_ks)
}
