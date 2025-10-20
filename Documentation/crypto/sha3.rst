.. SPDX-License-Identifier: GPL-2.0-or-later

==========================
SHA-3 Algorithm collection
==========================

.. Contents:

  - Overview
  - Basic API
    - Extendable-Output Functions
  - Convenience API
  - Internal API
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
instead as this binds each algorithm to a specific C type.


Basic API
=========

The basic API has a separate context struct for each algorithm in the SHA3
suite, none of the contents of which are expected to be accessed directly::

	struct sha3_224_ctx { ... };
	struct sha3_256_ctx { ... };
	struct sha3_384_ctx { ... };
	struct sha3_512_ctx { ... };
	struct shake128_ctx { ... };
	struct shake256_ctx { ... };

There are a collection of initialisation functions, one for each algorithm
supported, that initialise the context appropriately for that algorithm::

	void sha3_224_init(struct sha3_224_ctx *ctx);
	void sha3_256_init(struct sha3_256_ctx *ctx);
	void sha3_384_init(struct sha3_384_ctx *ctx);
	void sha3_512_init(struct sha3_512_ctx *ctx);
	void shake128_init(struct shake128_ctx *ctx);
	void shake256_init(struct shake256_ctx *ctx);

Data is then added with the appropriate update function, again one per
algorithm::

	void sha3_224_update(struct sha3_224_ctx *ctx,
			     const u8 *data, size_t len);
	void sha3_256_update(struct sha3_256_ctx *ctx,
			     const u8 *data, size_t len);
	void sha3_384_update(struct sha3_384_ctx *ctx,
			     const u8 *data, size_t len);
	void sha3_512_update(struct sha3_512_ctx *ctx,
			     const u8 *data, size_t len);
	void shake128_update(struct shake128_ctx *ctx,
			     const u8 *data, size_t len);
	void shake256_update(struct shake256_ctx *ctx,
			     const u8 *data, size_t len);

The update function may be called multiple times if need be to add
non-contiguous data.

For digest algorithms, the digest is finalised and extracted with the
algorithm-specific function::

	void sha3_224_final(struct sha3_224_ctx *ctx,
			    u8 out[SHA3_224_DIGEST_SIZE]);
	void sha3_256_final(struct sha3_256_ctx *ctx,
			    u8 out[SHA3_256_DIGEST_SIZE]);
	void sha3_384_final(struct sha3_384_ctx *ctx,
			    u8 out[SHA3_384_DIGEST_SIZE]);
	void sha3_512_final(struct sha3_512_ctx *ctx,
			    u8 out[SHA3_512_DIGEST_SIZE]);

which also explicitly clears the context.  The amount of data extracted is
determined by the type.


Extendable-Output Functions
---------------------------

For XOFs, once the data has been added to a context, a variable amount of data
may be extracted.  This can be done by calling the appropriate squeeze
function::

	void shake128_squeeze(struct shake128_ctx *ctx, u8 *out, size_t out_len);
	void shake256_squeeze(struct shake256_ctx *ctx, u8 *out, size_t out_len);

and telling it how much data should be extracted.  The squeeze function may be
called multiple times but it will only append the domain separation suffix on
the first invocation.

Note that performing a number of squeezes, with the output laid consequitively
in a buffer, gets exactly the same output as doing a single squeeze for the
combined amount over the same buffer.

Once all the desired output has been extracted, the context should be cleared
with the clear function appropriate to the algorithm::

	void shake128_clear(struct shake128_ctx *ctx);
	void shake256_clear(struct shake256_ctx *ctx);


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


Internal API
============

There is a common internal API underlying all of this that may be used to build
further algorithms or APIs as the engine in the same in all cases.  The
algorithm APIs all wrap the common context structure::

	struct sha3_ctx {
		struct sha3_state	state;
		u8			block_size;
		u8			padding;
		u8			absorb_offset;
		u8			squeeze_offset;
		bool			end_marked;
	};

	struct sha3_state {
		u64			st[SHA3_STATE_SIZE / 8];
	};

The fields are as follows:

 * ``state.st``

   An array of 25 64-bit state buckets that are used to hold the mathematical
   state of the Keccak engine.  Data is XOR'd onto part of this, the engine is
   cranked and then the output is copied from this.

   For the convenience of adding input and extract output from it, the array is
   kept in little-endian order most of the time, but is byteswapped to
   host-endian in order to perform the Keccak function and then byteswapped
   back again.  On an LE machine, the byteswapping is a no-op.

 * ``block_size``

   The size of the block of state that can be updated or extracted at a time.
   This is related to the algorithm size and is analogous to the "rate" in the
   algorithm definition.

 * ``padding``

   The terminating byte to add when finalising the stat.  This may differ
   between algorithms.

 * ``absorb_offset``

   This tracks which is the next byte of state to be updated; when it hits
   ``block_size``, the engine is cranked and this is reset to 0.

 * ``squeeze_offset``

   This tracks which is the next byte of state to be extracted; similar to
   ``partial``, when it hits ``block_size``, if more output is requested, the
   engine is cranked to generate more and this is reset to 0.

 * ``end_marked``

   This is set to true when the domain separation suffix and any padding have
   been appended to the state to prevent multiple squeezings from XOF
   algorithms from re-appending this.

Note that the size of the digest is *not* included here as that's only needed
at finalisation time for digest algorithms and can be supplied then.  It is not
relevant to XOFs.

To make use of the context, the following internal functions are provided::

	void sha3_update(struct sha3_ctx *ctx, const u8 *data, size_t len);
	void sha3_squeeze(struct sha3_ctx *ctx, u8 *out, size_t out_len);
	void sha3_clear(struct sha3_ctx *ctx);

These allow data to be appended to/absorbed into the state, output to be
extracted/squeezed from the state and for the state to be cleared.  Note that
there is no "final" function, per se, but that can be constructed by squeezing
and clearing.


Testing
-------

The sha3 module does a basic sanity test on initialisation, but there is also a
kunit test module available.


References
==========

[1] https://nvlpubs.nist.gov/nistpubs/FIPS/NIST.FIPS.202.pdf



API Function Reference
======================

.. kernel-doc:: crypto/lib/sha3.c
.. kernel-doc:: include/crypto/sha3.h
