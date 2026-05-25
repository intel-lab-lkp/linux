/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright 2026 Google LLC
 */

/**
 * DOC: ML-KEM (Module-Lattice-Based Key Encapsulation Mechanism)
 *
 * This is an implementation of ML-KEM, a "post-quantum" key encapsulation
 * mechanism that is specified in FIPS 203 and is based on CRYSTALS-Kyber.
 *
 * Specifically, the ML-KEM-768 and ML-KEM-1024 parameter sets are supported.
 *
 * This shall be used as part of a hybrid scheme such as X-Wing, not by itself.
 *
 * This implementation is designed to be constant-time, compact, and
 * memory-efficient, and to reuse the kernel's SHA-3 routines.  For simplicity,
 * it stores integers mod Q as their standard representatives in the interval
 * [0, Q - 1] across function boundaries.  (This makes it more similar to e.g.
 * BoringSSL than to the Kyber reference code, which uses a slightly more
 * optimized but harder-to-understand approach.)
 */

#ifndef _CRYPTO_MLKEM_H
#define _CRYPTO_MLKEM_H

#include <linux/types.h>

#define MLKEM_SEED_BYTES 64 /* Length of seed for KeyGen */
#define MLKEM_ESEED_BYTES 32 /* Length of seed for Encaps */
#define MLKEM_SHARED_SECRET_BYTES 32

#define MLKEM768_PUBLIC_KEY_BYTES 1184
#define MLKEM768_SECRET_KEY_BYTES 2400
#define MLKEM768_CIPHERTEXT_BYTES 1088

/**
 * mlkem768_keygen() - Generate an ML-KEM-768 key pair
 * @pk: (output) The public key (encapsulation key)
 * @sk: (output) The secret key (decapsulation key)
 *
 * Context: Might sleep
 *
 * Return: 0 on success, or -ENOMEM if out of memory.
 */
int mlkem768_keygen(u8 pk[MLKEM768_PUBLIC_KEY_BYTES],
		    u8 sk[MLKEM768_SECRET_KEY_BYTES]);

/**
 * mlkem768_encaps() - Generate and encapsulate shared secret with ML-KEM-768
 * @ct: (output) The ciphertext
 * @ss: (output) The generated shared secret
 * @pk: The public key (encapsulation key)
 *
 * Context: Might sleep
 *
 * Return:
 * * 0 on success
 * * -EBADMSG if the public key is malformed
 * * -ENOMEM if out of memory
 */
int mlkem768_encaps(u8 ct[MLKEM768_CIPHERTEXT_BYTES],
		    u8 ss[MLKEM_SHARED_SECRET_BYTES],
		    const u8 pk[MLKEM768_PUBLIC_KEY_BYTES]);

/**
 * mlkem768_decaps() - Decapsulate shared secret with ML-KEM-768
 * @ss: (output) The decapsulated shared secret
 * @ct: The ciphertext
 * @sk: The secret key (decapsulation key)
 *
 * Context: Might sleep
 *
 * Return:
 * * 0 on success, including the "implicit rejection" case where the ciphertext
 *   is invalid and a randomized shared secret is returned
 * * -EBADMSG if the secret key is malformed
 * * -ENOMEM if out of memory
 */
int mlkem768_decaps(u8 ss[MLKEM_SHARED_SECRET_BYTES],
		    const u8 ct[MLKEM768_CIPHERTEXT_BYTES],
		    const u8 sk[MLKEM768_SECRET_KEY_BYTES]);

#define MLKEM1024_PUBLIC_KEY_BYTES 1568
#define MLKEM1024_SECRET_KEY_BYTES 3168
#define MLKEM1024_CIPHERTEXT_BYTES 1568

/**
 * mlkem1024_keygen() - Generate an ML-KEM-1024 key pair
 * @pk: (output) The public key (encapsulation key)
 * @sk: (output) The secret key (decapsulation key)
 *
 * Context: Might sleep
 *
 * Return: 0 on success, or -ENOMEM if out of memory.
 */
int mlkem1024_keygen(u8 pk[MLKEM1024_PUBLIC_KEY_BYTES],
		     u8 sk[MLKEM1024_SECRET_KEY_BYTES]);

/**
 * mlkem1024_encaps() - Generate and encapsulate shared secret with ML-KEM-1024
 * @ct: (output) The ciphertext
 * @ss: (output) The generated shared secret
 * @pk: The public key (encapsulation key)
 *
 * Context: Might sleep
 *
 * Return:
 * * 0 on success
 * * -EBADMSG if the public key is malformed
 * * -ENOMEM if out of memory
 */
int mlkem1024_encaps(u8 ct[MLKEM1024_CIPHERTEXT_BYTES],
		     u8 ss[MLKEM_SHARED_SECRET_BYTES],
		     const u8 pk[MLKEM1024_PUBLIC_KEY_BYTES]);

/**
 * mlkem1024_decaps() - Decapsulate shared secret with ML-KEM-1024
 * @ss: (output) The decapsulated shared secret
 * @ct: The ciphertext
 * @sk: The secret key (decapsulation key)
 *
 * Context: Might sleep
 *
 * Return:
 * * 0 on success, including the "implicit rejection" case where the ciphertext
 *   is invalid and a randomized shared secret is returned
 * * -EBADMSG if the secret key is malformed
 * * -ENOMEM if out of memory
 */
int mlkem1024_decaps(u8 ss[MLKEM_SHARED_SECRET_BYTES],
		     const u8 ct[MLKEM1024_CIPHERTEXT_BYTES],
		     const u8 sk[MLKEM1024_SECRET_KEY_BYTES]);

/* Functions taking explicit seeds, only for KUnit testing and hybrid KEMs */
int mlkem768_keygen_internal(u8 pk[MLKEM768_PUBLIC_KEY_BYTES],
			     u8 sk[MLKEM768_SECRET_KEY_BYTES],
			     const u8 seed[MLKEM_SEED_BYTES]);
int mlkem768_encaps_internal(u8 ct[MLKEM768_CIPHERTEXT_BYTES],
			     u8 ss[MLKEM_SHARED_SECRET_BYTES],
			     const u8 pk[MLKEM768_PUBLIC_KEY_BYTES],
			     const u8 eseed[MLKEM_ESEED_BYTES]);
int mlkem1024_keygen_internal(u8 pk[MLKEM1024_PUBLIC_KEY_BYTES],
			      u8 sk[MLKEM1024_SECRET_KEY_BYTES],
			      const u8 seed[MLKEM_SEED_BYTES]);
int mlkem1024_encaps_internal(u8 ct[MLKEM1024_CIPHERTEXT_BYTES],
			      u8 ss[MLKEM_SHARED_SECRET_BYTES],
			      const u8 pk[MLKEM1024_PUBLIC_KEY_BYTES],
			      const u8 eseed[MLKEM_ESEED_BYTES]);

#endif /* _CRYPTO_MLKEM_H */
