.. SPDX-License-Identifier: GPL-2.0-or-later

==========================
SHA-3 Algorithm collection
==========================

.. Contents:

  - Overview
  - Digests
  - Extendable-Output Functions
  - Convenience API
  - Testing
  - References
  - API Function Reference


Overview
========

The SHA-3 algorithm base, as specified in NIST FIPS-202[1], provides a number
of specific variants all based on the same basic algorithm (the Keccak sponge
function and permutation).  The differences between them are: the "rate" (how
much of the common state buffer gets updated with new data between invocations
of the Keccak function and analogous to the "block size"), what domain
separation suffix/padding gets appended to the message and how much data is
extracted at the end.  The Keccak sponge function is designed such that
arbitrary amounts of output can be obtained for certain algorithms.

Four standard digest algorithms are provided:

 - SHA3-224
 - SHA3-256
 - SHA3-384
 - SHA3-512

and two Extendable-Output Functions (XOF):

 - SHAKE128
 - SHAKE256

If selectable algorithms are required then the crypto_hash API may be used
instead.


Digests
=======

The SHA-3 digest API uses the following struct::

	struct sha3_ctx { ... };

There are a collection of initialization functions, one for each algorithm::

	void sha3_224_init(struct sha3_ctx *ctx);
	void sha3_256_init(struct sha3_ctx *ctx);
	void sha3_384_init(struct sha3_ctx *ctx);
	void sha3_512_init(struct sha3_ctx *ctx);

Data is then added with::

	void sha3_update(struct sha3_ctx *ctx, const u8 *in, size_t in_len);

The update function may be called multiple times if need be to add
non-contiguous data.

Finally, the digest is generated using::

	void sha3_final(struct sha3_ctx *ctx, u8 *out);

which also zeroizes the context.  The length of the digest is determined by the
initialization function that was called.

Extendable-Output Functions
===========================

The SHA-3 extendable-output function (XOF) API uses the following struct::

	struct shake_ctx { ... };

Initialization is done with one of::

	void shake128_init(struct shake_ctx *ctx);
	void shake256_init(struct shake_ctx *ctx);

Data is then added with::

	void shake_update(struct shake_ctx *ctx, const u8 *in, size_t in_len);

The update function may be called multiple times if need be to add
non-contiguous data.

Finally, the output is extracted using::

	void shake_squeeze(struct shake_ctx *ctx, u8 *out, size_t out_len);

and telling it how much data should be extracted.  The squeeze function may be
called multiple times but it will only append the domain separation suffix on
the first invocation.

Note that performing a number of squeezes, with the output laid consequitively
in a buffer, gets exactly the same output as doing a single squeeze for the
combined amount over the same buffer.

Once all the desired output has been extracted, zeroize the context::

	void shake_zeroize_ctx(struct shake_ctx *ctx);

Convenience API
===============

It only a single contiguous buffer of input needs to be added and only a single
buffer of digest or XOF output is required, then a convenience API is provided
that wraps all the required steps into a single function.  There is one
function for each algorithm supported::

	void sha3_224(const u8 *in, size_t in_len, u8 out[SHA3_224_DIGEST_SIZE]);
	void sha3_256(const u8 *in, size_t in_len, u8 out[SHA3_256_DIGEST_SIZE]);
	void sha3_384(const u8 *in, size_t in_len, u8 out[SHA3_384_DIGEST_SIZE]);
	void sha3_512(const u8 *in, size_t in_len, u8 out[SHA3_512_DIGEST_SIZE]);
	void shake128(const u8 *in, size_t in_len, u8 *out, size_t out_len);
	void shake256(const u8 *in, size_t in_len, u8 *out, size_t out_len);


Testing
=======

To test the SHA-3 code, use sha3_kunit.

Since the SHA-3 algorithms are FIPS-approved, when the kernel is booted in FIPS
mode the SHA-3 library also performs a simple self-test.  This is purely to meet
a FIPS requirement.  Normal testing done by kernel developers and integrators
should use the much more comprehensive KUnit test suite instead.


References
==========

[1] https://nvlpubs.nist.gov/nistpubs/FIPS/NIST.FIPS.202.pdf



API Function Reference
======================

.. kernel-doc:: lib/crypto/sha3.c
.. kernel-doc:: include/crypto/sha3.h
