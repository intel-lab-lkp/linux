// SPDX-License-Identifier: GPL-2.0
/*
 * MMC pstore support based on pstore/zone API
 *
 * This driver provides pstore backend support for non-removable MMC devices (eMMC),
 * allowing kernel crash logs and other pstore data to be stored
 * on eMMC storage devices. Only works with non-removable cards.
 *
 */

#define pr_fmt(fmt) "mmcpstore: " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pstore_blk.h>
#include <linux/pstore.h>
#include <linux/pstore_zone.h>
#include <linux/completion.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/slab.h>
#include <linux/mmc/host.h>
#include <linux/fs.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/card.h>
#include "bus.h"
#include <linux/scatterlist.h>
#include <linux/mutex.h>
#include <linux/ctype.h>
#include <linux/reboot.h>
#include <linux/panic.h>
#include <linux/suspend.h>
#include <linux/pm_runtime.h>
#include "block.h"
#include "card.h"
#include "core.h"
#include "mmc_ops.h"

#define MMC_PSTORE_SECTOR_SIZE		512
#define MMC_PSTORE_MAX_BUFFER_SIZE	(256 * 1024)
#define MMC_PSTORE_BYTES_TO_KB(bytes)	((bytes) / 1024)
#define MMC_PSTORE_METADATA_SIZE	1024
#define MMC_PSTORE_DATA_TIMEOUT_NS	2000000000U
#define MMC_PSTORE_DEVICE_NAME_SIZE	32	/* Size for device name buffers */
#define MMC_PSTORE_DEV_PREFIX		"/dev/"
#define MMC_PSTORE_DEV_PREFIX_LEN	5	/* Length of "/dev/" */
#define MMC_PSTORE_PANIC_STABILIZE_DELAY_MS	5
#define MMC_PSTORE_PANIC_DELAY_MS		50
#define MMC_PSTORE_HARDWARE_TIMEOUT_MS		100

static unsigned long part_sect_ofs;
module_param_named(sector_offset, part_sect_ofs, ulong, 0644);
MODULE_PARM_DESC(sector_offset, "Sector offset within partition to start pstore storage");

static unsigned long part_sect_cnt;
module_param_named(sector_count, part_sect_cnt, ulong, 0644);
MODULE_PARM_DESC(sector_count, "Number of sectors to use for pstore storage");

/**
 * struct mmcpstore_context - MMC pstore context
 * @dev: pstore device info for registration
 * @card: MMC card associated with this pstore
 * @card_name: Registered device path for block I/O operations (e.g., "/dev/mmcblk1p5")
 *             Set once during registration and used for block device access
 * @start_sect: Starting sector for pstore data
 * @size: Total size available for pstore
 * @lock: Mutex to protect MMC operations
 * @buffer: Pre-allocated buffer for alignment handling
 * @buffer_size: Size of the pre-allocated buffer
 * @state: Current state of the context
 * @original_part_config: Original partition config before pstore operations
 * @part_config_saved: Whether we've saved the original partition config
 */
struct mmcpstore_context {
	struct pstore_device_info dev;
	struct mmc_card *card;
	char card_name[MMC_PSTORE_DEVICE_NAME_SIZE];
	sector_t start_sect;
	sector_t size;
	struct mutex lock; /* Protects MMC operations */
	char *buffer;
	size_t buffer_size;
	enum {
		MMCPSTORE_STATE_UNINITIALIZED,
		MMCPSTORE_STATE_INITIALIZING,
		MMCPSTORE_STATE_READY,
		MMCPSTORE_STATE_SUSPENDED,
		MMCPSTORE_STATE_SHUTDOWN,
		MMCPSTORE_STATE_ERROR
	} state;
	u8 original_part_config;
	bool part_config_saved;
};

static struct mmcpstore_context *mmcpstore_ctx;
static DEFINE_MUTEX(mmcpstore_global_lock);

/**
 * mmcpstore_save_partition_state - Save current partition configuration
 * @cxt: MMC pstore context
 *
 * Saves the current partition configuration so it can be restored later.
 * This should be called before any pstore operations that might change partitions.
 */
static void mmcpstore_save_partition_state(struct mmcpstore_context *cxt)
{
	if (!cxt || !cxt->card || !mmc_card_mmc(cxt->card))
		return;

	if (!cxt->part_config_saved) {
		cxt->original_part_config = cxt->card->ext_csd.part_config;
		cxt->part_config_saved = true;
		pr_debug("MMC pstore: Saved original partition config: 0x%02x\n",
			 cxt->original_part_config);
	}
}

/**
 * mmcpstore_panic_poll_card_busy - Poll card busy via host ops
 * @host: MMC host structure
 * @timeout_ms: Timeout in milliseconds
 *
 * Uses the host's card_busy op to poll until the card signals not-busy.
 * Returns: 0 if ready, -ETIMEDOUT if timeout, -ENODEV if no card_busy op
 */
static int mmcpstore_panic_poll_card_busy(struct mmc_host *host, int timeout_ms)
{
	int loops = timeout_ms * 1000 / 100;
	int i;

	if (!host->ops || !host->ops->card_busy)
		return -ENODEV;

	for (i = 0; i < loops; i++) {
		if (!host->ops->card_busy(host))
			return 0;
		udelay(100);
	}

	return -ETIMEDOUT;
}

/**
 * mmcpstore_panic_switch_partition - Panic-safe partition switch using direct CMD6
 * @cxt: MMC pstore context
 * @target_part: Target partition number (0 = main user area)
 *
 * Sends CMD6 (SWITCH) via mmc_start_request + polling. mmc_wait_for_cmd
 * cannot be used during panic because it calls wait_for_completion which
 * invokes schedule() — dead during panic.
 *
 * Returns: 0 on success, negative error on failure
 */
static int mmcpstore_panic_switch_partition(struct mmcpstore_context *cxt, u8 target_part)
{
	struct mmc_request mrq = {};
	struct mmc_command cmd = {};
	struct mmc_host *host = cxt->card->host;
	u8 part_config;
	u8 new_part_config;
	int ret;

	if (!mmc_card_mmc(cxt->card))
		return 0;

	part_config = cxt->card->ext_csd.part_config;
	new_part_config = (part_config & ~EXT_CSD_PART_CONFIG_ACC_MASK) | target_part;

	cmd.opcode = MMC_SWITCH;
	cmd.arg = (MMC_SWITCH_MODE_WRITE_BYTE << 24) |
		  (EXT_CSD_PART_CONFIG << 16) |
		  (new_part_config << 8) |
		  EXT_CSD_CMD_SET_NORMAL;
	cmd.flags = MMC_RSP_R1B | MMC_CMD_AC;

	mrq.cmd = &cmd;
	init_completion(&mrq.completion);
	mrq.done = NULL;

	ret = mmc_start_request(host, &mrq);
	if (ret)
		return ret;

	if (host->ops->panic_poll_completion)
		host->ops->panic_poll_completion(host, &mrq);

	if (host->ops->panic_complete)
		host->ops->panic_complete(host, &mrq);

	if (cmd.error)
		return cmd.error;

	ret = mmcpstore_panic_poll_card_busy(host, MMC_PSTORE_HARDWARE_TIMEOUT_MS);
	if (ret == -ENODEV)
		mdelay(MMC_PSTORE_PANIC_DELAY_MS);

	cxt->card->ext_csd.part_config = new_part_config;

	return 0;
}

/**
 * mmcpstore_switch_to_main_partition - Ensure eMMC is in main partition
 * @cxt: MMC pstore context
 *
 * Returns: 0 on success, negative error on failure
 */
static int mmcpstore_switch_to_main_partition(struct mmcpstore_context *cxt)
{
	u8 part_config;
	u8 current_part;
	u8 new_part_config;
	int ret;

	if (!mmc_card_mmc(cxt->card))
		return 0;

	part_config = cxt->card->ext_csd.part_config;
	current_part = (part_config & EXT_CSD_PART_CONFIG_ACC_MASK);
	if (current_part == 0)
		return 0;

	new_part_config = (part_config & ~EXT_CSD_PART_CONFIG_ACC_MASK);
	ret = mmc_switch(cxt->card, EXT_CSD_CMD_SET_NORMAL,
			 EXT_CSD_PART_CONFIG, new_part_config,
			 cxt->card->ext_csd.part_time);
	if (ret) {
		pr_err("MMC pstore: Failed to switch to main partition: %d\n", ret);
		return ret;
	}

	pr_debug("MMC pstore: Switched from partition %u to main partition\n", current_part);
	return 0;
}

/**
 * mmcpstore_do_request_internal - Perform single MMC read/write request
 * @buf: Buffer for data
 * @size: Size of data in bytes (must be <= max_req_size)
 * @sect_offset: Sector offset on the MMC device
 * @write: true for write, false for read
 * @panic: true if called during panic (polling mode)
 *
 * Internal function that performs a single MMC request without splitting.
 * Size must not exceed host->max_req_size.
 *
 * Returns: 0 on success, negative error on failure
 */
static int mmcpstore_do_request_internal(char *buf, size_t size,
					 loff_t sect_offset, bool write,
					 bool panic)
{
	struct mmcpstore_context *cxt = mmcpstore_ctx;
	struct mmc_request mrq = {};
	struct mmc_command cmd = {};
	struct mmc_command sbc = {};
	struct mmc_data data = {};
	struct scatterlist sg;
	struct mmc_host *host;
	u32 blocks = size >> 9;
	u32 opcode;
	u32 status;
	int ret;
	int start_ret;

	if (!cxt || !cxt->card)
		return -ENODEV;

	if (sect_offset + blocks > (cxt->size >> 9)) {
		if (panic)
			pr_info("PANIC: Request exceeds partition: offset=%lld, blocks=%u, sectors=%llu\n",
				sect_offset, blocks, cxt->size >> 9);
		else
			pr_err("Request exceeds partition: offset=%lld, blocks=%u, sectors=%llu\n",
			       sect_offset, blocks, cxt->size >> 9);
		return -EINVAL;
	}

	if (size % MMC_PSTORE_SECTOR_SIZE) {
		pr_err("Size must be multiple of %d bytes\n", MMC_PSTORE_SECTOR_SIZE);
		return -EINVAL;
	}

	host = cxt->card->host;

	/*
	 * Check against host's max_req_size to prevent request failures.
	 * The block layer handles this automatically, but we're bypassing it
	 * by calling mmc_start_request/mmc_wait_for_req directly.
	 * Large panic dumps (8MB+) can exceed this limit on some platforms.
	 */
	if (size > host->max_req_size) {
		if (panic)
			pr_info("PANIC: Request size %zu exceeds max_req_size %u, needs splitting\n",
				size, host->max_req_size);
		else
			pr_err("Request size %zu exceeds max_req_size %u\n",
			       size, host->max_req_size);
		return -EINVAL;
	}

	if (write) {
		opcode = (blocks > 1) ? MMC_WRITE_MULTIPLE_BLOCK :
					MMC_WRITE_BLOCK;
		data.flags = MMC_DATA_WRITE;
	} else {
		opcode = (blocks > 1) ? MMC_READ_MULTIPLE_BLOCK :
					MMC_READ_SINGLE_BLOCK;
		data.flags = MMC_DATA_READ;
	}

	cmd.opcode = opcode;
	if (mmc_card_is_blockaddr(cxt->card)) {
		cmd.arg = cxt->start_sect + sect_offset;
	} else {
		cmd.arg = (cxt->start_sect + sect_offset) *
			  MMC_PSTORE_SECTOR_SIZE;
	}

	pr_debug("MMC request: opcode=%u, arg=%u, blocks=%u, size=%zu, sect_offset=%lld, start_sect=%llu\n",
		 opcode, cmd.arg, blocks, size, sect_offset, cxt->start_sect);

	cmd.flags = MMC_RSP_R1 | MMC_CMD_ADTC;

	/*
	 * Use CMD23 (SET_BLOCK_COUNT) for all pstore operations on eMMC.
	 * For write operations, set bit 24 (Forced Programming) to bypass eMMC cache
	 * and write directly to flash. This provides better reliability for all
	 * pstore writes including single-block pmsg writes, ensuring data persistence
	 * without requiring explicit cache flush commands.
	 *
	 * Note: CMD23 is mandatory for eMMC 4.41+ (and optional for earlier versions).
	 */
	if (mmc_card_mmc(cxt->card) && (host->caps & MMC_CAP_CMD23)) {
		sbc.opcode = MMC_SET_BLOCK_COUNT;
		sbc.arg = blocks;

		/* Set Forced Programming bit (bit 24) for writes to bypass cache */
		if (write)
			sbc.arg |= BIT(24);

		sbc.flags = MMC_RSP_R1 | MMC_CMD_AC;
		mrq.sbc = &sbc;

		pr_debug("Using CMD23: blocks=%u, forced_prog=%d\n",
			 blocks, write ? 1 : 0);
	}

	data.blksz = MMC_PSTORE_SECTOR_SIZE;
	data.blocks = blocks;
	sg_init_one(&sg, buf, size);
	data.sg = &sg;
	data.sg_len = 1;
	mrq.cmd = &cmd;
	mrq.data = &data;

	mmc_set_data_timeout(&data, cxt->card);
	data.timeout_ns = MMC_PSTORE_DATA_TIMEOUT_NS;

	if (!panic) {
		mutex_lock(&cxt->lock);
		mmc_claim_host(host);

		/* Disable command queue engine for pstore operations */
		if (cxt->card->ext_csd.cmdq_en) {
			ret = mmc_cmdq_disable(cxt->card);
			if (ret) {
				pr_err("MMC pstore: Failed to disable CQE: %d\n", ret);
				mmc_release_host(host);
				mutex_unlock(&cxt->lock);
				return ret;
			}
			/* Mark that we need to re-enable CQE on cleanup */
			cxt->card->reenable_cmdq = true;
		}

		/*
		 * Ensure we're in the main partition (not boot/RPMB) for eMMC
		 */
		ret = mmcpstore_switch_to_main_partition(cxt);
		if (ret) {
			if (cxt->card->reenable_cmdq &&
			    !cxt->card->ext_csd.cmdq_en)
				mmc_cmdq_enable(cxt->card);
			mmc_release_host(host);
			mutex_unlock(&cxt->lock);
			return ret;
		}

		ret = mmc_send_status(cxt->card, &status);
		if (ret) {
			pr_info("MMC pstore: Card status check failed: %d\n", ret);
		} else if (R1_CURRENT_STATE(status) != R1_STATE_TRAN) {
			pr_info("MMC pstore: Card not in transfer state (status=0x%08x, state=%d)\n",
				status, R1_CURRENT_STATE(status));
		}
	} else {
		/*
		 * PANIC MODE: Force claim host for exclusive access during panic
		 * Background processes may be stuck/dead, so we force claim instead of waiting
		 */
		mmc_panic_claim_host(host);

		if (mmc_card_mmc(cxt->card)) {
			u8 part_config = cxt->card->ext_csd.part_config;
			u8 current_part = (part_config & EXT_CSD_PART_CONFIG_ACC_MASK);

			if (current_part != 0) {
				ret = mmcpstore_panic_switch_partition(cxt, 0);
				if (ret)
					pr_emerg("MMC pstore: Failed to switch partition: %d\n",
						 ret);
			}
		}

		/* Clear SDHCI state before starting the panic request */
		if (host->ops->panic_complete)
			host->ops->panic_complete(host, &mrq);
	}

	if (panic) {
		init_completion(&mrq.completion);
		mrq.done = NULL;

		start_ret = mmc_start_request(host, &mrq);
		if (start_ret) {
			pr_emerg("MMC pstore: Write failed to start: %d\n", start_ret);
			cmd.error = start_ret;
			goto panic_cleanup;
		}

		/*
		 * Poll for completion as fallback — the normal IRQ-driven
		 * path (sdhci_irq -> mmc_request_done) handles completion,
		 * but during panic IRQs may not fire. The poll checks the
		 * hardware registers directly.
		 */
		if (host->ops->panic_poll_completion)
			host->ops->panic_poll_completion(host, &mrq);

		if (host->ops->panic_complete)
			host->ops->panic_complete(host, &mrq);

		if (cmd.error || data.error || (mrq.stop && mrq.stop->error)) {
			pr_emerg("MMC pstore: Write failed: cmd=%d, data=%d, stop=%d\n",
				 cmd.error, data.error, mrq.stop ? mrq.stop->error : 0);
		}
	} else {
		/*
		 * NORMAL MODE: Use standard MMC request handling
		 */
		mmc_wait_for_req(host, &mrq);
	}

panic_cleanup:

	if (!panic) {
		/* Re-enable command queue engine if it was enabled before */
		if (cxt->card->reenable_cmdq && !cxt->card->ext_csd.cmdq_en)
			mmc_cmdq_enable(cxt->card);

		mmc_release_host(host);
		mutex_unlock(&cxt->lock);
	}

	ret = cmd.error;
	if (!ret)
		ret = data.error;
	if (!ret && mrq.stop)
		ret = mrq.stop->error;

	return ret;
}

/**
 * mmcpstore_do_request - Perform MMC read/write request with automatic splitting
 * @buf: Buffer for data
 * @size: Size of data in bytes
 * @sect_offset: Sector offset from start of partition
 * @write: true for write, false for read
 * @panic: true if called during panic (polling mode)
 *
 * Wrapper that automatically splits large requests to respect host->max_req_size.
 * This is necessary because we bypass the block layer which normally handles splitting.
 *
 * Returns: 0 on success, negative error on failure
 */
static int mmcpstore_do_request(char *buf, size_t size, loff_t sect_offset, bool write, bool panic)
{
	struct mmcpstore_context *cxt = mmcpstore_ctx;
	struct mmc_host *host;
	size_t max_chunk_size;
	size_t remaining = size;
	size_t offset = 0;
	size_t chunk_size;
	int ret;

	if (!cxt || !cxt->card)
		return -ENODEV;

	host = cxt->card->host;

	/*
	 * Calculate max chunk size respecting both sector alignment and host limits.
	 * Align down to sector boundary to ensure each chunk is sector-aligned.
	 */
	max_chunk_size = host->max_req_size & ~(MMC_PSTORE_SECTOR_SIZE - 1);

	/* Handle small requests that fit in one transfer */
	if (size <= max_chunk_size)
		return mmcpstore_do_request_internal(buf, size, sect_offset, write, panic);

	/* Split large requests into multiple transfers */
	if (panic)
		pr_info("PANIC: Splitting large request: total=%zu, max_chunk=%zu\n",
			size, max_chunk_size);
	else
		pr_debug("Splitting large request: total=%zu, max_chunk=%zu\n",
			 size, max_chunk_size);

	while (remaining > 0) {
		chunk_size = min(remaining, max_chunk_size);

		ret = mmcpstore_do_request_internal(buf + offset, chunk_size,
						    sect_offset + (offset >> 9),
						    write, panic);
		if (ret) {
			if (panic)
				pr_emerg("PANIC: Chunk transfer failed at offset %zu: %d\n",
					 offset, ret);
			else
				pr_err("Chunk transfer failed at offset %zu: %d\n",
				       offset, ret);
			return ret;
		}

		remaining -= chunk_size;
		offset += chunk_size;

		if (panic && remaining > 0)
			pr_info("PANIC: Chunk complete, remaining=%zu\n", remaining);
	}

	return 0;
}

/**
 * mmcpstore_check_pm_active - Check if system is in PM transition
 *
 * This function is designed to be deadlock-safe by using a lock-free approach for
 * the common case (PM state checking). The mutex is only acquired when auto-restore
 * is needed, preventing deadlocks when called from pstore read/write operations
 * that may already hold locks.
 *
 * Returns: true if PM is active, false otherwise
 */
static bool mmcpstore_check_pm_active(void)
{
	/* Check driver state - lock-free for read operations */
	bool pm_active = (mmcpstore_ctx && mmcpstore_ctx->state == MMCPSTORE_STATE_SUSPENDED);

	return pm_active;
}

/**
 * mmcpstore_read_zone - Read data from MMC device
 * @buf: Buffer to read data into
 * @size: Size of data to read
 * @offset: Offset in bytes from start of pstore area
 *
 * Returns: Number of bytes read on success, negative error on failure
 */

static ssize_t mmcpstore_read_zone(char *buf, size_t size, loff_t offset)
{
	struct mmcpstore_context *cxt = mmcpstore_ctx;
	loff_t aligned_offset = offset & ~(MMC_PSTORE_SECTOR_SIZE - 1);
	char *temp_buf;
	size_t read_size;
	loff_t sect_offset;
	bool need_copy = false;
	int ret;

	pr_debug("%s: offset=%lld, size=%zu\n", __func__, offset, size);

	if (!cxt || cxt->state != MMCPSTORE_STATE_READY || !cxt->card || !cxt->card->host) {
		pr_debug("%s: context not ready (cxt=%p, state=%d)\n",
			 __func__, cxt, cxt ? cxt->state : -1);
		return -ENODEV;
	}

	/* Check if system is in PM transition - be more aggressive */
	if (cxt->state == MMCPSTORE_STATE_SUSPENDED || mmcpstore_check_pm_active()) {
		pr_debug("%s: operations blocked during PM (state=%d, system=%d)\n",
			 __func__, cxt->state, system_state);
		return -ENODEV;
	}

	/* Additional validation */
	if (!cxt->buffer || cxt->buffer_size == 0) {
		pr_err("%s: invalid buffer state\n", __func__);
		return -ENODEV;
	}

	if (cxt->state == MMCPSTORE_STATE_SHUTDOWN)
		return -ENODEV;

	if ((offset & (MMC_PSTORE_SECTOR_SIZE - 1)) == 0 &&
	    (size & (MMC_PSTORE_SECTOR_SIZE - 1)) == 0) {
		/* Already aligned - read directly into user buffer */
		temp_buf = buf;
		read_size = size;
		sect_offset = offset >> 9;
	} else {
		/* Need alignment handling - use pre-allocated buffer */
		read_size = ALIGN(offset + size, MMC_PSTORE_SECTOR_SIZE) - aligned_offset;
		sect_offset = aligned_offset >> 9;

		/* Check if pre-allocated buffer is large enough */
		if (read_size > cxt->buffer_size) {
			pr_err("Read size %zu exceeds pre-allocated buffer size %zu\n",
			       read_size, cxt->buffer_size);
			return -EINVAL;
		}

		temp_buf = cxt->buffer;
		need_copy = true;
	}

	/* MMC read operation with retry for intermittent failures */
	ret = mmcpstore_do_request(temp_buf, read_size, sect_offset, false,
				   false);
	if (ret) {
		pr_info("%s: do_request failed: %d (offset=%lld, size=%zu, read_size=%zu, sect_offset=%lld)\n",
			__func__, ret, offset, size, read_size, sect_offset);
		/* For I/O errors, return -ENOMSG to indicate empty storage */
		if (ret == -EIO || ret == -ETIMEDOUT) {
			pr_debug("%s: MMC read failed, returning -ENOMSG\n", __func__);
			return -ENOMSG;
		}
		return ret;
	}

	/* Note: Don't check for all zeros here - let pstore_zone decide
	 * based on the signature. Our driver should return the actual
	 * data and let the upper layer validate it.
	 */

	if (need_copy)
		memcpy(buf, temp_buf + (offset - aligned_offset), size);

	pr_debug("%s: read successful, returning %zu bytes\n", __func__, size);
	return size;
}

/**
 * mmcpstore_write_common - Common write implementation
 * @buf: Buffer containing data to write
 * @size: Size of data to write
 * @offset: Offset in bytes from start of pstore area
 * @is_panic: True for panic writes (polling mode), false for normal writes
 *
 * Returns: Number of bytes written on success, negative error on failure
 */
static ssize_t mmcpstore_write_common(const char *buf, size_t size, loff_t offset, bool is_panic)
{
	struct mmcpstore_context *cxt = mmcpstore_ctx;
	loff_t aligned_offset = offset & ~(MMC_PSTORE_SECTOR_SIZE - 1);
	size_t aligned_size = ALIGN(offset + size, MMC_PSTORE_SECTOR_SIZE) -
			      aligned_offset;
	loff_t sect_offset = aligned_offset >> 9;
	char *temp_buf;
	int ret;

	if (!cxt || cxt->state != MMCPSTORE_STATE_READY)
		return -ENODEV;

	/* Ensure we have saved the original partition state before any write */
	if (!is_panic)
		mmcpstore_save_partition_state(cxt);

	if (cxt->state == MMCPSTORE_STATE_SHUTDOWN)
		return -ENODEV;

	/* Check if system is in PM transition - be more aggressive */
	if (cxt->state == MMCPSTORE_STATE_SUSPENDED || mmcpstore_check_pm_active()) {
		pr_debug("%s: operations blocked during PM (state=%d, system=%d)\n",
			 __func__, cxt->state, system_state);
		return -ENODEV;
	}

	if (!cxt || !cxt->card)
		return -ENODEV;

	/* Check bounds to prevent buffer overflow */
	if (offset + size > cxt->size) {
		if (!is_panic)
			pr_err("Write exceeds pstore area: offset=%lld, size=%zu, total=%llu\n",
			       offset, size, cxt->size);
		else
			pr_info("PANIC: Write exceeds pstore area: offset=%lld, size=%zu, total=%llu\n",
				offset, size, cxt->size);
		return -EINVAL;
	}

	/* Additional safety check for large writes */
	if (size > cxt->size) {
		if (!is_panic)
			pr_err("Write size %zu exceeds total pstore area %llu\n", size, cxt->size);
		else
			pr_info("PANIC: Write size %zu exceeds total pstore area %llu\n",
				size, cxt->size);
		return -EINVAL;
	}

	if ((offset & (MMC_PSTORE_SECTOR_SIZE - 1)) == 0 &&
	    (size & (MMC_PSTORE_SECTOR_SIZE - 1)) == 0) {
		pr_debug("Direct write: offset=%lld, size=%zu, remaining=%llu\n",
			 offset, size, cxt->size - offset);
		ret = mmcpstore_do_request((char *)buf, size, sect_offset,
					   true, is_panic);
		return ret ? ret : size;
	}

	if (aligned_size > cxt->buffer_size) {
		if (!is_panic)
			pr_err("Write size %zu exceeds buffer size %zu\n",
			       aligned_size, cxt->buffer_size);
		else
			pr_info("PANIC: Write size %zu exceeds buffer size %zu\n",
				aligned_size, cxt->buffer_size);
		return -EINVAL;
	}

	temp_buf = cxt->buffer;
	/* Read-modify-write for unaligned access */
	ret = mmcpstore_do_request(temp_buf, aligned_size, sect_offset,
				   false, is_panic);
	if (ret) {
		if (!is_panic)
			pr_err("mmcpstore_write: Read for RMW failed: %d\n",
			       ret);
		return ret;
	}

	/* Copy new data into aligned buffer */
	memcpy(temp_buf + (offset - aligned_offset), buf, size);
	/* Write the modified data back */
	ret = mmcpstore_do_request(temp_buf, aligned_size, sect_offset,
				   true, is_panic);

	return ret ? ret : size;
}

/**
 * mmcpstore_write - Write data to MMC device
 * @buf: Buffer containing data to write
 * @size: Size of data to write
 * @offset: Offset in bytes from start of pstore area
 *
 * May be called with preemption disabled (e.g. console pstore from
 * printk with console lock held). In that case, return -EBUSY so
 * pstore_zone marks the zone dirty and retries from its deferred
 * flush workqueue where sleeping is allowed.
 *
 * Returns: Number of bytes written on success, negative error on failure
 */
static ssize_t mmcpstore_write(const char *buf, size_t size, loff_t offset)
{
	if (!preemptible())
		return -EBUSY;

	return mmcpstore_write_common(buf, size, offset, false);
}

/**
 * mmcpstore_stop_all_background_io - Stop all background I/O immediately
 * @cxt: MMC pstore context
 *
 * Aggressively stops all background I/O operations that could interfere with
 * panic writes. Called as early as possible during panic.
 *
 * Returns: 0 on success, negative error on failure
 */
static int mmcpstore_stop_all_background_io(struct mmcpstore_context *cxt)
{
	struct mmc_host *host;
	int ret = 0;

	if (!cxt || !cxt->card || !cxt->card->host)
		return -ENODEV;

	host = cxt->card->host;

	/*
	 * Hardware drain + reset FIRST — this ensures the controller is
	 * idle and no stopped CPU is inside sdhci_irq holding host->lock.
	 * Only then is it safe to take the lock in mmc_panic_claim_host.
	 */
	if (host->ops->panic_prepare) {
		ret = host->ops->panic_prepare(host);
		if (ret)
			pr_emerg("MMC pstore: panic_prepare failed: %d\n", ret);
	}

	mmc_panic_claim_host(host);

	mdelay(MMC_PSTORE_PANIC_STABILIZE_DELAY_MS);

	return ret;
}

/**
 * mmcpstore_panic_write - Write data during panic with immediate I/O stopping
 * @buf: Buffer containing data to write
 * @size: Size of data to write
 * @offset: Offset in bytes from start of pstore area
 *
 * Immediately stops all background I/O and forces exclusive access to MMC
 * hardware before performing the panic write to prevent corruption.
 *
 * Returns: Number of bytes written on success, negative error on failure
 */
static ssize_t mmcpstore_panic_write(const char *buf, size_t size, loff_t offset)
{
	struct mmcpstore_context *cxt = mmcpstore_ctx;
	struct mmc_host *host;
	int ret;

	if (!cxt || cxt->state != MMCPSTORE_STATE_READY)
		return -ENODEV;

	if (offset + size > cxt->size) {
		pr_emerg("MMC pstore: PANIC write exceeds pstore area: offset=%lld, size=%zu, total=%llu\n",
			 offset, size, cxt->size);
		return -EINVAL;
	}

	ret = mmcpstore_stop_all_background_io(cxt);
	if (ret < 0) {
		pr_emerg("MMC pstore: Failed to stop background I/O: %d\n", ret);
		return ret;
	}

	ret = mmcpstore_write_common(buf, size, offset, true);
	if (ret < 0) {
		pr_emerg("MMC pstore: Panic write failed after stopping I/O: %d\n", ret);
		return ret;
	}

	mdelay(MMC_PSTORE_PANIC_STABILIZE_DELAY_MS);

	host = cxt->card->host;
	ret = mmcpstore_panic_poll_card_busy(host, MMC_PSTORE_HARDWARE_TIMEOUT_MS);
	if (ret == -ETIMEDOUT)
		pr_emerg("MMC pstore: Card still busy after %dms\n",
			 MMC_PSTORE_HARDWARE_TIMEOUT_MS);

	mdelay(MMC_PSTORE_PANIC_DELAY_MS);

	pr_info_once("Panic write complete\n");
	return size;
}

/* Helper function for size calculations */
static int mmcpstore_calculate_sizes(struct mmcpstore_context *cxt,
				     struct pstore_blk_config *conf,
				       unsigned long *required_size_out,
				       unsigned long *kmsg_records_out)
{
	unsigned long required_size = 0;
	unsigned long kmsg_area_size = 0;
	unsigned long kmsg_records = 0;
	unsigned long available_space;
	unsigned long record_size;

	if (conf->pmsg_size > 0)
		required_size += conf->pmsg_size;
	if (conf->console_size > 0)
		required_size += conf->console_size;
	if (conf->ftrace_size > 0)
		required_size += conf->ftrace_size;

	if (conf->kmsg_size > 0)
		kmsg_area_size = conf->kmsg_size;
	required_size += kmsg_area_size;

	if (cxt->size < required_size) {
		dev_err(&cxt->card->dev,
			"Effective pstore area too small (%llu < %lu bytes)\n",
			cxt->size, required_size);
		dev_err(&cxt->card->dev,
			"Required: kmsg=%lu KB, pmsg=%lu KB, console=%lu KB, ftrace=%lu KB\n",
			conf->kmsg_size / 1024,
			conf->pmsg_size / 1024, conf->console_size / 1024,
			conf->ftrace_size / 1024);
		return -EINVAL;
	}

	/* Calculate kmsg_records based on available space */
	if (conf->kmsg_size > 0) {
		available_space = cxt->size - (required_size - kmsg_area_size);
		/* Each kmsg record needs space for data + metadata */
		record_size = conf->kmsg_size + MMC_PSTORE_METADATA_SIZE;

		kmsg_records = available_space / record_size;
		pr_debug("Space calculation: total=%llu, available=%lu, record_size=%lu, records=%lu\n",
			cxt->size, available_space, record_size, kmsg_records);

		/* Check if we have enough space for at least one record */
		if (kmsg_records < 1) {
			dev_err(&cxt->card->dev,
				"Insufficient space for kmsg records: available=%lu bytes, needed=%lu bytes per record\n",
				available_space, record_size);
			return -EINVAL;
		}
	}

	*required_size_out = required_size;
	*kmsg_records_out = kmsg_records;

	return 0;
}

/**
 * mmcpstore_extract_base_device - Extract base device name from partition name
 * @devname: Device or partition name (e.g., "mmcblk1p5" or "mmcblk1")
 * @base_name: Buffer to store the base device name
 * @base_size: Size of the base_name buffer
 *
 * Extract the base device name from a device or partition name.
 * For example: "mmcblk1p5" -> "mmcblk1", "mmcblk1" -> "mmcblk1"
 *
 * Returns: 0 on success, negative error on failure
 */
static int mmcpstore_extract_base_device(const char *devname, char *base_name, size_t base_size)
{
	const char *p = devname;
	const char *partition_char;
	size_t len;

	if (!devname || !base_name || base_size == 0)
		return -EINVAL;

	/* Skip "/dev/" prefix if present */
	if (strncmp(p, MMC_PSTORE_DEV_PREFIX, MMC_PSTORE_DEV_PREFIX_LEN) == 0)
		p += MMC_PSTORE_DEV_PREFIX_LEN;

	/* Find the 'p' that indicates partition (e.g., mmcblk1p5) */
	partition_char = strstr(p, "p");
	if (partition_char && partition_char > p &&
	    isdigit(*(partition_char - 1)) && isdigit(*(partition_char + 1))) {
		/* This looks like a partition name */
		len = partition_char - p;
	} else {
		/* This is likely the base device name already */
		len = strlen(p);
	}

	if (len >= base_size)
		return -ENAMETOOLONG;

	memcpy(base_name, p, len);
	base_name[len] = '\0';

	return 0;
}

/**
 * mmcpstore_extract_partno - Extract partition number from device name
 * @devname: Device name (e.g., "/dev/mmcblk1p5" or "mmcblk1p5")
 *
 * Returns: partition number (>= 0) on success, -1 if no partition found
 */
static int mmcpstore_extract_partno(const char *devname)
{
	const char *p = devname;
	const char *pp;

	if (strncmp(p, MMC_PSTORE_DEV_PREFIX, MMC_PSTORE_DEV_PREFIX_LEN) == 0)
		p += MMC_PSTORE_DEV_PREFIX_LEN;

	/* Find the 'p' separator between disk name and partition number */
	for (pp = p + strlen(p) - 1; pp > p && isdigit(*pp); pp--)
		;

	if (*pp == 'p' && pp > p && isdigit(*(pp - 1))) {
		unsigned long partno;
		int ret;

		ret = kstrtoul(pp + 1, 10, &partno);
		if (ret)
			return -1;
		return partno;
	}

	return -1;
}

/**
 * mmcpstore_register_for_card - Register pstore for specific MMC card
 * @card: MMC card to register pstore for
 * @dev_name_param: Device path (e.g., "/dev/mmcblk1p5")
 * @disk: gendisk for the card (non-NULL for builtin path, NULL for module)
 *
 * Returns: 0 on success, negative error on failure
 */
static int mmcpstore_register_for_card(struct mmc_card *card,
				       const char *dev_name_param,
				       struct gendisk *disk)
{
	struct mmcpstore_context *cxt;
	struct pstore_blk_config conf;
	struct file *bdev_file;
	struct block_device *bdev;
	sector_t partition_sectors;
	unsigned long required_size;
	unsigned long kmsg_records;
	int ret;

	/*
	 * Open the block device to get partition start sector and size.
	 * When called from mmc_blk_probe() (builtin path), /dev/ may not
	 * be mounted yet, so use the gendisk + partition number to look
	 * up the dev_t directly.
	 */
	if (disk) {
		int partno = mmcpstore_extract_partno(dev_name_param);
		dev_t devt;

		if (partno < 0) {
			pr_warn("Cannot parse partition from %s\n",
				dev_name_param);
			return -EINVAL;
		}
		devt = part_devt(disk, partno);
		if (!devt) {
			pr_warn("Partition %d not found on %s\n",
				partno, disk->disk_name);
			return -ENODEV;
		}
		bdev_file = bdev_file_open_by_dev(devt, BLK_OPEN_READ,
						  NULL, NULL);
	} else {
		bdev_file = bdev_file_open_by_path(dev_name_param,
						   BLK_OPEN_READ, NULL, NULL);
	}
	if (IS_ERR(bdev_file)) {
		ret = PTR_ERR(bdev_file);
		pr_warn("Failed to open device %s: %d\n", dev_name_param, ret);
		return ret;
	}
	bdev = file_bdev(bdev_file);

	cxt = mmcpstore_ctx;
	if (!cxt) {
		pr_err("No context available\n");
		fput(bdev_file);
		return -ENODEV;
	}

	/* pre-allocated buffer for alignment handling */
	cxt->buffer_size = MMC_PSTORE_MAX_BUFFER_SIZE;
	cxt->buffer = kmalloc(cxt->buffer_size, GFP_KERNEL);
	if (!cxt->buffer) {
		pr_err("Failed to allocate %zu bytes for pstore buffer\n",
		       cxt->buffer_size);
		fput(bdev_file);
		return -ENOMEM;
	}

	if (!(card->host->caps & MMC_CAP_NONREMOVABLE)) {
		dev_err(&card->dev, "MMC pstore only supports non-removable cards (eMMC)\n");
		dev_err(&card->dev, "This card is removable and not suitable for pstore\n");
		ret = -EOPNOTSUPP;
		goto err_free;
	}

	/* Initialize context */
	cxt->card = card;
	strscpy(cxt->card_name, dev_name_param, sizeof(cxt->card_name));
	cxt->start_sect = bdev->bd_start_sect;
	cxt->size = bdev_nr_bytes(bdev);

	/* Apply user-specified sector offset and count within the partition */
	if (part_sect_ofs > 0 || part_sect_cnt > 0) {
		if ((part_sect_ofs > 0 && part_sect_cnt == 0) ||
		    (part_sect_ofs == 0 && part_sect_cnt > 0)) {
			dev_err(&card->dev, "sector_offset and sector_count must be specified\n");
			ret = -EINVAL;
			goto err_free;
		}

		/* Validate offset and count */
		partition_sectors = cxt->size >> 9;

		if (part_sect_ofs >= partition_sectors) {
			dev_err(&card->dev, "Sector offset %lu >= partition size %llu sectors\n",
				part_sect_ofs, partition_sectors);
			ret = -EINVAL;
			goto err_free;
		}

		if (part_sect_ofs + part_sect_cnt > partition_sectors) {
			dev_err(&card->dev, "Sector range %lu-%lu exceeds partition size %llu sectors\n",
				part_sect_ofs,
				part_sect_ofs + part_sect_cnt - 1,
				partition_sectors);
			ret = -EINVAL;
			goto err_free;
		}

		cxt->start_sect += part_sect_ofs;
		cxt->size = part_sect_cnt * MMC_PSTORE_SECTOR_SIZE;

		pr_info("Pstore will use: sectors %lu-%lu (%llu bytes total)\n",
			part_sect_ofs, part_sect_ofs + part_sect_cnt - 1,
			cxt->size);
	}

	fput(bdev_file);

	/* Configure pstore */
	memset(&conf, 0, sizeof(conf));
	strscpy(conf.device, dev_name_param, sizeof(conf.device));
	conf.max_reason = KMSG_DUMP_PANIC;

	/* Fetch pstore configuration including sizes from module parameters */
	ret = pstore_blk_get_config(&conf);
	if (ret != 0) {
		pr_err("Failed to get pstore block config: %d\n", ret);
		goto err_free;
	}

	/* Validate size requirements */
	ret = mmcpstore_calculate_sizes(cxt, &conf, &required_size,
					&kmsg_records);
	if (ret)
		goto err_free;

	pr_debug("Pstore requirements: kmsg=%lu KB, pmsg=%lu KB, console=%lu KB, ftrace=%lu KB\n",
		MMC_PSTORE_BYTES_TO_KB(conf.kmsg_size),
		MMC_PSTORE_BYTES_TO_KB(conf.pmsg_size),
		MMC_PSTORE_BYTES_TO_KB(conf.console_size),
		MMC_PSTORE_BYTES_TO_KB(conf.ftrace_size));
	pr_debug("Pstore capacity: ~%lu kmsg records possible (%lu KB each, varies with compression)\n",
		kmsg_records, MMC_PSTORE_BYTES_TO_KB(conf.kmsg_size));

	/* Set up pstore device info */
	cxt->dev.flags = 0; /* Support all backends */
	if (conf.kmsg_size > 0)
		cxt->dev.flags |= PSTORE_FLAGS_DMESG;
	if (conf.pmsg_size > 0)
		cxt->dev.flags |= PSTORE_FLAGS_PMSG;
	if (conf.console_size > 0)
		cxt->dev.flags |= PSTORE_FLAGS_CONSOLE;
	if (conf.ftrace_size > 0)
		cxt->dev.flags |= PSTORE_FLAGS_FTRACE;

	/* Set up zone structure for pstore/zone API */
	cxt->dev.zone.read = mmcpstore_read_zone;
	cxt->dev.zone.write = mmcpstore_write;
	cxt->dev.zone.panic_write = mmcpstore_panic_write;
	cxt->dev.zone.total_size = cxt->size;
	cxt->dev.zone.kmsg_size = conf.kmsg_size;
	cxt->dev.zone.pmsg_size = conf.pmsg_size;
	cxt->dev.zone.console_size = conf.console_size;
	cxt->dev.zone.ftrace_size = conf.ftrace_size;
	cxt->dev.zone.max_reason = conf.max_reason;
	cxt->dev.zone.name = "mmcpstore";
	cxt->dev.zone.owner = THIS_MODULE;

	mutex_init(&cxt->lock);

	/*
	 * Set state to READY before registration because register_pstore_device()
	 * triggers pstore_zone recovery which calls our read callback, and the
	 * read callback requires state == READY to proceed.
	 */
	mutex_lock(&mmcpstore_global_lock);
	cxt->state = MMCPSTORE_STATE_READY;
	mutex_unlock(&mmcpstore_global_lock);

	ret = register_pstore_device(&cxt->dev);
	if (ret) {
		dev_err(&card->dev, "Failed to register pstore device: %d\n",
			ret);
		mutex_lock(&mmcpstore_global_lock);
		cxt->state = MMCPSTORE_STATE_UNINITIALIZED;
		mutex_unlock(&mmcpstore_global_lock);
		goto err_free;
	}

	dev_info(&card->dev, "MMC pstore backend registered successfully\n");
	dev_info(&card->dev, "Device: %s, Size: %llu bytes (%llu KB)\n",
		 dev_name_param, cxt->size, MMC_PSTORE_BYTES_TO_KB(cxt->size));
	dev_info(&card->dev, "Start sector: %llu, Sector count: %llu\n",
		 cxt->start_sect, cxt->size / MMC_PSTORE_SECTOR_SIZE);
	dev_info(&card->dev, "Pstore components: kmsg=%lu KB, pmsg=%lu KB, console=%lu KB, ftrace=%lu KB\n",
		 MMC_PSTORE_BYTES_TO_KB(conf.kmsg_size),
		 MMC_PSTORE_BYTES_TO_KB(conf.pmsg_size),
		 MMC_PSTORE_BYTES_TO_KB(conf.console_size),
		 MMC_PSTORE_BYTES_TO_KB(conf.ftrace_size));

	return 0;

err_free:
	kfree(cxt->buffer);
	/* Don't free cxt here - it's the global context */
	return ret;
}

/**
 * mmcpstore_unregister_device - Unregister pstore backend
 */
static void mmcpstore_unregister_device(void)
{
	struct mmcpstore_context *cxt;

	mutex_lock(&mmcpstore_global_lock);

	cxt = mmcpstore_ctx;
	if (!cxt)
		goto out_unlock;

	cxt->state = MMCPSTORE_STATE_SHUTDOWN;
	unregister_pstore_device(&cxt->dev);
	mmcpstore_ctx = NULL;
	kfree(cxt->buffer);
	kfree(cxt);

	pr_info("Self-unregistered MMC pstore backend\n");

out_unlock:
	mutex_unlock(&mmcpstore_global_lock);
}

#ifndef MODULE
/**
 * mmcpstore_card_add - Called when an MMC card is probed (builtin path)
 * @card: The MMC card that was just probed
 * @disk: The gendisk associated with the card's block device
 *
 * Called from mmc_blk_probe() when a new card is detected. Checks if
 * this card matches the configured pstore_blk device and registers
 * the pstore backend if so. Only compiled when CONFIG_MMC_PSTORE=y.
 */
void mmcpstore_card_add(struct mmc_card *card, struct gendisk *disk)
{
	struct pstore_blk_config conf;
	char base_device[MMC_PSTORE_DEVICE_NAME_SIZE];
	char card_dev[MMC_PSTORE_DEVICE_NAME_SIZE];

	if (pstore_blk_get_config(&conf) != 0 || !conf.device[0])
		return;

	if (mmcpstore_extract_base_device(conf.device, base_device,
					  sizeof(base_device)) != 0)
		return;

	snprintf(card_dev, sizeof(card_dev), "mmcblk%d",
		 card->host->index);

	if (strcmp(card_dev, base_device) != 0)
		return;

	if (!(card->host->caps & MMC_CAP_NONREMOVABLE)) {
		pr_info("card %s is removable, skipping pstore\n",
			mmc_card_id(card));
		return;
	}

	if (!mmcpstore_ctx) {
		mmcpstore_ctx = kzalloc_obj(*mmcpstore_ctx, GFP_KERNEL);
		if (!mmcpstore_ctx)
			return;
	}

	mmcpstore_ctx->state = MMCPSTORE_STATE_INITIALIZING;
	mmcpstore_register_for_card(card, conf.device, disk);
}

/**
 * mmcpstore_card_remove - Called when an MMC card is removed (builtin path)
 * @card: The MMC card being removed
 *
 * Called from mmc_blk_remove(). Unregisters pstore if this card
 * was the active pstore backend. Only compiled when CONFIG_MMC_PSTORE=y.
 */
void mmcpstore_card_remove(struct mmc_card *card)
{
	mutex_lock(&mmcpstore_global_lock);
	if (!mmcpstore_ctx || mmcpstore_ctx->card != card) {
		mutex_unlock(&mmcpstore_global_lock);
		return;
	}
	mutex_unlock(&mmcpstore_global_lock);

	mmcpstore_unregister_device();
}
#else /* MODULE */
/**
 * mmcpstore_find_and_register - Find eMMC card and register pstore (module path)
 *
 * When loaded as a module, the eMMC card is already probed. Use
 * mmc_blk_get_card_by_name() to look up the card by device name
 * and register the pstore backend.
 *
 * Returns: 0 on success, negative error on failure
 */
static int mmcpstore_find_and_register(void)
{
	struct pstore_blk_config conf;
	struct mmc_card *card;
	char base_device[MMC_PSTORE_DEVICE_NAME_SIZE];

	if (pstore_blk_get_config(&conf) != 0 || !conf.device[0]) {
		pr_info("no pstore_blk device configured\n");
		return 0;
	}

	if (mmcpstore_extract_base_device(conf.device, base_device,
					  sizeof(base_device)) != 0)
		return -EINVAL;

	card = mmc_blk_get_card_by_name(base_device);
	if (!card) {
		pr_err("MMC device %s not found\n", base_device);
		return -ENODEV;
	}

	if (!(card->host->caps & MMC_CAP_NONREMOVABLE)) {
		pr_info("card %s is removable, skipping pstore\n",
			mmc_card_id(card));
		return -EINVAL;
	}

	mmcpstore_ctx = kzalloc_obj(*mmcpstore_ctx, GFP_KERNEL);
	if (!mmcpstore_ctx)
		return -ENOMEM;

	mmcpstore_ctx->state = MMCPSTORE_STATE_INITIALIZING;
	return mmcpstore_register_for_card(card, conf.device, NULL);
}
#endif /* MODULE */

/**
 * mmcpstore_pm_notifier - PM notifier for early PM detection
 * @nb: Notifier block
 * @action: PM action
 * @unused: Unused parameter
 *
 * Returns: NOTIFY_OK
 */
#ifdef CONFIG_PM_SLEEP
static int mmcpstore_pm_notifier(struct notifier_block *nb, unsigned long action, void *unused)
{
	int ret;

	switch (action) {
	case PM_SUSPEND_PREPARE:
		/* Set state to SUSPENDED early in PM process to block pstore operations */
		mutex_lock(&mmcpstore_global_lock);
		if (mmcpstore_ctx && mmcpstore_ctx->state == MMCPSTORE_STATE_READY) {
			mmcpstore_ctx->state = MMCPSTORE_STATE_SUSPENDED;
			pr_info("PM suspend prepare - blocking pstore operations\n");
		}
		mutex_unlock(&mmcpstore_global_lock);
		break;
	case PM_POST_SUSPEND:
		mutex_lock(&mmcpstore_global_lock);
		if (mmcpstore_ctx && mmcpstore_ctx->state == MMCPSTORE_STATE_SUSPENDED) {
			struct mmc_card *card = mmcpstore_ctx->card;

			if (!card || !card->host) {
				pr_warn("No card present for resume initialization\n");
				mutex_unlock(&mmcpstore_global_lock);
				break;
			}

			/* Force card initialization via runtime PM */
			ret = pm_runtime_get_sync(&card->dev);
			if (ret < 0) {
				pr_err("Failed to runtime resume card: %d\n", ret);
				pm_runtime_put_noidle(&card->dev);
				mutex_unlock(&mmcpstore_global_lock);
				break;
			}

			/* Release PM reference, card stays active due to autosuspend */
			pm_runtime_mark_last_busy(&card->dev);
			pm_runtime_put_autosuspend(&card->dev);

			mmcpstore_ctx->state = MMCPSTORE_STATE_READY;
			pr_info("PM resume: card initialized, pstore operations restored\n");
		}
		mutex_unlock(&mmcpstore_global_lock);
		break;
	default:
		break;
	}
	return NOTIFY_OK;
}
#else
static int mmcpstore_pm_notifier(struct notifier_block *nb, unsigned long action, void *unused)
{
	return NOTIFY_DONE;
}
#endif

static struct notifier_block mmcpstore_pm_nb = {
	.notifier_call = mmcpstore_pm_notifier,
	.priority = INT_MAX, /* Highest priority for early detection */
};

/**
 * mmcpstore_reboot_notifier - Handle system reboot/shutdown
 * @nb: notifier block
 * @action: reboot action (SYS_RESTART, SYS_HALT, SYS_POWER_OFF)
 * @data: unused
 *
 * Unregisters pstore backend before system shutdown to prevent
 * hanging during reboot when printk.always_kmsg_dump=1 is set.
 */
static int mmcpstore_reboot_notifier(struct notifier_block *nb, unsigned long action, void *data)
{
	switch (action) {
	case SYS_RESTART:
	case SYS_HALT:
	case SYS_POWER_OFF:
		pr_info("System shutdown detected, unregistering pstore\n");
		mutex_lock(&mmcpstore_global_lock);
		if (mmcpstore_ctx)
			mmcpstore_ctx->state = MMCPSTORE_STATE_SHUTDOWN;
		mutex_unlock(&mmcpstore_global_lock);
		mmcpstore_unregister_device();
		break;
	default:
		break;
	}
	return NOTIFY_DONE;
}

static struct notifier_block mmcpstore_reboot_nb = {
	.notifier_call = mmcpstore_reboot_notifier,
};

/**
 * mmcpstore_init - Initialize MMC pstore driver
 *
 * Returns: 0 on success, negative error on failure
 */
static int __init mmcpstore_init(void)
{
	int ret;

	pr_info("MMC pstore driver initializing\n");

	mmcpstore_ctx = NULL;

	ret = register_reboot_notifier(&mmcpstore_reboot_nb);
	if (ret) {
		pr_err("Failed to register reboot notifier: %d\n", ret);
		return ret;
	}

	ret = register_pm_notifier(&mmcpstore_pm_nb);
	if (ret) {
		pr_err("Failed to register PM notifier: %d\n", ret);
		unregister_reboot_notifier(&mmcpstore_reboot_nb);
		return ret;
	}

#ifdef MODULE
	/*
	 * Module: eMMC card is already probed, look it up and register now.
	 * Builtin: mmc_blk_probe() will call mmcpstore_card_add() later.
	 */
	ret = mmcpstore_find_and_register();
	if (ret) {
		unregister_pm_notifier(&mmcpstore_pm_nb);
		unregister_reboot_notifier(&mmcpstore_reboot_nb);
		return ret;
	}
#endif

	return 0;
}

/**
 * mmcpstore_exit - Cleanup MMC pstore driver
 */
static void __exit mmcpstore_exit(void)
{
	pr_info("Unregistering MMC pstore driver\n");

	unregister_pm_notifier(&mmcpstore_pm_nb);
	unregister_reboot_notifier(&mmcpstore_reboot_nb);

	mutex_lock(&mmcpstore_global_lock);
	if (mmcpstore_ctx) {
		mmcpstore_ctx->state = MMCPSTORE_STATE_SHUTDOWN;
		pr_info("Unregistering active pstore backend during module exit\n");
		unregister_pstore_device(&mmcpstore_ctx->dev);
		kfree(mmcpstore_ctx);
		mmcpstore_ctx = NULL;
	}
	mutex_unlock(&mmcpstore_global_lock);

	pr_info("MMC pstore driver unregistered\n");
}

module_init(mmcpstore_init);
module_exit(mmcpstore_exit);

MODULE_AUTHOR("Kamal Dasu <kamal.dasu@broadcom.com>");
MODULE_DESCRIPTION("MMC pstore backend driver for non-removable cards (eMMC)");
MODULE_LICENSE("GPL");
MODULE_ALIAS("mmcpstore");
