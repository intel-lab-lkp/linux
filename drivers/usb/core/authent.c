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
 * usb_authent_req_digest() - Check if device is known via its digest
 *
 * @dev:	[in]		pointer to the usb device to query.
 * @buffer:	[in, out]	buffer to hold request data.
 * @digest:	[out]		device digest.
 * @mask:	[out]		USB Authentication slot mask
 *
 * Context: task context, might sleep.
 *
 * This function sends a digest request to the usb device.
 *
 * Returns:
 * * %0		- OK
 * * %-ECOMM	- Failed to send or received a message to the device
 * * %-EINVAL	- If buffer or mask is NULL
 */

static int usb_authent_req_digest(struct usb_device *dev, u8 *const buffer,
				  u8 digest[USBAUTH_DIGESTS_SIZE], u8 *mask)
{
	int ret = 0;
	struct usb_authent_digest_resp *digest_resp = NULL;

	if (buffer == NULL || mask == NULL) {
		dev_err(&dev->dev, "invalid arguments\n");
		return -EINVAL;
	}
	ret = usb_control_msg(dev, usb_rcvctrlpipe(dev, 0), AUTH_IN, USB_DIR_IN,
			      (USB_SECURITY_PROTOCOL_VERSION << 8) +
				      USB_AUTHENT_DIGEST_REQ_TYPE,
			      0, buffer, USBAUTH_DIGEST_RSP_SIZE, USB_CTRL_GET_TIMEOUT);
	if (ret != USBAUTH_DIGEST_RSP_SIZE) {
		dev_err(&dev->dev, "Failed to get digest: %d\n", ret);
		ret = -ECOMM;
		goto exit;
	}

	digest_resp = (struct usb_authent_digest_resp *)buffer;
	*mask = digest_resp->slotMask;
	memcpy(digest, digest_resp->digests, USBAUTH_DIGESTS_SIZE);

	ret = 0;

exit:

	return ret;
}

/*
 * This structure is sent as is on USB BUS and thus needs to be packed.
 */
struct usb_auth_cert_req {
	u16 offset;
	u16 length;
} __packed;

/**
 * usb_auth_read_cert_part() -  Request a specific part of a certificate chain from the device
 *
 * @dev:	[in]		handle to the USB device
 * @buffer:	[in,out]	buffer used for communication, caller allocated
 * @slot:	[in]		slot in which to read the certificate
 * @offset:	[in]		offset at which the certificate fragment must be read
 * @length:	[in]		length of the certificate fragment to read
 * @cert_part:	[out]		buffer to hold the fragment, caller allocated
 *
 * Context: task context, might sleep
 *
 * Returns:
 * * %x00	- OK
 * * %-ECOMM	- failed to send or receive a message to the device
 * * %-EINVAL	- if buffer or cert_part is NULL
 */
static int usb_auth_read_cert_part(struct usb_device *dev, u8 *const buffer,
				   const u8 slot, const u16 offset,
				   const u16 length, u8 *cert_part)
{
	struct usb_auth_cert_req cert_req = {0};
	int ret = -1;

	if (buffer == NULL || cert_part == NULL) {
		dev_err(&dev->dev, "invalid argument\n");
		return -EINVAL;
	}

	cert_req.offset = cpu_to_le16(offset);
	cert_req.length = cpu_to_le16(length);

	memcpy(buffer, &cert_req, sizeof(struct usb_auth_cert_req));
	ret = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), AUTH_OUT,
			      USB_DIR_OUT,
			      (USB_SECURITY_PROTOCOL_VERSION << 8) +
				      USB_AUTHENT_CERTIFICATE_REQ_TYPE,
			      (slot << 8), buffer,
			      sizeof(struct usb_auth_cert_req),
			      USB_CTRL_GET_TIMEOUT);
	if (ret < 0) {
		dev_err(&dev->dev, "Failed to send certificate request: %d\n", ret);
		ret = -ECOMM;
		goto cleanup;
	}

	ret = usb_control_msg(dev, usb_rcvctrlpipe(dev, 0), AUTH_IN, USB_DIR_IN,
			      (USB_SECURITY_PROTOCOL_VERSION << 8) +
				      USB_AUTHENT_CERTIFICATE_RESP_TYPE,
			      (slot << 8), buffer, length + 4,
			      USB_CTRL_GET_TIMEOUT);
	if (ret != (length + 4)) {
		dev_err(&dev->dev, "Failed to get certificate from peripheral: %d\n", ret);
		ret = -ECOMM;
		goto cleanup;
	}

	/* TODO: parse received header */
	memcpy(cert_part, buffer + 4, length);

	ret = 0;

cleanup:

	return ret;
}

/**
 * usb_authent_read_certificate() - Read a device certificate
 *
 * @dev:	[in]		pointer to the usb device to query
 * @buffer:	[in, out]	buffer to hold request data, caller allocated
 * @slot:	[in]		certificate chain to be read
 * @cert_der:	[out]		buffer to hold received certificate chain
 * @cert_len:	[out]		length of received certificate
 *
 * Context: task context, might sleep.
 *
 * Returns:
 * * %0	- OK
 * * %-EINVAL - NULL pointer or invalid slot value
 * * %-ECOMM  - failed to send request to device
 * * %-ENOMEM - failed to allocate memory for certificate
 *
 */
static int usb_authent_read_certificate(struct usb_device *dev, u8 *const buffer,
					u8 slot, u8 **cert_der, size_t *cert_len)
{
	u16 read_offset = 0;
	u16 read_length = 0;
	u8 chain_part[64] = {0};

	if (slot >= 8 || buffer == NULL || cert_der == NULL || cert_len == NULL) {
		dev_err(&dev->dev, "invalid arguments\n");
		return -EINVAL;
	}

	if (usb_auth_read_cert_part(dev, buffer, slot, 0,
				    USBAUTH_CHAIN_HEADER_SIZE,
				    chain_part) != 0) {
		dev_err(&dev->dev, "Failed to get first certificate part\n");
		return -ECOMM;
	}

	*cert_len = le16_to_cpu(((u16 *)chain_part)[0]);

	*cert_der = kzalloc(*cert_len, GFP_KERNEL);
	if (!(*cert_der))
		return -ENOMEM;

	memcpy(*cert_der, chain_part, USBAUTH_CHAIN_HEADER_SIZE);
	read_offset = USBAUTH_CHAIN_HEADER_SIZE;

	while (read_offset < *cert_len) {
		read_length = (*cert_len - read_offset) >= 64 ? 64 : (*cert_len - read_offset);

		if (usb_auth_read_cert_part(dev, buffer, slot, read_offset,
					    read_length, chain_part) != 0) {
			dev_err(&dev->dev, "USB AUTH: Failed to get certificate part\n");
			return -ECOMM;
		}

		memcpy(*cert_der + read_offset, chain_part, read_length);
		read_offset += read_length;
	}

	return 0;
}

/**
 * usb_authent_challenge_dev() - Challenge a device
 *
 * @dev:	[in]	pointer to the usb device to query
 * @buffer:	[in]	pointer to the buffer allocated for USB query
 * @slot:	[in]	certificate chain to be used
 * @slot_mask:	[in]	slot mask of the device
 * @nonce:	[in]	nonce to use for the challenge, 32 bytes long
 * @chall:	[out]	buffer for chall response, 204 bytes long, caller allocated
 *
 * Context: task context, might sleep.
 *
 * Returns:
 * * %0		- OK
 * * %-EINVAL	- NULL input pointer or invalid slot value
 * * %-ECOMM	- failed to send or receive message from the device
 */
static int usb_authent_challenge_dev(struct usb_device *dev, u8 *buffer,
	const u8 slot, const u8 slot_mask, const u8 *const nonce,
	u8 *const chall)
{
	int ret = -1;

	if (buffer == NULL || slot >= 8 || nonce == NULL) {
		dev_err(&dev->dev, "invalid arguments\n");
		return -EINVAL;
	}

	memcpy(buffer, nonce, USBAUTH_NONCE_SIZE);
	ret = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), AUTH_OUT,
			      USB_DIR_OUT,
			      (USB_SECURITY_PROTOCOL_VERSION << 8) +
				      USB_AUTHENT_CHALLENGE_REQ_TYPE,
			      (slot << 8), buffer, USBAUTH_NONCE_SIZE, USB_CTRL_GET_TIMEOUT);
	if (ret < 0) {
		dev_err(&dev->dev, "Failed to send challenge request: %d\n", ret);
		ret = -ECOMM;
		goto cleanup;
	}

	((struct usb_chall_req_hd *) chall)->protocolVersion = USB_SECURITY_PROTOCOL_VERSION;
	((struct usb_chall_req_hd *) chall)->messageType = USB_AUTHENT_CHALLENGE_REQ_TYPE;
	((struct usb_chall_req_hd *) chall)->slotNumber = slot;
	((struct usb_chall_req_hd *) chall)->reserved = 0x00;
	memcpy(chall+4, nonce, USBAUTH_NONCE_SIZE);

	ret = usb_control_msg(dev, usb_rcvctrlpipe(dev, 0), AUTH_IN, USB_DIR_IN,
			      (USB_SECURITY_PROTOCOL_VERSION << 8) +
				      USB_AUTHENT_CHALLENGE_RESP_TYPE,
			      (slot << 8) + slot_mask, buffer, USBAUTH_CHALL_RSP_SIZE,
			      USB_CTRL_GET_TIMEOUT);
	if (ret != USBAUTH_CHALL_RSP_SIZE) {
		dev_err(&dev->dev, "Failed to get challenge response: %d\n", ret);
		ret = -ECOMM;
		goto cleanup;
	}

	memcpy(chall + USBAUTH_CHAIN_HEADER_SIZE, buffer, USBAUTH_CHALL_RSP_SIZE);
	ret = 0;

cleanup:

	return ret;
}

/**
 * usb_auth_create_dev_ctx() - Create a device context according to USB Type-C Authentication Specification, chapter 5.5
 *
 * @dev:	[in]		handle to the USB device
 * @ctx:	[in, out]	buffer to hold the device context, caller allocated
 * @buf_size:	[in]		available size in the context buffer
 * @ctx_size:	[out]		total size of the context if return equals 0
 *
 * The device context is composed of :
 *	1. Device Descriptor
 *	2. Complete BOS Descriptor (if present)
 *	3. Complete Configuration 1 Descriptor
 *	4. Complete Configuration 2 Descriptor (if present)
 *	5. ...
 *	6. Complete Configuration n Descriptor (if present)
 *
 * FIXME: Ensure the validity of the device context is complete:
 *	- Will the config order consistent ?
 *	- Do we need to also get the sub configuration strings ?
 *
 * Returns:
 * * %0		- OK
 * * %-EINVAL	- invalid dev, ctx or size
 *
 */
static int usb_auth_create_dev_ctx(struct usb_device *dev, u8 *ctx,
							const size_t buf_size, size_t *ctx_size)
{
	int cfgno = 0;
	int desc_size = 0;

	if (dev == NULL || ctx == NULL || ctx_size == NULL) {
		dev_err(&dev->dev, "invalid inputs\n");
		return -EINVAL;
	}

	*ctx_size = 0;

	if (buf_size < (size_t)dev->descriptor.bLength) {
		dev_err(&dev->dev, "buffer too small\n");
		return -EINVAL;
	}

	memcpy(ctx, (void *) &dev->descriptor, (size_t) dev->descriptor.bLength);
	*ctx_size += (size_t) dev->descriptor.bLength;

	if (dev->bos == NULL || dev->bos->desc == NULL) {
		dev_err(&dev->dev, "invalid BOS\n");
		return -EINVAL;
	}

	desc_size = le16_to_cpu(dev->bos->desc->wTotalLength);
	if (buf_size < (*ctx_size + desc_size)) {
		dev_err(&dev->dev, "buffer too small\n");
		return -EINVAL;
	}

	memcpy(ctx + (*ctx_size), (void *) dev->bos->desc, desc_size);
	*ctx_size += desc_size;

	if (dev->config == NULL) {
		dev_err(&dev->dev, "invalid configuration\n");
		return -EINVAL;
	}

	for (cfgno = 0; cfgno < dev->descriptor.bNumConfigurations; cfgno++) {
		desc_size = le16_to_cpu(dev->config[cfgno].desc.wTotalLength);

		if (buf_size < (*ctx_size + desc_size)) {
			dev_err(&dev->dev, "buffer too small\n");
			return -EINVAL;
		}

		memcpy(ctx + (*ctx_size), (void *) &dev->config[cfgno].desc, USB_DT_CONFIG_SIZE);
		*ctx_size += USB_DT_CONFIG_SIZE;
	}

	return 0;
}

/**
 * usb_auth_try_resume() - Check that the authentication can resume after a sleep
 *
 * @dev: [in] the usb device
 * @hub: [in] the parent hub
 *
 * Returns:
 * * %0		- OK
 * * %-ENODEV	- hub has been disconnected
 *
 */
static int usb_auth_try_resume(struct usb_device *dev, struct usb_device *hub)
{
	if (hub == NULL || dev == NULL ||
		     dev->port_is_suspended == 1 ||
		     dev->reset_in_progress == 1) {
		return -ENODEV;
	}

	/*
	 * TODO: test if the device has not been disconnected
	 * TODO: test if the device has not been disconnected then replaced with another one
	 */

	return 0;
}

static bool usb_has_authentication_capability(const struct usb_device *const dev)
{
	return dev->bos && dev->bos->authent_cap;
}

/**
 * usb_authenticate_device() - Challenge a device
 *
 * @dev: [in, out] pointer to device
 *
 * Authentication is done in the following steps:
 *  1. Get device certificates digest to determine if it is already known
 *       if yes, go to 3.
 *  2. Get device certificates
 *  3. Challenge device
 *  4. Based on previous result, determine if device is allowed under local
 *     security policy.
 *
 * Context: task context, might sleep.
 * TODO: complete all possible error case.
 * TODO: handle root hub device.
 *
 * Returns:
 * * %0		- OK
 * * %-ENOMEM	- failed to allocate memory for exchange
 *
 */
int usb_authenticate_device(struct usb_device *dev)
{
	int ret = 0;
	u8 is_valid = 0;
	u8 is_known = 0;
	u8 is_blocked = 0;
	u8 chain_nb = 0;
	u8 slot_mask = 0;
	u8 slot = 0;
	u8 digests[USBAUTH_DIGESTS_SIZE] = {0};
	u8 nonce[USBAUTH_NONCE_SIZE] = {0};
	u8 chall[USBAUTH_CHALL_SIZE] = {0};
	u32 dev_id = 0;
	size_t ctx_size = 0;
	int i = 0;

	u8 *cert_der = NULL;
	u8 *buffer = NULL;
	size_t cert_len = 0;

	if (dev == NULL)
		return -ENODEV;

	dev->authenticated = 0;
	if (!usb_has_authentication_capability(dev)) {
		dev_notice(&dev->dev, "No authentication capability\n");
		goto cleanup;
	}

	if (dev->parent == NULL)
		return -ENODEV;

	struct usb_device *hub = dev->parent;

	buffer = kzalloc(512, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	ret = usb_authent_req_digest(dev, buffer, digests, &slot_mask);
	if (ret != 0) {
		dev_err(&dev->dev, "failed to get digest: %d\n", ret);
		goto cleanup;
	}

	usb_unlock_device(hub);
	ret = usb_policy_engine_check_digest(dev->route, digests, slot_mask,
					     &is_known, &is_blocked, &dev_id);
	if (ret != 0) {
		dev_err(&dev->dev, "failed to check digest: %d\n", ret);
		usb_lock_device(hub);
		goto cleanup;
	}
	dev_info(&dev->dev, "waking up\n");
	usb_lock_device(hub);
	ret = usb_auth_try_resume(dev, hub);
	if (ret != 0) {
		dev_err(&dev->dev, "failed to resume: %d\n", ret);
		goto cleanup;
	}

	if (is_known)
		goto device_known;

	/*
	 * If device is not already known try to obtain a valid certificate
	 * Iterate over every device certificate slots, it gets them one by one
	 * in order to avoid spamming the device.
	 */
	if (!is_known) {
		for (i = 0; i < 8; i++) {
			if (1 == ((slot_mask >> i) & 1)) {
				ret = usb_authent_read_certificate(dev, buffer,
								   chain_nb,
								   &cert_der,
								   &cert_len);
				if (ret != 0) {
					goto cleanup;
				}

				usb_unlock_device(hub);
				ret = usb_policy_engine_check_cert_chain(
					dev->route, digests + i * USBAUTH_DIGEST_SIZE, cert_der,
					cert_len, &is_valid, &is_blocked,
					&dev_id);
				if (ret != 0) {
					dev_err(&dev->dev,
						"failed to validate certificate: %d\n",
						ret);
					usb_lock_device(hub);
					goto cleanup;
				}
				usb_lock_device(hub);

				ret = usb_auth_try_resume(dev, hub);
				if (ret != 0) {
					dev_err(&dev->dev, "failed to resume: %d\n", ret);
					goto cleanup;
				}

				if (is_valid) {
					slot = i;
					goto device_known;
				}
			}
		}
		goto done;
	} else {
		for (i = 0; i < 8; i++) {
			if (1 == ((is_known >> i) & 1)) {
				slot = i;
				break;
			}
		}
	}

device_known:
	/*
	 * Device is known, authenticate the device with a challenge request
	 */
	usb_unlock_device(hub);
	ret = usb_policy_engine_generate_challenge(dev_id, nonce);
	if (ret != 0) {
		dev_err(&dev->dev, "failed to generate challenge: %d\n", ret);
		usb_lock_device(hub);
		goto cleanup;
	}
	usb_lock_device(hub);

	ret = usb_auth_try_resume(dev, hub);
	if (ret != 0) {
		dev_err(&dev->dev, "failed to resume: %d\n", ret);
		goto cleanup;
	}

	ret = usb_authent_challenge_dev(dev, buffer, slot, slot_mask, nonce,
					chall);
	if (ret != 0) {
		dev_err(&dev->dev, "failed to challenge device: %d\n", ret);
		goto cleanup;
	}

	ret = usb_auth_create_dev_ctx(dev, buffer, 512, &ctx_size);
	if (ret != 0) {
		dev_err(&dev->dev, "failed to create context: %d\n", ret);
		goto cleanup;
	}

	usb_unlock_device(hub);
	ret = usb_policy_engine_check_challenge(dev_id, chall, buffer, ctx_size,
						&is_valid);
	if (ret != 0) {
		dev_err(&dev->dev, "failed to check challenge: %d\n", ret);
		usb_lock_device(hub);
		goto cleanup;
	}
	usb_lock_device(hub);

	ret = usb_auth_try_resume(dev, hub);
	if (ret != 0) {
		dev_err(&dev->dev, "failed to resume: %d\n", ret);
		goto cleanup;
	}

done:
	ret = 0;
	if (is_valid) {
		dev->authenticated = 1;
	} else {
		dev->authenticated = 0;
		dev_err(&dev->dev, "Device authentication failure\n");
	}
cleanup:
	kfree(buffer);
	kfree(cert_der);

	return ret;
}
