// SPDX-License-Identifier: GPL-2.0
/*
 * SPDX-FileCopyrightText: (C) 2025 ANSSI
 *
 * USB Authentication protocol implementation
 *
 * Author: Luc Bonnafoux <luc.bonnafoux@ssi.gouv.fr>
 * Author: Nicolas Bouchinet <nicolas.bouchinet@ssi.gouv.fr>
 *
 */

#include <linux/types.h>
#include <linux/usb.h>
#include <linux/usb/ch9.h>
#include <linux/usb/hcd.h>
#include <linux/usb/quirks.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <asm/byteorder.h>

#include "authent_netlink.h"

#include "authent.h"

/**
 * usb_authent_req_digest - Check if device is known via its digest
 * @dev:		[in]  pointer to the usb device to query
 * @buffer:     [inout] buffer to hold request data
 * @digest:     [out] device digest
 *
 * Context: task context, might sleep.
 *
 * This function sends a digest request to the usb device.
 *
 * Possible errors:
 *  - ECOMM : failed to send or received a message to the device
 *  - EINVAL : if buffer or mask is NULL
 *
 * Return: If successful, zero. Otherwise, a negative  error number.
 */
static int usb_authent_req_digest(struct usb_device *dev, uint8_t *const buffer,
				  uint8_t digest[256], uint8_t *mask)
{
	int ret = 0;
	struct usb_authent_digest_resp *digest_resp = NULL;

	if (unlikely((buffer == NULL || mask == NULL))) {
		pr_err("invalid arguments\n");
		return -EINVAL;
	}
	ret = usb_control_msg(dev, usb_rcvctrlpipe(dev, 0), AUTH_IN, USB_DIR_IN,
			      (USB_SECURITY_PROTOCOL_VERSION << 8) +
				      USB_AUTHENT_DIGEST_REQ_TYPE,
			      0, buffer, 260, USB_CTRL_GET_TIMEOUT);
	if (ret < 0) {
		pr_err("Failed to get digest: %d\n", ret);
		ret = -ECOMM;
		goto exit;
	}

	// Parse received digest response
	digest_resp = (struct usb_authent_digest_resp *)buffer;
	pr_notice("received digest response:\n");
	pr_notice("	protocolVersion: %x\n", digest_resp->protocolVersion);
	pr_notice("	messageType: %x\n", digest_resp->messageType);
	pr_notice("	capability: %x\n", digest_resp->capability);
	pr_notice("	slotMask: %x\n", digest_resp->slotMask);

	*mask = digest_resp->slotMask;
	memcpy(digest, digest_resp->digests, 256);

	ret = 0;

exit:

	return ret;
}

struct usb_auth_cert_req {
	uint16_t offset;
	uint16_t length;
} __packed;

/**
 * @brief Request a specific part of a certificate chain from the device
 *
 * Context: task context, might sleep
 *
 * Possible errors:
 *  - ECOMM : failed to send or receive a message to the device
 *  - EINVAL : if buffer or cert_part is NULL
 *
 * @param [in]     dev       : handle to the USB device
 * @param [in,out] buffer    : buffer used for communication, caller allocated
 * @param [in]     slot      : slot in which to read the certificate
 * @param [in]     offset    : at which the certificate fragment must be read
 * @param [in]     length    : of the certificate fragment to read
 * @param [out]    cert_part : buffer to hold the fragment, caller allocated
 *
 * @return 0 on SUCCESS else an error code
 */
static int usb_auth_read_cert_part(struct usb_device *dev, uint8_t *const buffer,
				   const uint8_t slot, const uint16_t offset,
				   const uint16_t length, uint8_t *cert_part)
{
	struct usb_auth_cert_req cert_req = { 0 };
	int ret = -1;

	if (unlikely(buffer == NULL || cert_part == NULL)) {
		pr_err("invalid argument\n");
		return -EINVAL;
	}

	cert_req.offset = offset;
	cert_req.length = length;

	// AUTH OUT request transfer
	memcpy(buffer, &cert_req, sizeof(struct usb_auth_cert_req));
	ret = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), AUTH_OUT,
			      USB_DIR_OUT,
			      (USB_SECURITY_PROTOCOL_VERSION << 8) +
				      USB_AUTHENT_CERTIFICATE_REQ_TYPE,
			      (slot << 8), buffer,
			      sizeof(struct usb_auth_cert_req),
			      USB_CTRL_GET_TIMEOUT);
	if (ret < 0) {
		pr_err("Failed to send certificate request: %d\n", ret);
		ret = -ECOMM;
		goto cleanup;
	}

	// AUTH IN certificate read
	ret = usb_control_msg(dev, usb_rcvctrlpipe(dev, 0), AUTH_IN, USB_DIR_IN,
			      (USB_SECURITY_PROTOCOL_VERSION << 8) +
				      USB_AUTHENT_CERTIFICATE_RESP_TYPE,
			      (slot << 8), buffer, length + 4,
			      USB_CTRL_GET_TIMEOUT);
	if (ret < 0) {
		pr_notice("Failed to get certificate from peripheral: %d\n", ret);
		ret = -ECOMM;
		goto cleanup;
	}

	// TODO: parse received header
	memcpy(cert_part, buffer + 4, length);

	ret = 0;

cleanup:

	return ret;
}

/**
 * usb_authent_read_certificate - Read a device certificate
 * @dev:		[in] pointer to the usb device to query
 * @buffer:		[inout] buffer to hold request data, caller allocated
 * @slot:		[in] certificate chain to be read
 * @cert_der:   [out] buffer to hold received certificate chain
 * @cert_len:   [out] length of received certificate
 *
 * Context: task context, might sleep.
 *
 * Possible errors:
 *  - EINVAL : NULL pointer or invalid slot value
 *  - ECOMM  : failed to send request to device
 *  - ENOMEM : failed to allocate memory for certificate
 *
 * Return: If successful, zero. Otherwise, a negative  error number.
 */
static int usb_authent_read_certificate(struct usb_device *dev, uint8_t *const buffer,
					uint8_t slot, uint8_t **cert_der, size_t *cert_len)
{
	uint16_t read_offset = 0;
	uint16_t read_length = 0;
	uint8_t chain_part[64] = { 0 };

	if (unlikely(slot >= 8 || buffer == NULL || cert_der == NULL || cert_len == NULL)) {
		pr_err("invalid arguments\n");
		return -EINVAL;
	}

	// First request to get certificate chain length
	if (usb_auth_read_cert_part(dev, buffer, slot, 0,
				    USB_AUTH_CHAIN_HEADER_SIZE,
				    chain_part) != 0) {
		pr_err("Failed to get first certificate part\n");
		return -ECOMM;
	}

	// Extract total length
	*cert_len = ((uint16_t *)chain_part)[0];
	pr_notice("Received header of chain with length: %ld\n",
	       (*cert_len) + USB_AUTH_CHAIN_HEADER_SIZE);

	// Allocate certificate DER buffer
	*cert_der = kzalloc(*cert_len, GFP_KERNEL);
	if (!(*cert_der))
		return -ENOMEM;

	// Write the chain header at the beginning of the chain.
	memcpy(*cert_der, chain_part, USB_AUTH_CHAIN_HEADER_SIZE);
	// Read the certificate chain starting after the header.
	read_offset = USB_AUTH_CHAIN_HEADER_SIZE;

	while (read_offset < *cert_len) {
		read_length = (*cert_len - read_offset) >= 64 ? 64 : (*cert_len - read_offset);

		if (usb_auth_read_cert_part(dev, buffer, slot, read_offset,
					    read_length, chain_part) != 0) {
			pr_err("USB AUTH: Failed to get certificate part\n");
			return -ECOMM;
		}

		memcpy(*cert_der + read_offset, chain_part, read_length);

		read_offset += read_length;
	}

	return 0;
}

/**
 * usb_authent_challenge_dev - Challenge a device
 * @dev:				[in] pointer to the usb device to query
 * @buffer:			[in] pointer to the buffer allocated for USB query
 * @slot:				[in] certificate chain to be used
 * @slot_mask:	[in] slot mask of the device
 * @nonce:			[in] nonce to use for the challenge, 32 bytes long
 * @chall:			[out] buffer for chall response, 204 bytes long, caller allocated
 *
 * Context: task context, might sleep.
 *
 * Possible errors:
 *  - EINVAL : NULL input pointer or invalid slot value
 *  - ECOMM  : failed to send or receive message from the device
 *
 * Return: If successful, zero. Otherwise, a negative  error number.
 */
static int usb_authent_challenge_dev(struct usb_device *dev, uint8_t *buffer,
	const uint8_t slot, const uint8_t slot_mask, const uint8_t *const nonce,
	uint8_t *const chall)
{
	int ret = -1;

	if (unlikely(buffer == NULL || slot >= 8 || nonce == NULL)) {
		pr_err("invalid arguments\n");
		return -EINVAL;
	}

	// AUTH OUT challenge request transfer
	memcpy(buffer, nonce, 32);
	ret = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), AUTH_OUT,
			      USB_DIR_OUT,
			      (USB_SECURITY_PROTOCOL_VERSION << 8) +
				      USB_AUTHENT_CHALLENGE_REQ_TYPE,
			      (slot << 8), buffer, 32, USB_CTRL_GET_TIMEOUT);
	if (ret < 0) {
		pr_err("Failed to send challenge request: %d\n", ret);
		ret = -ECOMM;
		goto cleanup;
	}

	// Complete the challenge with the request
	chall[1] = USB_SECURITY_PROTOCOL_VERSION;
	chall[0] = USB_AUTHENT_CHALLENGE_REQ_TYPE;
	chall[2] = slot;
	chall[3] = 0x00;
	memcpy(chall+4, nonce, 32);

	// AUTH IN challenge response transfer
	ret = usb_control_msg(dev, usb_rcvctrlpipe(dev, 0), AUTH_IN, USB_DIR_IN,
			      (USB_SECURITY_PROTOCOL_VERSION << 8) +
				      USB_AUTHENT_CHALLENGE_RESP_TYPE,
			      (slot << 8) + slot_mask, buffer, 168,
			      USB_CTRL_GET_TIMEOUT);
	if (ret < 0) {
		pr_err("Failed to get challenge response: %d\n", ret);
		ret = -ECOMM;
		goto cleanup;
	}

	pr_notice("received challenge response\n");

	// Complete last part of the challenge with what is returned by the device
	memcpy(chall+USB_AUTH_CHAIN_HEADER_SIZE, buffer, 168);

	ret = 0;

cleanup:

	return ret;
}

/**
 * @brief Create a device context according to USB Type-C Authentication Specification, chapter 5.5
 *	1. Device Descriptor
 *	2. Complete BOS Descriptor (if present)
 *	3. Complete Configuration 1 Descriptor
 *	4. Complete Configuration 2 Descriptor (if present)
 *	5. ...
 *	6. Complete Configuration n Descriptor (if present)
 *
 * Possible error codes:
 *  - EINVAL : invalid dev, ctx or size
 *
 * @param [in] dev       : handle to the USB device
 * @param [in, out] ctx  : buffer to hold the device context, caller allocated
 * @param [in] buf_size  : available size in the context buffer
 * @param [out] ctx_size : total size of the context if return equals 0
 *
 * @return 0 or error code
 */
static int usb_auth_create_dev_ctx(struct usb_device *dev, uint8_t *ctx,
							const size_t buf_size, size_t *ctx_size)
{
	int desc_size = 0;

	if (unlikely(dev == NULL || ctx == NULL)) {
		pr_err("invalid inputs\n");
		return -EINVAL;
	}

	*ctx_size = 0;

	// Device descriptor
	if (buf_size < (size_t)dev->descriptor.bLength) {
		pr_err("buffer too small\n");
		return -EINVAL;
	}

	memcpy(ctx, (void *) &dev->descriptor, (size_t) dev->descriptor.bLength);

	*ctx_size += (size_t) dev->descriptor.bLength;

	// Device BOS and capabilities
	if (unlikely(dev->bos == NULL || dev->bos->desc == NULL)) {
		pr_err("invalid BOS\n");
		return -EINVAL;
	}

	desc_size = le16_to_cpu(dev->bos->desc->wTotalLength);

	if (buf_size < (*ctx_size + desc_size)) {
		pr_err("buffer too small\n");
		return -EINVAL;
	}

	memcpy(ctx + (*ctx_size), (void *) dev->bos->desc, desc_size);

	*ctx_size += desc_size;

	// Device configuration descriptor
	if (unlikely(dev->config == NULL)) {
		pr_err("invalid configuration\n");
		return -EINVAL;
	}

	desc_size = le16_to_cpu(dev->config->desc.wTotalLength);

	if (buf_size < (*ctx_size + desc_size)) {
		pr_err("buffer too small\n");
		return -EINVAL;
	}

	memcpy(ctx + (*ctx_size), (void *) &dev->config->desc, 9);

	*ctx_size += 9;

	return 0;
}

/**
 * @brief Check that the authentication can resume after a sleep
 *
 * @param [in] dev : the usb device
 * @param [in] hub : the parent hub
 *
 * Possible error codes:
 *  - ENODEV : hub has been disconnected
 *
 * @return 0 if possible to resume, else an error code
 */
static int usb_auth_try_resume(struct usb_device *dev, struct usb_device *hub)
{
	// Test if the hub or the device has been disconnected
	if (unlikely(hub == NULL || dev == NULL ||
		     dev->port_is_suspended == 1 ||
		     dev->reset_in_progress == 1)) {
		return -ENODEV;
	}

	// TODO: test if the device has not been disconnected
	// TODO: test if the device has not been disconnected then replaced with another one

	return 0;
}

/**
 * usb_authenticate_device - Challenge a device
 * @dev:		[inout] pointer to device
 *
 * Context: task context, might sleep.
 *
 * Authentication is done in the following steps:
 *  1. Get device certificates digest to determine if it is already known
 *       if yes, go to 3.
 *  2. Get device certificates
 *  3. Challenge device
 *  4. Based on previous result, determine if device is allowed under local
 *     security policy.
 *
 * Possible error code:
 *  - ENOMEM : failed to allocate memory for exchange
 *  - TODO: complete all possible error case
 *
 * Return: If successful, zero. Otherwise, a negative  error number.
 */
int usb_authenticate_device(struct usb_device *dev)
{
	int ret = 0;

	uint8_t is_valid = 0;
	uint8_t is_known = 0;
	uint8_t is_blocked = 0;
	uint8_t chain_nb = 0;
	uint8_t slot_mask = 0;
	uint8_t slot = 0;
	uint8_t digests[256] = { 0 };
	uint8_t nonce[32] = {0};
	uint8_t chall[204] = {0};
	uint32_t dev_id = 0;
	size_t ctx_size = 0;
	int i = 0;

	uint8_t *cert_der = NULL;
	size_t cert_len = 0;

	if (unlikely(dev == NULL || dev->parent == NULL))
		return -ENODEV;

	struct usb_device *hub = dev->parent;

	// By default set authorization status at false
	dev->authorized = 0;
	dev->authenticated = 0;

	uint8_t *buffer = NULL;
	// Buffer to hold responses
	buffer = kzalloc(512, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	pr_notice("start of device authentication\n");

	/*
	 * Send DIGEST request to determine if it is a known device
	 */
	ret = usb_authent_req_digest(dev, buffer, digests, &slot_mask);
	if (ret != 0) {
		pr_err("failed to get digest: %d\n", ret);
		goto cleanup;
	}
	pr_notice("received digest\n");

	usb_unlock_device(hub);
	ret = usb_policy_engine_check_digest(dev->route, digests, slot_mask,
					     &is_known, &is_blocked, &dev_id);
	if (ret != 0) {
		pr_err("failed to check digest: %d\n", ret);
		usb_lock_device(hub);
		goto cleanup;
	}
	pr_info("waking up\n");
	usb_lock_device(hub);
	ret = usb_auth_try_resume(dev, hub);
	if (unlikely(ret != 0)) {
		pr_err("failed to resume: %d\n", ret);
		goto cleanup;
	}

	pr_info("resuming\n");

	/*
	 * If the device is already known and blocked, reject it
	 */
	if (is_known && is_blocked) {
		ret = 0;
		goto cleanup;
	}

	/*
	 * If device is not already known try to obtain a valid certificate
	 * Iterate over every device certificate slots, it gets them one by one
	 * in order to avoid spamming the device.
	 */
	if (!is_known) {
		// Iterate over slot containing a certificate until a valid one is found
		for (i = 0; i < 8; i++) {
			// Test if slot contains a certificate chain
			if (1 == ((slot_mask >> i) & 1)) {
				ret = usb_authent_read_certificate(dev, buffer,
								   chain_nb,
								   &cert_der,
								   &cert_len);
				if (ret != 0) {
					// Failed to read device certificate, abort authentication
					// Apply security policy on failed device
					goto cleanup;
				}
				pr_notice("received certificate\n");

				// validate the certificate
				usb_unlock_device(hub);
				ret = usb_policy_engine_check_cert_chain(
					dev->route, digests + i * 32, cert_der,
					cert_len, &is_valid, &is_blocked,
					&dev_id);
				if (ret != 0) {
					pr_err("failed to validate certificate: %d\n", ret);
					usb_lock_device(hub);
					goto cleanup;
				}
				pr_notice("validated certificate\n");
				usb_lock_device(hub);

				ret = usb_auth_try_resume(dev, hub);
				if (unlikely(ret != 0)) {
					pr_err("failed to resume: %d\n", ret);
					goto cleanup;
				}

				pr_info("resuming\n");

				if (is_valid && !is_blocked) {
					// Found a valid and authorized certificate,
					// continue with challenge
					slot = i;
					break;
				} else if (is_valid && is_blocked) {
					// Found a valid and unauthorized certificate,
					// reject device
					ret = 0;
					goto cleanup;
				}
			}
		}
	} else {
		// Pick a slot among the valid ones, take first one
		for (i = 0; i < 8; i++) {
			if (1 == ((is_known >> i) & 1)) {
				slot = i;
				break;
			}
		}
	}

	/*
	 * Authenticate the device with a challenge request
	 */
	// Obtain a nonce for the challenge
	usb_unlock_device(hub);
	ret = usb_policy_engine_generate_challenge(dev_id, nonce);
	if (ret != 0) {
		pr_err("failed to generate challenge: %d\n", ret);
		usb_lock_device(hub);
		goto cleanup;
	}
	pr_notice("generated challenge\n");
	usb_lock_device(hub);

	ret = usb_auth_try_resume(dev, hub);
	if (unlikely(ret != 0)) {
		pr_err("failed to resume: %d\n", ret);
		goto cleanup;
	}

	pr_info("resuming\n");

	// Send a challenge request
	ret = usb_authent_challenge_dev(dev, buffer, slot, slot_mask, nonce,
					chall);
	if (ret != 0) {
		pr_err("failed to challenge device: %d\n", ret);
		goto cleanup;
	}
	pr_notice("validated challenge\n");

	// Create device context
	ret = usb_auth_create_dev_ctx(dev, buffer, 512, &ctx_size);
	if (ret != 0) {
		pr_err("failed to create context: %d\n", ret);
		goto cleanup;
	}

	// Validate the challenge
	usb_unlock_device(hub);
	ret = usb_policy_engine_check_challenge(dev_id, chall, buffer, ctx_size,
						&is_valid);
	if (ret != 0) {
		pr_err("failed to check challenge: %d\n", ret);
		usb_lock_device(hub);
		goto cleanup;
	}
	pr_notice("checked challenge\n");
	usb_lock_device(hub);

	ret = usb_auth_try_resume(dev, hub);
	if (unlikely(ret != 0)) {
		pr_err("failed to resume: %d\n", ret);
		goto cleanup;
	}

	pr_info("resuming\n");

	// Apply authorization decision
	if (is_valid) {
		dev->authorized = 1;
		dev->authenticated = 1;
	}

	ret = 0;

cleanup:
	kfree(buffer);
	kfree(cert_der);

	return 0;
}
