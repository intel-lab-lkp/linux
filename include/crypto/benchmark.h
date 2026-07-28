/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _CRYPTO_BENCHMARK_H
#define _CRYPTO_BENCHMARK_H

#include <linux/types.h>

struct aead_request;
struct ahash_request;
struct skcipher_request;

/* Requests must use crypto_req_done() with struct crypto_wait callback data. */
int crypto_benchmark_aead_cycles(struct aead_request *req, bool encrypt,
				 unsigned int warmup_runs, unsigned int runs,
				 u64 *total_cycles);
int crypto_benchmark_ahash_cycles(struct ahash_request *req,
				  unsigned int block_size,
				  unsigned int update_size,
				  unsigned int warmup_runs, unsigned int runs,
				  u64 *total_cycles);
int crypto_benchmark_skcipher_cycles(struct skcipher_request *req, bool encrypt,
				     unsigned int warmup_runs,
				     unsigned int runs, u64 *total_cycles);

#endif /* _CRYPTO_BENCHMARK_H */
