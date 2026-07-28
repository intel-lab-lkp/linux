/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _CRYPTO_FALLBACK_H
#define _CRYPTO_FALLBACK_H

#include <linux/stddef.h>
#include <linux/types.h>

struct crypto_fallback;
struct device;
struct module;

/**
 * enum crypto_fallback_alg_type - algorithm API used for benchmarking
 * @CRYPTO_FALLBACK_SKCIPHER: symmetric key cipher
 * @CRYPTO_FALLBACK_AHASH: asynchronous hash
 * @CRYPTO_FALLBACK_AEAD: authenticated encryption
 */
enum crypto_fallback_alg_type {
	CRYPTO_FALLBACK_SKCIPHER,
	CRYPTO_FALLBACK_AHASH,
	CRYPTO_FALLBACK_AEAD,
};

/**
 * struct crypto_fallback_benchmark - benchmark algorithm description
 * @type: algorithm API
 * @name: generic algorithm name used to allocate software
 * @driver_name: hardware implementation driver name
 * @key: cipher or hash key
 * @keylen: length of @key
 * @authkey: authentication key for an authenc AEAD, or NULL
 * @authkeylen: length of @authkey
 * @authsize: AEAD authentication tag size
 */
struct crypto_fallback_benchmark {
	enum crypto_fallback_alg_type type;
	const char *name;
	const char *driver_name;
	const u8 *key;
	unsigned int keylen;
	const u8 *authkey;
	unsigned int authkeylen;
	unsigned int authsize;
};

/**
 * struct crypto_fallback_group - algorithms sharing one fallback threshold
 * @name: unique sysfs name for the group
 * @benchmark: representative algorithm used to find the threshold
 * @algs: hardware algorithms using the threshold
 * @num_algs: number of algorithms in @algs
 */
struct crypto_fallback_group {
	const char *name;
	struct crypto_fallback_benchmark benchmark;
	const char *const *algs;
	unsigned int num_algs;
};

#define __CRYPTO_FALLBACK_GROUP(_name, _type, _alg_name, _driver, _key,  \
				_authkey, _authkeylen, _authsize, _algs, \
				_num_algs)                               \
	{                                                                    \
		.name = (_name),                                              \
		.benchmark = {                                                \
			.type = (_type),                                      \
			.name = (_alg_name),                                  \
			.driver_name = (_driver),                             \
			.key = (_key),                                        \
			.keylen = sizeof(_key),                               \
			.authkey = (_authkey),                                \
			.authkeylen = (_authkeylen),                          \
			.authsize = (_authsize),                              \
		},                                                              \
		.algs = (_algs),                                                \
		.num_algs = (_num_algs),                                        \
	}

#define CRYPTO_FALLBACK_GROUP(_name, _type, _alg_name, _driver, _key, _algs,  \
			      _num_algs)                                      \
	__CRYPTO_FALLBACK_GROUP(_name, _type, _alg_name, _driver, _key, NULL, \
				0, 0, _algs, _num_algs)

#define CRYPTO_FALLBACK_GROUP_AUTHENC(_name, _alg_name, _driver, _key,       \
				      _authkey, _authsize, _algs, _num_algs) \
	__CRYPTO_FALLBACK_GROUP(_name, CRYPTO_FALLBACK_AEAD, _alg_name,      \
				_driver, _key, _authkey, sizeof(_authkey),   \
				_authsize, _algs, _num_algs)

struct crypto_fallback *
crypto_fallback_register(struct module *owner, struct device *dev,
			 const struct crypto_fallback_group *groups,
			 unsigned int num_groups);
void crypto_fallback_unregister(struct crypto_fallback *fallback);

#endif /* _CRYPTO_FALLBACK_H */
