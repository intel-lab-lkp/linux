// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Intel Corporation
 */

#include <linux/acpi.h>
#include <linux/cleanup.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/nvmem-provider.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/time64.h>
#include <linux/unaligned.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

#include <media/ipu-bridge.h>
#include <media/ipu6-pci-table.h>

#include "icvs.h"

/*
 * FW_LOADER_END has to wait for the device to actually finish writing the
 * image to flash and exit ICVS_DEV_STATE_DOWNLOAD. Measured completion time
 * on reference hardware is well under a second, but this is given extra
 * margin over the generic ICVS_CMD_TIMEOUT since flash commit time can
 * vary with image size and flash part across supported devices.
 */
#define FW_END_TIMEOUT		(10 * HZ)
#define FW_READY_DELAY_MS	100

#define ICVS_NVM_MIN_SIZE	SZ_4K
#define ICVS_NVM_MAX_SIZE	SZ_4M

#define PCI_DEVICE_ID_INTEL_IPU7		0x645d	/* MTL / LNL */
#define PCI_DEVICE_ID_INTEL_IPU7P5		0xb05d	/* ARL / PTL */
#define PCI_DEVICE_ID_INTEL_IPU8		0xd719	/* NVL */

/*
 * IPU7 PCI device IDs not covered by ipu6_pci_tbl in ipu6-pci-table.h.
 * Once the IPU6 driver gains support for IPU7, this table can be dropped.
 */
static const struct pci_device_id icvs_ipu7_tbl[] = {
	{ PCI_VDEVICE(INTEL, PCI_DEVICE_ID_INTEL_IPU7) },
	{ PCI_VDEVICE(INTEL, PCI_DEVICE_ID_INTEL_IPU7P5) },
	{ PCI_VDEVICE(INTEL, PCI_DEVICE_ID_INTEL_IPU8) },
	{ }
};

static const struct acpi_gpio_params gpio_wake = { 0, 0, false };
static const struct acpi_gpio_params gpio_rst  = { 1, 0, false };
static const struct acpi_gpio_params gpio_req  = { 2, 0, false };
static const struct acpi_gpio_params gpio_resp = { 3, 0, false };
static const struct acpi_gpio_mapping icvs_acpi_gpios[] = {
	{ "wake-gpio", &gpio_wake, 1 },
	{ "rst-gpio",  &gpio_rst,  1 },
	{ "req-gpio",  &gpio_req,  1 },
	{ "resp-gpio", &gpio_resp, 1 },
	{ }
};

static const struct acpi_gpio_params lgpio_req  = { 0, 0, false };
static const struct acpi_gpio_params lgpio_resp = { 1, 0, false };
static const struct acpi_gpio_mapping icvs_acpi_lgpios[] = {
	{ "req-gpio",  &lgpio_req,  1 },
	{ "resp-gpio", &lgpio_resp, 1 },
	{ }
};

/* Device quirk table */
static const struct icvs_device_quirk cvs_quirk_table[] = {
	{ 0x2ac1, 0x20d0, ICVS_NO_MIPI_CONFIG |
			  ICVS_NO_CAPS |
			  ICVS_NO_FW_UPDATE
	},	/* Lattice NX33 */
	{ 0x06CB, 0x0701, ICVS_SKIP_FW_RESET |
			  ICVS_HOST_SENSOR_PWR_CTRL |
			  ICVS_HOST_PRIV_CTRL |
			  ICVS_FW_BUF_SIZE_256 |
			  ICVS_FW_HEADER_SIZE_256
	},	/* Synaptics SVP7xxx */
	{ }
};

/**
 * cvs_set_quirks - Match device VID/PID and set quirks
 * @ctx: CVS device context
 * @vid: Vendor ID
 * @pid: Product ID
 *
 * Searches the quirk table for a matching VID/PID and populates ctx->quirks
 * with the corresponding quirk flags.
 * If no match is found, quirks is set to 0.
 */
static void cvs_set_quirks(struct icvs *ctx, u16 vid, u16 pid)
{
	ctx->quirks = 0;

	for (unsigned int i = 0; i < ARRAY_SIZE(cvs_quirk_table); i++) {
		if (cvs_quirk_table[i].vid == vid &&
		    cvs_quirk_table[i].pid == pid) {
			ctx->quirks = cvs_quirk_table[i].quirks;
			dev_info(cvs_dev(ctx),
				 "Quirks: 0x%lx (VID:0x%04x PID:0x%04x)\n",
				 ctx->quirks, vid, pid);
			return;
		}
	}

	dev_info(cvs_dev(ctx),
		 "No quirks for device (VID:0x%04x PID:0x%04x)\n", vid, pid);
}

/* I2C transport helpers */

/**
 * cvs_read_i2c - Issue a read-type command and fetch device response
 * @ctx: CVS device context
 * @cmd_id: Command identifier (big endian)
 * @resp: Destination buffer for response payload
 * @size: Size of payload to read into @resp (without prefix)
 *
 * Sends @cmd_id and reads back the response in a single I2C transaction.
 * When the device prepends a 4-byte protocol prefix, the combined
 * prefix+payload is read into a temporary buffer and only the payload is
 * copied to @resp, avoiding any dependency on the layout of the caller's
 * buffer.
 *
 * Return: 0 on success or negative errno.
 */
static int cvs_read_i2c(struct icvs *ctx, __be16 cmd_id, void *resp,
			size_t size)
{
	size_t prefix_size = ctx->prefix ? sizeof(u32) : 0;
	size_t read_size = size + prefix_size;
	struct i2c_client *i2c = ctx->i2c_client;
	u8 *buf __free(kfree) = NULL;
	int cnt;

	if (!resp || !size)
		return -EINVAL;

	cnt = i2c_master_send(i2c, (const char *)&cmd_id, sizeof(cmd_id));
	if (cnt != sizeof(cmd_id))
		return cnt < 0 ? cnt : -EIO;

	buf = kmalloc(read_size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	cnt = i2c_master_recv(i2c, buf, read_size);
	if (cnt != read_size) {
		dev_dbg(cvs_dev(ctx), "recv cmd 0x%04x short read (%d/%zu)\n",
			be16_to_cpu(cmd_id), cnt, read_size);
		return cnt < 0 ? cnt : -EIO;
	}

	memcpy(resp, buf + prefix_size, size);

	return 0;
}

/**
 * cvs_write_i2c - Write a raw command buffer to the device over I2C
 * @ctx: CVS device context
 * @data: Buffer containing command + payload
 * @size: Total bytes to write
 *
 * Return: 0 on success or negative errno.
 */
static int cvs_write_i2c(struct icvs *ctx, const void *data, int size)
{
	struct i2c_client *i2c = ctx->i2c_client;
	int cnt;

	if (size < 0 || !data)
		return -EINVAL;

	cnt = i2c_master_send(i2c, data, size);
	if (cnt != size) {
		dev_dbg(cvs_dev(ctx), "send short (%d/%d)\n", cnt, size);
		return cnt < 0 ? cnt : -EIO;
	}

	return 0;
}

/**
 * cvs_checksum - Simple additive checksum helper
 * @data: 32-bit aligned data buffer
 * @len: Length in bytes (multiple of 4)
 *
 * Return: 32-bit additive checksum of the dwords in @data.
 */
static u32 cvs_checksum(const void *data, size_t len)
{
	const u32 *words = data;
	u32 csum = 0;

	if (WARN_ON_ONCE(len % sizeof(u32)))
		return 0;

	for (unsigned int i = 0; i < len / sizeof(u32); i++)
		csum += words[i];

	return csum;
}

/**
 * cvs_schedule_and_wait - Schedule polling work then wait for completion
 * @ctx: CVS device context
 * @work_delay_ms: Delay in milliseconds before polling work executes
 * @wait_jiffies: Timeout (jiffies) to wait for cmd completion
 *
 * Queues ctx->work to run after @work_delay_ms and then waits up to
 * @wait_jiffies for ctx->cmd_completion.
 *
 * Return: 0 on success, -ETIMEDOUT on timeout, negative errno on error.
 */
static int cvs_schedule_and_wait(struct icvs *ctx, unsigned int work_delay_ms,
				 unsigned long wait_jiffies)
{
	int ret;

	schedule_delayed_work(&ctx->work, msecs_to_jiffies(work_delay_ms));
	ret = wait_for_completion_killable_timeout(&ctx->cmd_completion,
						   wait_jiffies);
	if (ret < 0)
		return ret;
	if (!ret)
		return -ETIMEDOUT;

	return 0;
}

/**
 * cvs_wait_wake_or_sleep - Wait for wake IRQ or sleep fallback
 * @ctx: CVS device context
 * @timeout_jiffies: Timeout (jiffies) for wake event on full-cap devices
 * @sleep_ms: Milliseconds to sleep on light-cap devices
 *
 * For full capability devices (ICVS_FULLCAP) waits interruptibly on
 * ctx->hostwake_event until ctx->hostwake_event_arg becomes true or
 * @timeout_jiffies elapses. The flag is cleared after the wait.
 * For light capability devices performs a blocking msleep(@sleep_ms).
 *
 * Return codes normalized for caller switch handling:
 *	<0		: error / interrupted
 *	-ETIMEDOUT	: timeout (wake event not observed)
 *	0		: success (wake observed OR light-cap sleep elapsed)
 *
 * Return: negative errno, -ETIMEDOUT on timeout, 0 on success.
 */
static int cvs_wait_wake_or_sleep(struct icvs *ctx,
				  unsigned long timeout_jiffies,
				  unsigned int sleep_ms)
{
	int ret;

	if (ctx->res == ICVS_FULLCAP) {
		ret = wait_event_interruptible_timeout(ctx->hostwake_event,
						       ctx->hostwake_event_arg,
						       timeout_jiffies);
		ctx->hostwake_event_arg = false;
		if (ret < 0)
			return ret;
		if (!ret)
			return -ETIMEDOUT;
		return 0;
	}

	msleep(sleep_ms);

	return 0; /* treat sleep path as success */
}

/**
 * cvs_config_mipi - Send a HOST_SET_MIPI_CONFIG command
 * @ctx: CVS device context
 * @c: Command container with conf field populated
 * @len: Length of original command structure (unused except for symmetry)
 *
 * Packages @c->param.conf into a cvs_mipi_data_packet including size and
 * checksum, then writes it to the device.
 *
 * Firmware note: the CVS firmware expects the MIPI configuration to be
 * sent as a single transaction, including all relevant parameters and checksum.
 *
 * Return: 0 on success, NO_MIPI_CONFIG or negative errno from I2C write.
 */
static int cvs_config_mipi(struct icvs *ctx, struct icvs_cmd *c, size_t len)
{
	struct icvs_mipi_data_packet pkt = {
		.cmd_id = c->cmd_id,
		.size = sizeof(c->param.conf),
		.crc = cvs_checksum(&c->param.conf, sizeof(c->param.conf)),
		.conf = c->param.conf,
	};

	if (ctx->quirks & ICVS_NO_MIPI_CONFIG)
		return 0;

	return cvs_write_i2c(ctx, &pkt, sizeof(pkt));
}

/**
 * cvs_get_device_state - Query current device state bitfield
 * @ctx: CVS device context
 * @state: Returned state value
 *
 * Issues GET_DEV_STATE and fills @state.
 *
 * Return: 0 on success or negative errno.
 */
static int cvs_get_device_state(struct icvs *ctx, u8 *state)
{
	struct icvs_resp n = {
		.cmd_id = cpu_to_be16(ICVS_GET_DEV_STATE),
	};
	int ret;

	ret = cvs_read_i2c(ctx, n.cmd_id, &n.resp.state, sizeof(n.resp.state));
	if (ret)
		return ret;

	*state = n.resp.state;

	return 0;
}

/**
 * cvs_get_device_caps - Read protocol capabilities
 * @ctx: CVS device context
 * @caps: Capability structure to populate
 *
 * Return: 0 on success or negative errno.
 */
static int cvs_get_device_caps(struct icvs *ctx,
			       struct icvs_dev_capabilities *caps)
{
	struct icvs_resp n = {
		.cmd_id = cpu_to_be16(ICVS_GET_DEV_CAPABILITY),
	};
	int ret;

	if (ctx->quirks & ICVS_NO_CAPS)
		return 0;

	ret = cvs_read_i2c(ctx, n.cmd_id, &n.resp.cap, sizeof(n.resp.cap));
	if (ret)
		return ret;

	*caps = n.resp.cap;

	return 0;
}

/**
 * cvs_hw_init - Probe device for prefix support and apply quirks
 * @ctx: CVS device context
 *
 * Sends GET_DEV_VID_PID and probes for a 32-bit prefix.
 * If it matches ICVS_PREFIX_VAL, sets ctx->prefix for subsequent reads.
 * Then reads VID/PID and applies matching quirks.
 * GET_DEV_VID_PID is supported by all protocol versions.
 *
 * Return: 0 on success or negative errno.
 */
static int cvs_hw_init(struct icvs *ctx)
{
	struct icvs_resp n = { };
	__be16 cmd = cpu_to_be16(ICVS_GET_DEV_VID_PID);
	u32 resp;
	int ret;

	/*
	 * Clear prefix so cvs_read_i2c always reads exactly sizeof(u32) bytes
	 * here, regardless of any value left over from a previous call (e.g.
	 * on resume).
	 */
	ctx->prefix = false;
	ret = cvs_read_i2c(ctx, cmd, &resp, sizeof(resp));
	if (ret)
		return ret;

	ctx->prefix = resp == ICVS_PREFIX_VAL;

	/* Now read VID/PID to apply quirks */
	ret = cvs_read_i2c(ctx, cmd,
			   &n.resp.vid_pid, sizeof(n.resp.vid_pid));
	if (ret)
		return ret;

	cvs_set_quirks(ctx, n.resp.vid_pid.v_id, n.resp.vid_pid.p_id);

	return 0;
}

/**
 * cvs_irq_handler - Wake IRQ handler (full capability devices)
 * @irq: IRQ number
 * @dev_id: Device context pointer
 *
 * Sets a waitqueue flag and wakes up sleeping waiters.
 *
 * Return: IRQ_HANDLED always.
 */
static irqreturn_t cvs_irq_handler(int irq, void *dev_id)
{
	struct icvs *ctx = dev_id;

	ctx->hostwake_event_arg = true;
	wake_up_interruptible(&ctx->hostwake_event);

	return IRQ_HANDLED;
}

/**
 * cvs_reset - Toggle reset GPIO for full capability devices
 * @ctx: CVS device context
 *
 * Drives reset low briefly then high if device resources indicate full
 * capability. Light devices have no reset line.
 */
static void cvs_reset(struct icvs *ctx)
{
	if (ctx->quirks & ICVS_SKIP_FW_RESET)
		return;

	if (ctx->res == ICVS_FULLCAP) {
		gpiod_set_value(ctx->rst, 0);
		fsleep(2000);
		gpiod_set_value(ctx->rst, 1);
	}
}

/**
 * cvs_recv - Delayed work handler polling for command completion
 * @work: Embedded delayed_work member
 *
 * Re-reads device state; if device_busy remains set, re-schedules itself.
 * When ctx->nvm.wait_dl_clear is set (FW_LOADER_END completion), also keeps
 * re-scheduling while ICVS_DEV_STATE_DOWNLOAD remains set, since the
 * device can clear BUSY slightly before it exits download mode.
 * Otherwise stores state into wq_resp and completes the command.
 */
static void cvs_recv(struct work_struct *work)
{
	struct icvs *ctx = container_of(work, struct icvs, work.work);
	u8 state = 0;
	int ret;

	ret = cvs_get_device_state(ctx, &state);
	if (ret < 0) {
		dev_dbg(cvs_dev(ctx), "state read failed: %d\n", ret);
		return;
	}

	if ((state & ICVS_DEV_STATE_BUSY) ||
	    (ctx->nvm.wait_dl_clear && (state & ICVS_DEV_STATE_DOWNLOAD))) {
		dev_dbg(cvs_dev(ctx), "device busy, reschedule\n");
		schedule_delayed_work(&ctx->work,
				      msecs_to_jiffies(FW_READY_DELAY_MS));
		return;
	}

	ctx->wq_resp.resp.state = state;
	complete(&ctx->cmd_completion);
}

/**
 * cvs_send - Common command submission path
 * @ctx: CVS device context
 * @cmd: Command buffer (icvs_cmd) with cmd_id and param populated
 * @len: Buffer length
 * @timeout: Timeout (jiffies) to wait for command completion; unused for
 *	     the GPIO-only ICVS_HOST_SENSOR_OWNER command
 *
 * Dispatches a set of supported commands:
 * - ICVS_SET_DEV_HOST_ID,
 * - ICVS_HOST_SENSOR_OWNER,
 * - ICVS_HOST_SET_MIPI_CONFIG
 * - ICVS_FW_LOADER_*
 *
 * For I2C based commands it sets big-endian cmd ids, writes to the device
 * and waits (via delayed work) for completion or timeout.
 * GPIO based ownership toggles are handled locally.
 *
 * Caller must hold ctx->lock when invoking this function and check for i2c
 * bus availability.
 *
 * Return: 0 on success, negative errno, -EINVAL for unsupported command
 * or status from device in ctx->wq_resp.
 */
int cvs_send(struct icvs *ctx, struct icvs_cmd *cmd, size_t len,
	     unsigned long timeout)
{
	int ret, status = 0;

	lockdep_assert_held(&ctx->lock);

	dev_dbg(cvs_dev(ctx), "send cmd = 0x%04x", be16_to_cpu(cmd->cmd_id));

	reinit_completion(&ctx->cmd_completion);

	switch (be16_to_cpu(cmd->cmd_id)) {
	case ICVS_SET_DEV_HOST_ID:
		cmd->cmd_id = cpu_to_be16(ICVS_SET_DEV_HOST_ID);
		ret = cvs_write_i2c(ctx, cmd, len);
		if (ret < 0)
			break;

		ret = cvs_schedule_and_wait(ctx, FW_READY_DELAY_MS, timeout);
		if (ret < 0)
			break;

		status = ctx->wq_resp.resp.state &
			  ICVS_DEV_STATE_ERROR ? -EINVAL : 0;
		break;
	case ICVS_HOST_SENSOR_OWNER:
		gpiod_set_value_cansleep(ctx->req, cmd->param.param);
		fsleep(FW_READY_DELAY_MS * USEC_PER_MSEC);
		ret = gpiod_get_value_cansleep(ctx->resp);
		status = cmd->param.param == ret ? 0 : -EINVAL;
		ret = 0; /* success */
		break;
	case ICVS_HOST_SET_MIPI_CONFIG:
		cmd->cmd_id = cpu_to_be16(ICVS_HOST_SET_MIPI_CONFIG);
		ret = cvs_config_mipi(ctx, cmd, len);
		if (ret < 0)
			break;

		ret = cvs_schedule_and_wait(ctx, FW_READY_DELAY_MS, timeout);
		status = (ctx->wq_resp.resp.state &
			  ICVS_DEV_STATE_ERROR) ? -EINVAL : 0;
		break;
	case ICVS_FW_LOADER_START:
		cmd->cmd_id = cpu_to_be16(ICVS_FW_LOADER_START);
		ret = cvs_write_i2c(ctx, cmd, len);
		if (ret < 0)
			break;

		ret = cvs_wait_wake_or_sleep(ctx, timeout, FW_READY_DELAY_MS);
		if (ret)
			break;

		ret = cvs_schedule_and_wait(ctx, FW_READY_DELAY_MS, timeout);
		status = (ctx->wq_resp.resp.state &
			  ICVS_DEV_STATE_DOWNLOAD) ? 0 : -EINVAL;
		break;
	case ICVS_FW_LOADER_DATA:
		/* Quirk for older protocols */
		if (ctx->caps.protocol_version_major >= 2 &&
		    ctx->caps.protocol_version_minor >= 2) {
			cmd->cmd_id = cpu_to_be16(ICVS_FW_LOADER_DATA);
			ret = cvs_write_i2c(ctx, cmd, len);
		} else {
			ret = cvs_write_i2c(ctx, &cmd->param,
					    len - sizeof(cmd->cmd_id));
		}

		if (ret < 0)
			return ret;

		ret = cvs_wait_wake_or_sleep(ctx, FW_READY_DELAY_MS,
					     FW_READY_DELAY_MS);
		if (ret)
			break;

		ret = cvs_schedule_and_wait(ctx, FW_READY_DELAY_MS, timeout);
		status = ctx->wq_resp.resp.state &
			  ICVS_DEV_STATE_ERROR ? -EINVAL : 0;
		break;
	case ICVS_FW_LOADER_END:
		cmd->cmd_id = cpu_to_be16(ICVS_FW_LOADER_END);
		ret = cvs_write_i2c(ctx, cmd, len);
		if (ret < 0)
			break;

		ret = cvs_wait_wake_or_sleep(ctx, timeout, FW_READY_DELAY_MS);
		if (ret)
			break;

		ctx->nvm.wait_dl_clear = true;
		ret = cvs_schedule_and_wait(ctx, FW_READY_DELAY_MS, timeout);
		ctx->nvm.wait_dl_clear = false;
		status = !(ctx->wq_resp.resp.state &
			   ICVS_DEV_STATE_DOWNLOAD) ? 0 : -EINVAL;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	if (ret < 0)
		return ret;

	return ctx->wq_resp.status = status;
}

/**
 * cvs_set_link_owner - Switch CSI-2 link ownership between host and device
 * @ctx: CVS device context
 * @owner: Desired owner (ICVS_CSI_LINK_HOST or ICVS_CSI_LINK_CVS)
 *
 * Called from runtime PM callbacks to claim or release the CSI-2 link.
 * Also callable directly for error recovery paths.
 *
 * Return: 0 on success or negative errno.
 */
int cvs_set_link_owner(struct icvs *ctx, enum icvs_csi_link_owner owner)
{
	struct icvs_cmd cmd = {
		.cmd_id = cpu_to_be16(ICVS_HOST_SENSOR_OWNER),
		.param.param = owner,
	};
	size_t cmd_size = sizeof(cmd.cmd_id) + sizeof(cmd.param.param);

	guard(mutex)(&ctx->lock);
	return cvs_send(ctx, &cmd, cmd_size, ICVS_CMD_TIMEOUT);
}

/**
 * cvs_configure_dev_caps - Configure device capability ownership bits
 * @ctx: CVS device context
 *
 * Tells the CVS device which of its features (privacy LED, RGB camera
 * power-up, vision sensing) are controlled by the host, then sends
 * SET_DEV_HOST_ID.
 *
 * Return: 0 on success or negative errno.
 */
static int cvs_configure_dev_caps(struct icvs *ctx)
{
	struct icvs_cmd cmd = { .cmd_id = cpu_to_be16(ICVS_SET_DEV_HOST_ID) };
	size_t sz = sizeof(cmd.cmd_id) + sizeof(cmd.param.host_id);

	if (ctx->quirks & ICVS_NO_CAPS)
		return 0;

	if (ctx->quirks & ICVS_HOST_VISION_SENSING)
		cmd.param.host_id |= ICVS_HOST_ID_VISION_SENSING;
	if (ctx->quirks & ICVS_HOST_PRIV_CTRL)
		cmd.param.host_id |= ICVS_HOST_ID_PRIVACY_LED;
	if (ctx->quirks & ICVS_HOST_SENSOR_PWR_CTRL)
		cmd.param.host_id |= ICVS_HOST_ID_RGBCAMERA_PWRUP;

	guard(mutex)(&ctx->lock);
	return cvs_send(ctx, &cmd, sz, ICVS_CMD_TIMEOUT);
}

/**
 * enum icvs_nvm_auth_mode - nvm_authenticate sysfs write values
 * @ICVS_NVM_AUTH_CLEAR: Clear the staged update state
 * @ICVS_NVM_AUTH_WRITE_AND_AUTH: Validate image, stream, and request reset
 * @ICVS_NVM_AUTH_WRITE_ONLY: Validate and stream image without reset request
 * @ICVS_NVM_AUTH_AUTH_ONLY: Request reset for previously streamed image
 */
enum icvs_nvm_auth_mode {
	ICVS_NVM_AUTH_CLEAR		= 0,
	ICVS_NVM_AUTH_WRITE_AND_AUTH	= 1,
	ICVS_NVM_AUTH_WRITE_ONLY	= 2,
	ICVS_NVM_AUTH_AUTH_ONLY		= 3,
};

/**
 * cvs_nvm_validate - Locate the payload within the staged firmware image
 * @ctx: CVS device context
 *
 * Only the staged image size is checked here. Signature verification and
 * anti-rollback enforcement are performed by the CVS device firmware, which
 * rejects images that fail either check.
 *
 * Return: 0 on success, -ENODATA when no usable image is staged.
 */
static int cvs_nvm_validate(struct icvs *ctx)
{
	struct icvs_nvm *nvm = &ctx->nvm;
	unsigned int header_size = SZ_1K;

	if (ctx->quirks & ICVS_FW_HEADER_SIZE_256)
		header_size = SZ_256;

	if (!nvm->buf || nvm->buf_total_size < ICVS_NVM_MIN_SIZE)
		return -ENODATA;

	/* The ICVS_NVM_MIN_SIZE check above keeps this from underflowing */
	nvm->buf_data_start = nvm->buf + header_size;
	nvm->buf_data_size = nvm->buf_total_size - header_size;

	return 0;
}

/**
 * cvs_do_fw_download - Stream firmware payload to the device over I2C
 * @ctx: CVS device context (ctx->lock held by caller)
 * @buf: Firmware payload (header already removed)
 * @size: Payload size in bytes
 *
 * Sends FW_LOADER_START, streams @buf in FW_LOADER_DATA chunks, then
 * FW_LOADER_END. cvs_send() already waits for and confirms actual flash
 * completion for FW_LOADER_END (via the BUSY/DOWNLOAD state polling in
 * cvs_recv()), so its return value is the final result here.
 *
 * Return: 0 on success or negative errno.
 */
static int cvs_do_fw_download(struct icvs *ctx, const u8 *buf, size_t size)
{
	struct icvs_cmd cmd = { };
	size_t chunk_max, chunk, pos;
	int ret, end_ret;

	if (ctx->quirks & ICVS_FW_BUF_SIZE_256)
		chunk_max = SZ_256;
	else
		chunk_max = SZ_1K;

	void *fw_buf __free(kfree) = kmalloc(chunk_max + sizeof(__be16),
					     GFP_KERNEL);
	if (!fw_buf)
		return -ENOMEM;

	cmd.cmd_id = cpu_to_be16(ICVS_FW_LOADER_START);
	ret = cvs_send(ctx, &cmd, sizeof(cmd.cmd_id), ICVS_CMD_TIMEOUT);
	if (ret < 0)
		return ret;

	for (pos = 0; pos < size; pos += chunk) {
		chunk = min(chunk_max, size - pos);
		put_unaligned_be16(ICVS_FW_LOADER_DATA, fw_buf);
		memcpy(fw_buf + sizeof(__be16), buf + pos, chunk);

		ret = cvs_send(ctx, fw_buf, sizeof(__be16) + chunk,
			       ICVS_CMD_TIMEOUT);
		if (ret < 0) {
			dev_err(cvs_dev(ctx),
				"FW data chunk send failed: %d\n", ret);
			break;
		}
	}

	/* Always send FW_LOADER_END, but keep any earlier DATA error. */
	cmd.cmd_id = cpu_to_be16(ICVS_FW_LOADER_END);
	end_ret = cvs_send(ctx, &cmd, sizeof(cmd.cmd_id), FW_END_TIMEOUT);

	return ret < 0 ? ret : end_ret;
}

/* cvs_nvm_active_read - NVMem read callback for active firmware region */
static int cvs_nvm_active_read(void *priv, unsigned int offset,
			       void *val, size_t bytes)
{
	__be16 op = cpu_to_be16(ICVS_GET_DEV_FW_VERSION);
	struct icvs_fw_version fw_ver = { };
	struct icvs *ctx = priv;
	struct device *dev = cvs_dev(ctx);
	size_t avail = sizeof(fw_ver);
	size_t len;
	int ret;

	if (offset >= avail)
		return -EINVAL;

	PM_RUNTIME_ACQUIRE_IF_ENABLED_AUTOSUSPEND(dev, pm);
	ret = PM_RUNTIME_ACQUIRE_ERR(&pm);
	if (ret)
		return ret;

	scoped_guard(mutex, &ctx->lock) {
		ret = cvs_read_i2c(ctx, op, &fw_ver, sizeof(fw_ver));
		if (ret)
			return ret;
	}

	len = min(bytes, avail - offset);
	memcpy(val, (void *)&fw_ver + offset, len);

	return 0;
}

/*
 * cvs_nvm_write_buf - NVMem write callback for non-active firmware staging
 *
 * Serialized against nvm_authenticate_store() via ctx->lock, since both
 * read and mutate the shared staging buffer and its size/flushed state.
 *
 * offset + bytes is always used as the new total size (rather than only
 * growing it) so that staging a new, smaller image after a larger one
 * does not leave stale trailing bytes from the previous image in the
 * buffer that would otherwise be sent to the device.
 */
static int cvs_nvm_write_buf(void *priv, unsigned int offset,
			     void *val, size_t bytes)
{
	struct icvs *ctx = priv;
	struct icvs_nvm *nvm = &ctx->nvm;

	guard(mutex)(&ctx->lock);

	if (!nvm->buf)
		nvm->buf = vmalloc(ICVS_NVM_MAX_SIZE);
	if (!nvm->buf)
		return -ENOMEM;

	memcpy(nvm->buf + offset, val, bytes);
	nvm->buf_total_size = offset + bytes;
	nvm->flushed = false;

	return 0;
}

/**
 * cvs_nvm_add_active - Register read-only NVMem device for active FW version
 * @ctx: CVS device context
 *
 * Registers an NVMem device named "nvm_active" whose read callback queries
 * the device for its currently running firmware version.
 *
 * Return: 0 on success or negative errno.
 */
static int cvs_nvm_add_active(struct icvs *ctx)
{
	struct nvmem_config config = {
		.name = "nvm_active",
		.id = NVMEM_DEVID_NONE,
		.dev = cvs_dev(ctx),
		.owner = THIS_MODULE,
		.read_only = true,
		.reg_read = cvs_nvm_active_read,
		.priv = ctx,
		.stride = 4,
		.word_size = 4,
		.size = sizeof(struct icvs_fw_version),
	};
	struct nvmem_device *nvmem;

	nvmem = nvmem_register(&config);
	if (IS_ERR(nvmem))
		return PTR_ERR(nvmem);

	ctx->nvm.active = nvmem;

	return 0;
}

/**
 * cvs_nvm_add_non_active - Register write-only NVMem device for FW staging
 * @ctx: CVS device context
 *
 * Registers an NVMem device named "nvm_non_active" that accepts an incoming
 * firmware image written by userspace (e.g. fwupd via dd). The image is
 * buffered in memory until nvm_authenticate triggers the actual download.
 * Access is restricted to root.
 *
 * Return: 0 on success or negative errno.
 */
static int cvs_nvm_add_non_active(struct icvs *ctx)
{
	struct nvmem_config config = {
		.name = "nvm_non_active",
		.id = NVMEM_DEVID_NONE,
		.dev = cvs_dev(ctx),
		.owner = THIS_MODULE,
		.root_only = true,
		.reg_write = cvs_nvm_write_buf,
		.priv = ctx,
		.stride = 4,
		.word_size = 4,
		.size = ICVS_NVM_MAX_SIZE,
	};
	struct nvmem_device *nvmem;

	nvmem = nvmem_register(&config);
	if (IS_ERR(nvmem))
		return PTR_ERR(nvmem);

	ctx->nvm.non_active = nvmem;

	return 0;
}

/**
 * cvs_nvm_release_buf - Free the firmware staging buffer
 * @nvm: NVM state to release the buffer from
 *
 * Frees the vmalloc'd staging buffer once it is no longer needed, either
 * right after a successful download or during teardown. Does not touch
 * @nvm->flushed, since a successfully streamed image must stay reflected
 * there even after the buffer backing it has been released.
 */
static void cvs_nvm_release_buf(struct icvs_nvm *nvm)
{
	vfree(nvm->buf);
	nvm->buf = NULL;
	nvm->buf_total_size = 0;
	nvm->buf_data_start = NULL;
	nvm->buf_data_size = 0;
}

/**
 * cvs_nvm_free - Release firmware staging buffer
 * @ctx: CVS device context
 *
 * Frees the vmalloc'd buffer used to stage the incoming firmware image
 * and resets all associated NVM state fields.
 */
static void cvs_nvm_free(struct icvs *ctx)
{
	nvmem_unregister(ctx->nvm.non_active);
	ctx->nvm.non_active = NULL;
	nvmem_unregister(ctx->nvm.active);
	ctx->nvm.active = NULL;
	cvs_nvm_release_buf(&ctx->nvm);
	ctx->nvm.flushed = false;
}

static ssize_t nvm_version_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	__be16 op = cpu_to_be16(ICVS_GET_DEV_FW_VERSION);
	struct icvs *ctx = dev_get_drvdata(dev);
	struct icvs_fw_version fw_ver = { };
	int ret;

	PM_RUNTIME_ACQUIRE_IF_ENABLED_AUTOSUSPEND(dev, pm);
	ret = PM_RUNTIME_ACQUIRE_ERR(&pm);
	if (ret)
		return ret;

	scoped_guard(mutex, &ctx->lock) {
		ret = cvs_read_i2c(ctx, op, &fw_ver, sizeof(fw_ver));
		if (ret)
			return ret;
	}

	return sysfs_emit(buf, "%u.%u\n", fw_ver.major, fw_ver.minor);
}
static DEVICE_ATTR_RO(nvm_version);

static ssize_t nvm_authenticate_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct icvs *ctx = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%#x\n", ctx->nvm.auth_status);
}

static ssize_t nvm_authenticate_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct icvs *ctx = dev_get_drvdata(dev);
	bool do_uevent = false;
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 0, &val);
	if (ret)
		return ret;
	if (val > ICVS_NVM_AUTH_AUTH_ONLY)
		return -EINVAL;

	PM_RUNTIME_ACQUIRE_IF_ENABLED_AUTOSUSPEND(dev, pm);
	ret = PM_RUNTIME_ACQUIRE_ERR(&pm);
	if (ret)
		return ret;

	mutex_lock(&ctx->lock);
	ctx->nvm.auth_status = 0;

	switch (val) {
	case ICVS_NVM_AUTH_CLEAR:
		ctx->nvm.flushed = false;
		break;

	case ICVS_NVM_AUTH_WRITE_ONLY:
	case ICVS_NVM_AUTH_WRITE_AND_AUTH:
		ret = cvs_nvm_validate(ctx);
		if (ret)
			goto err_status;

		ret = cvs_do_fw_download(ctx, ctx->nvm.buf_data_start,
					 ctx->nvm.buf_data_size);
		if (ret)
			goto err_status;

		ctx->nvm.flushed = true;
		cvs_nvm_release_buf(&ctx->nvm);

		if (val == ICVS_NVM_AUTH_WRITE_ONLY)
			break;

		fallthrough;

	case ICVS_NVM_AUTH_AUTH_ONLY:
		if (!ctx->nvm.flushed) {
			ret = -ENODATA;
			goto err_status;
		}

		do_uevent = true;
		break;
	}

	mutex_unlock(&ctx->lock);

	if (do_uevent)
		kobject_uevent(&dev->kobj, KOBJ_CHANGE);

	return count;

err_status:
	ctx->nvm.auth_status = -ret;
	mutex_unlock(&ctx->lock);

	return ret;
}
static DEVICE_ATTR_RW(nvm_authenticate);

static ssize_t device_id_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct icvs *ctx = dev_get_drvdata(dev);
	struct icvs_resp n = { };
	int ret;

	PM_RUNTIME_ACQUIRE_IF_ENABLED_AUTOSUSPEND(dev, pm);
	ret = PM_RUNTIME_ACQUIRE_ERR(&pm);
	if (ret)
		return ret;

	n.cmd_id = cpu_to_be16(ICVS_GET_DEV_VID_PID);
	scoped_guard(mutex, &ctx->lock) {
		ret = cvs_read_i2c(ctx, n.cmd_id, &n.resp.vid_pid,
				   sizeof(n.resp.vid_pid));
		if (ret)
			return ret;
	}

	return sysfs_emit(buf, "%04x:%04x\n",
			  n.resp.vid_pid.v_id, n.resp.vid_pid.p_id);
}
static DEVICE_ATTR_RO(device_id);

static umode_t cvs_fw_attr_visible(struct kobject *kobj,
				   const struct attribute *attr, int n)
{
	struct icvs *ctx = dev_get_drvdata(kobj_to_dev(kobj));

	/* Every attribute in this group needs the I2C transport */
	if (!ctx->i2c_client)
		return 0;

	if ((ctx->quirks & ICVS_NO_FW_UPDATE) &&
	    attr == &dev_attr_nvm_authenticate.attr)
		return 0;

	return attr->mode;
}

static const struct attribute *const cvs_fw_attrs[] = {
	&dev_attr_nvm_version.attr,
	&dev_attr_nvm_authenticate.attr,
	&dev_attr_device_id.attr,
	NULL
};

static const struct attribute_group cvs_fw_group = {
	.attrs_const = cvs_fw_attrs,
	.is_visible_const = cvs_fw_attr_visible,
};

static const struct attribute_group *cvs_fw_groups[] = {
	&cvs_fw_group,
	NULL
};

/**
 * cvs_core_probe - Shared probe path for I2C & platform instantiation
 * @dev: Parent device
 * @i2c: I2C client (NULL for platform devices)
 *
 * Discovers IPU, parses ACPI resources, sets up GPIOs/IRQs, initializes
 * sub-device (CSI) and host identifier, and exposes sysfs firmware interface.
 *
 * Return: 0 on success or negative errno.
 */
static int cvs_core_probe(struct device *dev, struct i2c_client *i2c)
{
	struct pci_dev *ipu = NULL;
	struct icvs *ctx;
	int ret;

	/* Locate IPU device */
	for (unsigned int i = 0; !ipu && ipu6_pci_tbl[i].vendor; i++)
		ipu = pci_get_device(ipu6_pci_tbl[i].vendor,
				     ipu6_pci_tbl[i].device, NULL);
	for (unsigned int i = 0; !ipu && icvs_ipu7_tbl[i].vendor; i++)
		ipu = pci_get_device(icvs_ipu7_tbl[i].vendor,
				     icvs_ipu7_tbl[i].device, NULL);
	if (!ipu)
		return -ENODEV;

	ret = ipu_bridge_init(&ipu->dev, ipu_bridge_parse_ssdb);
	if (ret < 0)
		goto err_put_ipu;

	if (!dev_fwnode(dev)) {
		ret = -ENXIO;
		goto err_put_ipu;
	}

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		ret = -ENOMEM;
		goto err_put_ipu;
	}

	ctx->i2c_client = i2c;

	ret = gpiod_count(dev, NULL);
	switch (ret) {
	case ICVS_GPIO_SYNC:
		ctx->res = ICVS_LIGHTCAP;
		break;
	case ICVS_GPIO_ASYNC:
		ctx->res = ICVS_FULLCAP;
		break;
	default:
		dev_err(dev, "unexpected GPIO count %d\n", ret);
		ret = -EINVAL;
		goto err_put_ipu;
	}

	ret = devm_acpi_dev_add_driver_gpios(dev,
					     ctx->res == ICVS_FULLCAP ?
					     icvs_acpi_gpios :
					     icvs_acpi_lgpios);
	if (ret) {
		dev_err_probe(dev, ret, "failed to add ACPI GPIOs\n");
		goto err_put_ipu;
	}

	ctx->req = devm_gpiod_get(dev, "req", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->req)) {
		ret = dev_err_probe(dev, PTR_ERR(ctx->req),
				    "failed to get req GPIO\n");
		goto err_put_ipu;
	}

	ctx->resp = devm_gpiod_get(dev, "resp", GPIOD_IN);
	if (IS_ERR(ctx->resp)) {
		ret = dev_err_probe(dev, PTR_ERR(ctx->resp),
				    "failed to get resp GPIO\n");
		goto err_put_ipu;
	}

	if (ctx->res == ICVS_FULLCAP) {
		struct gpio_desc *wake;

		ctx->rst = devm_gpiod_get(dev, "rst", GPIOD_OUT_HIGH);
		if (IS_ERR(ctx->rst)) {
			ret = dev_err_probe(dev, PTR_ERR(ctx->rst),
					    "failed to get rst GPIO\n");
			goto err_put_ipu;
		}

		wake = devm_gpiod_get(dev, "wake", GPIOD_IN);
		if (IS_ERR(wake)) {
			ret = dev_err_probe(dev, PTR_ERR(wake),
					    "failed to get wake GPIO\n");
			goto err_put_ipu;
		}

		ctx->irq = gpiod_to_irq(wake);
		if (ctx->irq < 0) {
			ret = dev_err_probe(dev, ctx->irq,
					    "failed to get wake IRQ\n");
			goto err_put_ipu;
		}

		ret = devm_request_threaded_irq(dev, ctx->irq, NULL,
						cvs_irq_handler,
						IRQF_ONESHOT | IRQF_NO_SUSPEND,
						"cvs_wake", ctx);
		if (ret) {
			dev_err_probe(dev, ret, "failed to request IRQ\n");
			goto err_put_ipu;
		}
	}

	ret = devm_mutex_init(dev, &ctx->lock);
	if (ret)
		goto err_put_ipu;

	init_completion(&ctx->cmd_completion);
	init_waitqueue_head(&ctx->hostwake_event);
	INIT_DELAYED_WORK(&ctx->work, cvs_recv);

	if (i2c) {
		ret = cvs_hw_init(ctx);
		if (ret) {
			dev_err(dev, "HW init failed (%d)\n", ret);
			/*
			 * Fallback to GPIO-only mode.
			 * Some BIOS show the device on the I2C bus, however,
			 * the device is not accessible via I2C.
			 */
			ctx->i2c_client = NULL;
			goto fail_i2c;
		}

		ret = cvs_get_device_caps(ctx, &ctx->caps);
		if (ret) {
			dev_err_probe(dev, ret, "get caps failed\n");
			goto err_put_ipu;
		}

		ret = cvs_configure_dev_caps(ctx);
		if (ret) {
			dev_err_probe(dev, ret,
				      "configure dev caps failed\n");
			goto err_put_ipu;
		}
	}

fail_i2c:
	ret = cvs_csi_init(ctx, dev, i2c);
	if (ret) {
		dev_err_probe(dev, ret, "CSI init failed\n");
		goto err_put_ipu;
	}

	dev_set_drvdata(dev, ctx);
	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_enable(dev);
	pm_runtime_idle(dev);

	if (ctx->i2c_client) {
		ret = cvs_nvm_add_active(ctx);
		if (ret) {
			dev_err_probe(dev, ret, "NVM active register failed\n");
			goto err_csi_remove;
		}

		ret = cvs_nvm_add_non_active(ctx);
		if (ret) {
			dev_err_probe(dev, ret,
				      "NVM non-active register failed\n");
			goto err_csi_remove;
		}
	}

	/*
	 * Create a PM runtime device link with IPU as consumer and CVS as
	 * supplier. When the IPU runtime-resumes to start streaming, the PM
	 * framework automatically resumes CVS first, triggering
	 * cvs_runtime_resume() which hands CSI-2 link ownership to the host.
	 * Kept as the last probe step so it is only established once the
	 * device is otherwise fully initialized.
	 */
	ctx->ipu_link = device_link_add(&ipu->dev, dev,
					DL_FLAG_PM_RUNTIME |
					DL_FLAG_RPM_ACTIVE |
					DL_FLAG_STATELESS);
	if (!ctx->ipu_link) {
		dev_err(dev, "IPU device link failed\n");
		ret = -ENODEV;
		goto err_csi_remove;
	}

	if (has_acpi_companion(dev))
		acpi_dev_clear_dependencies(ACPI_COMPANION(dev));

	put_device(&ipu->dev);

	return 0;

err_csi_remove:
	cvs_nvm_free(ctx);
	if (ctx->ipu_link)
		device_link_del(ctx->ipu_link);
	cvs_csi_remove(ctx);
	pm_runtime_dont_use_autosuspend(dev);
	pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);

err_put_ipu:
	put_device(&ipu->dev);

	return ret;
}

/**
 * cvs_probe - I2C driver probe entry
 * @i2c: I2C client
 *
 * Return: 0 on success or negative errno.
 */
static int cvs_probe(struct i2c_client *i2c)
{
	return cvs_core_probe(&i2c->dev, i2c);
}

/**
 * cvs_core_remove - Shared remove logic
 * @dev: Device
 */
static void cvs_core_remove(struct device *dev)
{
	struct icvs *ctx = dev_get_drvdata(dev);

	cancel_delayed_work_sync(&ctx->work);
	cvs_csi_remove(ctx);
	cvs_nvm_free(ctx);

	if (ctx->ipu_link)
		device_link_del(ctx->ipu_link);

	pm_runtime_put_noidle(dev);
	pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);

	cvs_reset(ctx);
}

/**
 * cvs_remove - I2C driver remove
 * @client: I2C client
 */
static void cvs_remove(struct i2c_client *client)
{
	cvs_core_remove(&client->dev);
}

/**
 * cvs_suspend - System suspend callback
 * @dev: Device
 *
 * Return: 0.
 */
static int __maybe_unused cvs_suspend(struct device *dev)
{
	struct icvs *ctx = dev_get_drvdata(dev);

	cancel_delayed_work_sync(&ctx->work);

	return 0;
}

/**
 * cvs_resume - System resume callback
 * @dev: Device
 *
 * Re-validates I2C link prefix and re-sends host id if transport available.
 *
 * Return: 0 on success or negative errno if I2C check fails.
 */
static int __maybe_unused cvs_resume(struct device *dev)
{
	struct icvs *ctx = dev_get_drvdata(dev);
	int ret;

	if (ctx->i2c_client) {
		ret = cvs_hw_init(ctx);
		if (ret)
			return ret;
		return cvs_configure_dev_caps(ctx);
	}

	return 0;
}

/**
 * cvs_runtime_resume - Runtime PM resume: claim CSI-2 link ownership
 * @dev: Device
 *
 * Triggered automatically when the IPU (consumer) runtime-resumes, because
 * a DL_FLAG_PM_RUNTIME device link makes CVS the supplier. Transfers CSI-2
 * link ownership to the host so the IPU can start receiving sensor frames.
 *
 * Return: 0 on success or negative errno.
 */
static int __maybe_unused cvs_runtime_resume(struct device *dev)
{
	struct icvs *ctx = dev_get_drvdata(dev);

	return cvs_set_link_owner(ctx, ICVS_CSI_LINK_HOST);
}

/**
 * cvs_runtime_suspend - Runtime PM suspend: release CSI-2 link ownership
 * @dev: Device
 *
 * Called when the streaming reference is dropped by cvs_csi_disable_streams
 * via pm_runtime_put_autosuspend. Returns CSI-2 link ownership to CVS firmware.
 *
 * Return: 0 on success or negative errno.
 */
static int __maybe_unused cvs_runtime_suspend(struct device *dev)
{
	struct icvs *ctx = dev_get_drvdata(dev);

	return cvs_set_link_owner(ctx, ICVS_CSI_LINK_CVS);
}

static const struct dev_pm_ops __maybe_unused cvs_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(cvs_suspend, cvs_resume)
	SET_RUNTIME_PM_OPS(cvs_runtime_suspend, cvs_runtime_resume, NULL)
};

static const struct acpi_device_id intel_cvs_acpi_match[] = {
	{ "INTC10DE" }, /* LNL */
	{ "INTC10E0" }, /* ARL */
	{ "INTC10E1" }, /* PTL */
	{ "INTC10FA" }, /* NVL */
	{ }
};
MODULE_DEVICE_TABLE(acpi, intel_cvs_acpi_match);

static struct i2c_driver cvs_driver = {
	.driver = {
		.name = "intel_cvs",
		.acpi_match_table = intel_cvs_acpi_match,
		.dev_groups = cvs_fw_groups,
		.pm = pm_ptr(&cvs_pm_ops),
	},
	.probe = cvs_probe,
	.remove = cvs_remove,
};

static int cvs_platform_probe(struct platform_device *pdev)
{
	return cvs_core_probe(&pdev->dev, NULL);
}

static void cvs_platform_remove(struct platform_device *pdev)
{
	cvs_core_remove(&pdev->dev);
}

/*
 * Platform driver structure.
 *
 * Some platforms may instantiate the CVS device as a platform device
 * without I2C support. This driver binding allows such platforms to use the
 * CVS core functionality (GPIOs, CSI sub-device) without I2C.
 */
static struct platform_driver cvs_platform_driver = {
	.driver = {
		.name = "cvs_platform",
		.acpi_match_table = intel_cvs_acpi_match,
		.dev_groups = cvs_fw_groups,
		.pm = pm_ptr(&cvs_pm_ops),
	},
	.probe = cvs_platform_probe,
	.remove = cvs_platform_remove,
};

/**
 * cvs_init - Module init registering I2C and platform drivers
 *
 * Return: 0 on success or negative errno.
 */
static int __init cvs_init(void)
{
	int ret;

	ret = i2c_add_driver(&cvs_driver);
	if (ret)
		return ret;

	ret = platform_driver_register(&cvs_platform_driver);
	if (ret) {
		i2c_del_driver(&cvs_driver);
		return ret;
	}

	return 0;
}

/**
 * cvs_exit - Module exit unregistering drivers
 */
static void __exit cvs_exit(void)
{
	platform_driver_unregister(&cvs_platform_driver);
	i2c_del_driver(&cvs_driver);
}
module_init(cvs_init);
module_exit(cvs_exit);

MODULE_IMPORT_NS("INTEL_IPU_BRIDGE");
MODULE_AUTHOR("Miguel Vadillo <miguel.vadillo@intel.com>");
MODULE_DESCRIPTION("Intel Vision Sensing Controller driver");
MODULE_LICENSE("GPL");
