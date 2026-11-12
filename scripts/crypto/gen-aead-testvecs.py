#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Script that generates known-good data used in the AEAD tests.
#
# Requires that python-cryptography be installed, except for AEGIS-128X4.
#
# Copyright 2026 Google LLC

import hashlib
import sys


# Deterministically generate 'length' random bytes.
def rand_bytes(length):
    seed = length
    out = []
    for _ in range(length):
        seed = (seed * 25214903917 + 11) % 2**48
        out.append((seed >> 16) % 256)
    return bytes(out)


# Multiply two elements of GF(2^8), using the AES polynomial.
def gf256_mul(a, b):
    product = 0
    for _ in range(8):
        if b & 1:
            product ^= a
        a = (a << 1) ^ (0x11B if a & 0x80 else 0)
        b >>= 1
    return product


# The AES S-box: the multiplicative inverse in GF(2^8), then the affine map.
def gen_aes_sbox():
    sbox = []
    for x in range(256):
        inv = next((y for y in range(256) if gf256_mul(x, y) == 1), 0)
        s = inv
        for i in range(1, 5):
            s ^= ((inv << i) | (inv >> (8 - i))) & 0xFF
        sbox.append(s ^ 0x63)
    return sbox


AES_SBOX = gen_aes_sbox()


def xor_bytes(a, b):
    return bytes(x ^ y for x, y in zip(a, b))


def and_bytes(a, b):
    return bytes(x & y for x, y in zip(a, b))


# The AES round function: SubBytes, ShiftRows, MixColumns and AddRoundKey.
def aes_round(block, round_key):
    s = [AES_SBOX[block[(i + 4 * (i % 4)) % 16]] for i in range(16)]
    out = []
    for c in range(4):
        col = s[4 * c : 4 * c + 4]
        for r in range(4):
            out.append(
                gf256_mul(col[r], 2)
                ^ gf256_mul(col[(r + 1) % 4], 3)
                ^ col[(r + 2) % 4]
                ^ col[(r + 3) % 4]
            )
    return xor_bytes(out, round_key)


# AEGIS-128X encryption with parallelism degree 'd', as specified in RFC 10032
# section 5.4, since python-cryptography doesn't support AEGIS.
def aegis128x_encrypt(d, key, nonce, ad, pt, tag_len):
    c0 = bytes.fromhex("000101020305080d1522375990e97962")
    c1 = bytes.fromhex("db3d18556dc22ff12011314273b528dd")
    rate = 32 * d
    half = rate // 2

    def update(v, m0, m1):
        for i, s in enumerate(v):
            v[i] = [
                aes_round(s[7], xor_bytes(s[0], m0[16 * i : 16 * i + 16])),
                aes_round(s[0], s[1]),
                aes_round(s[1], s[2]),
                aes_round(s[2], s[3]),
                aes_round(s[3], xor_bytes(s[4], m1[16 * i : 16 * i + 16])),
                aes_round(s[4], s[5]),
                aes_round(s[5], s[6]),
                aes_round(s[6], s[7]),
            ]

    # Init
    kn = xor_bytes(key, nonce)
    kc0 = xor_bytes(key, c0)
    kc1 = xor_bytes(key, c1)
    v = [[kn, c1, c0, c1, kn, kc0, kc1, kc0] for _ in range(d)]
    for _ in range(10):
        for i in range(d):
            ctx = bytes([i, d - 1]) + bytes(14)
            v[i][3] = xor_bytes(v[i][3], ctx)
            v[i][7] = xor_bytes(v[i][7], ctx)
        update(v, nonce * d, key * d)

    # Absorb
    padded = ad + bytes(-len(ad) % rate)
    for i in range(0, len(padded), rate):
        update(v, padded[i : i + half], padded[i + half : i + rate])

    # Enc
    ct = b""
    padded = pt + bytes(-len(pt) % rate)
    for i in range(0, len(padded), rate):
        z0 = b"".join(
            xor_bytes(xor_bytes(s[1], s[6]), and_bytes(s[2], s[3])) for s in v
        )
        z1 = b"".join(
            xor_bytes(xor_bytes(s[2], s[5]), and_bytes(s[6], s[7])) for s in v
        )
        block = padded[i : i + rate]
        ct += xor_bytes(block, z0 + z1)
        update(v, block[:half], block[half:])

    # Finalize
    u = (len(ad) * 8).to_bytes(8, "little")
    u += (len(pt) * 8).to_bytes(8, "little")
    t = b"".join(xor_bytes(s[2], u) for s in v)
    for _ in range(7):
        update(v, t, t)
    if tag_len == 16:
        tag = bytes(16)
        for s in v:
            for j in range(7):
                tag = xor_bytes(tag, s[j])
    else:
        tag0 = tag1 = bytes(16)
        for s in v:
            for j in range(4):
                tag0 = xor_bytes(tag0, s[j])
                tag1 = xor_bytes(tag1, s[j + 4])
        tag = tag0 + tag1
    return ct[: len(pt)] + tag


# Deterministically generate many different AEAD inputs using exactly the same
# method that the test uses; encrypt them using an independent implementation of
# the algorithm; compute the checksum of all the resulting (ciphertext, authtag)
# pairs concatenated to each other; and print the checksum as a C struct.
def gen_monte_carlo_checksum(alg):
    blake2s = hashlib.blake2s()
    for data_len in range(1025):
        ad_len = data_len % 293
        pt = rand_bytes(data_len)
        ad = rand_bytes(ad_len)
        if alg == "aegis128x4":
            key = rand_bytes(16)
            nonce = rand_bytes(16)
            tag_len = [16, 32][data_len % 2]
            ct_and_tag = aegis128x_encrypt(4, key, nonce, ad, pt, tag_len)
        elif alg == "aes-ccm":
            import cryptography.hazmat.primitives.ciphers.aead

            key_len = [16, 24, 32][data_len % 3]
            key = rand_bytes(key_len)
            nonce = rand_bytes([7, 8, 9, 10, 11, 12, 13][data_len % 7])
            tag_len = [4, 6, 8, 10, 12, 14, 16][data_len % 7]
            ccm = cryptography.hazmat.primitives.ciphers.aead.AESCCM(
                key, tag_length=tag_len
            )
            ct_and_tag = ccm.encrypt(nonce, pt, ad)
        elif alg == "aes-gcm":
            import cryptography.hazmat.primitives.ciphers.aead

            key_len = [16, 24, 32][data_len % 3]
            key = rand_bytes(key_len)
            nonce = rand_bytes(12)
            tag_len = [4, 8, 12, 13, 14, 15, 16][data_len % 7]
            gcm = cryptography.hazmat.primitives.ciphers.aead.AESGCM(key)
            # python-cryptography supports only 16-byte GCM tags.  However, in
            # GCM, shorter tags are simply truncated.  Do that below.
            ct_and_tag = gcm.encrypt(nonce, pt, ad)[: data_len + tag_len]

        blake2s.update(ct_and_tag)

    print_checksum(f"{alg.replace('-', '_')}_monte_carlo_checksum", blake2s)


# Lengths of the (data, associated data) pairs in test_aegis128x4_long(), which
# cross the 4 KiB units that the x86 code splits its work into.
AEGIS128X4_LONG_LENS = ((4097, 0), (8320, 4100), (12345, 129))


# Checksum of the (ciphertext, authtag) pairs of test_aegis128x4_long().
def gen_aegis128x4_long_checksum():
    blake2s = hashlib.blake2s()
    for data_len, ad_len in AEGIS128X4_LONG_LENS:
        pt = rand_bytes(data_len)
        ad = rand_bytes(ad_len)
        key = rand_bytes(16)
        nonce = rand_bytes(16)
        blake2s.update(aegis128x_encrypt(4, key, nonce, ad, pt, 32))
    print_checksum("aegis128x4_long_checksum", blake2s)


def print_checksum(name, blake2s):
    value = blake2s.digest()
    print(f"static const u8 {name}[BLAKE2S_HASH_SIZE] = {{")
    for i in range(0, len(value), 11):
        line = "\t" + "".join(f"0x{b:02x}, " for b in value[i : i + 11])
        print(f"{line.rstrip()}")
    print("};")


ALGS = ("aegis128x4", "aes-ccm", "aes-gcm")
if len(sys.argv) != 2 or sys.argv[1] not in ALGS:
    sys.stderr.write(f"Usage: gen-aead-testvecs.py [{'|'.join(ALGS)}]\n")
    sys.exit(1)

gen_monte_carlo_checksum(sys.argv[1])
if sys.argv[1] == "aegis128x4":
    gen_aegis128x4_long_checksum()
