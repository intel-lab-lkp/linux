// SPDX-License-Identifier: GPL-2.0
/*
 * Randomness driver for the ARM SMCCC TRNG Firmware Interface
 * https://developer.arm.com/documentation/den0098/latest/
 *
 *  Copyright (C) 2020 Arm Ltd.
 *
 * The ARM TRNG firmware interface specifies a protocol to read entropy
 * from a higher exception level, to abstract from any machine specific
 * implemenations and allow easier use in hypervisors.
 *
 * The firmware interface is realised using the SMCCC specification.
 */

#include <linux/bits.h>
#include <linux/device.h>
#include <linux/hw_random.h>
#include <linux/module.h>
#include <linux/device/faux.h>
#include <linux/arm-smccc.h>

#include <asm/archrandom.h>

#ifdef CONFIG_ARM64
#define ARM_SMCCC_TRNG_RND	ARM_SMCCC_TRNG_RND64
#define MAX_BITS_PER_CALL	(3 * 64UL)
#else
#define ARM_SMCCC_TRNG_RND	ARM_SMCCC_TRNG_RND32
#define MAX_BITS_PER_CALL	(3 * 32UL)
#endif

/* We don't want to allow the firmware to stall us forever. */
#define SMCCC_TRNG_MAX_TRIES	20

#define SMCCC_RET_TRNG_INVALID_PARAMETER	-2
#define SMCCC_RET_TRNG_NO_ENTROPY		-3

bool __ro_after_init smccc_trng_available;

static int copy_from_registers(char *buf, struct arm_smccc_res *res,
			       size_t bytes)
{
	unsigned int chunk, copied;

	if (bytes == 0)
		return 0;

	chunk = min(bytes, sizeof(long));
	memcpy(buf, &res->a3, chunk);
	copied = chunk;
	if (copied >= bytes)
		return copied;

	chunk = min((bytes - copied), sizeof(long));
	memcpy(&buf[copied], &res->a2, chunk);
	copied += chunk;
	if (copied >= bytes)
		return copied;

	chunk = min((bytes - copied), sizeof(long));
	memcpy(&buf[copied], &res->a1, chunk);

	return copied + chunk;
}

static int smccc_trng_read(struct hwrng *rng, void *data, size_t max, bool wait)
{
	struct arm_smccc_res res;
	u8 *buf = data;
	unsigned int copied = 0;
	int tries = 0;

	while (copied < max) {
		size_t bits = min_t(size_t, (max - copied) * BITS_PER_BYTE,
				  MAX_BITS_PER_CALL);

		arm_smccc_1_1_invoke(ARM_SMCCC_TRNG_RND, bits, &res);

		switch ((int)res.a0) {
		case SMCCC_RET_SUCCESS:
			copied += copy_from_registers(buf + copied, &res,
						      bits / BITS_PER_BYTE);
			tries = 0;
			break;
		case SMCCC_RET_TRNG_NO_ENTROPY:
			if (!wait)
				return copied;
			tries++;
			if (tries >= SMCCC_TRNG_MAX_TRIES)
				return copied;
			cond_resched();
			break;
		default:
			return -EIO;
		}
	}

	return copied;
}

static int smccc_trng_probe(struct faux_device *fdev)
{
	struct hwrng *trng;

	trng = devm_kzalloc(&fdev->dev, sizeof(*trng), GFP_KERNEL);
	if (!trng)
		return -ENOMEM;

	trng->name = "smccc_trng";
	trng->read = smccc_trng_read;

	return devm_hwrng_register(&fdev->dev, trng);
}

static struct faux_device_ops smccc_trng_ops = {
	.probe = smccc_trng_probe,
};

static int __init smccc_trng_init(void)
{
	struct faux_device *fdev;

	smccc_trng_available = smccc_probe_trng();
	if (!smccc_trng_available)
		return 0;

	fdev = faux_device_create("smccc_trng", NULL, &smccc_trng_ops);
	if (!fdev) {
		pr_err("smccc_trng: could not create the device\n");
		return -ENODEV;
	}

	return 0;
}
device_initcall(smccc_trng_init);

MODULE_ALIAS("faux:smccc_trng");
MODULE_AUTHOR("Andre Przywara");
MODULE_DESCRIPTION("Arm SMCCC TRNG firmware interface support");
MODULE_LICENSE("GPL");
