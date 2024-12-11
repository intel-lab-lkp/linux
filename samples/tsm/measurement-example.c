// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2024 Intel Corporation. All rights reserved. */

#define pr_fmt(x) KBUILD_MODNAME ": " x

#include <linux/module.h>
#include <linux/tsm.h>
#include <crypto/hash_info.h>
#include <crypto/hash.h>

struct {
	u8 static_mr[SHA384_DIGEST_SIZE];
	u8 config_mr[SHA512_DIGEST_SIZE];
	u8 rtmr0[SHA256_DIGEST_SIZE];
	u8 rtmr1[SHA384_DIGEST_SIZE];
	u8 user_data[SHA512_DIGEST_SIZE];
	u8 report_digest[SHA512_DIGEST_SIZE];
} example_report = {
	.static_mr = "static_mr",
	.config_mr = "config_mr",
	.rtmr0 = "rtmr0",
	.rtmr1 = "rtmr1",
	.user_data = "user_data",
};

DEFINE_FREE(shash, struct crypto_shash *, if (!IS_ERR(_T)) crypto_free_shash(_T));

static int example_report_refresh(struct tsm_measurement *tmr,
				  const struct tsm_measurement_register *mr)
{
	pr_debug("%s(%s,%s)\n", __func__, tmr->name, mr->mr_name);
	struct crypto_shash *tfm __free(shash) =
		crypto_alloc_shash(hash_algo_name[HASH_ALGO_SHA512], 0, 0);
	if (IS_ERR(tfm))
		return PTR_ERR(tfm);
	return crypto_shash_tfm_digest(tfm, (u8 *)&example_report,
				       offsetof(typeof(example_report), report_digest),
				       example_report.report_digest);
}

static int example_report_extend_mr(struct tsm_measurement *tmr,
				    const struct tsm_measurement_register *mr, const u8 *data)
{
	SHASH_DESC_ON_STACK(desc, 0);
	int rc;

	pr_debug("%s(%s,%s)\n", __func__, tmr->name, mr->mr_name);

	desc->tfm = crypto_alloc_shash(hash_algo_name[mr->mr_hash], 0, 0);
	if (IS_ERR(desc->tfm))
		return PTR_ERR(desc->tfm);

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
	/* static MR, read-only */
	{ MR_(static_mr, SHA384) },
	/* config MR, read-only */
	{ MR_(config_mr, SHA512) },
	/* RTMR, direct extension prohibited */
	{ MR_(rtmr0, SHA256) | TSM_MR_F_RTMR },
	/* RTMR, direct extension allowed */
	{ MR_(rtmr1, SHA384) | TSM_MR_F_RTMR | TSM_MR_F_W },
	/* RTMR, crypto agile, alaised to rtmr0 and rtmr1, respectively */
	{ .mr_value = &example_report.rtmr0,
	  TSM_MR_(rtmr_crypto_agile, SHA256) | TSM_MR_F_RTMR | TSM_MR_F_W },
	{ .mr_value = &example_report.rtmr1,
	  TSM_MR_(rtmr_crypto_agile, SHA384) | TSM_MR_F_RTMR | TSM_MR_F_W },
	/* most CC archs allow including user data in attestation */
	{ MR_(report_digest, SHA512) | TSM_MR_F_L },
	/* terminating NULL entry */
	{}
};
#undef MR_

static struct tsm_measurement example_measurement_provider = {
	.name = "measurement-example",
	.mrs = example_mrs,
	.refresh = example_report_refresh,
	.extend = example_report_extend_mr,
};

static int __init measurement_example_init(void)
{
	int rc;

	rc = tsm_register_measurement(&example_measurement_provider);
	pr_debug("tsm_register_measurement(%p)=%d\n", &example_measurement_provider, rc);
	return rc;
}

static void __exit measurement_example_exit(void)
{
	int rc;

	rc = tsm_unregister_measurement(&example_measurement_provider);
	pr_debug("tsm_unregister_measurement(%p)=%d\n", &example_measurement_provider, rc);
}

module_init(measurement_example_init);
module_exit(measurement_example_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Sample tsm_measurement implementation");
