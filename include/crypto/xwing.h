/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright 2026 Google LLC
 */

/**
 * DOC: X-Wing key encapsulation mechanism
 *
 * Implementation of X-Wing, a general-purpose post-quantum/traditional hybrid
 * key encapsulation mechanism using X25519 and ML-KEM-768.  X-Wing is the
 * recommended KEM for new applications.  X-Wing is specified at
 * https://datatracker.ietf.org/doc/html/draft-connolly-cfrg-xwing-kem-10
 */

#ifndef _CRYPTO_XWING_H
#define _CRYPTO_XWING_H

#include <linux/types.h>

#define XWING_SEED_BYTES 32 /* Length of seed for KeyGen */
#define XWING_PUBLIC_KEY_BYTES 1216
#define XWING_SECRET_KEY_BYTES 32
#define XWING_ESEED_BYTES 64 /* Length of seed for Encaps */
#define XWING_CIPHERTEXT_BYTES 1120
#define XWING_SHARED_SECRET_BYTES 32

/**
 * xwing_keygen() - Generate an X-Wing key pair
 * @pk: (output) The public key (encapsulation key)
 * @sk: (output) The secret key (decapsulation key)
 *
 * Context: Might sleep
 *
 * Return: 0 on success, or -ENOMEM if out of memory.
 */
int xwing_keygen(u8 pk[XWING_PUBLIC_KEY_BYTES], u8 sk[XWING_SECRET_KEY_BYTES]);

/**
 * xwing_encaps() - Generate and encapsulate shared secret with X-Wing
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
int xwing_encaps(u8 ct[XWING_CIPHERTEXT_BYTES],
		 u8 ss[XWING_SHARED_SECRET_BYTES],
		 const u8 pk[XWING_PUBLIC_KEY_BYTES]);

/**
 * xwing_decaps() - Decapsulate shared secret with X-Wing
 * @ss: (output) The decapsulated shared secret
 * @ct: The ciphertext
 * @sk: The secret key (decapsulation key)
 *
 * Context: Might sleep
 *
 * Return:
 * * 0 on success, including the implicit rejection cases where the ciphertext
 *   is invalid and a randomized shared secret is returned
 * * -EBADMSG if the secret key is malformed
 * * -ENOMEM if out of memory
 */
int xwing_decaps(u8 ss[XWING_SHARED_SECRET_BYTES],
		 const u8 ct[XWING_CIPHERTEXT_BYTES],
		 const u8 sk[XWING_SECRET_KEY_BYTES]);

#if IS_ENABLED(CONFIG_KUNIT)
/* Functions taking explicit seeds, only for KUnit testing */
int xwing_keygen_internal(u8 pk[XWING_PUBLIC_KEY_BYTES],
			  u8 sk[XWING_SECRET_KEY_BYTES],
			  const u8 seed[XWING_SEED_BYTES]);
int xwing_encaps_internal(u8 ct[XWING_CIPHERTEXT_BYTES],
			  u8 ss[XWING_SHARED_SECRET_BYTES],
			  const u8 pk[XWING_PUBLIC_KEY_BYTES],
			  const u8 eseed[XWING_ESEED_BYTES]);
#endif /* CONFIG_KUNIT */

#endif /* _CRYPTO_XWING_H */
