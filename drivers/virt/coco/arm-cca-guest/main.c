// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023 - 2025 ARM Ltd.
 */

#include <linux/arm-rsi-cmds.h>
#include <linux/arm-smccc-bus.h>
#include <linux/arm-smccc-rsi.h>
#include <linux/cc_platform.h>
#include <linux/kernel.h>
#include <linux/device-id/platform.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/smp.h>
#include <linux/tsm.h>
#include <linux/tsm-mr.h>
#include <linux/types.h>

#include <crypto/hash.h>

/* MR buffer */
static u8 *arm_cca_mr_buf;

/**
 * arm_cca_mrs - ARM CCA measurement register set.
 *
 * Defines a static array of measurement registers used by the ARM
 * Confidential Compute Architecture (CCA). These registers are used
 * for attestation and runtime integrity tracking.
 *
 * Register types:
 *   - rim: Realm initial measurement register (RIM)
 *   - rem0–rem3: Runtime extensible measurement registers (REMs)
 */
static struct tsm_measurement_register arm_cca_mrs[] = {
	{ TSM_MR_(rim, SHA256)  | TSM_MR_F_READABLE },
	{ TSM_MR_(rem0, SHA256) | TSM_MR_F_RTMR },
	{ TSM_MR_(rem1, SHA256) | TSM_MR_F_RTMR },
	{ TSM_MR_(rem2, SHA256) | TSM_MR_F_RTMR },
	{ TSM_MR_(rem3, SHA256) | TSM_MR_F_RTMR }
};

/**
 * arm_cca_mr_refresh - Refresh measurement registers for ARM CCA.
 *
 * @tm: Pointer to a struct tsm_measurements containing measurement registers.
 *
 * Iterates through all measurement registers in @tm and refreshes those
 * marked with TSM_MR_F_LIVE or TSM_MR_F_READABLE by invoking
 * rsi_measurement_read() for each.
 *
 * Return: 0 on success, or -EINVAL if @tm is NULL or a read operation fails.
 */
static int arm_cca_mr_refresh(const struct tsm_measurements *tm)
{
	int retval;
	int index = 0;
	const struct tsm_measurement_register *mr;

	if (!tm)
		return -EINVAL;

	while (index < tm->nr_mrs) {
		mr = &tm->mrs[index];

		/* Skip if the MR is not Live or Readable. */
		if ((mr->mr_flags & (TSM_MR_F_LIVE | TSM_MR_F_READABLE)) != 0) {
			retval = rsi_measurement_read(index,
						      mr->mr_value,
						      mr->mr_size);
			if (retval != 0)
				return -EINVAL;
		}

		index++;
	}

	return 0;
}

/**
 * arm_cca_mr_extend - Extend a measurement register with new data.
 *
 * @tm:   Pointer to the tsm_measurements structure containing measurement
 *        registers.
 * @mr:   Pointer to the specific measurement register to extend.
 * @data: Pointer to the data to be used for extension.
 *
 * This function extends a measurement register with new input data.
 *
 * Return: 0 on success, or a negative error code (e.g., -EINVAL for invalid
 * arguments).
 */
static int arm_cca_mr_extend(const struct tsm_measurements *tm,
			     const struct tsm_measurement_register *mr,
			     const u8 *data)
{
	if (!tm || !mr || !data)
		return -EINVAL;

	return rsi_measurement_extend((mr - tm->mrs), data, mr->mr_size);
}

/**
 * arm_cca_measurements - ARM CCA measurement configuration instance.
 *
 * This defines the measurement set and behavior for the ARM
 * Confidential Compute Architecture, enabling measurements
 * for attestation and runtime validation.
 */
static struct tsm_measurements arm_cca_measurements = {
	.mrs = arm_cca_mrs,
	.nr_mrs = ARRAY_SIZE(arm_cca_mrs),
	.refresh = arm_cca_mr_refresh,
	.write = arm_cca_mr_extend,
};

/**
 * arm_cca_attr_groups - Attribute groups for the arm_cca_misc_dev miscellaneous
 * device.
 *
 */
static const struct attribute_group *arm_cca_attr_groups[] = {
	NULL, /* measurements */
	NULL
};

/**
 * arm_cca_misc_dev - Miscellaneous device for ARM CCA functionality.
 *
 */
static struct miscdevice arm_cca_misc_dev = {
	.name = KBUILD_MODNAME,
	.minor = MISC_DYNAMIC_MINOR,
	.groups = arm_cca_attr_groups,
};

/**
 * arm_cca_get_hash_algorithm - Get the hash algorithm and digest size for
 * a Realm.
 *
 * @hash_algo:   Pointer to an int to receive the internal hash algorithm ID
 *               (e.g., HASH_ALGO_SHA256 or HASH_ALGO_SHA512).
 * @digest_size: Pointer to an int to receive the digest size in bytes
 *               (e.g., SHA256_DIGEST_SIZE or SHA512_DIGEST_SIZE).
 *
 * This function retrieves the hash algorithm used in a Realm's configuration
 * by invoking the `rsi_get_realm_config()` interface.
 *
 * Return:
 * * %0        - Success. The hash algorithm and digest size are returned.
 * * %-ENOMEM  - Memory allocation failed.
 * * %-EINVAL  - Configuration fetch failed or algorithm is unsupported.
 *
 */
static int arm_cca_get_hash_algorithm(int *hash_algo, int *digest_size)
{
	int ret = 0;
	unsigned long result;
	struct realm_config *cfg = NULL;

	cfg = alloc_pages_exact(sizeof(*cfg), GFP_KERNEL);
	if (!cfg)
		return -ENOMEM;

	result = rsi_get_realm_config(cfg);
	if (result != RSI_SUCCESS) {
		ret = -EINVAL;
		goto exit_free_realm_config;
	}

	switch (cfg->hash_algo) {
	case RSI_HASH_SHA_512:
		*hash_algo = HASH_ALGO_SHA512;
		*digest_size = SHA512_DIGEST_SIZE;
		break;
	case RSI_HASH_SHA_256:
		*hash_algo = HASH_ALGO_SHA256;
		*digest_size = SHA256_DIGEST_SIZE;
		break;
	default:
		/* Unknown/unsupported algorithm. */
		ret = -EINVAL;
		break;
	}

exit_free_realm_config:
	free_pages_exact(cfg, RSI_GRANULE_SIZE);
	return ret;
}

/**
 * arm_cca_mr_init - Initialize ARM CCA measurement register infrastructure.
 *
 * This function sets up the internal data structures for handling ARM CCA
 * measurement registers (MRs) and creates a sysfs attribute group. It also
 * registers a miscelaneous device for exposing the Arm CCA measurement
 * registers to userspace.
 *
 * Return:
 * * %0       - On success.
 * * %-ENOMEM - if memory allocation fails.
 * * %-EINVAL - On hash algorithm retrieval or attribute group creation
 *   failure.
 */
static int arm_cca_mr_init(void)
{
	const struct attribute_group *g;
	int ret;
	int hash_algo;
	int digest_size;
	int digest_buf_size;

	/* Retrieve the hash algorithm and digest size. */
	ret = arm_cca_get_hash_algorithm(&hash_algo, &digest_size);
	if (ret)
		return ret;

	/*
	 * Allocate a single contiguous buffer to hold the digest values
	 * for all MRs.
	 */
	digest_buf_size = ARRAY_SIZE(arm_cca_mrs) * digest_size;
	u8 *digest_buf __free(kfree) = kzalloc(digest_buf_size, GFP_KERNEL);
	if (!digest_buf)
		return -ENOMEM;

	arm_cca_mr_buf = digest_buf;

	/* Initialise the mr_value storage and the mr_size. */
	for (size_t i = 0; i < ARRAY_SIZE(arm_cca_mrs); ++i) {
		arm_cca_mrs[i].mr_value = digest_buf + (digest_size * i);
		arm_cca_mrs[i].mr_size = digest_size;
		arm_cca_mrs[i].mr_hash = hash_algo;
	}

	/* Read the measurement registers. */
	ret = arm_cca_mr_refresh(&arm_cca_measurements);
	if (ret)
		return ret;

	/*
	 * Create a sysfs attribute group to expose the measurements
	 * to userspace.
	 */
	g = tsm_mr_create_attribute_group(&arm_cca_measurements);
	if (IS_ERR_OR_NULL(g))
		return PTR_ERR(g);

	/* Initialise the attribute group before registering the misc device. */
	arm_cca_attr_groups[0] = g;

	/*
	 * Register a miscelaneous device for exposing
	 * the Arm CCA measurement registers to userspace.
	 */
	ret = misc_register(&arm_cca_misc_dev);
	if (ret < 0) {
		tsm_mr_free_attribute_group(g);
		return ret;
	}

	arm_cca_mr_buf = no_free_ptr(digest_buf);

	return 0;
}

/**
 * arm_cca_mr_cleanup - Unregister sysfs attribute group and free the
 * measurement digest buffer region.
 *
 * @mr_grp: Pointer to the sysfs attribute group.
 *
 * This function performs cleanup for the Arm CCA memory registers (MR).
 *
 * The function should be called during the teardown or cleanup phase
 * to ensure proper resource deallocation.
 */
static void arm_cca_mr_cleanup(const struct attribute_group *mr_grp)
{
	misc_deregister(&arm_cca_misc_dev);
	tsm_mr_free_attribute_group(mr_grp);
	kfree(arm_cca_mr_buf);
}

/**
 * struct arm_cca_token_info - a descriptor for the token buffer.
 * @granule:		PA of the granule to which the token will be written
 * @offset:		Offset within granule to start of buffer in bytes
 */
struct arm_cca_token_info {
	phys_addr_t     granule;
	unsigned long   offset;
};

/**
 * arm_cca_attestation_continue - Retrieve the attestation token data.
 *
 * @info: pointer to the arm_cca_token_info
 *
 * Attestation token generation is a long running operation and therefore
 * the token data may not be retrieved in a single call. Moreover, the
 * token retrieval operation must be requested on the same CPU on which the
 * attestation token generation was initialised.
 * This helper function must therefore be executed on the same CPU multiple
 * times until the entire token data is retrieved.
 */
static unsigned long
arm_cca_attestation_continue(struct arm_cca_token_info *info)
{
	unsigned long ret;
	unsigned long len;
	unsigned long size;

	size = RSI_GRANULE_SIZE - info->offset;
	ret = rsi_attestation_token_continue(info->granule, info->offset, size,
					     &len);
	info->offset += len;
	return ret;
}

/**
 * arm_cca_report_new - Generate a new attestation token.
 *
 * @report: pointer to the TSM report context information.
 * @data:  pointer to the context specific data for this module.
 *
 * Initialise the attestation token generation using the challenge data
 * passed in the TSM descriptor. Allocate memory for the attestation token
 * and retrieve the attestation token on the same CPU on which the
 * attestation token generation was initialised.
 *
 * The challenge data must be at least 32 bytes and no more than 64 bytes. If
 * less than 64 bytes are provided it will be zero padded to 64 bytes.
 *
 * Return:
 * * %0        - Attestation token generated successfully.
 * * %-EINVAL  - A parameter was not valid.
 * * %-ENOMEM  - Out of memory.
 * * %-EFAULT  - Failed to get IPA for memory page(s).
 */
static int arm_cca_report_new(struct tsm_report *report, void *data)
{
	int ret = 0;
	unsigned long rsi_result;
	long max_size;
	unsigned long token_size = 0;
	struct arm_cca_token_info info;
	void *buf;
	u8 *token __free(kvfree) = NULL;
	struct tsm_report_desc *desc = &report->desc;

	if (desc->inblob_len < 32 || desc->inblob_len > 64)
		return -EINVAL;

	/*
	 * The attestation token 'init' and 'continue' calls must be
	 * performed on the same CPU, so disable CPU migration around
	 * those operations.
	 */
	migrate_disable();

	max_size = rsi_attestation_token_init(desc->inblob, desc->inblob_len);
	if (max_size <= 0) {
		ret = -EINVAL;
		goto exit_migrate_enable;
	}

	/* Allocate outblob */
	token = kvzalloc(max_size, GFP_KERNEL);
	if (!token) {
		ret = -ENOMEM;
		goto exit_migrate_enable;
	}

	/*
	 * Since the outblob may not be physically contiguous, use a page
	 * to bounce the buffer from RMM.
	 */
	buf = alloc_pages_exact(RSI_GRANULE_SIZE, GFP_KERNEL);
	if (!buf) {
		ret = -ENOMEM;
		goto exit_migrate_enable;
	}

	/* Get the PA of the memory page(s) that were allocated */
	info.granule = (unsigned long)virt_to_phys(buf);

	/* Loop until the token is ready or there is an error */
	do {
		/* Retrieve one RSI_GRANULE_SIZE data per loop iteration */
		info.offset = 0;
		do {
			/*
			 * Retrieve a sub-granule chunk of data per loop
			 * iteration.
			 */
			rsi_result = arm_cca_attestation_continue(&info);
		} while (rsi_result == RSI_INCOMPLETE &&
			 info.offset < RSI_GRANULE_SIZE);

		/* Break out in case of failure */
		if (rsi_result != RSI_SUCCESS && rsi_result != RSI_INCOMPLETE) {
			ret = -ENXIO;
			token_size = 0;
			goto exit_free_granule_page;
		}

		/*
		 * Copy the retrieved token data from the granule
		 * to the token buffer, ensuring that the RMM doesn't
		 * overflow the buffer.
		 */
		if (WARN_ON(token_size + info.offset > max_size))
			break;
		memcpy(&token[token_size], buf, info.offset);
		token_size += info.offset;
	} while (rsi_result == RSI_INCOMPLETE);

	report->outblob = no_free_ptr(token);
exit_free_granule_page:
	report->outblob_len = token_size;
	free_pages_exact(buf, RSI_GRANULE_SIZE);
exit_migrate_enable:
	migrate_enable();
	return ret;
}

static const struct tsm_report_ops arm_cca_tsm_report_ops = {
	.name = KBUILD_MODNAME,
	.report_new = arm_cca_report_new,
};

static int cca_tsm_probe(struct arm_smccc_device *sdev)
{
	int ret;

	if (!is_realm_world())
		return -ENODEV;

	ret = arm_cca_mr_init();
	if (ret < 0) {
		pr_err("Error %d initialising MRs\n", ret);
		return ret;
	}

	ret = tsm_report_register(&arm_cca_tsm_report_ops, NULL);
	if (ret < 0) {
		arm_cca_mr_cleanup(arm_cca_attr_groups[0]);
		return dev_err_probe(&sdev->dev,
				     ret, "Error registering with TSM\n");
	}

	return 0;
}

static void cca_tsm_remove(struct arm_smccc_device *sdev)
{
	tsm_report_unregister(&arm_cca_tsm_report_ops);
	arm_cca_mr_cleanup(arm_cca_attr_groups[0]);
}

static const struct arm_smccc_device_id cca_tsm_id_table[] = {
	{ .func_id = SMC_RSI_ABI_VERSION },
	{}
};
MODULE_DEVICE_TABLE(arm_smccc, cca_tsm_id_table);

static struct arm_smccc_driver cca_tsm_driver = {
	.name = KBUILD_MODNAME,
	.probe = cca_tsm_probe,
	.remove = cca_tsm_remove,
	.id_table = cca_tsm_id_table,
};
module_arm_smccc_driver(cca_tsm_driver);
MODULE_AUTHOR("Sami Mujawar <sami.mujawar@arm.com>");
MODULE_DESCRIPTION("Arm CCA Guest TSM Driver");
MODULE_LICENSE("GPL");
