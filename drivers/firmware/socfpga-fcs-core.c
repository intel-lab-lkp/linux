// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Altera Corporation
 */

#include <linux/delay.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/firmware/intel/socfpga-fcs.h>
#include <linux/firmware/intel/stratix10-svc-client.h>

#define OWNER_ID_OFFSET				12
#define OWNER_ID_SIZE				8

#define SDOS_DECRYPTION_REPROVISION_KEY_WARN	0x102
#define SDOS_DECRYPTION_NOT_LATEST_KEY_WARN	0x103

#define MSG_RETRY				3
#define RETRY_SLEEP_MS				1

static struct socfpga_fcs_priv *priv;

/**
 * fcs_atf_version_callback() - service-layer callback for the ATF version query
 * @client: pointer to the stratix10-svc client
 * @data: pointer to the service-layer callback data
 *
 * Store the returned Arm Trusted Firmware version (or mailbox error) in @priv
 * and signal completion to the waiting caller.
 */
static void fcs_atf_version_callback(struct stratix10_svc_client *client,
				     struct stratix10_svc_cb_data *data)
{
	struct socfpga_fcs_priv *p = client->priv;

	p->status = data->status;
	if (data->status == BIT(SVC_STATUS_OK)) {
		p->status = 0;
		p->atf_version[0] = *((unsigned int *)data->kaddr1);
		p->atf_version[1] = *((unsigned int *)data->kaddr2);
		p->atf_version[2] = *((unsigned int *)data->kaddr3);
	} else if (data->status == BIT(SVC_STATUS_ERROR)) {
		p->status = *((unsigned int *)data->kaddr1);
		dev_err(client->dev, "mbox_error=0x%x\n", p->status);
	}

	complete(&p->completion);
}

/**
 * fcs_async_callback() - completion callback for an async service request
 * @ptr: pointer to the completion to signal
 */
static void fcs_async_callback(void *ptr)
{
	if (ptr)
		complete(ptr);
}

/**
 * fcs_svc_send_request() - build and send an FCS command to the service layer
 * @command: FCS command code to dispatch
 * @timeout: time to wait for completion, in jiffies
 *
 * Build the service-layer message for @command and send it through the
 * stratix10-svc service driver, using the synchronous path for the ATF version
 * query and the asynchronous mailbox path (with retries) for the remaining
 * commands.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int fcs_svc_send_request(enum fcs_command_code command,
				unsigned long timeout)
{
	struct fcs_cmd_context *k_ctx = &priv->k_ctx;
	struct stratix10_svc_cb_data data;
	void *handle = NULL;
	int status, index;
	int ret = 0;
	struct stratix10_svc_client_msg *msg = kzalloc(sizeof(*msg), GFP_KERNEL);

	priv->status = 0;
	priv->resp = 0;

	switch (command) {
	case FCS_DEV_CRYPTO_OPEN_SESSION:
		pr_debug("Sending command: COMMAND_FCS_CRYPTO_OPEN_SESSION\n");
		msg->command = COMMAND_FCS_CRYPTO_OPEN_SESSION;
		break;

	case FCS_DEV_CRYPTO_CLOSE_SESSION:
		pr_debug("Sending command: COMMAND_FCS_CRYPTO_CLOSE_SESSION with session_id: 0x%x\n",
			 priv->session_id);
		msg->arg[0] = priv->session_id;
		msg->command = COMMAND_FCS_CRYPTO_CLOSE_SESSION;
		break;

	case FCS_DEV_ATF_VERSION:
		pr_debug("Sending command: COMMAND_SMC_ATF_BUILD_VER\n");
		msg->command = COMMAND_SMC_ATF_BUILD_VER;
		break;

	case FCS_DEV_SDOS_DATA_EXT:
		pr_debug("Sending command: COMMAND_FCS_SDOS_DATA_EXT with session_id: 0x%x, context_id: 0x%x, op_mode: 0x%x, own: 0x%llx\n",
			 priv->session_id, k_ctx->sdos.context_id,
			 k_ctx->sdos.op_mode, k_ctx->sdos.own);
		msg->arg[0] = priv->session_id;
		msg->arg[1] = k_ctx->sdos.context_id;
		msg->arg[2] = k_ctx->sdos.op_mode;
		msg->arg[3] = k_ctx->sdos.own;
		msg->payload = k_ctx->sdos.src;
		msg->payload_length = k_ctx->sdos.src_size;
		msg->payload_output = k_ctx->sdos.dst;
		msg->payload_length_output = *k_ctx->sdos.dst_size;
		msg->command = COMMAND_FCS_SDOS_DATA_EXT;
		break;

	default:
		pr_err("Unknown command: 0x%x\n", command);
		ret = -EINVAL;
		break;
	}

	if (ret) {
		kfree(msg);
		return ret;
	}

	if (command == FCS_DEV_ATF_VERSION) {
		reinit_completion(&priv->completion);

		/*
		 * receive_cb is a persistent field on the shared client and
		 * is only consumed by the synchronous stratix10_svc_send()
		 * path. Set it immediately before the send and clear it right
		 * after so it is non-NULL only for the duration of this
		 * transaction. This keeps the callback correct per command and
		 * prevents a future synchronous caller from silently
		 * inheriting a stale fcs_atf_version_callback.
		 */
		priv->client.receive_cb = fcs_atf_version_callback;

		ret = stratix10_svc_send(priv->chan, msg);
		if (ret) {
			pr_err("failed to send message to service channel\n");
			goto fun_ret;
		}

		if (!wait_for_completion_timeout(&priv->completion,
						 msecs_to_jiffies(timeout))) {
			pr_err("svc timeout to get completed status\n");
			ret = -ETIMEDOUT;
		}
fun_ret:
		priv->client.receive_cb = NULL;
		kfree(msg);
		return ret;
	}

	/*
	 * Use the device-lifetime priv->completion as the async callback arg
	 * rather than an on-stack completion: on a timeout/abort this function
	 * returns while the svc layer still holds a pointer to it in the
	 * transaction handle, so a stack object would be freed under it. FCS
	 * serializes commands under priv->lock (one in-flight), so reusing
	 * priv->completion here is safe.
	 */
	reinit_completion(&priv->completion);

	for (index = 0; index < MSG_RETRY; index++) {
		status = stratix10_svc_async_send(priv->chan, msg, &handle,
						  fcs_async_callback,
						  &priv->completion);
		if (status == 0)
			break;
		msleep(RETRY_SLEEP_MS);
	}

	if (!handle || status != 0) {
		pr_err("Failed to send async message\n");
		kfree(msg);
		return -ETIMEDOUT;
	}

	ret = wait_for_completion_io_timeout(&priv->completion,
					     msecs_to_jiffies(timeout));
	if (ret > 0)
		pr_debug("Received async interrupt\n");
	else
		pr_err("timeout occurred while waiting for async message\n");

	ret = stratix10_svc_async_poll(priv->chan, handle, &data);

	if (ret == -EAGAIN) {
		/*
		 * SDM still owns this transaction (STATUS_BUSY). Skip
		 * stratix10_svc_async_done() so its transaction_id is not
		 * recycled while in flight (which would alias responses); the
		 * handle/id are reclaimed at teardown and the caller can retry.
		 * msg is left unfreed to avoid a dangling handle->msg (small,
		 * bounded leak). Safe only while completions are poll-delivered.
		 */
		pr_err("SDM transaction is busy, aborting\n");
		return -EINPROGRESS;
	}

	if (ret) {
		pr_err("Failed to poll async message\n");
		goto out;
	}

	priv->status = data.status;

	if (data.kaddr1)
		priv->resp = *((u32 *)data.kaddr1);
	else
		priv->resp = 0;

out:
	stratix10_svc_async_done(priv->chan, handle);
	kfree(msg);

	return ret;
}

/**
 * fcs_session_open() - open an FCS crypto service session
 * @k_ctx: pointer to the kernel-side FCS command context
 *
 * Request a new session from the SDM, generate the session UUID and copy it,
 * together with the mailbox status, back to user space.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fcs_session_open(struct fcs_cmd_context *const k_ctx)
{
	int ret = 0;

	ret = fcs_svc_send_request(FCS_DEV_CRYPTO_OPEN_SESSION,
				   SVC_FCS_REQUEST_TIMEOUT_MS);
	if (ret) {
		pr_err("Failed to send the cmd=%d,ret=%d\n",
		       FCS_DEV_CRYPTO_OPEN_SESSION, ret);
		return ret;
	}

	if (priv->status) {
		ret = -EIO;
		pr_err("Mailbox error, Failed to open session ret: %d\n", ret);
		goto copy_mbox_status;
	}

	uuid_gen(&priv->uuid_id);

	memcpy(&priv->session_id, &priv->resp, sizeof(priv->session_id));

	ret = copy_to_user(k_ctx->open_session.suuid, &priv->uuid_id,
			   sizeof(uuid_t)) ? -EFAULT : 0;
	if (ret) {
		pr_err("Failed to copy session ID to user suuid addr: %p ret: %d\n",
		       k_ctx->open_session.suuid, ret);
	}

copy_mbox_status:
	if (copy_to_user(k_ctx->error_code_addr, &priv->status,
			 sizeof(priv->status))) {
		pr_err("Failed to copy mail box status code to user\n");
		/* surface the copy failure only if nothing failed earlier */
		if (!ret)
			ret = -EFAULT;
	}

	return ret;
}

/**
 * fcs_session_close() - close an FCS crypto service session
 * @k_ctx: pointer to the kernel-side FCS command context
 *
 * Validate the caller-supplied session UUID, ask the SDM to close the session
 * and copy the mailbox status back to user space.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fcs_session_close(struct fcs_cmd_context *const k_ctx)
{
	int ret = 0;
	struct fcs_cmd_context ctx;

	memcpy(&ctx, k_ctx, sizeof(struct fcs_cmd_context));

	if (!uuid_equal(&priv->uuid_id, &ctx.close_session.suuid)) {
		ret = -EINVAL;
		pr_err("Session UUID Mismatch ret: %d\n", ret);
		return ret;
	}

	ret = fcs_svc_send_request(FCS_DEV_CRYPTO_CLOSE_SESSION,
				   SVC_FCS_REQUEST_TIMEOUT_MS);
	if (ret) {
		pr_err("Failed to send the cmd=%d,ret=%d\n",
		       FCS_DEV_CRYPTO_CLOSE_SESSION, ret);
		return ret;
	}

	memset(&priv->uuid_id, 0, sizeof(uuid_t));
	priv->session_id = 0;
	if (priv->status) {
		ret = -EIO;
		pr_err("Mailbox error, Failed to close session ret: %d\n", ret);
	}

	if (copy_to_user(ctx.error_code_addr, &priv->status,
			 sizeof(priv->status))) {
		pr_err("Failed to copy mail box status code to user\n");
		/* surface the copy failure only if nothing failed earlier */
		if (!ret)
			ret = -EFAULT;
	}

	return ret;
}

/**
 * fcs_get_atf_version() - return the cached Arm Trusted Firmware version
 * @version: array of three u32 entries to receive the major, minor and patch
 *           version numbers
 */
void fcs_get_atf_version(u32 *version)
{
	memcpy(version, priv->atf_version, sizeof(priv->atf_version));
}

/**
 * fcs_sdos_crypt() - perform an SDOS encrypt or decrypt operation
 * @k_ctx: pointer to the kernel-side FCS command context
 *
 * Allocate service-layer source and destination buffers, copy the input from
 * user space, drive the SDOS data command and copy the result and length back
 * to user space. The operation direction is selected by @k_ctx->sdos.op_mode.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fcs_sdos_crypt(struct fcs_cmd_context *const k_ctx)
{
	void *s_buf = NULL, *d_buf = NULL;
	struct fcs_cmd_context ctx;
	u32 output_size;
	u32 dst_cap;
	u64 owner_id;
	int ret = 0;

	memcpy(&ctx, k_ctx, sizeof(struct fcs_cmd_context));

	/* Authorize the caller against the open session before doing any work */
	if (!uuid_equal(&priv->uuid_id, &ctx.sdos.suuid)) {
		pr_err("Session UUID mismatch\n");
		return -EINVAL;
	}

	if (!ctx.sdos.dst || !ctx.sdos.dst_size)
		return -EINVAL;

	/* Caller-provided output buffer capacity (in/out parameter) */
	if (copy_from_user(&dst_cap, ctx.sdos.dst_size, sizeof(dst_cap)))
		return -EFAULT;

	if (ctx.sdos.op_mode) {
		output_size = SDOS_ENCRYPTED_MAX_SZ;
		/* encrypt: input is header + plaintext */
		if (ctx.sdos.src_size < SDOS_DECRYPTED_MIN_SZ ||
		    ctx.sdos.src_size > SDOS_DECRYPTED_MAX_SZ) {
			pr_err("Invalid SDOS src_size %u\n", ctx.sdos.src_size);
			return -EINVAL;
		}
	} else {
		output_size = SDOS_DECRYPTED_MAX_SZ;
		/* decrypt: input is header + plaintext + HMAC */
		if (ctx.sdos.src_size < SDOS_ENCRYPTED_MIN_SZ ||
		    ctx.sdos.src_size > SDOS_ENCRYPTED_MAX_SZ) {
			pr_err("Invalid SDOS src_size %u\n", ctx.sdos.src_size);
			return -EINVAL;
		}
	}

	s_buf = stratix10_svc_allocate_memory(priv->chan, ctx.sdos.src_size);
	if (IS_ERR(s_buf)) {
		ret = -ENOMEM;
		pr_err("Failed to allocate memory for SDOS input data kernel buffer ret: %d\n",
		       ret);
		return ret;
	}

	/*
	 * The remaining k_ctx writes intentionally target priv->k_ctx (k_ctx
	 * points at it): fcs_svc_send_request() reads the outgoing request from
	 * priv->k_ctx, so the kernel buffers and params are staged there rather
	 * than in the local ctx snapshot. dst_size must point at device-lifetime
	 * storage (priv->sdos_output_size), never a caller stack variable, so
	 * the staged pointer cannot dangle after this function returns.
	 */
	priv->sdos_output_size = output_size;
	k_ctx->sdos.dst_size = &priv->sdos_output_size;

	d_buf = stratix10_svc_allocate_memory(priv->chan, output_size);
	if (IS_ERR(d_buf)) {
		ret = -ENOMEM;
		pr_err("Failed to allocate memory for SDOS output kernel buffer ret: %d\n", ret);
		goto free_sbuf;
	}

	/* Copy the user space input data to the input data kernel buffer */
	ret = copy_from_user(s_buf, ctx.sdos.src,
			     ctx.sdos.src_size) ? -EFAULT : 0;
	if (ret) {
		pr_err("Failed to copy SDOS data from user to kernel buffer ret: %d\n", ret);
		goto free_dbuf;
	}

	/* Get Owner ID from buf */
	memcpy(&owner_id, (u8 *)s_buf + OWNER_ID_OFFSET, OWNER_ID_SIZE);
	k_ctx->sdos.own = owner_id;
	k_ctx->sdos.src = s_buf;
	k_ctx->sdos.dst = d_buf;

	ret = fcs_svc_send_request(FCS_DEV_SDOS_DATA_EXT,
				   SVC_FCS_REQUEST_TIMEOUT_MS);

	if (ret == -EINPROGRESS) {
		/*
		 * SDM still owns this transaction and may still DMA into
		 * d_buf. Do NOT free s_buf/d_buf or copy results: returning
		 * them to the gen_pool would let the delayed firmware write
		 * corrupt reallocated memory. Leak them along with the
		 * abandoned transaction (bounded, exceptional stuck-SDM path).
		 */
		return -ETIMEDOUT;
	}

	if (ret) {
		pr_err("Failed to send the cmd=%d,ret=%d\n", FCS_DEV_SDOS_DATA_EXT, ret);
		goto free_dbuf;
	}
	if (priv->status &&
	    priv->status != SDOS_DECRYPTION_REPROVISION_KEY_WARN &&
	    priv->status != SDOS_DECRYPTION_NOT_LATEST_KEY_WARN) {
		pr_err("Failed to perform SDOS operation ret: %d Mailbox Status = %d\n",
		       ret, priv->status);
		goto copy_mbox_status;
	}

	/*
	 * priv->resp is reported by firmware; never trust it to read back
	 * more than the kernel output buffer (d_buf) actually holds,
	 * otherwise the copy below would leak adjacent kernel memory.
	 */
	if (priv->resp > output_size) {
		pr_err("SDOS output %u exceeds kernel buffer %u\n",
		       priv->resp, output_size);
		ret = -EIO;
		goto copy_mbox_status;
	}

	/* Do not write past the caller-provided output buffer */
	if (priv->resp > dst_cap) {
		pr_err("SDOS output %u exceeds caller buffer %u\n",
		       priv->resp, dst_cap);
		ret = -EMSGSIZE;
		goto copy_mbox_status;
	}

	/* Copy the encrypted/decrypted output from kernel space to user space */
	ret = copy_to_user(ctx.sdos.dst, d_buf, priv->resp) ? -EFAULT : 0;
	if (ret) {
		pr_err("Failed to copy encrypted output to user ret: %d\n", ret);
		goto copy_mbox_status;
	}

	/* Copy the encrypted output length from kernel space to user space */
	ret = copy_to_user(ctx.sdos.dst_size, &priv->resp,
			   sizeof(priv->resp)) ? -EFAULT : 0;
	if (ret)
		pr_err("Failed to copy encrypted output length to user ret: %d\n", ret);

copy_mbox_status:
	if (copy_to_user(ctx.error_code_addr, &priv->status,
			 sizeof(priv->status))) {
		pr_err("Failed to copy mailbox status code to user\n");
		/* surface the copy failure only if nothing failed earlier */
		if (!ret)
			ret = -EFAULT;
	}
free_dbuf:
	stratix10_svc_free_memory(priv->chan, d_buf);
free_sbuf:
	stratix10_svc_free_memory(priv->chan, s_buf);

	return ret;
}

/**
 * fcs_acquire_cmd_ctx() - take the FCS lock and return the command context
 *
 * Serialises access to the shared command context across concurrent callers.
 * The caller must release it with fcs_release_cmd_ctx().
 *
 * Return: pointer to the locked FCS command context.
 */
struct fcs_cmd_context *fcs_acquire_cmd_ctx(void)
{
	if (!priv)
		return NULL;

	mutex_lock(&priv->lock);
	return &priv->k_ctx;
}

/**
 * fcs_release_cmd_ctx() - release the FCS command context lock
 * @k_ctx: pointer to the FCS command context previously acquired
 */
void fcs_release_cmd_ctx(struct fcs_cmd_context *const k_ctx)
{
	mutex_unlock(&priv->lock);
}

/**
 * fcs_read_version_from_atf() - query the Arm Trusted Firmware build version
 *
 * Send the ATF version command to the SDM and cache the result in @priv.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int fcs_read_version_from_atf(void)
{
	int ret = 0;

	ret = fcs_svc_send_request(FCS_DEV_ATF_VERSION,
				   SVC_FCS_REQUEST_TIMEOUT_MS);
	if (ret) {
		pr_err("Failed to send the cmd=%d,ret=%d\n", FCS_DEV_ATF_VERSION, ret);
		return ret;
	}

	if (priv->status) {
		ret = -EIO;
		pr_err("Mailbox error, Failed to read ATF version ret: %d\n", ret);
	}

	stratix10_svc_done(priv->chan);

	return ret;
}

/**
 * fcs_init() - allocate and initialise the FCS private state
 * @dev: pointer to fcs device
 *
 * Allocate @priv, request the service channel, register the async client,
 * and read the ATF version.
 *
 * Return: 0 on success, -EPROBE_DEFER or negative errno on failure.
 */
int fcs_init(struct device *dev)
{
	int ret;

	if (priv)
		return -EBUSY;   /* singleton: one FCS instance only */

	priv = devm_kzalloc(dev, sizeof(struct socfpga_fcs_priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	mutex_init(&priv->lock);

	priv->dev = dev;
	priv->client.dev = dev;
	priv->client.receive_cb = NULL;
	priv->client.priv = priv;

	priv->chan = stratix10_svc_request_channel_byname(&priv->client,
							  SVC_CLIENT_FCS);
	if (IS_ERR(priv->chan)) {
		pr_err("couldn't get service channel %s\n", SVC_CLIENT_FCS);
		return -EPROBE_DEFER;
	}

	ret = stratix10_svc_add_async_client(priv->chan, true);
	if (ret) {
		pr_err("Failed to add async client\n");
		goto free_chan;
	}

	init_completion(&priv->completion);

	fcs_read_version_from_atf();

	return 0;

free_chan:
	stratix10_svc_free_channel(priv->chan);

	return ret;
}

/**
 * fcs_deinit() - tear down the FCS private state
 *
 * Close any open session, remove the async client, free the service channel
 * and clear @priv.
 */
void fcs_deinit(void)
{
	if (priv && priv->session_id) {
		int ret = fcs_svc_send_request(FCS_DEV_CRYPTO_CLOSE_SESSION,
					       SVC_FCS_REQUEST_TIMEOUT_MS);
		if (ret)
			pr_err("Failed to close FCS service session,ret=%d\n", ret);
	}

	if (priv) {
		stratix10_svc_remove_async_client(priv->chan);
		stratix10_svc_free_channel(priv->chan);
	}

	priv = NULL;
}

/**
 * fcs_cleanup() - release the FCS service channel and clear the state
 */
void fcs_cleanup(void)
{
	if (priv)
		stratix10_svc_free_channel(priv->chan);

	priv = NULL;
}
