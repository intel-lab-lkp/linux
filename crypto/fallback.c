// SPDX-License-Identifier: GPL-2.0

#include <crypto/authenc.h>
#include <crypto/benchmark.h>
#include <crypto/fallback.h>
#include <crypto/internal/aead.h>
#include <crypto/internal/hash.h>
#include <crypto/internal/skcipher.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/limits.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/random.h>
#include <linux/rtnetlink.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/sysfs.h>

static const unsigned int crypto_fallback_block_sizes[] = {
	16, 64, 128, 256, 512, 1024, 1420, 2048, 4096, 8192, 16384,
};

enum crypto_fallback_implementation {
	CRYPTO_FALLBACK_HARDWARE,
	CRYPTO_FALLBACK_SOFTWARE,
};

struct fallback_threshold {
	int value;
};

struct fallback_alg;

struct crypto_fallback_group_state {
	struct fallback_threshold threshold;
	int value;
	struct crypto_fallback_group group;
	struct crypto_fallback *fallback;
	struct device_attribute dev_attr;
};

struct crypto_fallback {
	struct device *dev;
	struct crypto_fallback_group_state *groups;
	struct attribute_group sysfs_group;
	struct attribute **sysfs_attrs;
	struct device_attribute enabled_attr;
	struct kref refcount;
	struct mutex lock; /* Serializes tuning and sysfs writes. */
	spinlock_t alg_lock; /* Protects algs and fallback_alg state. */
	struct list_head algs;
	struct module *owner;
	unsigned int num_groups;
	bool enabled;
};

static const char *
crypto_fallback_alg_name(const struct crypto_fallback_benchmark *alg,
			 enum crypto_fallback_implementation implementation)
{
	if (implementation == CRYPTO_FALLBACK_HARDWARE)
		return alg->driver_name;

	return alg->name;
}

static u32
crypto_fallback_alg_mask(enum crypto_fallback_implementation implementation)
{
	if (implementation == CRYPTO_FALLBACK_HARDWARE)
		return 0;

	return CRYPTO_ALG_ASYNC | CRYPTO_ALG_NEED_FALLBACK;
}

static int fallback_bench_skcipher(const struct crypto_fallback_benchmark *alg,
				   enum crypto_fallback_implementation impl,
				   unsigned int block_size,
				   unsigned int warmup_runs, unsigned int runs,
				   u64 *total_cycles)
{
	struct skcipher_request *req = NULL;
	struct crypto_skcipher *tfm;
	struct crypto_wait wait;
	struct scatterlist sg;
	unsigned int ivsize;
	unsigned int len;
	u8 *data = NULL;
	u8 *iv = NULL;
	int err;

	tfm = crypto_alloc_skcipher(crypto_fallback_alg_name(alg, impl), 0,
				    crypto_fallback_alg_mask(impl));
	if (IS_ERR(tfm))
		return PTR_ERR(tfm);

	err = crypto_skcipher_setkey(tfm, alg->key, alg->keylen);
	if (err)
		goto out_free_tfm;

	len = round_up(block_size, crypto_skcipher_blocksize(tfm));
	ivsize = crypto_skcipher_ivsize(tfm);
	data = kmalloc(len, GFP_KERNEL);
	iv = kzalloc(ivsize, GFP_KERNEL);
	if (!data || (ivsize && !iv)) {
		err = -ENOMEM;
		goto out_free_buffers;
	}

	get_random_bytes(data, len);
	sg_init_one(&sg, data, len);

	req = skcipher_request_alloc(tfm, GFP_KERNEL);
	if (!req) {
		err = -ENOMEM;
		goto out_free_buffers;
	}

	crypto_init_wait(&wait);
	skcipher_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				      crypto_req_done, &wait);
	skcipher_request_set_crypt(req, &sg, &sg, len, iv);
	err = crypto_benchmark_skcipher_cycles(req, true, warmup_runs, runs,
					       total_cycles);

out_free_buffers:
	skcipher_request_free(req);
	kfree(iv);
	kfree(data);
out_free_tfm:
	crypto_free_skcipher(tfm);

	return err;
}

static int fallback_bench_ahash(const struct crypto_fallback_benchmark *alg,
				enum crypto_fallback_implementation impl,
				unsigned int block_size,
				unsigned int warmup_runs, unsigned int runs,
				u64 *total_cycles)
{
	struct ahash_request *req = NULL;
	struct crypto_ahash *tfm;
	struct crypto_wait wait;
	struct scatterlist src;
	u8 *input = NULL;
	u8 *output = NULL;
	int err;

	tfm = crypto_alloc_ahash(crypto_fallback_alg_name(alg, impl), 0,
				 crypto_fallback_alg_mask(impl));
	if (IS_ERR(tfm))
		return PTR_ERR(tfm);

	if (alg->keylen) {
		err = crypto_ahash_setkey(tfm, alg->key, alg->keylen);
		if (err)
			goto out_free_tfm;
	}

	input = kmalloc(block_size, GFP_KERNEL);
	output = kmalloc(crypto_ahash_digestsize(tfm), GFP_KERNEL);
	if (!input || !output) {
		err = -ENOMEM;
		goto out_free_buffers;
	}

	get_random_bytes(input, block_size);
	sg_init_one(&src, input, block_size);

	req = ahash_request_alloc(tfm, GFP_KERNEL);
	if (!req) {
		err = -ENOMEM;
		goto out_free_buffers;
	}

	crypto_init_wait(&wait);
	ahash_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				   crypto_req_done, &wait);
	ahash_request_set_crypt(req, &src, output, block_size);
	err = crypto_benchmark_ahash_cycles(req, block_size, block_size,
					    warmup_runs, runs, total_cycles);

out_free_buffers:
	ahash_request_free(req);
	kfree(output);
	kfree(input);
out_free_tfm:
	crypto_free_ahash(tfm);

	return err;
}

static int
crypto_fallback_aead_setkey(struct crypto_aead *tfm,
			    const struct crypto_fallback_benchmark *alg)
{
	struct crypto_authenc_key_param *param;
	struct rtattr *rta;
	unsigned int len;
	u8 *key;
	int err;

	if (!alg->authkeylen)
		return crypto_aead_setkey(tfm, alg->key, alg->keylen);

	len = RTA_LENGTH(sizeof(*param)) + alg->authkeylen + alg->keylen;
	key = kzalloc(len, GFP_KERNEL);
	if (!key)
		return -ENOMEM;

	rta = (struct rtattr *)key;
	rta->rta_type = CRYPTO_AUTHENC_KEYA_PARAM;
	rta->rta_len = RTA_LENGTH(sizeof(*param));
	param = RTA_DATA(rta);
	param->enckeylen = cpu_to_be32(alg->keylen);
	memcpy(key + rta->rta_len, alg->authkey, alg->authkeylen);
	memcpy(key + rta->rta_len + alg->authkeylen, alg->key, alg->keylen);

	err = crypto_aead_setkey(tfm, key, len);
	kfree_sensitive(key);

	return err;
}

static int fallback_bench_aead(const struct crypto_fallback_benchmark *alg,
			       enum crypto_fallback_implementation impl,
			       unsigned int block_size,
			       unsigned int warmup_runs, unsigned int runs,
			       u64 *total_cycles)
{
	struct aead_request *req = NULL;
	struct crypto_aead *tfm;
	struct crypto_wait wait;
	struct scatterlist src;
	struct scatterlist dst;
	unsigned int ivsize;
	unsigned int len;
	u8 *input = NULL;
	u8 *output = NULL;
	u8 *iv = NULL;
	int err;

	tfm = crypto_alloc_aead(crypto_fallback_alg_name(alg, impl), 0,
				crypto_fallback_alg_mask(impl));
	if (IS_ERR(tfm))
		return PTR_ERR(tfm);

	err = crypto_aead_setauthsize(tfm, alg->authsize);
	if (err)
		goto out_free_tfm;
	err = crypto_fallback_aead_setkey(tfm, alg);
	if (err)
		goto out_free_tfm;

	len = round_up(block_size, crypto_aead_blocksize(tfm));
	ivsize = crypto_aead_ivsize(tfm);
	input = kmalloc(len, GFP_KERNEL);
	output = kmalloc(len + alg->authsize, GFP_KERNEL);
	iv = kzalloc(ivsize, GFP_KERNEL);
	if (!input || !output || (ivsize && !iv)) {
		err = -ENOMEM;
		goto out_free_buffers;
	}

	get_random_bytes(input, len);
	sg_init_one(&src, input, len);
	sg_init_one(&dst, output, len + alg->authsize);

	req = aead_request_alloc(tfm, GFP_KERNEL);
	if (!req) {
		err = -ENOMEM;
		goto out_free_buffers;
	}

	crypto_init_wait(&wait);
	aead_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				  crypto_req_done, &wait);
	aead_request_set_crypt(req, &src, &dst, len, iv);
	aead_request_set_ad(req, 0);
	err = crypto_benchmark_aead_cycles(req, true, warmup_runs, runs,
					   total_cycles);

out_free_buffers:
	aead_request_free(req);
	kfree(iv);
	kfree(output);
	kfree(input);
out_free_tfm:
	crypto_free_aead(tfm);

	return err;
}

static int fallback_bench(const struct crypto_fallback_benchmark *alg,
			  enum crypto_fallback_implementation impl,
			  unsigned int block_size, unsigned int warmup_runs,
			  unsigned int runs, u64 *total_cycles)
{
	switch (alg->type) {
	case CRYPTO_FALLBACK_SKCIPHER:
		return fallback_bench_skcipher(alg, impl, block_size,
					       warmup_runs, runs, total_cycles);
	case CRYPTO_FALLBACK_AHASH:
		return fallback_bench_ahash(alg, impl, block_size, warmup_runs,
					    runs, total_cycles);
	case CRYPTO_FALLBACK_AEAD:
		return fallback_bench_aead(alg, impl, block_size, warmup_runs,
					   runs, total_cycles);
	}

	return -EINVAL;
}

static ssize_t fallback_threshold_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct crypto_fallback_group_state *group;

	group = container_of(attr, struct crypto_fallback_group_state,
			     dev_attr);

	return sysfs_emit(buf, "%d\n", READ_ONCE(group->value));
}

static ssize_t fallback_threshold_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct crypto_fallback_group_state *group;
	int threshold;
	int err;

	err = kstrtoint(buf, 0, &threshold);
	if (err)
		return err;
	if (threshold < -1)
		return -ERANGE;

	group = container_of(attr, struct crypto_fallback_group_state,
			     dev_attr);
	mutex_lock(&group->fallback->lock);
	WRITE_ONCE(group->value, threshold);
	if (group->fallback->enabled)
		WRITE_ONCE(group->threshold.value, threshold);
	mutex_unlock(&group->fallback->lock);

	return count;
}

static int crypto_fallback_tune_group(const struct crypto_fallback_group *group,
				      int *threshold)
{
	u64 hardware_cycles;
	u64 software_cycles;
	unsigned int i;
	int err;

	for (i = 0; i < ARRAY_SIZE(crypto_fallback_block_sizes); i++) {
		u64 difference;
		u64 lower;

		err = fallback_bench(&group->benchmark,
				     CRYPTO_FALLBACK_HARDWARE,
				     crypto_fallback_block_sizes[i], 4, 8,
				     &hardware_cycles);
		if (err)
			return err;

		err = fallback_bench(&group->benchmark,
				     CRYPTO_FALLBACK_SOFTWARE,
				     crypto_fallback_block_sizes[i], 4, 8,
				     &software_cycles);
		if (err)
			return err;
		if (!hardware_cycles || !software_cycles)
			return -EOPNOTSUPP;

		difference = hardware_cycles > software_cycles ?
				     hardware_cycles - software_cycles :
				     software_cycles - hardware_cycles;
		lower = min(hardware_cycles, software_cycles);

		/* Extend results whose initial averages differ by at most 10%. */
		if (difference <= lower / 10) {
			u64 extra_cycles;

			err = fallback_bench(&group->benchmark,
					     CRYPTO_FALLBACK_HARDWARE,
					     crypto_fallback_block_sizes[i], 0,
					     16, &extra_cycles);
			if (err)
				return err;
			if (!extra_cycles)
				return -EOPNOTSUPP;
			hardware_cycles += extra_cycles;

			err = fallback_bench(&group->benchmark,
					     CRYPTO_FALLBACK_SOFTWARE,
					     crypto_fallback_block_sizes[i], 0,
					     16, &extra_cycles);
			if (err)
				return err;
			if (!extra_cycles)
				return -EOPNOTSUPP;
			software_cycles += extra_cycles;
		}

		/*
		 * Both totals contain the same number of samples, so comparing
		 * them is equivalent to comparing their arithmetic averages.
		 */
		if (hardware_cycles < software_cycles) {
			*threshold = i ? crypto_fallback_block_sizes[i - 1] : 0;
			return 0;
		}
	}

	*threshold = -1;

	return 0;
}

static int fallback_register_algs(struct crypto_fallback *fallback);
static void fallback_unregister_algs(struct crypto_fallback *fallback);

static ssize_t fallback_enabled_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct crypto_fallback *fallback;

	fallback = container_of(attr, struct crypto_fallback, enabled_attr);

	return sysfs_emit(buf, "%u\n", READ_ONCE(fallback->enabled));
}

static ssize_t fallback_enabled_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct crypto_fallback *fallback;
	bool enabled;
	unsigned int i;
	int err;

	err = kstrtobool(buf, &enabled);
	if (err)
		return err;

	fallback = container_of(attr, struct crypto_fallback, enabled_attr);
	mutex_lock(&fallback->lock);
	if (!enabled && !fallback->enabled)
		goto out_unlock;

	if (enabled) {
		for (i = 0; i < fallback->num_groups; i++) {
			struct crypto_fallback_group_state *state;
			int threshold;

			state = &fallback->groups[i];
			err = crypto_fallback_tune_group(&state->group,
							 &threshold);
			if (err) {
				dev_warn(fallback->dev,
					 "%s benchmark failed: %d; using hardware\n",
					 state->group.name, err);
				threshold = 0;
				err = 0;
			}
			WRITE_ONCE(state->value, threshold);
			dev_info(fallback->dev,
				 "%s CPU fallback threshold: %d\n",
				 state->group.name, threshold);
		}

		for (i = 0; i < fallback->num_groups; i++)
			WRITE_ONCE(fallback->groups[i].threshold.value,
				   fallback->groups[i].value);

		if (!fallback->enabled) {
			err = fallback_register_algs(fallback);
			if (err) {
				for (i = 0; i < fallback->num_groups; i++)
					WRITE_ONCE(fallback->groups[i].threshold.value,
						   0);
				goto out_unlock;
			}
		}
	} else {
		WRITE_ONCE(fallback->enabled, false);
		for (i = 0; i < fallback->num_groups; i++)
			WRITE_ONCE(fallback->groups[i].threshold.value, 0);
		fallback_unregister_algs(fallback);
	}
	WRITE_ONCE(fallback->enabled, enabled);

out_unlock:
	mutex_unlock(&fallback->lock);

	return err ?: count;
}

static void crypto_fallback_free(struct kref *ref)
{
	struct crypto_fallback *fallback;
	unsigned int i;

	fallback = container_of(ref, struct crypto_fallback, refcount);

	for (i = 0; i < fallback->num_groups; i++)
		kfree_const(fallback->groups[i].group.name);
	kfree(fallback->sysfs_attrs);
	kfree(fallback->groups);
	mutex_destroy(&fallback->lock);
	put_device(fallback->dev);
	kfree(fallback);
}

static struct crypto_fallback *
fallback_alloc(struct module *owner, struct device *dev,
	       const struct crypto_fallback_group *groups,
	       unsigned int num_groups)
{
	struct crypto_fallback *fallback;
	unsigned int i;
	int err;

	if (!dev || !groups || !num_groups)
		return ERR_PTR(-EINVAL);

	fallback = kzalloc_obj(*fallback, GFP_KERNEL);
	if (!fallback)
		return ERR_PTR(-ENOMEM);

	kref_init(&fallback->refcount);
	mutex_init(&fallback->lock);
	spin_lock_init(&fallback->alg_lock);
	INIT_LIST_HEAD(&fallback->algs);
	fallback->dev = get_device(dev);
	fallback->owner = owner;
	fallback->num_groups = num_groups;
	fallback->groups =
		kcalloc(num_groups, sizeof(*fallback->groups), GFP_KERNEL);
	fallback->sysfs_attrs =
		kcalloc(num_groups + 2, sizeof(*fallback->sysfs_attrs),
			GFP_KERNEL);
	if (!fallback->groups || !fallback->sysfs_attrs) {
		err = -ENOMEM;
		goto err_free;
	}

	for (i = 0; i < num_groups; i++) {
		struct crypto_fallback_group_state *state;

		if (!groups[i].name || !groups[i].name[0] ||
		    !strcmp(groups[i].name, "enabled") ||
		    !groups[i].benchmark.name ||
		    !groups[i].benchmark.driver_name ||
		    groups[i].benchmark.type > CRYPTO_FALLBACK_AEAD ||
		    (groups[i].benchmark.keylen && !groups[i].benchmark.key) ||
		    (groups[i].benchmark.authkeylen &&
		     !groups[i].benchmark.authkey) ||
		    (groups[i].benchmark.type == CRYPTO_FALLBACK_AEAD &&
		     !groups[i].benchmark.authsize)) {
			err = -EINVAL;
			goto err_free;
		}

		state = &fallback->groups[i];
		state->fallback = fallback;
		state->group = groups[i];
		state->group.name = kstrdup_const(groups[i].name, GFP_KERNEL);
		if (!state->group.name) {
			err = -ENOMEM;
			goto err_free;
		}

		sysfs_attr_init(&state->dev_attr.attr);
		state->dev_attr.attr.name = state->group.name;
		state->dev_attr.attr.mode = 0644;
		state->dev_attr.show = fallback_threshold_show;
		state->dev_attr.store = fallback_threshold_store;
		fallback->sysfs_attrs[i + 1] = &state->dev_attr.attr;
	}

	sysfs_attr_init(&fallback->enabled_attr.attr);
	fallback->enabled_attr.attr.name = "enabled";
	fallback->enabled_attr.attr.mode = 0644;
	fallback->enabled_attr.show = fallback_enabled_show;
	fallback->enabled_attr.store = fallback_enabled_store;
	fallback->sysfs_attrs[0] = &fallback->enabled_attr.attr;

	fallback->sysfs_group.name = "cpu_fallback_thresholds";
	fallback->sysfs_group.attrs = fallback->sysfs_attrs;
	err = sysfs_create_group(&dev->kobj, &fallback->sysfs_group);
	if (err)
		goto err_free;

	return fallback;

err_free:
	kref_put(&fallback->refcount, crypto_fallback_free);

	return ERR_PTR(err);
}

static bool fallback_use_software(const struct fallback_threshold *threshold,
				  unsigned int input_size)
{
	int value = READ_ONCE(threshold->value);

	return value < 0 || (value > 0 && input_size <= value);
}

struct fallback_skcipher_ctx {
	struct crypto_skcipher *hardware;
	struct crypto_skcipher *software;
};

struct fallback_aead_ctx {
	struct crypto_aead *hardware;
	struct crypto_aead *software;
};

struct fallback_ahash_ctx {
	struct crypto_ahash *hardware;
	struct crypto_ahash *software;
};

struct fallback_ahash_reqctx {
	bool use_software;
	struct ahash_request subreq;
};

struct fallback_ahash_state {
	bool use_software;
	u8 state[] CRYPTO_MINALIGN_ATTR;
};

/*
 * Allocate proxy algorithms per enable cycle because their destruction can be
 * deferred until transforms allocated before disable release them.
 */
struct fallback_alg {
	struct list_head list;
	struct crypto_fallback *fallback;
	struct kref refcount;
	const struct fallback_threshold *threshold;
	char driver_name[CRYPTO_MAX_ALG_NAME];
	enum crypto_fallback_alg_type type;
	bool registration_complete;
	bool registered;
	bool dead;
	union {
		struct skcipher_alg skcipher;
		struct aead_alg aead;
		struct ahash_alg ahash;
	} alg;
};

static void fallback_alg_release(struct kref *ref)
{
	struct fallback_alg *alg;
	struct crypto_fallback *fallback;

	alg = container_of(ref, struct fallback_alg, refcount);
	fallback = alg->fallback;
	kfree(alg);
	kref_put(&fallback->refcount, crypto_fallback_free);
}

static void fallback_alg_destroy(struct fallback_alg *alg)
{
	struct crypto_fallback *fallback = alg->fallback;
	unsigned long flags;
	bool release = false;

	spin_lock_irqsave(&fallback->alg_lock, flags);
	alg->dead = true;
	alg->registered = false;
	if (alg->registration_complete) {
		list_del_init(&alg->list);
		release = true;
	}
	spin_unlock_irqrestore(&fallback->alg_lock, flags);

	if (release)
		kref_put(&alg->refcount, fallback_alg_release);
}

static void fallback_skcipher_destroy(struct crypto_alg *base)
{
	struct skcipher_alg *alg;

	alg = container_of(base, struct skcipher_alg, base);
	fallback_alg_destroy(container_of(alg, struct fallback_alg,
					  alg.skcipher));
}

static void fallback_aead_destroy(struct crypto_alg *base)
{
	struct aead_alg *alg;

	alg = container_of(base, struct aead_alg, base);
	fallback_alg_destroy(container_of(alg, struct fallback_alg, alg.aead));
}

static void fallback_ahash_destroy(struct crypto_alg *base)
{
	struct hash_alg_common *halg;
	struct ahash_alg *alg;

	halg = container_of(base, struct hash_alg_common, base);
	alg = container_of(halg, struct ahash_alg, halg);
	fallback_alg_destroy(container_of(alg, struct fallback_alg, alg.ahash));
}

static int fallback_alg_complete_registration(struct fallback_alg *alg, int err)
{
	struct crypto_fallback *fallback = alg->fallback;
	unsigned long flags;
	bool release = false;

	spin_lock_irqsave(&fallback->alg_lock, flags);
	alg->registration_complete = true;
	if (err || alg->dead) {
		list_del_init(&alg->list);
		release = true;
		if (!err)
			err = -ENODEV;
	} else {
		alg->registered = true;
	}
	spin_unlock_irqrestore(&fallback->alg_lock, flags);

	if (release)
		kref_put(&alg->refcount, fallback_alg_release);

	return err;
}

static struct fallback_alg *fallback_skcipher_alg(struct crypto_skcipher *tfm)
{
	return container_of(crypto_skcipher_alg(tfm), struct fallback_alg,
			    alg.skcipher);
}

static struct fallback_alg *fallback_aead_alg(struct crypto_aead *tfm)
{
	return container_of(crypto_aead_alg(tfm), struct fallback_alg,
			    alg.aead);
}

static struct fallback_alg *fallback_ahash_alg(struct crypto_ahash *tfm)
{
	return container_of(crypto_ahash_alg(tfm), struct fallback_alg,
			    alg.ahash);
}

static int fallback_skcipher_setkey(struct crypto_skcipher *tfm, const u8 *key,
				    unsigned int keylen)
{
	struct fallback_skcipher_ctx *ctx = crypto_skcipher_ctx(tfm);
	u32 flags = crypto_skcipher_get_flags(tfm) & CRYPTO_TFM_REQ_MASK;
	int err;

	crypto_skcipher_clear_flags(ctx->software, CRYPTO_TFM_REQ_MASK);
	crypto_skcipher_set_flags(ctx->software, flags);
	err = crypto_skcipher_setkey(ctx->software, key, keylen);
	if (err)
		return err;

	crypto_skcipher_clear_flags(ctx->hardware, CRYPTO_TFM_REQ_MASK);
	crypto_skcipher_set_flags(ctx->hardware, flags);

	return crypto_skcipher_setkey(ctx->hardware, key, keylen);
}

static int fallback_skcipher_crypt(struct skcipher_request *req, bool encrypt)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct fallback_skcipher_ctx *ctx = crypto_skcipher_ctx(tfm);
	struct fallback_alg *alg = fallback_skcipher_alg(tfm);
	struct crypto_skcipher *child = ctx->hardware;
	struct skcipher_request *subreq = skcipher_request_ctx(req);

	if (fallback_use_software(alg->threshold, req->cryptlen))
		child = ctx->software;

	skcipher_request_set_tfm(subreq, child);
	skcipher_request_set_callback(subreq, req->base.flags,
				      req->base.complete, req->base.data);
	skcipher_request_set_crypt(subreq, req->src, req->dst, req->cryptlen,
				   req->iv);

	if (encrypt)
		return crypto_skcipher_encrypt(subreq);

	return crypto_skcipher_decrypt(subreq);
}

static int fallback_skcipher_encrypt(struct skcipher_request *req)
{
	return fallback_skcipher_crypt(req, true);
}

static int fallback_skcipher_decrypt(struct skcipher_request *req)
{
	return fallback_skcipher_crypt(req, false);
}

static int fallback_skcipher_init(struct crypto_skcipher *tfm)
{
	struct fallback_skcipher_ctx *ctx = crypto_skcipher_ctx(tfm);
	struct fallback_alg *alg = fallback_skcipher_alg(tfm);
	struct crypto_skcipher *hardware;
	struct crypto_skcipher *software;
	const char *name;
	u32 mask = CRYPTO_ALG_ASYNC | CRYPTO_ALG_NEED_FALLBACK;

	hardware = crypto_alloc_skcipher(alg->driver_name, 0, 0);
	if (IS_ERR(hardware))
		return PTR_ERR(hardware);

	name = crypto_tfm_alg_name(crypto_skcipher_tfm(tfm));
	software = crypto_alloc_skcipher(name, 0, mask);
	if (IS_ERR(software)) {
		crypto_free_skcipher(hardware);
		return PTR_ERR(software);
	}

	ctx->hardware = hardware;
	ctx->software = software;
	crypto_skcipher_set_reqsize(tfm,
				    sizeof(struct skcipher_request) +
					    max(crypto_skcipher_reqsize(hardware),
						crypto_skcipher_reqsize(software)));

	return 0;
}

static void fallback_skcipher_exit(struct crypto_skcipher *tfm)
{
	struct fallback_skcipher_ctx *ctx = crypto_skcipher_ctx(tfm);

	crypto_free_skcipher(ctx->software);
	crypto_free_skcipher(ctx->hardware);
}

static int fallback_aead_setkey(struct crypto_aead *tfm, const u8 *key,
				unsigned int keylen)
{
	struct fallback_aead_ctx *ctx = crypto_aead_ctx(tfm);
	u32 flags = crypto_aead_get_flags(tfm) & CRYPTO_TFM_REQ_MASK;
	int err;

	crypto_aead_clear_flags(ctx->software, CRYPTO_TFM_REQ_MASK);
	crypto_aead_set_flags(ctx->software, flags);
	err = crypto_aead_setkey(ctx->software, key, keylen);
	if (err)
		return err;

	crypto_aead_clear_flags(ctx->hardware, CRYPTO_TFM_REQ_MASK);
	crypto_aead_set_flags(ctx->hardware, flags);

	return crypto_aead_setkey(ctx->hardware, key, keylen);
}

static int fallback_aead_setauthsize(struct crypto_aead *tfm,
				     unsigned int authsize)
{
	struct fallback_aead_ctx *ctx = crypto_aead_ctx(tfm);
	int err;

	err = crypto_aead_setauthsize(ctx->software, authsize);
	if (err)
		return err;

	return crypto_aead_setauthsize(ctx->hardware, authsize);
}

static int fallback_aead_crypt(struct aead_request *req, bool encrypt)
{
	struct crypto_aead *tfm = crypto_aead_reqtfm(req);
	struct fallback_aead_ctx *ctx = crypto_aead_ctx(tfm);
	struct fallback_alg *alg = fallback_aead_alg(tfm);
	struct crypto_aead *child = ctx->hardware;
	struct aead_request *subreq = aead_request_ctx(req);
	unsigned int input_size = req->cryptlen;

	if (!encrypt) {
		unsigned int authsize = crypto_aead_authsize(tfm);

		if (input_size < authsize)
			return -EINVAL;
		input_size -= authsize;
	}

	if (fallback_use_software(alg->threshold, input_size))
		child = ctx->software;

	aead_request_set_tfm(subreq, child);
	aead_request_set_callback(subreq, req->base.flags, req->base.complete,
				  req->base.data);
	aead_request_set_crypt(subreq, req->src, req->dst, req->cryptlen,
			       req->iv);
	aead_request_set_ad(subreq, req->assoclen);

	if (encrypt)
		return crypto_aead_encrypt(subreq);

	return crypto_aead_decrypt(subreq);
}

static int fallback_aead_encrypt(struct aead_request *req)
{
	return fallback_aead_crypt(req, true);
}

static int fallback_aead_decrypt(struct aead_request *req)
{
	return fallback_aead_crypt(req, false);
}

static int fallback_aead_init(struct crypto_aead *tfm)
{
	struct fallback_aead_ctx *ctx = crypto_aead_ctx(tfm);
	struct fallback_alg *alg = fallback_aead_alg(tfm);
	struct crypto_aead *hardware;
	struct crypto_aead *software;

	hardware = crypto_alloc_aead(alg->driver_name, 0, 0);
	if (IS_ERR(hardware))
		return PTR_ERR(hardware);

	software =
		crypto_alloc_aead(crypto_tfm_alg_name(crypto_aead_tfm(tfm)), 0,
				  CRYPTO_ALG_ASYNC | CRYPTO_ALG_NEED_FALLBACK);
	if (IS_ERR(software)) {
		crypto_free_aead(hardware);
		return PTR_ERR(software);
	}

	ctx->hardware = hardware;
	ctx->software = software;
	crypto_aead_set_reqsize(tfm,
				sizeof(struct aead_request) +
					max(crypto_aead_reqsize(hardware),
					    crypto_aead_reqsize(software)));

	return 0;
}

static void fallback_aead_exit(struct crypto_aead *tfm)
{
	struct fallback_aead_ctx *ctx = crypto_aead_ctx(tfm);

	crypto_free_aead(ctx->software);
	crypto_free_aead(ctx->hardware);
}

static struct ahash_request *fallback_ahash_subreq(struct ahash_request *req,
						   bool reset)
{
	struct fallback_ahash_reqctx *rctx = ahash_request_ctx(req);
	struct fallback_ahash_ctx *ctx =
		crypto_ahash_ctx(crypto_ahash_reqtfm(req));
	struct crypto_ahash *child = rctx->use_software ? ctx->software :
							  ctx->hardware;
	struct ahash_request *subreq = &rctx->subreq;

	if (reset)
		memset(subreq, 0, sizeof(*subreq));
	ahash_request_set_tfm(subreq, child);
	ahash_request_set_callback(subreq, req->base.flags, req->base.complete,
				   req->base.data);
	if (ahash_request_isvirt(req))
		ahash_request_set_virt(subreq, req->svirt, req->result,
				       req->nbytes);
	else
		ahash_request_set_crypt(subreq, req->src, req->result,
					req->nbytes);

	return subreq;
}

static int fallback_ahash_setkey(struct crypto_ahash *tfm, const u8 *key,
				 unsigned int keylen)
{
	struct fallback_ahash_ctx *ctx = crypto_ahash_ctx(tfm);
	u32 flags = crypto_ahash_get_flags(tfm) & CRYPTO_TFM_REQ_MASK;
	int err;

	crypto_ahash_clear_flags(ctx->software, CRYPTO_TFM_REQ_MASK);
	crypto_ahash_set_flags(ctx->software, flags);
	err = crypto_ahash_setkey(ctx->software, key, keylen);
	if (err)
		return err;

	crypto_ahash_clear_flags(ctx->hardware, CRYPTO_TFM_REQ_MASK);
	crypto_ahash_set_flags(ctx->hardware, flags);

	return crypto_ahash_setkey(ctx->hardware, key, keylen);
}

static int fallback_ahash_init_req(struct ahash_request *req)
{
	struct fallback_ahash_reqctx *rctx = ahash_request_ctx(req);
	struct fallback_alg *alg = fallback_ahash_alg(crypto_ahash_reqtfm(req));

	rctx->use_software = fallback_use_software(alg->threshold, UINT_MAX);

	return crypto_ahash_init(fallback_ahash_subreq(req, true));
}

static int fallback_ahash_update(struct ahash_request *req)
{
	return crypto_ahash_update(fallback_ahash_subreq(req, false));
}

static int fallback_ahash_final(struct ahash_request *req)
{
	return crypto_ahash_final(fallback_ahash_subreq(req, false));
}

static int fallback_ahash_finup(struct ahash_request *req)
{
	return crypto_ahash_finup(fallback_ahash_subreq(req, false));
}

static int fallback_ahash_digest(struct ahash_request *req)
{
	struct fallback_ahash_reqctx *rctx = ahash_request_ctx(req);
	struct fallback_alg *alg = fallback_ahash_alg(crypto_ahash_reqtfm(req));

	rctx->use_software = fallback_use_software(alg->threshold, req->nbytes);

	return crypto_ahash_digest(fallback_ahash_subreq(req, true));
}

static int fallback_ahash_export(struct ahash_request *req, void *out)
{
	struct fallback_ahash_reqctx *rctx = ahash_request_ctx(req);
	struct fallback_ahash_state *state = out;

	state->use_software = rctx->use_software;

	return crypto_ahash_export(fallback_ahash_subreq(req, false),
				   state->state);
}

static int fallback_ahash_import(struct ahash_request *req, const void *in)
{
	const struct fallback_ahash_state *state = in;
	struct fallback_ahash_reqctx *rctx = ahash_request_ctx(req);

	rctx->use_software = state->use_software;

	return crypto_ahash_import(fallback_ahash_subreq(req, true),
				   state->state);
}

static int fallback_ahash_init(struct crypto_ahash *tfm)
{
	struct fallback_ahash_ctx *ctx = crypto_ahash_ctx(tfm);
	struct fallback_alg *alg = fallback_ahash_alg(tfm);
	struct crypto_ahash *hardware;
	struct crypto_ahash *software;

	hardware = crypto_alloc_ahash(alg->driver_name, 0, 0);
	if (IS_ERR(hardware))
		return PTR_ERR(hardware);

	software = crypto_alloc_ahash(crypto_ahash_alg_name(tfm),
				      CRYPTO_ALG_REQ_VIRT,
				      CRYPTO_ALG_ASYNC | CRYPTO_ALG_REQ_VIRT |
					      CRYPTO_ALG_NEED_FALLBACK);
	if (IS_ERR(software)) {
		crypto_free_ahash(hardware);
		return PTR_ERR(software);
	}

	ctx->hardware = hardware;
	ctx->software = software;
	crypto_ahash_set_reqsize(tfm,
				 sizeof(struct fallback_ahash_reqctx) +
					 max(crypto_ahash_reqsize(hardware),
					     crypto_ahash_reqsize(software)));
	crypto_ahash_set_statesize(tfm,
				   offsetof(struct fallback_ahash_state, state) +
					   max(crypto_ahash_statesize(hardware),
					       crypto_ahash_statesize(software)));

	return 0;
}

static void fallback_ahash_exit(struct crypto_ahash *tfm)
{
	struct fallback_ahash_ctx *ctx = crypto_ahash_ctx(tfm);

	crypto_free_ahash(ctx->software);
	crypto_free_ahash(ctx->hardware);
}

static int fallback_alg_name(struct crypto_alg *alg, const char *name,
			     const char *driver_name)
{
	if (strscpy(alg->cra_name, name, CRYPTO_MAX_ALG_NAME) < 0)
		return -ENAMETOOLONG;

	if (snprintf(alg->cra_driver_name, CRYPTO_MAX_ALG_NAME, "fallback(%s)",
		     driver_name) >= CRYPTO_MAX_ALG_NAME)
		return -ENAMETOOLONG;

	return 0;
}

static void fallback_init_base(struct crypto_alg *alg,
			       const struct crypto_alg *hardware,
			       const struct crypto_alg *software,
			       struct module *owner)
{
	alg->cra_flags =
		CRYPTO_ALG_ASYNC | CRYPTO_ALG_NO_FALLBACK |
		((hardware->cra_flags | software->cra_flags) &
		 (CRYPTO_ALG_ALLOCATES_MEMORY | CRYPTO_ALG_KERN_DRIVER_ONLY));
	alg->cra_priority = hardware->cra_priority + 1;
	alg->cra_blocksize = hardware->cra_blocksize;
	alg->cra_alignmask =
		max(hardware->cra_alignmask, software->cra_alignmask);
	alg->cra_module = owner;
}

static int fallback_register_skcipher(struct fallback_alg *entry,
				      struct module *owner)
{
	struct crypto_skcipher *hardware;
	struct crypto_skcipher *software;
	struct skcipher_alg *alg = &entry->alg.skcipher;
	struct skcipher_alg_common *hcommon;
	struct skcipher_alg_common *scommon;
	int err;

	hardware = crypto_alloc_skcipher(entry->driver_name, 0, 0);
	if (IS_ERR(hardware))
		return PTR_ERR(hardware);

	hcommon = crypto_skcipher_alg_common(hardware);
	software = crypto_alloc_skcipher(hcommon->base.cra_name, 0,
					 CRYPTO_ALG_ASYNC |
						 CRYPTO_ALG_NEED_FALLBACK);
	if (IS_ERR(software)) {
		err = PTR_ERR(software);
		goto out_free_hardware;
	}
	scommon = crypto_skcipher_alg_common(software);

	if (hcommon->base.cra_blocksize != scommon->base.cra_blocksize ||
	    hcommon->ivsize != scommon->ivsize) {
		err = -EINVAL;
		goto out_free_software;
	}

	err = fallback_alg_name(&alg->base, hcommon->base.cra_name,
				entry->driver_name);
	if (err)
		goto out_free_software;
	fallback_init_base(&alg->base, &hcommon->base, &scommon->base, owner);
	alg->base.cra_destroy = fallback_skcipher_destroy;
	alg->min_keysize = max(hcommon->min_keysize, scommon->min_keysize);
	alg->max_keysize = min(hcommon->max_keysize, scommon->max_keysize);
	if (alg->min_keysize > alg->max_keysize) {
		err = -EINVAL;
		goto out_free_software;
	}
	alg->ivsize = hcommon->ivsize;
	alg->chunksize = max(hcommon->chunksize, scommon->chunksize);
	alg->walksize = alg->chunksize;
	alg->base.cra_ctxsize = sizeof(struct fallback_skcipher_ctx);
	alg->init = fallback_skcipher_init;
	alg->exit = fallback_skcipher_exit;
	alg->setkey = fallback_skcipher_setkey;
	alg->encrypt = fallback_skcipher_encrypt;
	alg->decrypt = fallback_skcipher_decrypt;

	err = crypto_register_skcipher(alg);

out_free_software:
	crypto_free_skcipher(software);
out_free_hardware:
	crypto_free_skcipher(hardware);

	return err;
}

static int fallback_register_aead(struct fallback_alg *entry,
				  struct module *owner)
{
	struct crypto_aead *hardware;
	struct crypto_aead *software;
	struct aead_alg *alg = &entry->alg.aead;
	struct aead_alg *halg;
	struct aead_alg *salg;
	int err;

	hardware = crypto_alloc_aead(entry->driver_name, 0, 0);
	if (IS_ERR(hardware))
		return PTR_ERR(hardware);
	halg = crypto_aead_alg(hardware);

	software =
		crypto_alloc_aead(halg->base.cra_name, 0,
				  CRYPTO_ALG_ASYNC | CRYPTO_ALG_NEED_FALLBACK);
	if (IS_ERR(software)) {
		err = PTR_ERR(software);
		goto out_free_hardware;
	}
	salg = crypto_aead_alg(software);

	if (halg->base.cra_blocksize != salg->base.cra_blocksize ||
	    halg->ivsize != salg->ivsize) {
		err = -EINVAL;
		goto out_free_software;
	}

	err = fallback_alg_name(&alg->base, halg->base.cra_name,
				entry->driver_name);
	if (err)
		goto out_free_software;
	fallback_init_base(&alg->base, &halg->base, &salg->base, owner);
	alg->base.cra_destroy = fallback_aead_destroy;
	alg->ivsize = halg->ivsize;
	alg->maxauthsize = min(halg->maxauthsize, salg->maxauthsize);
	alg->chunksize = max(halg->chunksize, salg->chunksize);
	alg->base.cra_ctxsize = sizeof(struct fallback_aead_ctx);
	alg->init = fallback_aead_init;
	alg->exit = fallback_aead_exit;
	alg->setkey = fallback_aead_setkey;
	alg->setauthsize = fallback_aead_setauthsize;
	alg->encrypt = fallback_aead_encrypt;
	alg->decrypt = fallback_aead_decrypt;

	err = crypto_register_aead(alg);

out_free_software:
	crypto_free_aead(software);
out_free_hardware:
	crypto_free_aead(hardware);

	return err;
}

static int fallback_register_ahash(struct fallback_alg *entry,
				   struct module *owner)
{
	struct crypto_ahash *hardware;
	struct crypto_ahash *software;
	struct ahash_alg *alg = &entry->alg.ahash;
	struct hash_alg_common *hcommon;
	struct hash_alg_common *scommon;
	int err;

	hardware = crypto_alloc_ahash(entry->driver_name, 0, 0);
	if (IS_ERR(hardware))
		return PTR_ERR(hardware);
	hcommon = crypto_hash_alg_common(hardware);

	software = crypto_alloc_ahash(hcommon->base.cra_name,
				      CRYPTO_ALG_REQ_VIRT,
				      CRYPTO_ALG_ASYNC | CRYPTO_ALG_REQ_VIRT |
					      CRYPTO_ALG_NEED_FALLBACK);
	if (IS_ERR(software)) {
		err = PTR_ERR(software);
		goto out_free_hardware;
	}
	scommon = crypto_hash_alg_common(software);

	if (hcommon->base.cra_blocksize != scommon->base.cra_blocksize ||
	    hcommon->digestsize != scommon->digestsize) {
		err = -EINVAL;
		goto out_free_software;
	}

	err = fallback_alg_name(&alg->halg.base, hcommon->base.cra_name,
				entry->driver_name);
	if (err)
		goto out_free_software;
	fallback_init_base(&alg->halg.base, &hcommon->base, &scommon->base,
			   owner);
	alg->halg.base.cra_destroy = fallback_ahash_destroy;
	alg->halg.base.cra_flags |= CRYPTO_ALG_REQ_VIRT;
	alg->halg.base.cra_flags |= hcommon->base.cra_flags &
				    CRYPTO_ALG_OPTIONAL_KEY;
	alg->halg.digestsize = hcommon->digestsize;
	alg->halg.statesize = offsetof(struct fallback_ahash_state, state) +
			      max(crypto_ahash_statesize(hardware),
				  crypto_ahash_statesize(software));
	alg->halg.base.cra_ctxsize = sizeof(struct fallback_ahash_ctx);
	alg->init_tfm = fallback_ahash_init;
	alg->exit_tfm = fallback_ahash_exit;
	alg->init = fallback_ahash_init_req;
	alg->update = fallback_ahash_update;
	alg->final = fallback_ahash_final;
	alg->finup = fallback_ahash_finup;
	alg->digest = fallback_ahash_digest;
	alg->export = fallback_ahash_export;
	alg->import = fallback_ahash_import;
	if (crypto_hash_alg_has_setkey(hcommon))
		alg->setkey = fallback_ahash_setkey;

	err = crypto_register_ahash(alg);

out_free_software:
	crypto_free_ahash(software);
out_free_hardware:
	crypto_free_ahash(hardware);

	return err;
}

static int fallback_register_alg(struct fallback_alg *alg, struct module *owner)
{
	switch (alg->type) {
	case CRYPTO_FALLBACK_SKCIPHER:
		return fallback_register_skcipher(alg, owner);
	case CRYPTO_FALLBACK_AHASH:
		return fallback_register_ahash(alg, owner);
	case CRYPTO_FALLBACK_AEAD:
		return fallback_register_aead(alg, owner);
	}

	return -EINVAL;
}

static void fallback_unregister_alg(struct fallback_alg *alg)
{
	switch (alg->type) {
	case CRYPTO_FALLBACK_SKCIPHER:
		crypto_unregister_skcipher(&alg->alg.skcipher);
		break;
	case CRYPTO_FALLBACK_AHASH:
		crypto_unregister_ahash(&alg->alg.ahash);
		break;
	case CRYPTO_FALLBACK_AEAD:
		crypto_unregister_aead(&alg->alg.aead);
		break;
	}
}

static void fallback_unregister_algs(struct crypto_fallback *fallback)
{
	struct fallback_alg *alg;
	struct fallback_alg *candidate;
	unsigned long flags;

	for (;;) {
		alg = NULL;
		spin_lock_irqsave(&fallback->alg_lock, flags);
		list_for_each_entry(candidate, &fallback->algs, list) {
			if (!candidate->registered)
				continue;

			alg = candidate;
			alg->registered = false;
			kref_get(&alg->refcount);
			break;
		}
		spin_unlock_irqrestore(&fallback->alg_lock, flags);

		if (!alg)
			break;

		fallback_unregister_alg(alg);
		kref_put(&alg->refcount, fallback_alg_release);
	}
}

static int fallback_register_algs(struct crypto_fallback *fallback)
{
	unsigned int i, j;
	int err;

	for (i = 0; i < fallback->num_groups; i++) {
		const struct crypto_fallback_group *group;

		group = &fallback->groups[i].group;
		for (j = 0; j < group->num_algs; j++) {
			struct fallback_alg *alg;
			unsigned long flags;

			alg = kzalloc_obj(*alg, GFP_KERNEL);
			if (!alg) {
				err = -ENOMEM;
				goto err_unregister_algs;
			}

			INIT_LIST_HEAD(&alg->list);
			kref_init(&alg->refcount);
			alg->fallback = fallback;
			alg->threshold = &fallback->groups[i].threshold;
			alg->type = group->benchmark.type;
			if (strscpy(alg->driver_name, group->algs[j],
				    sizeof(alg->driver_name)) < 0) {
				kfree(alg);
				err = -ENAMETOOLONG;
				goto err_unregister_algs;
			}

			kref_get(&fallback->refcount);
			spin_lock_irqsave(&fallback->alg_lock, flags);
			list_add_tail(&alg->list, &fallback->algs);
			spin_unlock_irqrestore(&fallback->alg_lock, flags);

			err = fallback_register_alg(alg, fallback->owner);
			err = fallback_alg_complete_registration(alg, err);
			if (err == -ENOENT)
				continue;
			if (err)
				goto err_unregister_algs;
		}
	}

	return 0;

err_unregister_algs:
	fallback_unregister_algs(fallback);

	return err;
}

struct crypto_fallback *
crypto_fallback_register(struct module *owner, struct device *dev,
			 const struct crypto_fallback_group *groups,
			 unsigned int num_groups)
{
	unsigned int i, j;

	if (!owner || !dev || !groups || !num_groups)
		return ERR_PTR(-EINVAL);

	for (i = 0; i < num_groups; i++) {
		if (!groups[i].algs || !groups[i].num_algs)
			return ERR_PTR(-EINVAL);
		for (j = 0; j < groups[i].num_algs; j++)
			if (!groups[i].algs[j])
				return ERR_PTR(-EINVAL);
	}

	return fallback_alloc(owner, dev, groups, num_groups);
}
EXPORT_SYMBOL_GPL(crypto_fallback_register);

void crypto_fallback_unregister(struct crypto_fallback *fallback)
{
	unsigned int i;

	if (!fallback || IS_ERR(fallback))
		return;

	sysfs_remove_group(&fallback->dev->kobj, &fallback->sysfs_group);
	mutex_lock(&fallback->lock);
	WRITE_ONCE(fallback->enabled, false);
	for (i = 0; i < fallback->num_groups; i++)
		WRITE_ONCE(fallback->groups[i].threshold.value, 0);
	fallback_unregister_algs(fallback);
	mutex_unlock(&fallback->lock);
	kref_put(&fallback->refcount, crypto_fallback_free);
}
EXPORT_SYMBOL_GPL(crypto_fallback_unregister);

MODULE_AUTHOR("Jihong Min <hurryman2212@gmail.com>");
MODULE_DESCRIPTION("Crypto API dynamic fallback support");
MODULE_LICENSE("GPL");
