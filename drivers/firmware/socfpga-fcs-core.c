// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Altera Corporation
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/firmware/intel/socfpga-fcs.h>
#include <linux/firmware/intel/stratix10-svc-client.h>

#define OWNER_ID_OFFSET				12

#define SDOS_DECRYPTION_REPROVISION_KEY_WARN	0x102
#define SDOS_DECRYPTION_NOT_LATEST_KEY_WARN	0x103

#define MSG_RETRY				3
#define FCS_RETRY_SLEEP_MS			1

struct fcs_cmd_params {
	const void	*src;
	void		*dst;
	u32		src_len;
	u32		dst_len;
	u32		op_mode;
	u64		own;
};

/**
 * fcs_atf_version_callback() - service-layer callback for the ATF version query
 * @client: pointer to the stratix10-svc client
 * @data: pointer to the service-layer callback data
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
		p->atf_version_valid = true;
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
 * fcs_svc_send_sync() - run a command on the synchronous service path
 * @msg: service-layer message to send
 * @timeout: time to wait for the response
 * Return: 0 on success, negative errno on failure.
 */
static int fcs_svc_send_sync(struct socfpga_fcs_priv *priv,
			     struct stratix10_svc_client_msg *msg,
			     unsigned long timeout)
{
	int ret;

	reinit_completion(&priv->completion);

	/*
	 * receive_cb is only used by the sync send path; leave it set so a
	 * late response cannot find a NULL callback.
	 */
	priv->client.receive_cb = fcs_atf_version_callback;

	ret = stratix10_svc_send(priv->chan, msg);
	if (ret) {
		pr_err("failed to send message to service channel\n");
		priv->client.receive_cb = NULL;
		return ret;
	}

	if (!wait_for_completion_timeout(&priv->completion,
					 msecs_to_jiffies(timeout))) {
		pr_err("svc timeout to get completed status\n");
		return -ETIMEDOUT;
	}

	return 0;
}

/**
 * fcs_svc_send_async() - run a command on the asynchronous mailbox path
 * @msg: service-layer message to send
 * @timeout: time to wait for the response
 *
 * Return: 0 once the transaction completed, negative errno on transport
 *         failure or timeout.
 */
static int fcs_svc_send_async(struct socfpga_fcs_priv *priv,
			      struct stratix10_svc_client_msg *msg,
			      unsigned long timeout)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout);
	struct stratix10_svc_cb_data data;
	void *handle = NULL;
	int status, index;
	int ret;

	/*
	 * Use priv->completion, not a stack one: on timeout this function
	 * returns while the svc layer still holds a pointer to it.
	 */
	reinit_completion(&priv->completion);

	for (index = 0; index < MSG_RETRY; index++) {
		status = stratix10_svc_async_send(priv->chan, msg, &handle,
						  fcs_async_callback,
						  &priv->completion);
		if (status == 0)
			break;
		msleep(FCS_RETRY_SLEEP_MS);
	}

	if (status || !handle) {
		pr_err("Failed to send async message\n");
		/*
		 * A NULL handle with a success status would otherwise be
		 * reported as a completed transaction that never ran.
		 */
		return status ? status : -EIO;
	}

	ret = -ETIMEDOUT;
	while (!time_after(jiffies, deadline)) {
		status = stratix10_svc_async_poll(priv->chan, handle, &data);

		if (status == 0) {
			ret = 0;
			break;
		}

		/*
		 * Keep polling until the deadline. Leaving an in-flight
		 * transaction orphans the SDM crypto session.
		 */
		ret = status;
		msleep(FCS_RETRY_SLEEP_MS);
	}

	if (ret) {
		pr_err("Failed to poll async message\n");
		goto out;
	}

	priv->status = data.status;

	/*
	 * Non-zero SDM status is a firmware result, not a transport failure.
	 * Store it in priv->status and return success to the caller.
	 */
	if (data.status) {
		pr_err("%s: SDM mailbox status 0x%x\n", __func__, data.status);
		goto out;
	}

	if (data.kaddr1)
		priv->resp = *((u32 *)data.kaddr1);

out:
	stratix10_svc_async_done(priv->chan, handle);

	return ret;
}

/**
 * fcs_svc_send_request() - build and send an FCS command to the service layer
 * @command: FCS command code to dispatch
 * @timeout: time to wait for completion, in milliseconds
 * @params: payload and arguments for @command, or NULL for commands that
 *          carry none
 *
 * Return: 0 on success, negative errno on failure.
 */
static int fcs_svc_send_request(struct socfpga_fcs_priv *priv,
				enum fcs_command_code command,
				unsigned long timeout,
				const struct fcs_cmd_params *params)
{
	struct stratix10_svc_client_msg *msg;
	int ret = 0;

	/*
	 * The service layer keeps this message alive in its transaction handle
	 * and still dereferences it from stratix10_svc_async_done(), so it
	 * cannot live on our stack.
	 */
	msg = kzalloc_obj(*msg);
	if (!msg)
		return -ENOMEM;

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
		if (!params) {
			ret = -EINVAL;
			break;
		}
		pr_debug("Sending command: COMMAND_FCS_SDOS_DATA_EXT with session_id: 0x%x, context_id: 0x%x, op_mode: 0x%x, own: 0x%llx\n",
			 priv->session_id, priv->context_id,
			 params->op_mode, params->own);
		msg->arg[0] = priv->session_id;
		msg->arg[1] = priv->context_id;
		msg->arg[2] = params->op_mode;
		msg->arg[3] = params->own;
		msg->payload = (void *)params->src;
		msg->payload_length = params->src_len;
		msg->payload_output = params->dst;
		msg->payload_length_output = params->dst_len;
		msg->command = COMMAND_FCS_SDOS_DATA_EXT;
		break;

	default:
		pr_err("Unknown command: 0x%x\n", command);
		ret = -EINVAL;
		break;
	}

	if (!ret) {
		if (command == FCS_DEV_ATF_VERSION)
			/* ATF fast call for simple command */
			ret = fcs_svc_send_sync(priv, msg, timeout);
		else
			ret = fcs_svc_send_async(priv, msg, timeout);
	}

	kfree(msg);

	return ret;
}

/**
 * fcs_open_session_locked() - open a crypto session on the SDM
 *
 * Enforce the single-session rule and, on success, record the SDM session
 * handle in @priv->session_id. The caller must hold @priv->lock.
 * @priv->status carries the mailbox status.
 *
 * Return: 0 on success, -EBUSY if a session is already open, or negative
 *         errno on transport/mailbox failure.
 */
static int fcs_open_session_locked(struct socfpga_fcs_priv *priv)
{
	int ret;

	lockdep_assert_held(&priv->lock);

	if (priv->session_id)
		/* SDM allows one crypto session at a time */
		return -EBUSY;

	ret = fcs_svc_send_request(priv, FCS_DEV_CRYPTO_OPEN_SESSION,
				   SVC_FCS_REQUEST_TIMEOUT_MS, NULL);
	if (ret)
		return ret;

	if (priv->status)
		return -EIO;

	priv->session_id = priv->resp;

	return 0;
}

/**
 * fcs_close_session_locked() - close the crypto session on the SDM
 *
 * Caller must hold @priv->lock. The local session id is cleared even if
 * close fails, so a stuck session cannot block future opens.
 *
 * Return: 0 on success or when no session is open, negative errno otherwise.
 */
static int fcs_close_session_locked(struct socfpga_fcs_priv *priv)
{
	int ret;

	lockdep_assert_held(&priv->lock);

	if (!priv->session_id)
		/* nothing to close */
		return 0;

	ret = fcs_svc_send_request(priv, FCS_DEV_CRYPTO_CLOSE_SESSION,
				   SVC_FCS_REQUEST_TIMEOUT_MS, NULL);

	priv->session_id = 0;

	if (!ret && priv->status)
		ret = -EIO;

	return ret;
}

/**
 * fcs_ctx_begin() - open a crypto session and start a context on it
 *
 * The SDM runs one crypto context at a time and will not start another until
 * the current one finishes, so the caller must hold @priv->lock for the whole
 * operation. Every command issued in between then picks up
 * @priv->context_id.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int fcs_ctx_begin(struct socfpga_fcs_priv *priv)
{
	int ret;

	lockdep_assert_held(&priv->lock);

	ret = fcs_open_session_locked(priv);
	if (ret)
		return ret;

	/*
	 * SDM requires a non-zero context ID per request. A random value
	 * also avoids mistaking a late response from a retired context.
	 */
	priv->context_id = get_random_u32_above(0);

	return 0;
}

/**
 * fcs_ctx_end() - finish the current context and close the session
 *
 * Closing the session is what reclaims the SDM context, so this runs on every
 * exit path of an operation whether it succeeded or not.
 *
 * Return: 0 on success, negative errno otherwise.
 */
static int fcs_ctx_end(struct socfpga_fcs_priv *priv)
{
	lockdep_assert_held(&priv->lock);

	priv->context_id = 0;

	return fcs_close_session_locked(priv);
}

/**
 * fcs_get_atf_version() - return the cached Arm Trusted Firmware version
 * @version: array of three u32 entries to receive the major, minor and patch
 *           version numbers
 *
 * Return: 0 on success, -ENODEV if the driver is not initialised, -ENODATA if
 *         the probe-time query produced no version.
 */
int fcs_get_atf_version(struct socfpga_fcs_priv *priv, u32 *version)
{
	if (!priv->atf_version_valid)
		return -ENODATA;

	memcpy(version, priv->atf_version, sizeof(priv->atf_version));

	return 0;
}

/**
 * fcs_alloc_buf() - allocate a service-layer buffer for a mailbox payload
 * Wraps the stratix10-svc allocator so front-ends can stage payloads without
 * touching the service channel themselves.
 *
 * @len: size of the buffer in bytes
 * Return: pointer to the buffer, or an ERR_PTR on failure.
 */
void *fcs_alloc_buf(struct socfpga_fcs_priv *priv, size_t len)
{
	return stratix10_svc_allocate_memory(priv->chan, len);
}

/**
 * fcs_free_buf() - release a buffer obtained from fcs_alloc_buf()
 * @buf: buffer to release; NULL and error pointers are ignored
 */
void fcs_free_buf(struct socfpga_fcs_priv *priv, void *buf)
{
	if (!IS_ERR_OR_NULL(buf))
		stratix10_svc_free_memory(priv->chan, buf);
}

/**
 * fcs_sdos_output_size() - validate an SDOS input length and size its output
 * @op_mode: non-zero to encrypt, zero to decrypt
 * @src_len: length of the input, including the SDOS header
 * @out_len: receives the output capacity the SDM may need
 *
 * Return: 0 on success, -EINVAL if @src_len is out of range for @op_mode.
 */
int fcs_sdos_output_size(u32 op_mode, u32 src_len, u32 *out_len)
{
	if (op_mode) {
		/* encrypt: input is header + plaintext */
		if (src_len < SDOS_DECRYPTED_MIN_SZ ||
		    src_len > SDOS_DECRYPTED_MAX_SZ)
			return -EINVAL;

		*out_len = SDOS_ENCRYPTED_MAX_SZ;
	} else {
		/* decrypt: input is header + plaintext + HMAC */
		if (src_len < SDOS_ENCRYPTED_MIN_SZ ||
		    src_len > SDOS_ENCRYPTED_MAX_SZ)
			return -EINVAL;

		*out_len = SDOS_DECRYPTED_MAX_SZ;
	}

	return 0;
}

/**
 * fcs_sdos_crypt() - perform an SDOS encrypt or decrypt operation
 * @req: request describing the operation
 *
 * Return: 0 on success, negative errno on failure.
 */
int fcs_sdos_crypt(struct socfpga_fcs_priv *priv, struct fcs_sdos_req *req)
{
	struct fcs_cmd_params params = { };
	u32 output_size;
	int ret;

	if (!req->src || !req->dst)
		return -EINVAL;

	ret = fcs_sdos_output_size(req->op_mode, req->src_len, &output_size);
	if (ret) {
		pr_err("Invalid SDOS src_size %u\n", req->src_len);
		return ret;
	}

	/* The caller must have sized the output buffer for the worst case. */
	if (req->dst_len < output_size)
		return -EINVAL;

	params.op_mode = req->op_mode;
	params.src = req->src;
	params.src_len = req->src_len;
	params.dst = req->dst;
	params.dst_len = req->dst_len;
	/* Owner ID is stored little-endian in the SDOS header (offset 12) */
	params.own = get_unaligned_le64((const u8 *)req->src + OWNER_ID_OFFSET);

	/*
	 * Only one SDM transaction may be in flight. Wait interruptibly so
	 * a blocked caller remains killable.
	 */
	if (mutex_lock_interruptible(&priv->lock))
		return -ERESTARTSYS;

	/*
	 * The device may have been removed while this caller held only a file
	 * reference.
	 */
	if (priv->removed) {
		mutex_unlock(&priv->lock);
		return -ENODEV;
	}

	/*
	 * SDOS is a single-command request: start a context, run the command
	 * and finish the context before returning.
	 */
	ret = fcs_ctx_begin(priv);
	if (ret) {
		pr_err("SDOS: failed to start crypto context ret: %d\n", ret);
		mutex_unlock(&priv->lock);
		return ret;
	}

	ret = fcs_svc_send_request(priv, FCS_DEV_SDOS_DATA_EXT,
				   SVC_FCS_REQUEST_TIMEOUT_MS, &params);
	if (ret) {
		pr_err("Failed to send the cmd=%d,ret=%d\n", FCS_DEV_SDOS_DATA_EXT, ret);
		goto end_ctx;
	}

	req->status = priv->status;
	req->status_valid = true;

	if (priv->status &&
	    priv->status != SDOS_DECRYPTION_REPROVISION_KEY_WARN &&
	    priv->status != SDOS_DECRYPTION_NOT_LATEST_KEY_WARN) {
		ret = -EIO;
		pr_err("Failed to perform SDOS operation ret: %d Mailbox Status = 0x%x\n",
		       ret, priv->status);
		goto end_ctx;
	}

	if (priv->resp > req->dst_len) {
		pr_err("SDOS output %u exceeds kernel buffer %u\n",
		       priv->resp, req->dst_len);
		ret = -EIO;
		goto end_ctx;
	}

	req->dst_len = priv->resp;

end_ctx:
	/* Best-effort; the local session and context state is dropped regardless. */
	fcs_ctx_end(priv);
	mutex_unlock(&priv->lock);

	return ret;
}

/**
 * fcs_read_version_from_atf() - query the Arm Trusted Firmware build version
 * Send the ATF version command to the SDM and cache the result in @priv.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int fcs_read_version_from_atf(struct socfpga_fcs_priv *priv)
{
	int ret;

	ret = fcs_svc_send_request(priv, FCS_DEV_ATF_VERSION,
				   SVC_FCS_REQUEST_TIMEOUT_MS, NULL);
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
 * fcs_release() - final teardown, run when the last reference is dropped
 * @kref: reference counter embedded in the FCS state

 */
static void fcs_release(struct kref *kref)
{
	struct socfpga_fcs_priv *priv =
		container_of(kref, struct socfpga_fcs_priv, refcount);

	if (priv->session_id) {
		int ret;

		mutex_lock(&priv->lock);
		ret = fcs_close_session_locked(priv);
		mutex_unlock(&priv->lock);

		if (ret)
			dev_err(priv->client.dev,
				"Failed to close FCS service session,ret=%d\n",
				ret);
	}

	stratix10_svc_remove_async_client(priv->chan);
	stratix10_svc_free_channel(priv->chan);
	mutex_destroy(&priv->lock);
	kfree(priv);
}

/**
 * fcs_get() - take a reference on the FCS state
 * @priv: state returned by fcs_init()

 */
void fcs_get(struct socfpga_fcs_priv *priv)
{
	kref_get(&priv->refcount);
}

/**
 * fcs_put() - drop a reference on the FCS state
 * @priv: state returned by fcs_init()
 */
void fcs_put(struct socfpga_fcs_priv *priv)
{
	kref_put(&priv->refcount, fcs_release);
}

/**
 * fcs_mark_removed() - refuse further operations after the device is gone
 * @priv: state returned by fcs_init()
 */
void fcs_mark_removed(struct socfpga_fcs_priv *priv)
{
	mutex_lock(&priv->lock);
	priv->removed = true;
	mutex_unlock(&priv->lock);
}

/**
 * fcs_init() - allocate and initialise the FCS private state
 * @dev: pointer to fcs device

 *
 * Return: the new state, or an ERR_PTR on failure (which may be
 *         -EPROBE_DEFER from the service layer).
 */
struct socfpga_fcs_priv *fcs_init(struct device *dev)
{
	struct socfpga_fcs_priv *priv;
	int ret;

	priv = kzalloc_obj(*priv);
	if (!priv)
		return ERR_PTR(-ENOMEM);

	kref_init(&priv->refcount);
	mutex_init(&priv->lock);

	/* kzalloc() already cleared client.receive_cb. */
	priv->client.dev = dev;
	priv->client.priv = priv;

	priv->chan = stratix10_svc_request_channel_byname(&priv->client,
							  SVC_CLIENT_FCS);
	if (IS_ERR(priv->chan)) {
		dev_err(dev, "couldn't get service channel %s\n", SVC_CLIENT_FCS);
		ret = PTR_ERR(priv->chan);
		goto err_free;
	}

	ret = stratix10_svc_add_async_client(priv->chan, true);
	if (ret) {
		dev_err(dev, "Failed to add async client\n");
		stratix10_svc_free_channel(priv->chan);
		goto err_free;
	}

	init_completion(&priv->completion);

	/*
	 * Version query failure is non-fatal; sysfs reports -ENODATA.
	 */
	fcs_read_version_from_atf(priv);

	return priv;

err_free:
	mutex_destroy(&priv->lock);
	kfree(priv);

	return ERR_PTR(ret);
}
