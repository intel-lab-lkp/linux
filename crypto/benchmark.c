// SPDX-License-Identifier: GPL-2.0

#include <crypto/aead.h>
#include <crypto/benchmark.h>
#include <crypto/hash.h>
#include <crypto/skcipher.h>
#include <linux/module.h>
#include <linux/timex.h>

int crypto_benchmark_aead_cycles(struct aead_request *req, bool encrypt,
				 unsigned int warmup_runs, unsigned int runs,
				 u64 *total_cycles)
{
	struct crypto_wait *wait;
	unsigned int i;
	u64 cycles = 0;
	int ret;

	if (!req || !req->base.data || !runs || !total_cycles)
		return -EINVAL;

	wait = req->base.data;
	*total_cycles = 0;

	for (i = 0; i < warmup_runs; i++) {
		if (encrypt)
			ret = crypto_wait_req(crypto_aead_encrypt(req), wait);
		else
			ret = crypto_wait_req(crypto_aead_decrypt(req), wait);
		if (ret)
			return ret;
	}

	for (i = 0; i < runs; i++) {
		cycles_t start, end;

		start = get_cycles();
		if (encrypt)
			ret = crypto_wait_req(crypto_aead_encrypt(req), wait);
		else
			ret = crypto_wait_req(crypto_aead_decrypt(req), wait);
		end = get_cycles();
		if (ret)
			return ret;

		cycles += end - start;
	}

	*total_cycles = cycles;

	return 0;
}
EXPORT_SYMBOL_GPL(crypto_benchmark_aead_cycles);

int crypto_benchmark_ahash_cycles(struct ahash_request *req,
				  unsigned int block_size,
				  unsigned int update_size,
				  unsigned int warmup_runs, unsigned int runs,
				  u64 *total_cycles)
{
	struct crypto_wait *wait;
	unsigned int processed;
	unsigned int i;
	u64 cycles = 0;
	int ret;

	if (!req || !req->base.data || !block_size || !update_size ||
	    block_size % update_size || !runs || !total_cycles)
		return -EINVAL;

	wait = req->base.data;
	*total_cycles = 0;

	for (i = 0; i < warmup_runs; i++) {
		if (update_size == block_size) {
			ret = crypto_wait_req(crypto_ahash_digest(req), wait);
			if (ret)
				return ret;
			continue;
		}

		ret = crypto_wait_req(crypto_ahash_init(req), wait);
		if (ret)
			return ret;
		for (processed = 0; processed < block_size;
		     processed += update_size) {
			ret = crypto_wait_req(crypto_ahash_update(req), wait);
			if (ret)
				return ret;
		}
		ret = crypto_wait_req(crypto_ahash_final(req), wait);
		if (ret)
			return ret;
	}

	for (i = 0; i < runs; i++) {
		cycles_t start, end;

		start = get_cycles();
		if (update_size == block_size) {
			ret = crypto_wait_req(crypto_ahash_digest(req), wait);
		} else {
			ret = crypto_wait_req(crypto_ahash_init(req), wait);
			if (ret)
				goto measure_end;
			for (processed = 0; processed < block_size;
			     processed += update_size) {
				ret = crypto_wait_req(crypto_ahash_update(req),
						      wait);
				if (ret)
					goto measure_end;
			}
			ret = crypto_wait_req(crypto_ahash_final(req), wait);
		}
measure_end:
		end = get_cycles();
		if (ret)
			return ret;

		cycles += end - start;
	}

	*total_cycles = cycles;

	return 0;
}
EXPORT_SYMBOL_GPL(crypto_benchmark_ahash_cycles);

int crypto_benchmark_skcipher_cycles(struct skcipher_request *req, bool encrypt,
				     unsigned int warmup_runs,
				     unsigned int runs, u64 *total_cycles)
{
	struct crypto_wait *wait;
	unsigned int i;
	u64 cycles = 0;
	int ret;

	if (!req || !req->base.data || !runs || !total_cycles)
		return -EINVAL;

	wait = req->base.data;
	*total_cycles = 0;

	for (i = 0; i < warmup_runs; i++) {
		if (encrypt)
			ret = crypto_wait_req(crypto_skcipher_encrypt(req),
					      wait);
		else
			ret = crypto_wait_req(crypto_skcipher_decrypt(req),
					      wait);
		if (ret)
			return ret;
	}

	for (i = 0; i < runs; i++) {
		cycles_t start, end;

		start = get_cycles();
		if (encrypt)
			ret = crypto_wait_req(crypto_skcipher_encrypt(req),
					      wait);
		else
			ret = crypto_wait_req(crypto_skcipher_decrypt(req),
					      wait);
		end = get_cycles();
		if (ret)
			return ret;

		cycles += end - start;
	}

	*total_cycles = cycles;

	return 0;
}
EXPORT_SYMBOL_GPL(crypto_benchmark_skcipher_cycles);

MODULE_AUTHOR("Jihong Min <hurryman2212@gmail.com>");
MODULE_DESCRIPTION("Crypto API benchmark helpers");
MODULE_LICENSE("GPL");
