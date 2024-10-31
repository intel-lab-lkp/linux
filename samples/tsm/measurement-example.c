// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2024 Intel Corporation. All rights reserved. */

#include <linux/module.h>
#include <linux/tsm.h>
#include <crypto/hash_info.h>
#include <crypto/hash.h>

struct __aligned(16)
{
	u8 static_mr[SHA384_DIGEST_SIZE];
	u8 config_mr[SHA512_DIGEST_SIZE];
	u8 rtmr0[SHA256_DIGEST_SIZE];
	u8 rtmr1[SHA384_DIGEST_SIZE];
	u8 user_data[SHA512_DIGEST_SIZE];
	u8 report_digest[SHA512_DIGEST_SIZE];
}
example_report = {
	.static_mr = "static_mr",
	.config_mr = "config_mr",
	.rtmr0 = "rtmr0",
	.rtmr1 = "rtmr1",
	.user_data = "user_data",
};

DEFINE_FREE(shash, struct crypto_shash *,
	    if (!IS_ERR(_T)) crypto_free_shash(_T));

static int _refresh_report(struct tsm_measurement *tmr,
			   const struct tsm_measurement_register *mr)
{
	pr_debug(KBUILD_MODNAME ": %s(%s,%s)\n", __func__, tmr->name,
		 mr->mr_name);
	struct crypto_shash *tfm __free(shash) =
		crypto_alloc_shash(hash_algo_name[HASH_ALGO_SHA512], 0, 0);
	if (IS_ERR(tfm))
		return PTR_ERR(tfm);
	return crypto_shash_tfm_digest(tfm, (u8 *)&example_report,
				       offsetof(typeof(example_report),
						report_digest),
				       example_report.report_digest);
}

static int _extend_mr(struct tsm_measurement *tmr,
		      const struct tsm_measurement_register *mr, const u8 *data)
{
	SHASH_DESC_ON_STACK(desc, 0);
	int rc;

	pr_debug(KBUILD_MODNAME ": %s(%s,%s)\n", __func__, tmr->name,
		 mr->mr_name);

	desc->tfm = crypto_alloc_shash(hash_algo_name[mr->mr_hash], 0, 0);
	if (IS_ERR(desc->tfm))
		return PTR_ERR(desc->tfm);

	BUG_ON(crypto_shash_digestsize(desc->tfm) != mr->mr_size);

	rc = crypto_shash_init(desc);
	if (!rc)
		rc = crypto_shash_update(desc, mr->mr_value, mr->mr_size);
	if (!rc)
		rc = crypto_shash_finup(desc, data, mr->mr_size, mr->mr_value);

	crypto_free_shash(desc->tfm);
	return rc;
}

#define MR_(mr, hash) .mr_value = &example_report.mr, TSM_MR_(mr, hash)
static const struct tsm_measurement_register example_mrs[] = {
	/* the entire report can be considered as a LIVE MR */
	{ "full_report", &example_report, sizeof(example_report),
	  TSM_MR_F_LIVE | TSM_MR_F_F },
	/* static MR, read-only */
	{ MR_(static_mr, SHA384) },
	/* config MR, read-only */
	{ MR_(config_mr, SHA512) },
	/* RTMR, direct extension prohibited */
	{ MR_(rtmr0, SHA256) | TSM_MR_F_RTMR },
	/* RTMR, direct extension allowed */
	{ MR_(rtmr1, SHA384) | TSM_MR_F_RTMR | TSM_MR_F_W },
	/* most CC archs allow including user data in attestation */
	{ MR_(user_data, SHA512) | TSM_MR_F_W },
	/* LIVE MR example, usually doesn't exist in real CC arch */
	{ MR_(report_digest, SHA512) | TSM_MR_F_L },
	/* terminating NULL entry */
	{}
};
#undef MR_

static struct tsm_measurement example_measurement_provider = {
	.name = "measurement-example",
	.mrs = example_mrs,
	.refresh = _refresh_report,
	.extend = _extend_mr,
};

static int __init measurement_example_init(void)
{
	int rc = tsm_register_measurement(&example_measurement_provider);
	pr_debug(KBUILD_MODNAME ": tsm_register_measurement(%p)=%d\n",
		 &example_measurement_provider, rc);
	return rc;
}

static void __exit measurement_example_exit(void)
{
	int rc = tsm_unregister_measurement(&example_measurement_provider);
	pr_debug(KBUILD_MODNAME ": tsm_unregister_measurement(%p)=%d\n",
		 &example_measurement_provider, rc);
}

module_init(measurement_example_init);
module_exit(measurement_example_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Sample tsm_measurement implementation");
