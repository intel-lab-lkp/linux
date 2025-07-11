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

#include <linux/usb/ch11.h>
#include <linux/sched.h>
#include <linux/time.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/err.h>
#include <linux/capability.h>
#include <net/genetlink.h>
#include <uapi/linux/usb/usb_auth_netlink.h>
#include "authent.h"
#include "authent_netlink.h"

#define WAIT_USERSPACE_TIMEOUT 30
#define WAIT_RESPONSE_TIMEOUT 300
#define USBAUTH_MAX_RESP_SIZE 128

/**
 * struct usb_auth_req - Define an outstanding request between the kernel and userspace
 *
 * @used:	1 if the slot is currently used, access must be protected.
 * @done:	1 if the response has been received, used as wait condition.
 * @error:	userspace response error code.
 * @resp:	arbitrary response buffer.
 */
struct usb_auth_req {
	u8 used;
	u8 done;
	u8 error;
	u8 resp[USBAUTH_MAX_RESP_SIZE];
};

static struct genl_family usbauth_genl_fam;

/* TODO: add mutex for PID access */
static u32 pol_eng_pid;
static struct net *pol_eng_net;

static wait_queue_head_t usb_req_wq;

#define USBAUTH_MAX_OUTSTANDING_REQS USB_MAXCHILDREN
/* Mutex is used to protect access to the `used` field */
DEFINE_MUTEX(usb_auth_reqs_mutex);
static struct usb_auth_req usb_auth_outstanding_reqs[USBAUTH_MAX_OUTSTANDING_REQS];

////////////////////////////////////////////////////////////////////////////////
// USB request utilities

/**
 * usb_auth_get_reqs_slot() - Find the first available slot in the outstanding requests array and
 * reserve it.
 *
 * @index: [out] reserved slot index, valid if return equals 0
 *
 * Context: this function will block on the request list mutex
 *
 * Returns:
 * * %0		- OK
 * * %-EXFULL	- too many outstanding requests
 *
 */
static int usb_auth_get_reqs_slot(u32 *index)
{
	int ret = -EXFULL;
	u32 i = 0;

	mutex_lock(&usb_auth_reqs_mutex);

	/* Take the first available slot */
	for (i = 0; i < USBAUTH_MAX_OUTSTANDING_REQS; i++) {
		if (usb_auth_outstanding_reqs[i].used == 0) {
			usb_auth_outstanding_reqs[i].used = 1;
			usb_auth_outstanding_reqs[i].done = 0;
			usb_auth_outstanding_reqs[i].error = USBAUTH_OK;
			memset(usb_auth_outstanding_reqs[i].resp, 0,
			       USBAUTH_MAX_RESP_SIZE);
			*index = i;
			ret = 0;
			break;
		}
	}

	mutex_unlock(&usb_auth_reqs_mutex);

	return ret;
}

/**
 * usb_auth_release_reqs_slot() - release a request slot
 *
 * @index : [in] slot index to be released
 *
 * context: this function will block on the request list mutex
 */
static void usb_auth_release_reqs_slot(const u32 index)
{
	mutex_lock(&usb_auth_reqs_mutex);

	usb_auth_outstanding_reqs[index].used = 0;

	mutex_unlock(&usb_auth_reqs_mutex);
}

////////////////////////////////////////////////////////////////////////////////
// Generic netlink socket utilities

/**
 * usb_auth_register_req_doit() - Handle a registration request from userspace
 *
 * @skb:	[in] Netlink socket buffer
 * @info:	[in] Generic Netlink receiving information
 *
 * It will overwrite the current userspace registered PID with the one provided
 * in the request.
 *
 * Returns:
 * * %0		- OK
 * * %-EPERM	- Permission denied for netlink usage
 * * %-ENOMEM	- if message creation failed
 * * %-EMSGSIZE	- if message creation failed
 */
static int usb_auth_register_req_doit(struct sk_buff *skb, struct genl_info *info)
{
	int ret = 0;
	void *hdr = NULL;
	struct sk_buff *msg = NULL;

	if (!netlink_capable(skb, CAP_SYS_ADMIN)) {
		pr_err("%s: invalid permissions\n", __func__);
		return -EPERM;
	}

	pol_eng_pid = info->snd_portid;
	pol_eng_net = genl_info_net(info);

	wake_up_all(&usb_req_wq);

	msg = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (msg == NULL) {
		pr_err("%s: failed to allocate message buffer\n", __func__);
		return -ENOMEM;
	}

	hdr = genlmsg_put(msg, info->snd_portid, info->snd_seq,
			  &usbauth_genl_fam, 0, USBAUTH_CMD_REGISTER);
	if (hdr == NULL) {
		pr_err("%s: failed to create genetlink header\n", __func__);
		nlmsg_free(msg);
		return -EMSGSIZE;
	}

	genlmsg_end(msg, hdr);

	ret = genlmsg_reply(msg, info);

	return ret;
}

/**
 * usb_auth_digest_resp_doit() - Handle a CHECK_DIGEST response from userspace
 *
 * @skb:	[in] Netlink socket buffer
 * @info:	[in] Generic Netlink receiving information
 *
 * The response must contain:
 *  - USBAUTH_A_REQ_ID
 *  - USBAUTH_A_ERROR_CODE
 *  - USBAUTH_A_DEV_ID
 *  - USBAUTH_A_KNOWN
 *  - USBAUTH_A_BLOCKED
 *
 * Returns:
 * * %0		- OK
 * * %-EPERM	- Permission denied for netlink usage.
 * * %-ECOMM	- Netlink communication failure
 * * %-EINVAL	- Invalid value in messages
 */
static int usb_auth_digest_resp_doit(struct sk_buff *skb, struct genl_info *info)
{
	u32 index = 0;

	if (!netlink_capable(skb, CAP_SYS_ADMIN)) {
		pr_err("%s: invalid permissions\n", __func__);
		return -EPERM;
	}

	if (!pol_eng_pid || !pol_eng_net) {
		pr_err("%s: No policy engine registered\n", __func__);
		return -ECOMM;
	}

	if (info->snd_portid != pol_eng_pid) {
		pr_err("Sender id differ from registered policy engine\n");
		return -EPERM;
	}

	if (!info->attrs[USBAUTH_A_REQ_ID]) {
		pr_err("%s: invalid response: no req ID\n", __func__);
		return -EINVAL;
	}

	index = nla_get_u32(info->attrs[USBAUTH_A_REQ_ID]);

	if (!info->attrs[USBAUTH_A_ERROR_CODE]) {
		pr_err("%s: invalid response: missing attributes\n", __func__);
		usb_auth_outstanding_reqs[index].error = USBAUTH_INVRESP;
		usb_auth_outstanding_reqs[index].done = 1;
		return -EINVAL;
	}

	usb_auth_outstanding_reqs[index].error =
		nla_get_u8(info->attrs[USBAUTH_A_ERROR_CODE]);

	if (usb_auth_outstanding_reqs[index].error != USBAUTH_OK) {
		pr_err("%s: response error\n", __func__);
		usb_auth_outstanding_reqs[index].done = 1;
		return -EINVAL;
	}

	if (!info->attrs[USBAUTH_A_DEV_ID] || !info->attrs[USBAUTH_A_KNOWN] ||
	    !info->attrs[USBAUTH_A_BLOCKED]) {
		pr_err("%s: invalid response: missing attributes\n", __func__);
		usb_auth_outstanding_reqs[index].error = USBAUTH_INVRESP;
		usb_auth_outstanding_reqs[index].done = 1;
		return -EINVAL;
	}

	usb_auth_outstanding_reqs[index].resp[0] =
		nla_get_u8(info->attrs[USBAUTH_A_KNOWN]);
	usb_auth_outstanding_reqs[index].resp[1] =
		nla_get_u8(info->attrs[USBAUTH_A_BLOCKED]);
	((u32 *)usb_auth_outstanding_reqs[index].resp + 2)[0] =
		nla_get_u32(info->attrs[USBAUTH_A_DEV_ID]);

	usb_auth_outstanding_reqs[index].done = 1;

	wake_up_all(&usb_req_wq);

	return 0;
}

/**
 * usb_auth_cert_resp_doit() - Handle a CHECK_CERTIFICATE response from userspace
 *
 * @skb:	[in] Netlink socket buffer
 * @info:	[in] Generic Netlink receiving information
 *
 * The response must contain:
 *  - USBAUTH_A_REQ_ID
 *  - USBAUTH_A_ERROR_CODE
 *  - USBAUTH_A_VALID
 *  - USBAUTH_A_BLOCKED
 *  - USBAUTH_A_DEV_ID
 *
 * Returns:
 * * %0		- OK
 * * %-EPERM	- Permission denied for netlink usage
 * * %-ECOMM	- Netlink communication failure
 * * %-EINVAL	- Invalid value in messages
 *
 */
static int usb_auth_cert_resp_doit(struct sk_buff *skb, struct genl_info *info)
{
	u32 index = 0;

	if (!netlink_capable(skb, CAP_SYS_ADMIN)) {
		pr_err("%s: invalid permissions\n", __func__);
		return -EPERM;
	}

	if (!pol_eng_pid || !pol_eng_net) {
		pr_err("%s: No policy engine registered", __func__);
		return -ECOMM;
	}

	if (info->snd_portid != pol_eng_pid) {
		pr_err("Sender id differ from registered policy engine\n");
		return -EPERM;
	}

	if (!info->attrs[USBAUTH_A_REQ_ID]) {
		pr_err("%s: invalid response: no req ID\n", __func__);
		return -EINVAL;
	}

	index = nla_get_u32(info->attrs[USBAUTH_A_REQ_ID]);

	if (!info->attrs[USBAUTH_A_ERROR_CODE]) {
		pr_err("%s: invalid response: missing attributes\n", __func__);
		usb_auth_outstanding_reqs[index].error = USBAUTH_INVRESP;
		usb_auth_outstanding_reqs[index].done = 1;
		return -EINVAL;
	}

	usb_auth_outstanding_reqs[index].error =
		nla_get_u8(info->attrs[USBAUTH_A_ERROR_CODE]);

	if (usb_auth_outstanding_reqs[index].error != USBAUTH_OK) {
		pr_err("%s: response error\n", __func__);
		usb_auth_outstanding_reqs[index].done = 1;
		return -EINVAL;
	}

	if (!info->attrs[USBAUTH_A_DEV_ID] || !info->attrs[USBAUTH_A_VALID] ||
	    !info->attrs[USBAUTH_A_BLOCKED]) {
		pr_err("%s: invalid response: missing attributes\n", __func__);
		usb_auth_outstanding_reqs[index].done = 1;
		usb_auth_outstanding_reqs[index].error = USBAUTH_INVRESP;
		return -EINVAL;
	}

	usb_auth_outstanding_reqs[index].resp[0] =
		nla_get_u8(info->attrs[USBAUTH_A_VALID]);
	usb_auth_outstanding_reqs[index].resp[1] =
		nla_get_u8(info->attrs[USBAUTH_A_BLOCKED]);
	((u32 *)usb_auth_outstanding_reqs[index].resp + 2)[0] =
		nla_get_u32(info->attrs[USBAUTH_A_DEV_ID]);

	usb_auth_outstanding_reqs[index].done = 1;

	wake_up_all(&usb_req_wq);

	return 0;
}

/**
 * usb_auth_gen_nonce_doit() - Handle a GEN_NONCE response from userspace
 *
 * @skb:	[in] Netlink socket buffer
 * @info:	[in] Generic Netlink receiving information
 *
 * The response must contain:
 *  - USBAUTH_A_REQ_ID
 *  - USBAUTH_A_ERROR_CODE
 *  - USBAUTH_A_NONCE
 *
 * Returns:
 * * %0		- OK
 * * %-EPERM	- Permission denied for netlink usage
 * * %-ECOMM	- Netlink communication failure
 * * %-EINVAL	- Invalid value in messages
 */
static int usb_auth_gen_nonce_doit(struct sk_buff *skb, struct genl_info *info)
{
	u32 index = 0;

	if (!netlink_capable(skb, CAP_SYS_ADMIN)) {
		pr_err("%s: invalid permissions\n", __func__);
		return -EPERM;
	}

	if (!pol_eng_pid || !pol_eng_net) {
		pr_err("%s: No policy engine registered", __func__);
		return -ECOMM;
	}

	if (info->snd_portid != pol_eng_pid) {
		pr_err("Sender id differ from registered policy engine\n");
		return -EPERM;
	}

	if (!info->attrs[USBAUTH_A_REQ_ID]) {
		pr_err("%s: invalid response: no req ID\n", __func__);
		return -EINVAL;
	}

	index = nla_get_u32(info->attrs[USBAUTH_A_REQ_ID]);

	if (!info->attrs[USBAUTH_A_ERROR_CODE]) {
		pr_err("%s: invalid response: missing attributes\n", __func__);
		usb_auth_outstanding_reqs[index].error = USBAUTH_INVRESP;
		usb_auth_outstanding_reqs[index].done = 1;
		return -EINVAL;
	}

	usb_auth_outstanding_reqs[index].error =
		nla_get_u8(info->attrs[USBAUTH_A_ERROR_CODE]);

	if (usb_auth_outstanding_reqs[index].error != USBAUTH_OK) {
		pr_err("%s: response error\n", __func__);
		usb_auth_outstanding_reqs[index].done = 1;
		return -EINVAL;
	}

	if (!info->attrs[USBAUTH_A_NONCE]) {
		pr_err("%s: invalid response: missing attributes\n", __func__);
		usb_auth_outstanding_reqs[index].error = USBAUTH_INVRESP;
		usb_auth_outstanding_reqs[index].done = 1;
		return -EINVAL;
	}

	nla_memcpy(usb_auth_outstanding_reqs[index].resp,
				info->attrs[USBAUTH_A_NONCE],
				USBAUTH_NONCE_SIZE);
	usb_auth_outstanding_reqs[index].done = 1;
	wake_up_all(&usb_req_wq);

	return 0;
}

/**
 * usb_auth_check_chall_doit() - Handle a CHECK_CHALL response from userspace
 *
 * @skb:	[in] Netlink socket buffer
 * @info:	[in] Generic Netlink receiving information
 *
 * The response must contain:
 *  - USBAUTH_A_REQ_ID
 *  - USBAUTH_A_ERROR_CODE
 *  - USBAUTH_A_VALID
 *
 * Returns:
 * * %0		- OK
 * * %-EPERM	- Permission denied for netlink usage
 * * %-ECOMM	- Netlink communication failure
 * * %-EINVAL	- Invalid value in messages
 */
static int usb_auth_check_chall_doit(struct sk_buff *skb, struct genl_info *info)
{
	u32 index = 0;

	if (!netlink_capable(skb, CAP_SYS_ADMIN)) {
		pr_err("%s: invalid permissions\n", __func__);
		return -EPERM;
	}

	if (!pol_eng_pid || !pol_eng_net) {
		pr_err("%s: No policy engine registered", __func__);
		return -ECOMM;
	}

	if (info->snd_portid != pol_eng_pid) {
		pr_err("Sender id differ from registered policy engine\n");
		return -EPERM;
	}

	if (!info->attrs[USBAUTH_A_REQ_ID]) {
		pr_err("%s: invalid response: no req ID\n", __func__);
		return -EINVAL;
	}

	index = nla_get_u32(info->attrs[USBAUTH_A_REQ_ID]);

	if (!info->attrs[USBAUTH_A_ERROR_CODE]) {
		pr_err("%s: invalid response: missing attributes\n", __func__);
		usb_auth_outstanding_reqs[index].error = USBAUTH_INVRESP;
		usb_auth_outstanding_reqs[index].done = 1;
		return -EINVAL;
	}

	usb_auth_outstanding_reqs[index].error =
		nla_get_u8(info->attrs[USBAUTH_A_ERROR_CODE]);

	if (usb_auth_outstanding_reqs[index].error != USBAUTH_OK) {
		pr_err("%s: response error\n", __func__);
		usb_auth_outstanding_reqs[index].done = 1;
		return -EINVAL;
	}

	if (!info->attrs[USBAUTH_A_VALID]) {
		pr_err("%s: invalid response: missing attributes\n", __func__);
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

static struct nla_policy usbauth_attr_pol[USBAUTH_A_MAX + 1] = {
	[USBAUTH_A_REQ_ID]  = {.type = NLA_U32,},
	[USBAUTH_A_DEV_ID]  = {.type = NLA_U32,},
	[USBAUTH_A_DIGEST] = {.type = NLA_UNSPEC, .len = USBAUTH_DIGEST_SIZE,},
	[USBAUTH_A_DIGESTS] = {.type = NLA_UNSPEC, .len = USBAUTH_DIGESTS_SIZE,},
	[USBAUTH_A_SLOT_MASK]  = {.type = NLA_U8,},
	[USBAUTH_A_KNOWN]  = {.type = NLA_U8,},
	[USBAUTH_A_BLOCKED]  = {.type = NLA_U8,},
	[USBAUTH_A_VALID]  = {.type = NLA_U8,},
	[USBAUTH_A_CERTIFICATE] = {.type = NLA_UNSPEC, .max = USBAUTH_MAX_CERT_SIZE,},
	[USBAUTH_A_CERT_LEN] = {.type = NLA_U32},
	[USBAUTH_A_ROUTE] = {.type = NLA_U32},
	[USBAUTH_A_NONCE] = {.type = NLA_BINARY, .len = USBAUTH_NONCE_SIZE,},
	[USBAUTH_A_CHALL] = {.type = NLA_UNSPEC, .len = USBAUTH_CHALL_SIZE,},
	[USBAUTH_A_DESCRIPTOR] = {.type = NLA_UNSPEC, .len = USBAUTH_MAX_DESC_SIZE},
	[USBAUTH_A_BOS] = {.type = NLA_UNSPEC, .len = USBAUTH_MAX_BOS_SIZE},
	[USBAUTH_A_ERROR_CODE] = {.type = NLA_U8},
};

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

static struct genl_family usbauth_genl_fam = {
	.name = USBAUTH_GENL_NAME,
	.version = USBAUTH_GENL_VERSION,
	.maxattr = USBAUTH_A_MAX,
	.ops = usbauth_genl_ops,
	.n_ops = ARRAY_SIZE(usbauth_genl_ops),
	.mcgrps = NULL,
	.n_mcgrps = 0,
};

/**
 * usb_auth_init_netlink() - Initialises the netlink socket for userspace usb
 * authentication policy engine to register.
 *
 * Returns:
 * * %0 - OK
 */

int usb_auth_init_netlink(void)
{
	int ret = 0;
	u8 i = 0;

	for (i = 0; i < USBAUTH_MAX_OUTSTANDING_REQS; i++)
		usb_auth_outstanding_reqs[i].used = 0;

	init_waitqueue_head(&usb_req_wq);
	ret = genl_register_family(&usbauth_genl_fam);
	if (ret) {
		pr_err("%s: failed to init netlink: %d\n",
		       __func__, ret);
		return ret;
	}

	return ret;
}

////////////////////////////////////////////////////////////////////////////////
// Policy engine API

/**
 * usb_policy_engine_check_digest - Check if a digest match a device
 *
 * @route:	[in]	Information on the device to construct the ID
 * @digests:	[in]	USB Authentication digest, must be 256 B
 * @mask:	[in]	USB Authentication slot mask
 * @is_known:	[out]	1 at each index with a known digest, 0 otherwise
 * @is_blocked: [out]	1 if the device is known and banned, 0 otherwise
 * @id:		[out]	if is_known and !is_blocked then contains the device handle
 *
 * This function blocks until a response has been received from userspace or in
 * case of timeout.
 * The function blocks if no policy engine is registered with a timeout.
 *
 * Context: task context, might sleep.
 *
 * Returns:
 * * %0		- OK
 * * %-EINVAL	- if digest is NULL
 * * %-ECOMM	- if userspace policy engine is not available or busy
 *		  or message transmission failed
 * * %-ENOMEM	- if message creation failed
 * * %-EMSGSIZE	- if message creation failed
 *
 *
 */
int usb_policy_engine_check_digest(const u32 route, const u8 *const digests,
	const u8 mask, u8 *is_known, u8 *is_blocked, u32 *id)
{
	int ret = -1;
	void *hdr = NULL;
	struct sk_buff *skb = NULL;
	u32 index = 0;

	if (digests == NULL) {
		pr_err("%s: invalid inputs\n", __func__);
		return -EINVAL;
	}

	if (!wait_event_timeout(usb_req_wq, pol_eng_pid != 0, HZ * WAIT_USERSPACE_TIMEOUT)) {
		pr_err("%s: userspace not available\n", __func__);
		return -ECOMM;
	}

	if (usb_auth_get_reqs_slot(&index)) {
		pr_err("%s: failed to get request slot\n", __func__);
		return -ECOMM;
	}
	skb = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (skb == NULL) {
		pr_err("%s: failed to allocated buffer\n", __func__);
		return -ENOMEM;
	}

	hdr = genlmsg_put(skb, 0, 0, &usbauth_genl_fam, 0,
			  USBAUTH_CMD_CHECK_DIGEST);
	if (hdr == NULL) {
		pr_err("%s: failed to place header\n", __func__);
		nlmsg_free(skb);
		return -ENOMEM;
	}

	ret = nla_put_u32(skb, USBAUTH_A_REQ_ID, index);
	if (ret) {
		pr_err("%s: failed to place req ID\n", __func__);
		genlmsg_cancel(skb, hdr);
		nlmsg_free(skb);
		return ret;
	}

	ret = nla_put_u32(skb, USBAUTH_A_ROUTE, route);
	if (ret) {
		pr_err("%s: failed to place route\n", __func__);
		genlmsg_cancel(skb, hdr);
		nlmsg_free(skb);
		return ret;
	}

	ret = nla_put(skb, USBAUTH_A_DIGESTS, USBAUTH_DIGESTS_SIZE, digests);
	if (ret) {
		pr_err("%s: failed to place digests\n", __func__);
		genlmsg_cancel(skb, hdr);
		nlmsg_free(skb);
		return ret;
	}

	ret = nla_put_u8(skb, USBAUTH_A_SLOT_MASK, mask);
	if (ret) {
		pr_err("%s: failed to place slot mask\n", __func__);
		genlmsg_cancel(skb, hdr);
		nlmsg_free(skb);
		return ret;
	}

	genlmsg_end(skb, hdr);

	ret = genlmsg_unicast(pol_eng_net, skb, pol_eng_pid);
	if (ret != 0) {
		pr_err("%s: failed to send message: %d\n",
		       __func__, ret);
		return -ECOMM;
	}

	if (!wait_event_timeout(usb_req_wq,
				usb_auth_outstanding_reqs[index].done == 1,
				HZ * WAIT_RESPONSE_TIMEOUT)) {
		pr_err("%s: userspace response not available\n", __func__);
		usb_auth_release_reqs_slot(index);
		return -ECOMM;
	}


	if (usb_auth_outstanding_reqs[index].error == USBAUTH_INVRESP) {
		pr_err("%s: userspace response error: %d\n",
			__func__, usb_auth_outstanding_reqs[index].error);
		usb_auth_release_reqs_slot(index);
		return -ECOMM;
	}

	*is_known = usb_auth_outstanding_reqs[index].resp[0];
	*is_blocked = usb_auth_outstanding_reqs[index].resp[1];
	*id = ((u32 *)usb_auth_outstanding_reqs[index].resp + 2)[0];

	usb_auth_release_reqs_slot(index);

	return 0;
}

/**
 * usb_policy_engine_check_cert_chain() - Check if a certificate chain is valid and authorized
 *
 * @route:	[in]	Information on the device to construct the ID
 * @digest:	[in]	Digest corresponding to the certificate chain
 * @chain:	[in]	Certificate chain to check, at most 4096 bytes
 * @chain_len:	[in]	Certificate chain length
 * @is_valid:	[out]	1 if the certificate chain can be validated
 * @is_blocked:	[out]	1 if the chain is valid but one of the certificate is blocked
 * @id:		[out]	If is_known and !is_blocked then contains the device handle
 *
 * A certificate chain is valid if it can be successfully verified with one of the
 *  root CA in store.
 * A certificate chain is blocked if one of the certificate of chain is blocked,
 *  due to revocation, blacklist...
 *
 * Context: task context, might sleep.
 *
 * Returns:
 * * %0		- OK
 * * %-EINVAL	- if digest is NULL
 * * %-ECOMM	- if userspace policy engine is not available or busy
 *		  or message transmission failed
 * * %-ENOMEM	- if message creation failed
 * * %-EMSGSIZE	- if message creation failed
 *
 */

int usb_policy_engine_check_cert_chain(const u32 route,
	const u8 *const digest, const u8 *const chain,
	const size_t chain_len, u8 *is_valid, u8 *is_blocked, u32 *id)
{
	int ret = -1;
	void *hdr = NULL;
	struct sk_buff *skb = NULL;
	u32 index = 0;

	if (chain == NULL || chain_len > USBAUTH_MAX_CERT_SIZE || digest == NULL) {
		pr_err("%s: invalid inputs\n", __func__);
		return -EINVAL;
	}

	if (!wait_event_timeout(usb_req_wq, pol_eng_pid != 0, HZ * WAIT_USERSPACE_TIMEOUT)) {
		pr_err("%s: userspace not available\n", __func__);
		return -ECOMM;
	}

	if (usb_auth_get_reqs_slot(&index)) {
		pr_err("%s: failed to get request slot\n", __func__);
		return -ECOMM;
	}
	skb = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (skb == NULL) {
		pr_err("%s: failed to allocated buffer\n", __func__);
		return -ENOMEM;
	}

	hdr = genlmsg_put(skb, 0, 0, &usbauth_genl_fam, 0,
		USBAUTH_CMD_CHECK_CERTIFICATE);
	if (hdr == NULL) {
		pr_err("%s: failed to place header\n", __func__);
		nlmsg_free(skb);
		return -ENOMEM;
	}

	ret = nla_put_u32(skb, USBAUTH_A_REQ_ID, index);
	if (ret) {
		pr_err("%s: failed to place req ID\n", __func__);
		genlmsg_cancel(skb, hdr);
		nlmsg_free(skb);
		return ret;
	}

	ret = nla_put_u32(skb, USBAUTH_A_ROUTE, route);
	if (ret) {
		pr_err("%s: failed to place route\n", __func__);
		genlmsg_cancel(skb, hdr);
		nlmsg_free(skb);
		return ret;
	}

	ret = nla_put(skb, USBAUTH_A_DIGEST, USBAUTH_DIGEST_SIZE, digest);
	if (ret) {
		pr_err("%s: failed to place digest\n", __func__);
		genlmsg_cancel(skb, hdr);
		nlmsg_free(skb);
		return ret;
	}

	ret = nla_put(skb, USBAUTH_A_CERTIFICATE, chain_len, chain);
	if (ret) {
		pr_err("%s: failed to place certificate\n", __func__);
		genlmsg_cancel(skb, hdr);
		nlmsg_free(skb);
		return ret;
	}

	ret = nla_put_u32(skb, USBAUTH_A_CERT_LEN, chain_len);
	if (ret) {
		pr_err("%s: failed to place chain length\n", __func__);
		genlmsg_cancel(skb, hdr);
		nlmsg_free(skb);
		return ret;
	}

	genlmsg_end(skb, hdr);

	ret = genlmsg_unicast(pol_eng_net, skb, pol_eng_pid);
	if (ret != 0) {
		pr_err("%s: failed to send message: %d\n",
		       __func__, ret);
		return -ECOMM;
	}

	if (!wait_event_timeout(usb_req_wq,
				usb_auth_outstanding_reqs[index].done == 1,
				HZ * WAIT_RESPONSE_TIMEOUT)) {
		pr_err("%s: userspace response not available\n", __func__);
		usb_auth_release_reqs_slot(index);
		return -ECOMM;
	}

	*is_valid = usb_auth_outstanding_reqs[index].resp[0];
	*is_blocked = usb_auth_outstanding_reqs[index].resp[1];
	*id = ((u32 *)usb_auth_outstanding_reqs[index].resp + 2)[0];

	usb_auth_release_reqs_slot(index);

	return 0;
}

/**
 * usb_policy_engine_generate_challenge() - Generate a nonce for the authentication challenge
 *
 * @id:		[in]	Device ID
 * @nonce:	[out]	32 bytes nonce buffer, caller allocated
 *
 * Context: task context, might sleep.
 *
 * Returns:
 * * %0		- OK
 * * %-EINVAL	- if digest is NULL
 * * %-ECOMM	- if userspace policy engine is not available or busy
 *                or message transmission failed
 * * %-ENOMEM	- if message creation failed
 * * %-EMSGSIZE	- if message creation failed
 */
int usb_policy_engine_generate_challenge(const u32 id, u8 *nonce)
{
	int ret = -1;
	void *hdr = NULL;
	struct sk_buff *skb = NULL;
	u32 index = 0;

	/* Arbitrary 30s wait before giving up */
	if (!wait_event_timeout(usb_req_wq, pol_eng_pid != 0, HZ * WAIT_USERSPACE_TIMEOUT)) {
		pr_err("%s: userspace not available\n", __func__);
		return -ECOMM;
	}

	if (usb_auth_get_reqs_slot(&index)) {
		pr_err("%s: failed to get request slot\n", __func__);
		return -ECOMM;
	}
	skb = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (skb == NULL) {
		pr_err("%s: failed to allocated buffer\n", __func__);
		return -ENOMEM;
	}

	hdr = genlmsg_put(skb, 0, 0, &usbauth_genl_fam, 0,
		USBAUTH_CMD_GEN_NONCE);
	if (hdr == NULL) {
		pr_err("%s: failed to place header\n", __func__);
		nlmsg_free(skb);
		return -ENOMEM;
	}

	ret = nla_put_u32(skb, USBAUTH_A_REQ_ID, index);
	if (ret) {
		pr_err("%s: failed to place req ID\n", __func__);
		genlmsg_cancel(skb, hdr);
		nlmsg_free(skb);
		return ret;
	}

	ret = nla_put_u32(skb, USBAUTH_A_DEV_ID, id);
	if (ret) {
		pr_err("%s: failed to place dev ID\n", __func__);
		genlmsg_cancel(skb, hdr);
		nlmsg_free(skb);
		return ret;
	}

	genlmsg_end(skb, hdr);

	ret = genlmsg_unicast(pol_eng_net, skb, pol_eng_pid);
	if (ret != 0) {
		pr_err("%s: failed to send message: %d\n", __func__, ret);
		return -ECOMM;
	}

	if (!wait_event_timeout(usb_req_wq,
				usb_auth_outstanding_reqs[index].done == 1,
				HZ * WAIT_RESPONSE_TIMEOUT)) {
		pr_err("%s: userspace response not available\n", __func__);
		usb_auth_release_reqs_slot(index);
		return -ECOMM;
	}

	memcpy(nonce, usb_auth_outstanding_reqs[index].resp, USBAUTH_NONCE_SIZE);
	usb_auth_release_reqs_slot(index);

	return 0;
}

/**
 * usb_policy_engine_check_challenge() - Validate the authentication challenge
 *
 * @id:			[in]	device handle
 * @challenge:		[in]	challenge response, must be 204 bytes.
 *				@challenge is the concatenation of :
 *				message (140B) | signature (64B)
 * @context:		[in]	usb device context
 * @context_size:	[in]	usb device context size
 * @is_valid:		[out]	1 if the signature is valid, 0 otherwise
 *
 * Check that the response challenge contains the right nonce
 * Check that the device signature is valid
 *
 * Context: task context, might sleep.
 *
 * Returns:
 * * %0		- OK
 * * %-EINVAL	- if challenge, desc or bos is NULL or invalid parameter size
 * * %-ECOMM	- if userspace policy engine is not available or busy
 *		  or message transmission failed
 * * %-ENOMEM	- if message creation failed
 * * %-EMSGSIZE	- if message creation failed
 */
int usb_policy_engine_check_challenge(const u32 id,
	const u8 *const challenge, const u8 *const context,
	const size_t context_size, u8 *is_valid)
{
	int ret = -1;
	void *hdr = NULL;
	struct sk_buff *skb = NULL;
	u32 index = 0;

	if (challenge == NULL || context == NULL ||
	      context_size > USBAUTH_MAX_BOS_SIZE) {
		pr_err("%s: invalid inputs\n", __func__);
		return -EINVAL;
	}

	if (!wait_event_timeout(usb_req_wq, pol_eng_pid != 0, HZ * WAIT_USERSPACE_TIMEOUT)) {
		pr_err("%s: userspace not available\n", __func__);
		return -ECOMM;
	}

	if (usb_auth_get_reqs_slot(&index)) {
		pr_err("%s: failed to get request slot\n", __func__);
		return -ECOMM;
	}
	skb = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (skb == NULL) {
		pr_err("%s: failed to allocated buffer\n", __func__);
		return -ENOMEM;
	}

	hdr = genlmsg_put(skb, 0, 0, &usbauth_genl_fam, 0,
		USBAUTH_CMD_CHECK_CHALL);
	if (hdr == NULL) {
		pr_err("%s: failed to place header\n", __func__);
		nlmsg_free(skb);
		return -ENOMEM;
	}

	ret = nla_put_u32(skb, USBAUTH_A_REQ_ID, index);
	if (ret) {
		pr_err("%s: failed to place req ID\n", __func__);
		genlmsg_cancel(skb, hdr);
		nlmsg_free(skb);
		return ret;
	}

	ret = nla_put(skb, USBAUTH_A_CHALL, USBAUTH_CHALL_SIZE, challenge);
	if (ret) {
		pr_err("%s: failed to place challenge\n", __func__);
		genlmsg_cancel(skb, hdr);
		nlmsg_free(skb);
		return ret;
	}

	ret = nla_put(skb, USBAUTH_A_DESCRIPTOR, context_size, context);
	if (ret) {
		pr_err("%s: failed to place descriptor\n", __func__);
		genlmsg_cancel(skb, hdr);
		nlmsg_free(skb);
		return ret;
	}

	ret = nla_put_u32(skb, USBAUTH_A_DEV_ID, id);
	if (ret) {
		pr_err("%s: failed to place dev ID\n", __func__);
		genlmsg_cancel(skb, hdr);
		nlmsg_free(skb);
		return ret;
	}

	genlmsg_end(skb, hdr);

	ret = genlmsg_unicast(pol_eng_net, skb, pol_eng_pid);
	if (ret != 0) {
		pr_err("%s: failed to send message: %d\n",
		       __func__, ret);
		return -ECOMM;
	}
	if (!wait_event_timeout(usb_req_wq,
				usb_auth_outstanding_reqs[index].done == 1,
				HZ * WAIT_RESPONSE_TIMEOUT)) {
		pr_err("%s: userspace response not available\n", __func__);
		usb_auth_release_reqs_slot(index);
		return -ECOMM;
	}

	*is_valid = usb_auth_outstanding_reqs[index].resp[0];
	usb_auth_release_reqs_slot(index);

	return 0;
}
