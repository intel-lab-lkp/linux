// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple SMC (System Management Controller) MFD driver
 *
 * Copyright The Asahi Linux Contributors
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/math.h>
#include <linux/mfd/core.h>
#include <linux/mfd/macsmc.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/soc/apple/rtkit.h>
#include <linux/unaligned.h>

#define SMC_ENDPOINT			0x20

/* We don't actually know the true size here but this seem reasonable */
#define SMC_SHMEM_SIZE			0x1000
#define SMC_MAX_SIZE			255

#define SMC_MSG_READ_KEY		0x10
#define SMC_MSG_WRITE_KEY		0x11
#define SMC_MSG_GET_KEY_BY_INDEX	0x12
#define SMC_MSG_GET_KEY_INFO		0x13
#define SMC_MSG_INITIALIZE		0x17
#define SMC_MSG_NOTIFICATION		0x18
#define SMC_MSG_RW_KEY			0x20

#define SMC_DATA			GENMASK_ULL(63, 32)
#define SMC_WSIZE			GENMASK_ULL(31, 24)
#define SMC_SIZE			GENMASK_ULL(23, 16)
#define SMC_ID				GENMASK_ULL(15, 12)
#define SMC_MSG				GENMASK_ULL(7, 0)
#define SMC_RESULT			SMC_MSG

#define SMC_RECV_TIMEOUT		500

static const struct mfd_cell apple_smc_devs[] = {
	MFD_CELL_OF("macsmc-gpio", NULL, NULL, 0, 0, "apple,smc-gpio"),
	MFD_CELL_OF("macsmc-reboot", NULL, NULL, 0, 0, "apple,smc-reboot"),
};

static int apple_smc_cmd_locked(struct apple_smc *smc, u64 cmd, u64 arg,
				  u64 size, u64 wsize, u32 *ret_data)
{
	int ret;
	u64 msg;
	u8 result;

	lockdep_assert_held(&smc->mutex);

	if (!smc->alive)
		return -EIO;
	if (smc->atomic_mode)
		return -EIO;

	reinit_completion(&smc->cmd_done);

	smc->msg_id = (smc->msg_id + 1) & 0xf;
	msg = (FIELD_PREP(SMC_MSG, cmd) |
	       FIELD_PREP(SMC_SIZE, size) |
	       FIELD_PREP(SMC_WSIZE, wsize) |
	       FIELD_PREP(SMC_ID, smc->msg_id) |
	       FIELD_PREP(SMC_DATA, arg));

	ret = apple_rtkit_send_message(smc->rtk, SMC_ENDPOINT, msg, NULL, false);
	if (ret < 0) {
		dev_err(smc->dev, "Failed to send command\n");
		return ret;
	}

	do {
		if (wait_for_completion_timeout(&smc->cmd_done,
						msecs_to_jiffies(SMC_RECV_TIMEOUT)) == 0) {
			dev_err(smc->dev, "Command timed out (%llx)", msg);
			return -ETIMEDOUT;
		}
		if (FIELD_GET(SMC_ID, smc->cmd_ret) == smc->msg_id)
			break;
		dev_err(smc->dev, "Command sequence mismatch (expected %d, got %d)\n",
			smc->msg_id, (unsigned int)FIELD_GET(SMC_ID, smc->cmd_ret));
	} while (1);

	result = FIELD_GET(SMC_RESULT, smc->cmd_ret);
	if (result != 0)
		return -result;

	if (ret_data)
		*ret_data = FIELD_GET(SMC_DATA, smc->cmd_ret);

	return FIELD_GET(SMC_SIZE, smc->cmd_ret);
}

static int apple_smc_cmd(struct apple_smc *smc, u64 cmd, u64 arg,
			 u64 size, u64 wsize, u32 *ret_data)
{
	guard(mutex)(&smc->mutex);

	return apple_smc_cmd_locked(smc, cmd, arg, size, wsize, ret_data);
}

static int apple_smc_rw_locked(struct apple_smc *smc, smc_key key,
				const void *wbuf, size_t wsize,
				void *rbuf, size_t rsize)
{
	int ret;
	u64 cmd;
	u64 smc_size, smc_wsize;
	u32 rdata;

	lockdep_assert_held(&smc->mutex);

	dev_dbg(smc->dev, "SMC key: %p4ch, wsize: %zu, rsize: %zu\n", &key, wsize, rsize);

	if (rsize > SMC_MAX_SIZE)
		return -EINVAL;
	if (wsize > SMC_MAX_SIZE)
		return -EINVAL;

	if (rsize && wsize) {
		cmd = SMC_MSG_RW_KEY;
		memcpy_toio(smc->shmem.iomem, wbuf, wsize);
		smc_size = rsize;
		smc_wsize = wsize;
	} else if (wsize && !rsize) {
		cmd = SMC_MSG_WRITE_KEY;
		memcpy_toio(smc->shmem.iomem, wbuf, wsize);
		/*
		 * Setting size to the length we want to write and wsize to 0
		 * looks silly but that's how the SMC protocol works ¯\_(ツ)_/¯
		 */
		smc_size = wsize;
		smc_wsize = 0;
	} else if (!wsize && rsize) {
		cmd = SMC_MSG_READ_KEY;
		smc_size = rsize;
		smc_wsize = 0;
	} else {
		return -EINVAL;
	}

	ret = apple_smc_cmd_locked(smc, cmd, key, smc_size, smc_wsize, &rdata);
	if (ret < 0)
		return ret;

	if (rsize) {
		/*
		 * Small data <= 4 bytes is returned as part of the reply
		 * message which is sent over the mailbox FIFO. Everything
		 * bigger has to be copied from SRAM which is mapped as
		 * Device memory.
		 */
		if (rsize <= 4)
			memcpy(rbuf, &rdata, rsize);
		else
			memcpy_fromio(rbuf, smc->shmem.iomem, rsize);
	}

	return ret;
}

int apple_smc_read(struct apple_smc *smc, smc_key key, void *buf, size_t size)
{
	guard(mutex)(&smc->mutex);

	return apple_smc_rw_locked(smc, key, NULL, 0, buf, size);
}
EXPORT_SYMBOL(apple_smc_read);

int apple_smc_write(struct apple_smc *smc, smc_key key, void *buf, size_t size)
{
	guard(mutex)(&smc->mutex);

	return apple_smc_rw_locked(smc, key, buf, size, NULL, 0);
}
EXPORT_SYMBOL(apple_smc_write);

int apple_smc_rw(struct apple_smc *smc, smc_key key, void *wbuf, size_t wsize,
	void *rbuf, size_t rsize)
{
	guard(mutex)(&smc->mutex);

	return apple_smc_rw_locked(smc, key, wbuf, wsize, rbuf, rsize);
}
EXPORT_SYMBOL(apple_smc_rw);

int apple_smc_get_key_by_index(struct apple_smc *smc, int index, smc_key *key)
{
	int ret;

	ret = apple_smc_cmd(smc, SMC_MSG_GET_KEY_BY_INDEX, index, 0, 0, key);

	*key = swab32(*key);
	return ret;
}
EXPORT_SYMBOL(apple_smc_get_key_by_index);

int apple_smc_get_key_info(struct apple_smc *smc, smc_key key, struct apple_smc_key_info *info)
{
	u8 key_info[6];
	int ret;

	ret = apple_smc_cmd(smc, SMC_MSG_GET_KEY_INFO, key, 0, 0, NULL);
	if (ret >= 0 && info) {
		memcpy_fromio(key_info, smc->shmem.iomem, sizeof(key_info));
		info->size = key_info[0];
		info->type_code = get_unaligned_be32(&key_info[1]);
		info->flags = key_info[5];
	}
	return ret;
}
EXPORT_SYMBOL(apple_smc_get_key_info);

int apple_smc_find_first_key_index(struct apple_smc *smc, smc_key key)
{
	int start, count;

	/* return early if the key is out of bounds */
	if (key <= smc->first_key)
		return 0;
	if (key > smc->last_key)
		return smc->key_count;

	/* binary search to find index of first SMC key bigger or equal to key */
	start = 0;
	count = smc->key_count;
	while (count > 1) {
		int ret;
		smc_key pkey;
		int pivot = start + ((count - 1) >> 1);

		ret = apple_smc_get_key_by_index(smc, pivot, &pkey);
		if (ret < 0)
			return ret;

		if (pkey == key)
			return pivot;

		pivot++;

		if (pkey < key) {
			count -= pivot - start;
			start = pivot;
		} else {
			count = pivot - start;
		}
	}

	return start;
}
EXPORT_SYMBOL(apple_smc_find_first_key_index);

int apple_smc_read_f32_scaled(struct apple_smc *smc, smc_key key, int *p, int scale)
{
	u32 fval;
	u64 val;
	int ret, exp;

	/* pretend the 4 bytes returned by SMC are a 32bit unsigned integer */
	ret = apple_smc_read_u32(smc, key, &fval);
	if (ret < 0)
		return ret;

	/* extract exponent and fraction from the IEEE 754 32bit float */
	val = ((u64)((fval & GENMASK(22, 0)) | BIT(23)));
	exp = ((fval >> 23) & 0xff) - 127 - 23;

	/* move fraction to target scale */
	if (scale < 0) {
		val <<= 32;
		exp -= 32;
		val /= -scale;
	} else {
		val *= scale;
	}

	/* apply exponent if possible and fall back to 0 / U64_MAX on overflow */
	if (exp > 63)
		val = U64_MAX;
	else if (exp < -63)
		val = 0;
	else if (exp < 0)
		val >>= -exp;
	else if (exp != 0 && (val & ~((1UL << (64 - exp)) - 1))) /* overflow */
		val = U64_MAX;
	else
		val <<= exp;

	/* handle IEEE 754 32bit float sign bit and catch possible overflows */
	if (fval & BIT(31)) {
		if (val > (-(s64)INT_MIN))
			*p = INT_MIN;
		else
			*p = -val;
	} else {
		if (val > INT_MAX)
			*p = INT_MAX;
		else
			*p = val;
	}

	return ret;
}
EXPORT_SYMBOL(apple_smc_read_f32_scaled);

int apple_smc_read_ioft_scaled(struct apple_smc *smc, smc_key key, u64 *p,
			       int scale)
{
	u64 val;
	int ret;

	ret = apple_smc_read_u64(smc, key, &val);
	if (ret < 0)
		return ret;

	/*
	 * The value val is represented in 48.16 fixed-point format, where
	 * the upper 48 bits represent the integer part and the lower 16 bits
	 * represent the fractional part. Dividing by 1 << 16 extracts the
	 * integer part by discarding the fractional portion.
	 * To scale the value as requested mult_frac is used to multiply val
	 * by the scaling factor while dividing by 1 << 16 in a single step.
	 */
	*p = mult_frac(val, scale, (1<<16));

	return 0;
}
EXPORT_SYMBOL(apple_smc_read_ioft_scaled);

int apple_smc_enter_atomic(struct apple_smc *smc)
{
	guard(mutex)(&smc->mutex);

	/*
	 * Disable notifications since this is called before shutdown and no
	 * notification handler will be able to handle the notification
	 * using atomic operations only. Also ignore any failure here
	 * because we're about to shut down or reboot anyway.
	 * We can't use apple_smc_write_flag here since that would try to lock
	 * smc->mutex again.
	 */
	const u8 flag = 0;

	apple_smc_rw_locked(smc, SMC_KEY(NTAP), &flag, sizeof(flag), NULL, 0);

	smc->atomic_mode = true;

	return 0;
}
EXPORT_SYMBOL(apple_smc_enter_atomic);

int apple_smc_write_atomic(struct apple_smc *smc, smc_key key, void *buf, size_t size)
{
	guard(spinlock_irqsave)(&smc->lock);
	int ret;
	u64 msg;
	u8 result;

	if (size > SMC_MAX_SIZE || size == 0)
		return -EINVAL;

	if (!smc->alive)
		return -EIO;
	if (!smc->atomic_mode)
		return -EIO;

	memcpy_toio(smc->shmem.iomem, buf, size);
	smc->msg_id = (smc->msg_id + 1) & 0xf;
	msg = (FIELD_PREP(SMC_MSG, SMC_MSG_WRITE_KEY) |
	       FIELD_PREP(SMC_SIZE, size) |
	       FIELD_PREP(SMC_ID, smc->msg_id) |
	       FIELD_PREP(SMC_DATA, key));
	smc->atomic_pending = true;

	ret = apple_rtkit_send_message(smc->rtk, SMC_ENDPOINT, msg, NULL, true);
	if (ret < 0) {
		dev_err(smc->dev, "Failed to send command (%d)\n", ret);
		return ret;
	}

	while (smc->atomic_pending) {
		ret = apple_rtkit_poll(smc->rtk);
		if (ret < 0) {
			dev_err(smc->dev, "RTKit poll failed (%llx)", msg);
			return ret;
		}
		udelay(100);
	}

	if (FIELD_GET(SMC_ID, smc->cmd_ret) != smc->msg_id) {
		dev_err(smc->dev, "Command sequence mismatch (expected %d, got %d)\n",
			smc->msg_id, (unsigned int)FIELD_GET(SMC_ID, smc->cmd_ret));
		return -EIO;
	}

	result = FIELD_GET(SMC_RESULT, smc->cmd_ret);
	if (result != 0)
		return -result;

	return FIELD_GET(SMC_SIZE, smc->cmd_ret);
}
EXPORT_SYMBOL(apple_smc_write_atomic);

static void apple_smc_rtkit_crashed(void *cookie, const void *bfr, size_t bfr_len)
{
	struct apple_smc *smc = cookie;

	dev_err(smc->dev, "SMC crashed! Your system will reboot in a few seconds...\n");
	smc->alive = false;
}

static int apple_smc_rtkit_shmem_setup(void *cookie, struct apple_rtkit_shmem *bfr)
{
	struct apple_smc *smc = cookie;
	struct resource res = {
		.start = bfr->iova,
		.end = bfr->iova + bfr->size - 1,
		.name = "rtkit_map",
		.flags = smc->sram->flags,
	};

	if (!bfr->iova) {
		dev_err(smc->dev, "RTKit wants a RAM buffer\n");
		return -EIO;
	}

	if (res.end < res.start || !resource_contains(smc->sram, &res)) {
		dev_err(smc->dev,
			"RTKit buffer request outside SRAM region: %pR", &res);
		return -EFAULT;
	}

	bfr->iomem = smc->sram_base + (res.start - smc->sram->start);
	bfr->is_mapped = true;

	return 0;
}

static void apple_smc_rtkit_shmem_destroy(void *cookie, struct apple_rtkit_shmem *bfr)
{
	// no-op
}

static bool apple_smc_rtkit_recv_early(void *cookie, u8 endpoint, u64 message)
{
	struct apple_smc *smc = cookie;

	if (endpoint != SMC_ENDPOINT) {
		dev_err(smc->dev, "Received message for unknown endpoint 0x%x\n", endpoint);
		return false;
	}

	if (!smc->initialized) {
		int ret;

		smc->shmem.iova = message;
		smc->shmem.size = SMC_SHMEM_SIZE;
		ret = apple_smc_rtkit_shmem_setup(smc, &smc->shmem);
		if (ret < 0)
			dev_err(smc->dev, "Failed to initialize shared memory\n");
		else
			smc->alive = true;
		smc->initialized = true;
		complete(&smc->init_done);
	} else if (FIELD_GET(SMC_MSG, message) == SMC_MSG_NOTIFICATION) {
		/* Handle these in the RTKit worker thread */
		return false;
	} else {
		smc->cmd_ret = message;
		if (smc->atomic_pending)
			smc->atomic_pending = false;
		else
			complete(&smc->cmd_done);
	}

	return true;
}

static void apple_smc_rtkit_recv(void *cookie, u8 endpoint, u64 message)
{
	struct apple_smc *smc = cookie;
	uint32_t event;

	if (endpoint != SMC_ENDPOINT) {
		dev_err(smc->dev, "Received message for unknown endpoint 0x%x\n", endpoint);
		return;
	}

	if (FIELD_GET(SMC_MSG, message) != SMC_MSG_NOTIFICATION) {
		dev_err(smc->dev, "Received unknown message from worker: 0x%llx\n", message);
		return;
	}

	event = FIELD_GET(SMC_DATA, message);
	dev_dbg(smc->dev, "Event: 0x%08x\n", event);
	blocking_notifier_call_chain(&smc->event_handlers, event, NULL);
}

static const struct apple_rtkit_ops apple_smc_rtkit_ops = {
	.crashed = apple_smc_rtkit_crashed,
	.recv_message = apple_smc_rtkit_recv,
	.recv_message_early = apple_smc_rtkit_recv_early,
	.shmem_setup = apple_smc_rtkit_shmem_setup,
	.shmem_destroy = apple_smc_rtkit_shmem_destroy,
};

static int apple_smc_platform_probe(struct platform_device *pdev)
{
	int ret;
	u32 count;
	struct apple_smc *smc;
	struct device *dev = &pdev->dev;

	smc = devm_kzalloc(dev, sizeof(*smc), GFP_KERNEL);
	if (!smc)
		return -ENOMEM;

	smc->dev = &pdev->dev;
	smc->sram = platform_get_resource_byname(pdev, IORESOURCE_MEM, "sram");
	if (!smc->sram)
		return dev_err_probe(dev, EIO,
				     "No SRAM region");

	smc->sram_base = devm_ioremap_resource(dev, smc->sram);
	if (IS_ERR(smc->sram_base))
		return dev_err_probe(dev, PTR_ERR(smc->sram_base),
				     "Failed to map SRAM region");

	smc->rtk =
		devm_apple_rtkit_init(dev, smc, NULL, 0, &apple_smc_rtkit_ops);
	if (IS_ERR(smc->rtk))
		return dev_err_probe(dev, PTR_ERR(smc->rtk),
				     "Failed to initialize RTKit");

	ret = apple_rtkit_wake(smc->rtk);
	if (ret != 0)
		return dev_err_probe(dev, ret,
				     "Failed to wake up SMC");

	ret = apple_rtkit_start_ep(smc->rtk, SMC_ENDPOINT);
	if (ret != 0) {
		ret = dev_err_probe(dev, ret,
				     "Failed to start SMC endpoint");
		goto cleanup;
	}

	init_completion(&smc->init_done);
	init_completion(&smc->cmd_done);

	ret = apple_rtkit_send_message(smc->rtk, SMC_ENDPOINT,
				       FIELD_PREP(SMC_MSG, SMC_MSG_INITIALIZE), NULL, false);
	if (ret < 0) {
		ret = dev_err_probe(dev, ret, "Failed to send init message");
		goto cleanup;
	}

	if (wait_for_completion_timeout(&smc->init_done,
					msecs_to_jiffies(SMC_RECV_TIMEOUT)) == 0) {
		ret = -ETIMEDOUT;
		dev_err(dev, "Timed out initializing SMC");
		goto cleanup;
	}

	if (!smc->alive) {
		ret = -EIO;
		goto cleanup;
	}

	dev_set_drvdata(&pdev->dev, smc);
	BLOCKING_INIT_NOTIFIER_HEAD(&smc->event_handlers);

	ret = apple_smc_read_u32(smc, SMC_KEY(#KEY), &count);
	if (ret) {
		ret = dev_err_probe(smc->dev, ret, "Failed to get key count");
		goto cleanup;
	}
	smc->key_count = be32_to_cpu(count);

	ret = apple_smc_get_key_by_index(smc, 0, &smc->first_key);
	if (ret) {
		ret = dev_err_probe(smc->dev, ret, "Failed to get first key");
		goto cleanup;
	}

	ret = apple_smc_get_key_by_index(smc, smc->key_count - 1, &smc->last_key);
	if (ret) {
		ret = dev_err_probe(smc->dev, ret, "Failed to get last key");
		goto cleanup;
	}

	/* Enable notifications */
	apple_smc_write_flag(smc, SMC_KEY(NTAP), true);

	dev_info(smc->dev, "Initialized (%d keys %p4ch ... %p4ch)\n",
		 smc->key_count, &smc->first_key, &smc->last_key);

	ret = mfd_add_devices(smc->dev, -1,
			      apple_smc_devs, ARRAY_SIZE(apple_smc_devs),
			      NULL, 0, NULL);
	if (ret) {
		ret = dev_err_probe(smc->dev, ret, "Subdevice initialization failed");
		goto cleanup;
	}

	return 0;

cleanup:
	/* Try to shut down RTKit, if it's not completely wedged */
	if (apple_rtkit_is_running(smc->rtk))
		apple_rtkit_quiesce(smc->rtk);

	return ret;
}

static void apple_smc_platform_remove(struct platform_device *pdev)
{
	struct apple_smc *smc = platform_get_drvdata(pdev);

	mfd_remove_devices(smc->dev);

	/* Disable notifications */
	apple_smc_write_flag(smc, SMC_KEY(NTAP), false);

	/* Shut down SMC firmware */
	if (apple_rtkit_is_running(smc->rtk))
		apple_rtkit_quiesce(smc->rtk);
}

static const struct of_device_id apple_smc_of_match[] = {
	{ .compatible = "apple,smc" },
	{},
};
MODULE_DEVICE_TABLE(of, apple_smc_of_match);

static struct platform_driver apple_smc_driver = {
	.driver = {
		.name = "mfd-macsmc",
		.owner = THIS_MODULE,
		.of_match_table = apple_smc_of_match,
	},
	.probe = apple_smc_platform_probe,
	.remove = apple_smc_platform_remove,
};
module_platform_driver(apple_smc_driver);

MODULE_AUTHOR("Hector Martin <marcan@marcan.st>");
MODULE_AUTHOR("Sven Peter <sven@svenpeter.dev>");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("Apple SMC driver");
