// SPDX-License-Identifier: GPL-2.0
/*
 * SPDX-FileCopyrightText: (C) 2025 ANSSI
 *
 * USB Authentication netlink interface
 *
 * Author: Luc Bonnafoux <luc.bonnafoux@ssi.gouv.fr>
 * Author: Nicolas Bouchinet <nicolas.bouchinet@ssi.gouv.fr>
 *
 */

#include <linux/sched.h>
#include <linux/time.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/err.h>
#include <linux/capability.h>

#include <net/genetlink.h>

#include <uapi/linux/usb/usb_auth_netlink.h>

#include "authent_netlink.h"

#define WAIT_USERSPACE_TIMEOUT 30
#define WAIT_RESPONSE_TIMEOUT 300
#define USB_AUTH_MAX_RESP_SIZE 128

/**
 * Define an outstanding request between the kernel and userspace
 */
struct usb_auth_req {
	uint8_t used; /**< 1 if the slot is currently used, access must be protected */
	uint8_t done; /**< 1 if the response has been received, used as wait condition */
	uint8_t error; /**< userspace response error code */
	uint8_t resp[USB_AUTH_MAX_RESP_SIZE]; /**< arbitrary response buffer */
};

static struct genl_family usbauth_genl_fam;

// TODO: add mutex for PID access
static u32 pol_eng_pid;
static struct net *pol_eng_net;

static wait_queue_head_t usb_req_wq;

#define USB_AUTH_MAX_OUTSTANDING_REQS 10
// Mutex is used to protect access to the `used` field
DEFINE_MUTEX(usb_auth_reqs_mutex);
static struct usb_auth_req usb_auth_outstanding_reqs[USB_AUTH_MAX_OUTSTANDING_REQS];

////////////////////////////////////////////////////////////////////////////////
// USB request utilities
////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Find the first available slot in the outstanding requests array and
 *  reserve it.
 *
 * CAUTION: this function will block on the request list mutex
 *
 * Possible error codes:
 *  - EXFULL : too many outstanding requests already
 *
 * @param [out] index : reserved slot index, valid if return equals 0
 *
 * @return 0 on SUCCESS or error code
 */
static int usb_auth_get_reqs_slot(uint32_t *index)
{
	int ret = -EXFULL;
	uint32_t i = 0;

	mutex_lock(&usb_auth_reqs_mutex);

	// Take the first available slot
	for (i = 0; i < USB_AUTH_MAX_OUTSTANDING_REQS; i++) {
		if (usb_auth_outstanding_reqs[i].used == 0) {
			usb_auth_outstanding_reqs[i].used = 1;
			usb_auth_outstanding_reqs[i].done = 0;
			usb_auth_outstanding_reqs[i].error = USBAUTH_OK;
			memset(usb_auth_outstanding_reqs[i].resp, 0,
			       USB_AUTH_MAX_RESP_SIZE);
			*index = i;
			ret = 0;
			break;
		}
	}

	mutex_unlock(&usb_auth_reqs_mutex);

	return ret;
}

/**
 * @brief release a request slot
 *
 * CAUTION: this function will block on the request list mutex
 *
 * @param [in] index : slot index to be released
 */
static void usb_auth_release_reqs_slot(const uint32_t index)
{
	mutex_lock(&usb_auth_reqs_mutex);

	usb_auth_outstanding_reqs[index].used = 0;

	mutex_unlock(&usb_auth_reqs_mutex);
}

////////////////////////////////////////////////////////////////////////////////
// Generic netlink socket utilities
////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Handle a registration request from userspace
 *
 * It will overwrite the current userspace registered PID with the one provided
 *  in the request
 */
static int usb_auth_register_req_doit(struct sk_buff *skb, struct genl_info *info)
{
	int ret = 0;
	void *hdr = NULL;
	struct sk_buff *msg = NULL;

	pr_info("message received\n");

	if (!capable(CAP_SYS_ADMIN)) {
		pr_err("invalid permissions\n");
		return -EPERM;
	}

	// Register Policy engine PID, overwrite value if already set
	pol_eng_pid = info->snd_portid;
	pol_eng_net = genl_info_net(info);

	wake_up_all(&usb_req_wq);

	msg = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (msg == NULL) {
		pr_err("failed to allocate message buffer\n");
		return -ENOMEM;
	}

	hdr = genlmsg_put(msg, info->snd_portid, info->snd_seq,
			  &usbauth_genl_fam, 0, USBAUTH_CMD_REGISTER);
	if (hdr == NULL) {
		pr_err("failed to create genetlink header\n");
		nlmsg_free(msg);
		return -EMSGSIZE;
	}

	genlmsg_end(msg, hdr);

	ret = genlmsg_reply(msg, info);

	pr_info("reply sent\n");

	return ret;
}

/**
 * @brief Handle a CHECK_DIGEST response from userspace
 *
 * The response must contain:
 *  - USBAUTH_A_REQ_ID
 *  - USBAUTH_A_ERROR_CODE
 *  - USBAUTH_A_DEV_ID
 *  - USBAUTH_A_KNOWN
 *  - USBAUTH_A_BLOCKED
 *
 */
static int usb_auth_digest_resp_doit(struct sk_buff *skb, struct genl_info *info)
{
	uint32_t index = 0;

	pr_info("message received\n");

	if (!capable(CAP_SYS_ADMIN)) {
		pr_err("invalid permissions\n");
		return -EPERM;
	}

	if (!info->attrs[USBAUTH_A_REQ_ID]) {
		pr_err("digest_resp_doit: invalid response: no req ID\n");
		return -EINVAL;
	}

	index = nla_get_u32(info->attrs[USBAUTH_A_REQ_ID]);

	// Test for error
	if (!info->attrs[USBAUTH_A_ERROR_CODE]) {
		pr_err("digest_resp_doit: invalid response: missing attributes\n");
		usb_auth_outstanding_reqs[index].error = USBAUTH_INVRESP;
		usb_auth_outstanding_reqs[index].done = 1;
		return -EINVAL;
	}

	usb_auth_outstanding_reqs[index].error =
		nla_get_u8(info->attrs[USBAUTH_A_ERROR_CODE]);

	if (usb_auth_outstanding_reqs[index].error != USBAUTH_OK) {
		pr_err("digest_resp_doit: response error\n");
		usb_auth_outstanding_reqs[index].done = 1;
		return -EINVAL;
	}

	if (!info->attrs[USBAUTH_A_DEV_ID] || !info->attrs[USBAUTH_A_KNOWN] ||
	    !info->attrs[USBAUTH_A_BLOCKED]) {
		pr_err("digest_resp_doit: invalid response: missing attributes\n");
		usb_auth_outstanding_reqs[index].error = USBAUTH_INVRESP;
		usb_auth_outstanding_reqs[index].done = 1;
		return -EINVAL;
	}

	usb_auth_outstanding_reqs[index].resp[0] =
		nla_get_u8(info->attrs[USBAUTH_A_KNOWN]);
	usb_auth_outstanding_reqs[index].resp[1] =
		nla_get_u8(info->attrs[USBAUTH_A_BLOCKED]);
	((uint32_t *)usb_auth_outstanding_reqs[index].resp + 2)[0] =
		nla_get_u32(info->attrs[USBAUTH_A_DEV_ID]);

	usb_auth_outstanding_reqs[index].done = 1;

	wake_up_all(&usb_req_wq);

	return 0;
}

/**
 * @brief Handle a CHECK_CERTIFICATE response from userspace
 *
 * The response must contain:
 *  - USBAUTH_A_REQ_ID
 *  - USBAUTH_A_ERROR_CODE
 *  - USBAUTH_A_VALID
 *  - USBAUTH_A_BLOCKED
 *  - USBAUTH_A_DEV_ID
 *
 */
static int usb_auth_cert_resp_doit(struct sk_buff *skb, struct genl_info *info)
{
	uint32_t index = 0;

	pr_info("message received\n");

	if (!capable(CAP_SYS_ADMIN)) {
		pr_err("invalid permissions\n");
		return -EPERM;
	}

	if (!info->attrs[USBAUTH_A_REQ_ID]) {
		pr_err("invalid response: no req ID\n");
		return -EINVAL;
	}

	index = nla_get_u32(info->attrs[USBAUTH_A_REQ_ID]);

	// Test for error
	if (!info->attrs[USBAUTH_A_ERROR_CODE]) {
		pr_err("invalid response: missing attributes\n");
		usb_auth_outstanding_reqs[index].error = USBAUTH_INVRESP;
		usb_auth_outstanding_reqs[index].done = 1;
		return -EINVAL;
	}

	usb_auth_outstanding_reqs[index].error =
		nla_get_u8(info->attrs[USBAUTH_A_ERROR_CODE]);

	if (usb_auth_outstanding_reqs[index].error != USBAUTH_OK) {
		pr_err("response error\n");
		usb_auth_outstanding_reqs[index].done = 1;
		return -EINVAL;
	}

	if (!info->attrs[USBAUTH_A_DEV_ID] || !info->attrs[USBAUTH_A_VALID] ||
	    !info->attrs[USBAUTH_A_BLOCKED]) {
		pr_err("invalid response: missing attributes\n");
		usb_auth_outstanding_reqs[index].done = 1;
		usb_auth_outstanding_reqs[index].error = USBAUTH_INVRESP;
		return -EINVAL;
	}

	usb_auth_outstanding_reqs[index].resp[0] =
		nla_get_u8(info->attrs[USBAUTH_A_VALID]);
	usb_auth_outstanding_reqs[index].resp[1] =
		nla_get_u8(info->attrs[USBAUTH_A_BLOCKED]);
	((uint32_t *)usb_auth_outstanding_reqs[index].resp + 2)[0] =
		nla_get_u32(info->attrs[USBAUTH_A_DEV_ID]);

	usb_auth_outstanding_reqs[index].done = 1;

	wake_up_all(&usb_req_wq);

	return 0;
}

/**
 * @brief Handle a REMOVE_DEV response from userspace
 *
 * The response must contain:
 *  - USBAUTH_A_REQ_ID
 *  - USBAUTH_A_ERROR_CODE
 *  - USBAUTH_A_VALID
 *
 */
static int usb_auth_remove_dev_doit(struct sk_buff *skb, struct genl_info *info)
{
	uint32_t index = 0;

	pr_info("message received\n");

	if (!capable(CAP_SYS_ADMIN)) {
		pr_err("invalid permissions\n");
		return -EPERM;
	}

	if (!info->attrs[USBAUTH_A_REQ_ID]) {
		pr_err("invalid response: no req ID\n");
		return -EINVAL;
	}

	index = nla_get_u32(info->attrs[USBAUTH_A_REQ_ID]);

	// Test for error
	if (!info->attrs[USBAUTH_A_ERROR_CODE]) {
		pr_err("invalid response: missing attributes\n");
		usb_auth_outstanding_reqs[index].error = USBAUTH_INVRESP;
		usb_auth_outstanding_reqs[index].done = 1;
		return -EINVAL;
	}

	usb_auth_outstanding_reqs[index].error =
		nla_get_u8(info->attrs[USBAUTH_A_ERROR_CODE]);

	if (usb_auth_outstanding_reqs[index].error != USBAUTH_OK) {
		pr_err("response error\n");
		usb_auth_outstanding_reqs[index].done = 1;
		return -EINVAL;
	}

	if (!info->attrs[USBAUTH_A_VALID]) {
		pr_err("invalid response: missing attributes\n");
		usb_auth_outstanding_reqs[index].error = USBAUTH_INVRESP;
		usb_auth_outstanding_reqs[index].done = 1;
		return -EINVAL;
	}

	usb_auth_outstanding_reqs[index].resp[0] =
		nla_get_u8(info->attrs[USBAUTH_A_VALID]);

	usb_auth_outstanding_reqs[index].done = 1;

	wake_up_all(&usb_req_wq);

	return 0;
}

/**
 * @brief Handle a GEN_NONCE response from userspace
 *
 * The response must contain:
 *  - USBAUTH_A_REQ_ID
 *  - USBAUTH_A_ERROR_CODE
 *  - USBAUTH_A_NONCE
 *
 */
static int usb_auth_gen_nonce_doit(struct sk_buff *skb, struct genl_info *info)
{
	uint32_t index = 0;

	pr_info("message received\n");

	if (!capable(CAP_SYS_ADMIN)) {
		pr_err("invalid permissions\n");
		return -EPERM;
	}

	if (!info->attrs[USBAUTH_A_REQ_ID]) {
		pr_err("invalid response: no req ID\n");
		return -EINVAL;
	}

	index = nla_get_u32(info->attrs[USBAUTH_A_REQ_ID]);

	// Test for error
	if (!info->attrs[USBAUTH_A_ERROR_CODE]) {
		pr_err("invalid response: missing attributes\n");
		usb_auth_outstanding_reqs[index].error = USBAUTH_INVRESP;
		usb_auth_outstanding_reqs[index].done = 1;
		return -EINVAL;
	}

	usb_auth_outstanding_reqs[index].error =
		nla_get_u8(info->attrs[USBAUTH_A_ERROR_CODE]);

	if (usb_auth_outstanding_reqs[index].error != USBAUTH_OK) {
		pr_err("response error\n");
		usb_auth_outstanding_reqs[index].done = 1;
		return -EINVAL;
	}

	if (!info->attrs[USBAUTH_A_NONCE]) {
		pr_err("invalid response: missing attributes\n");
		usb_auth_outstanding_reqs[index].error = USBAUTH_INVRESP;
		usb_auth_outstanding_reqs[index].done = 1;
		return -EINVAL;
	}

	nla_memcpy(usb_auth_outstanding_reqs[index].resp, info->attrs[USBAUTH_A_NONCE], 32);

	usb_auth_outstanding_reqs[index].done = 1;

	wake_up_all(&usb_req_wq);

	return 0;
}

/**
 * @brief Handle a CHECK_CHALL response from userspace
 *
 * The response must contain:
 *  - USBAUTH_A_REQ_ID
 *  - USBAUTH_A_ERROR_CODE
 *  - USBAUTH_A_VALID
 *
 */
static int usb_auth_check_chall_doit(struct sk_buff *skb, struct genl_info *info)
{
	uint32_t index = 0;

	pr_info("message received\n");

	if (!capable(CAP_SYS_ADMIN)) {
		pr_err("invalid permissions\n");
		return -EPERM;
	}

	if (!info->attrs[USBAUTH_A_REQ_ID]) {
		pr_err("invalid response: no req ID\n");
		return -EINVAL;
	}

	index = nla_get_u32(info->attrs[USBAUTH_A_REQ_ID]);

	// Test for error
	if (!info->attrs[USBAUTH_A_ERROR_CODE]) {
		pr_err("invalid response: missing attributes\n");
		usb_auth_outstanding_reqs[index].error = USBAUTH_INVRESP;
		usb_auth_outstanding_reqs[index].done = 1;
		return -EINVAL;
	}

	usb_auth_outstanding_reqs[index].error =
		nla_get_u8(info->attrs[USBAUTH_A_ERROR_CODE]);

	if (usb_auth_outstanding_reqs[index].error != USBAUTH_OK) {
		pr_err("response error\n");
		usb_auth_outstanding_reqs[index].done = 1;
		return -EINVAL;
	}

	if (!info->attrs[USBAUTH_A_VALID]) {
		pr_err("invalid response: missing attributes\n");
		usb_auth_outstanding_reqs[index].error = USBAUTH_INVRESP;
		usb_auth_outstanding_reqs[index].done = 1;
		return -EINVAL;
	}

	usb_auth_outstanding_reqs[index].resp[0] =
		nla_get_u8(info->attrs[USBAUTH_A_VALID]);

	usb_auth_outstanding_reqs[index].done = 1;

	wake_up_all(&usb_req_wq);

	return 0;
}

/* Attribute validation policy */
static struct nla_policy usbauth_attr_pol[USBAUTH_A_MAX + 1] = {
	[USBAUTH_A_REQ_ID]  = {.type = NLA_U32,},
	[USBAUTH_A_DEV_ID]  = {.type = NLA_U32,},
	[USBAUTH_A_DIGEST] = {.type = NLA_UNSPEC, .len = 32,},
	[USBAUTH_A_DIGESTS] = {.type = NLA_UNSPEC, .len = 256,},
	[USBAUTH_A_SLOT_MASK]  = {.type = NLA_U8,},
	[USBAUTH_A_KNOWN]  = {.type = NLA_U8,},
	[USBAUTH_A_BLOCKED]  = {.type = NLA_U8,},
	[USBAUTH_A_VALID]  = {.type = NLA_U8,},
	[USBAUTH_A_CERTIFICATE] = {.type = NLA_UNSPEC, .max = 4096,},
	[USBAUTH_A_CERT_LEN] = {.type = NLA_U32},
	[USBAUTH_A_ROUTE] = {.type = NLA_U32},
	[USBAUTH_A_NONCE] = {.type = NLA_BINARY, .len = 32,},
	[USBAUTH_A_CHALL] = {.type = NLA_UNSPEC, .len = 204,},
	[USBAUTH_A_DESCRIPTOR] = {.type = NLA_UNSPEC, .len = USBAUTH_MAX_DESC_SIZE},
	[USBAUTH_A_BOS] = {.type = NLA_UNSPEC, .len = USBAUTH_MAX_BOS_SIZE},
	[USBAUTH_A_ERROR_CODE] = {.type = NLA_U8},
};

/* Operations */
static struct genl_ops usbauth_genl_ops[] = {
	{
		.cmd = USBAUTH_CMD_REGISTER,
		.policy = usbauth_attr_pol,
		.doit = usb_auth_register_req_doit,
	},
	{
		.cmd = USBAUTH_CMD_RESP_DIGEST,
		.policy = usbauth_attr_pol,
		.doit = usb_auth_digest_resp_doit,
	},
	{
		.cmd = USBAUTH_CMD_RESP_CERTIFICATE,
		.policy = usbauth_attr_pol,
		.doit = usb_auth_cert_resp_doit,
	},
	{
		.cmd = USBAUTH_CMD_RESP_REMOVE_DEV,
		.policy = usbauth_attr_pol,
		.doit = usb_auth_remove_dev_doit,
	},
	{
		.cmd = USBAUTH_CMD_RESP_GEN_NONCE,
		.policy = usbauth_attr_pol,
		.doit = usb_auth_gen_nonce_doit,
	},
	{
		.cmd = USBAUTH_CMD_RESP_CHECK_CHALL,
		.policy = usbauth_attr_pol,
		.doit = usb_auth_check_chall_doit,
	}
};

/* USB Authentication netlink family definition */
static struct genl_family usbauth_genl_fam = {
	.name = USBAUTH_GENL_NAME,
	.version = USBAUTH_GENL_VERSION,
	.maxattr = USBAUTH_A_MAX,
	.ops = usbauth_genl_ops,
	.n_ops = ARRAY_SIZE(usbauth_genl_ops),
	.mcgrps = NULL,
	.n_mcgrps = 0,
};

int usb_auth_init_netlink(void)
{
	int ret = 0;
	uint8_t i = 0;

	for (i = 0; i < USB_AUTH_MAX_OUTSTANDING_REQS; i++)
		usb_auth_outstanding_reqs[i].used = 0;

	init_waitqueue_head(&usb_req_wq);

	ret = genl_register_family(&usbauth_genl_fam);
	if (unlikely(ret)) {
		pr_err("failed to init netlink: %d\n",
		       ret);
		return ret;
	}

	pr_info("initialized\n");

	return ret;
}

////////////////////////////////////////////////////////////////////////////////
// Policy engine API
////////////////////////////////////////////////////////////////////////////////

int usb_policy_engine_check_digest(const uint32_t route, const uint8_t *const digests,
	const uint8_t mask, uint8_t *is_known, uint8_t *is_blocked, uint32_t *id)
{
	int ret = -1;
	void *hdr = NULL;
	struct sk_buff *skb = NULL;
	uint32_t index = 0;

	if (digests == NULL) {
		pr_err("invalid inputs\n");
		return -EINVAL;
	}

	// Arbitrary 30s wait before giving up
	if (!wait_event_timeout(usb_req_wq, pol_eng_pid != 0, HZ * WAIT_USERSPACE_TIMEOUT)) {
		pr_err("userspace not available\n");
		return -ECOMM;
	}

	pr_info("got connection to userspace\n");

	// Get a slot in the outstanding request list
	if (usb_auth_get_reqs_slot(&index)) {
		pr_err("failed to get request slot\n");
		return -ECOMM;
	}
	pr_info("got request slot\n");

	// Create digests check request
	skb = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (skb == NULL) {
		pr_err("failed to allocated buffer\n");
		return -ENOMEM;
	}

	hdr = genlmsg_put(skb, 0, 0, &usbauth_genl_fam, 0,
			  USBAUTH_CMD_CHECK_DIGEST);
	if (unlikely(hdr == NULL)) {
		pr_err("failed to place header\n");
		nlmsg_free(skb);
		return -ENOMEM;
	}

	ret = nla_put_u32(skb, USBAUTH_A_REQ_ID, index);
	if (ret) {
		pr_err("failed to place req ID\n");
		genlmsg_cancel(skb, hdr);
		nlmsg_free(skb);
		return ret;
	}

	ret = nla_put_u32(skb, USBAUTH_A_ROUTE, route);
	if (ret) {
		pr_err("failed to place route\n");
		genlmsg_cancel(skb, hdr);
		nlmsg_free(skb);
		return ret;
	}

	ret = nla_put(skb, USBAUTH_A_DIGESTS, 260, digests);
	if (ret) {
		pr_err("failed to place digests\n");
		genlmsg_cancel(skb, hdr);
		nlmsg_free(skb);
		return ret;
	}

	ret = nla_put_u8(skb, USBAUTH_A_SLOT_MASK, mask);
	if (ret) {
		pr_err("failed to place slot mask\n");
		genlmsg_cancel(skb, hdr);
		nlmsg_free(skb);
		return ret;
	}

	genlmsg_end(skb, hdr);

	// Send message to userspace
	ret = genlmsg_unicast(pol_eng_net, skb, pol_eng_pid);
	if (ret != 0) {
		pr_err("failed to send message: %d\n",
		       ret);
		return -ECOMM;
	}
	pr_info("sent CHECK_DIGEST request\n");

	// Wait for userspace response
	// Arbitrary 5 min wait before giving up
	if (!wait_event_timeout(usb_req_wq,
				usb_auth_outstanding_reqs[index].done == 1,
				HZ * WAIT_RESPONSE_TIMEOUT)) {
		pr_err("userspace response not available\n");
		usb_auth_release_reqs_slot(index);
		return -ECOMM;
	}

	pr_info("received CHECK_DIGEST response\n");

	// Get response
	if (usb_auth_outstanding_reqs[index].error == USBAUTH_INVRESP) {
		pr_err("userspace response error: %d\n",
			usb_auth_outstanding_reqs[index].error);
		usb_auth_release_reqs_slot(index);
		return -ECOMM;
	}

	*is_known = usb_auth_outstanding_reqs[index].resp[0];
	*is_blocked = usb_auth_outstanding_reqs[index].resp[1];
	*id = ((uint32_t *)usb_auth_outstanding_reqs[index].resp + 2)[0];

	// Release request slot
	usb_auth_release_reqs_slot(index);

	return 0;
}

int usb_policy_engine_check_cert_chain(const uint32_t route,
	const uint8_t *const digest, const uint8_t *const chain,
	const size_t chain_len, uint8_t *is_valid, uint8_t *is_blocked, uint32_t *id)
{
	int ret = -1;
	void *hdr = NULL;
	struct sk_buff *skb = NULL;
	uint32_t index = 0;

	if (chain == NULL || chain_len > 4096 || digest == NULL) {
		pr_err("invalid inputs\n");
		return -EINVAL;
	}

	// Arbitrary 30s wait before giving up
	if (!wait_event_timeout(usb_req_wq, pol_eng_pid != 0, HZ * WAIT_USERSPACE_TIMEOUT)) {
		pr_err("userspace not available\n");
		return -ECOMM;
	}

	pr_info("got connection to userspace\n");

	// Get a slot in the outstanding request list
	if (usb_auth_get_reqs_slot(&index)) {
		pr_err("failed to get request slot\n");
		return -ECOMM;
	}
	pr_info("got request slot\n");

	// Create digest check request
	skb = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (skb == NULL) {
		pr_err("failed to allocated buffer\n");
		return -ENOMEM;
	}

	hdr = genlmsg_put(skb, 0, 0, &usbauth_genl_fam, 0,
		USBAUTH_CMD_CHECK_CERTIFICATE);
	if (unlikely(hdr == NULL)) {
		pr_err("failed to place header\n");
		nlmsg_free(skb);
		return -ENOMEM;
	}

	ret = nla_put_u32(skb, USBAUTH_A_REQ_ID, index);
	if (ret) {
		pr_err("failed to place req ID\n");
		genlmsg_cancel(skb, hdr);
		nlmsg_free(skb);
		return ret;
	}

	ret = nla_put_u32(skb, USBAUTH_A_ROUTE, route);
	if (ret) {
		pr_err("failed to place route\n");
		genlmsg_cancel(skb, hdr);
		nlmsg_free(skb);
		return ret;
	}

	ret = nla_put(skb, USBAUTH_A_DIGEST, 32, digest);
	if (ret) {
		pr_err("failed to place digest\n");
		genlmsg_cancel(skb, hdr);
		nlmsg_free(skb);
		return ret;
	}

	ret = nla_put(skb, USBAUTH_A_CERTIFICATE, chain_len, chain);
	if (ret) {
		pr_err("failed to place certificate\n");
		genlmsg_cancel(skb, hdr);
		nlmsg_free(skb);
		return ret;
	}

	ret = nla_put_u32(skb, USBAUTH_A_CERT_LEN, chain_len);
	if (ret) {
		pr_err("failed to place chain length\n");
		genlmsg_cancel(skb, hdr);
		nlmsg_free(skb);
		return ret;
	}

	genlmsg_end(skb, hdr);

	// Send message to userspace
	ret = genlmsg_unicast(pol_eng_net, skb, pol_eng_pid);
	if (ret != 0) {
		pr_err("failed to send message: %d\n",
		       ret);
		return -ECOMM;
	}
	pr_info("sent CHECK_CERTIFICATE request\n");

	// Wait for userspace response
	// Arbitrary 5 min wait before giving up
	if (!wait_event_timeout(usb_req_wq,
				usb_auth_outstanding_reqs[index].done == 1,
				HZ * WAIT_RESPONSE_TIMEOUT)) {
		pr_err("userspace response not available\n");
		usb_auth_release_reqs_slot(index);
		return -ECOMM;
	}

	pr_info("received CHECK_CERTIFICATE response\n");

	// Get response
	*is_valid = usb_auth_outstanding_reqs[index].resp[0];
	*is_blocked = usb_auth_outstanding_reqs[index].resp[1];
	*id = ((uint32_t *)usb_auth_outstanding_reqs[index].resp + 2)[0];

	// Release request slot
	usb_auth_release_reqs_slot(index);

	return 0;
}

int usb_policy_engine_remove_dev(const uint32_t id)
{
	int ret = -1;
	void *hdr = NULL;
	struct sk_buff *skb = NULL;
	uint32_t index = 0;

	// Arbitrary 30s wait before giving up
	if (!wait_event_timeout(usb_req_wq, pol_eng_pid != 0, HZ * WAIT_USERSPACE_TIMEOUT)) {
		pr_err("userspace not available\n");
		return -ECOMM;
	}

	pr_info("got connection to userspace\n");

	// Get a slot in the outstanding request list
	if (usb_auth_get_reqs_slot(&index)) {
		pr_err("failed to get request slot\n");
		return -ECOMM;
	}
	pr_info("got request slot\n");

	// Create digest check request
	skb = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (skb == NULL) {
		pr_err("failed to allocated buffer\n");
		return -ENOMEM;
	}

	hdr = genlmsg_put(skb, 0, 0, &usbauth_genl_fam, 0,
		USBAUTH_CMD_REMOVE_DEV);
	if (unlikely(hdr == NULL)) {
		pr_err("failed to place header\n");
		nlmsg_free(skb);
		return -ENOMEM;
	}

	ret = nla_put_u32(skb, USBAUTH_A_REQ_ID, index);
	if (ret) {
		pr_err("failed to place req ID\n");
		genlmsg_cancel(skb, hdr);
		nlmsg_free(skb);
		return ret;
	}

	ret = nla_put_u32(skb, USBAUTH_A_DEV_ID, id);
	if (ret) {
		pr_err("failed to place dev ID\n");
		genlmsg_cancel(skb, hdr);
		nlmsg_free(skb);
		return ret;
	}

	genlmsg_end(skb, hdr);

	// Send message to userspace
	ret = genlmsg_unicast(pol_eng_net, skb, pol_eng_pid);
	if (ret != 0) {
		pr_err("failed to send message: %d\n",
		       ret);
		return -ECOMM;
	}
	pr_info("sent REMOVE_DEV request\n");

	// Wait for userspace response
	// Arbitrary 5 min wait before giving up
	if (!wait_event_timeout(usb_req_wq,
				usb_auth_outstanding_reqs[index].done == 1,
				HZ * WAIT_RESPONSE_TIMEOUT)) {
		pr_err("userspace response not available\n");
		usb_auth_release_reqs_slot(index);
		return -ECOMM;
	}

	pr_info("received REMOVE_DEV response\n");

	// Release request slot
	usb_auth_release_reqs_slot(index);

	return 0;
}

int usb_policy_engine_generate_challenge(const uint32_t id, uint8_t *nonce)
{
	int ret = -1;
	void *hdr = NULL;
	struct sk_buff *skb = NULL;
	uint32_t index = 0;

	// Arbitrary 30s wait before giving up
	if (!wait_event_timeout(usb_req_wq, pol_eng_pid != 0, HZ * WAIT_USERSPACE_TIMEOUT)) {
		pr_err("userspace not available\n");
		return -ECOMM;
	}

	pr_info("got connection to userspace\n");

	// Get a slot in the outstanding request list
	if (usb_auth_get_reqs_slot(&index)) {
		pr_err("failed to get request slot\n");
		return -ECOMM;
	}
	pr_info("got request slot\n");

	// Create digest check request
	skb = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (skb == NULL) {
		pr_err("failed to allocated buffer\n");
		return -ENOMEM;
	}

	hdr = genlmsg_put(skb, 0, 0, &usbauth_genl_fam, 0,
		USBAUTH_CMD_GEN_NONCE);
	if (unlikely(hdr == NULL)) {
		pr_err("failed to place header\n");
		nlmsg_free(skb);
		return -ENOMEM;
	}

	ret = nla_put_u32(skb, USBAUTH_A_REQ_ID, index);
	if (ret) {
		pr_err("failed to place req ID\n");
		genlmsg_cancel(skb, hdr);
		nlmsg_free(skb);
		return ret;
	}

	ret = nla_put_u32(skb, USBAUTH_A_DEV_ID, id);
	if (ret) {
		pr_err("failed to place dev ID\n");
		genlmsg_cancel(skb, hdr);
		nlmsg_free(skb);
		return ret;
	}

	genlmsg_end(skb, hdr);

	// Send message to userspace
	ret = genlmsg_unicast(pol_eng_net, skb, pol_eng_pid);
	if (ret != 0) {
		pr_err("failed to send message: %d\n", ret);
		return -ECOMM;
	}
	pr_info("sent GEN_NONCE request\n");

	// Wait for userspace response
	// Arbitrary 5 min wait before giving up
	if (!wait_event_timeout(usb_req_wq,
				usb_auth_outstanding_reqs[index].done == 1,
				HZ * WAIT_RESPONSE_TIMEOUT)) {
		pr_err("userspace response not available\n");
		usb_auth_release_reqs_slot(index);
		return -ECOMM;
	}

	pr_info("received GEN_NONCE response\n");

	// Get response
	memcpy(nonce, usb_auth_outstanding_reqs[index].resp, 32);

	// Release request slot
	usb_auth_release_reqs_slot(index);

	return 0;
}

int usb_policy_engine_check_challenge(const uint32_t id,
	const uint8_t *const challenge, const uint8_t *const context,
	const size_t context_size, uint8_t *is_valid)
{
	int ret = -1;
	void *hdr = NULL;
	struct sk_buff *skb = NULL;
	uint32_t index = 0;

	if (unlikely(challenge == NULL || context == NULL ||
	      context_size > USBAUTH_MAX_BOS_SIZE)) {
		pr_err("invalid inputs\n");
		return -EINVAL;
	}

	// Arbitrary 30s wait before giving up
	if (!wait_event_timeout(usb_req_wq, pol_eng_pid != 0, HZ * WAIT_USERSPACE_TIMEOUT)) {
		pr_err("userspace not available\n");
		return -ECOMM;
	}

	pr_info("got connection to userspace\n");

	// Get a slot in the outstanding request list
	if (usb_auth_get_reqs_slot(&index)) {
		pr_err("failed to get request slot\n");
		return -ECOMM;
	}
	pr_info("got request slot\n");

	// Create digest check request
	skb = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (skb == NULL) {
		pr_err("failed to allocated buffer\n");
		return -ENOMEM;
	}

	hdr = genlmsg_put(skb, 0, 0, &usbauth_genl_fam, 0,
		USBAUTH_CMD_CHECK_CHALL);
	if (unlikely(hdr == NULL)) {
		pr_err("failed to place header\n");
		nlmsg_free(skb);
		return -ENOMEM;
	}

	ret = nla_put_u32(skb, USBAUTH_A_REQ_ID, index);
	if (ret) {
		pr_err("failed to place req ID\n");
		genlmsg_cancel(skb, hdr);
		nlmsg_free(skb);
		return ret;
	}

	ret = nla_put(skb, USBAUTH_A_CHALL, 204, challenge);
	if (ret) {
		pr_err("failed to place challenge\n");
		genlmsg_cancel(skb, hdr);
		nlmsg_free(skb);
		return ret;
	}

	ret = nla_put(skb, USBAUTH_A_DESCRIPTOR, context_size, context);
	if (ret) {
		pr_err("failed to place descriptor\n");
		genlmsg_cancel(skb, hdr);
		nlmsg_free(skb);
		return ret;
	}

	ret = nla_put_u32(skb, USBAUTH_A_DEV_ID, id);
	if (ret) {
		pr_err("failed to place dev ID\n");
		genlmsg_cancel(skb, hdr);
		nlmsg_free(skb);
		return ret;
	}

	genlmsg_end(skb, hdr);

	// Send message to userspace
	ret = genlmsg_unicast(pol_eng_net, skb, pol_eng_pid);
	if (ret != 0) {
		pr_err("failed to send message: %d\n",
		       ret);
		return -ECOMM;
	}
	pr_info("sent CHECK_CHALL request\n");

	// Wait for userspace response
	// Arbitrary 5 min wait before giving up
	if (!wait_event_timeout(usb_req_wq,
				usb_auth_outstanding_reqs[index].done == 1,
				HZ * WAIT_RESPONSE_TIMEOUT)) {
		pr_err("userspace response not available\n");
		usb_auth_release_reqs_slot(index);
		return -ECOMM;
	}

	pr_info("received CHECK_CHALL response\n");

	// Get response
	*is_valid = usb_auth_outstanding_reqs[index].resp[0];

	// Release request slot
	usb_auth_release_reqs_slot(index);

	return 0;
}
