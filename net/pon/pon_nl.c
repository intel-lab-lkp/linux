// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2026 John Crispin <john@phrozen.org> */

#include <linux/atomic.h>
#include <linux/limits.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/xarray.h>
#include <net/genetlink.h>
#include <net/pon.h>
#include <net/sock.h>

#include "pon-nl-gen.h"
#include "pon.h"

static const char *const pon_nl_mode_names[] = {
	[PON_MODE_GPON]		= "gpon",
	[PON_MODE_XG_PON]	= "xg-pon",
	[PON_MODE_XGS_PON]	= "xgs-pon",
};

/* The object dumps walk every device the caller's namespace can see, or the
 * one device the request names and within a device one list. A dump that
 * fills its skb stops at the object it could not fit and resumes there:
 * args[0] is the device, args[1] the objects of it already sent. The count
 * is only good for the list it was taken from, so every object that joins or
 * leaves a list moves the generation and a dump that resumes on another
 * generation is marked as interrupted. The generation is read under the lock
 * that the walk holds, so no change falls between the two.
 */
typedef int (*pon_nl_obj_fill_t)(struct pon_dev *pdev, struct list_head *pos,
				 struct sk_buff *rsp,
				 const struct genl_info *info);

typedef int (*pon_nl_dev_fill_t)(struct pon_dev *pdev, struct sk_buff *rsp,
				 const struct genl_info *info);

static atomic_t pon_nl_obj_gen = ATOMIC_INIT(1);

/* Netlink helpers */

/**
 * pon_nl_dev_reply() - answer a request with one message about the device
 * @info: the request info, user_ptr[0] holds the device
 * @fill: puts the message into the reply
 *
 * Context: Called with the device lock held by pon_device_get_locked().
 * Return: 0, -ENOMEM, the error of @fill, or the error of genlmsg_reply().
 */
static int pon_nl_dev_reply(struct genl_info *info, pon_nl_dev_fill_t fill)
{
	struct pon_dev *pdev = info->user_ptr[0];
	struct sk_buff *rsp;
	int err;

	rsp = genlmsg_new(GENLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!rsp)
		return -ENOMEM;

	err = fill(pdev, rsp, info);
	if (err) {
		nlmsg_free(rsp);
		return err;
	}

	return genlmsg_reply(rsp, info);
}

/**
 * pon_nl_obj_reply() - answer a request with one message about an object
 * @info: the request info, user_ptr[0] holds the device
 * @pos: the list entry of the object
 * @fill: puts the message into the reply
 *
 * Context: Called with the device lock held by pon_device_get_locked().
 * Return: 0, -ENOMEM, the error of @fill, or the error of genlmsg_reply().
 */
static int pon_nl_obj_reply(struct genl_info *info, struct list_head *pos,
			    pon_nl_obj_fill_t fill)
{
	struct pon_dev *pdev = info->user_ptr[0];
	struct sk_buff *rsp;
	int err;

	rsp = genlmsg_new(GENLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!rsp)
		return -ENOMEM;

	err = fill(pdev, pos, rsp, info);
	if (err) {
		nlmsg_free(rsp);
		return err;
	}

	return genlmsg_reply(rsp, info);
}

/**
 * pon_nl_notify_obj() - send an object notification to the mgmt group
 * @pdev: the PON device
 * @pos: the list entry of the object
 * @cmd: the notification command
 * @fill: puts the object into the notification
 *
 * Object notifications share the GET reply format, so a listener parses one
 * message shape whether it asked for the object or was told about it. Does
 * nothing when the group has no listener in the device's namespace.
 *
 * Context: Called with @pdev->lock held. May sleep.
 */
static void pon_nl_notify_obj(struct pon_dev *pdev, struct list_head *pos,
			      u32 cmd, pon_nl_obj_fill_t fill)
{
	struct net *net = dev_net(pdev->main_netdev);
	struct genl_info info;
	struct sk_buff *ntf;

	lockdep_assert_held(&pdev->lock);

	if (!genl_has_listeners(&pon_nl_family, net, PON_NLGRP_MGMT))
		return;

	ntf = genlmsg_new(GENLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!ntf)
		return;

	genl_info_init_ntf(&info, &pon_nl_family, cmd);
	genl_info_net_set(&info, net);
	if (fill(pdev, pos, ntf, &info)) {
		nlmsg_free(ntf);
		return;
	}

	genlmsg_multicast_netns(&pon_nl_family, net, ntf, 0, PON_NLGRP_MGMT,
				GFP_KERNEL);
}

/* Device lookup and locking */

/**
 * pon_device_get_and_lock() - look up a PON device and take its lock
 * @net: the network namespace of the request
 * @dev_id: the device id attribute
 *
 * Takes pon_devs_lock for the lookup and holds it until @pdev->lock is
 * taken, so the device cannot go away in between. A device in another
 * namespace is not found.
 *
 * Return: the device with its lock held, or ERR_PTR(-ENODEV).
 */
static struct pon_dev *
pon_device_get_and_lock(struct net *net, struct nlattr *dev_id)
{
	struct pon_dev *pdev;

	mutex_lock(&pon_devs_lock);
	pdev = xa_load(&pon_devs, nla_get_u32(dev_id));
	if (!pdev) {
		mutex_unlock(&pon_devs_lock);
		return ERR_PTR(-ENODEV);
	}

	mutex_lock(&pdev->lock);
	mutex_unlock(&pon_devs_lock);

	if (dev_net(pdev->main_netdev) != net) {
		mutex_unlock(&pdev->lock);
		return ERR_PTR(-ENODEV);
	}

	return pdev;
}

/**
 * pon_device_get_locked() - genl pre_doit that looks up and locks the device
 * @ops: the operation of the request
 * @skb: the request
 * @info: the request info, user_ptr[0] receives the device
 *
 * Every attribute set carries the device id as attribute 1, so one pre_doit
 * serves all commands that name a device.
 *
 * Context: Runs as the genl pre_doit. On success the device lock stays held
 * until pon_device_unlock().
 * Return: 0, -EINVAL without a device id, or -ENODEV.
 */
int pon_device_get_locked(const struct genl_split_ops *ops,
			  struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr *id = info->attrs[PON_A_DEV_ID];

	/* Every attribute set carries the device id as attribute 1. */
	BUILD_BUG_ON((int)PON_A_DEV_ID != (int)PON_A_TCONT_DEV_ID ||
		     (int)PON_A_DEV_ID != (int)PON_A_GEM_DEV_ID ||
		     (int)PON_A_DEV_ID != (int)PON_A_GEM_MAP_DEV_ID ||
		     (int)PON_A_DEV_ID != (int)PON_A_OMCI_DEV_ID);

	if (!id) {
		NL_SET_ERR_MSG(info->extack, "device id is missing");
		return -EINVAL;
	}

	info->user_ptr[0] = pon_device_get_and_lock(genl_info_net(info), id);
	if (IS_ERR(info->user_ptr[0])) {
		NL_SET_ERR_MSG_ATTR(info->extack, id, "no such device");
		return PTR_ERR(info->user_ptr[0]);
	}

	return 0;
}

/**
 * pon_device_unlock() - genl post_doit that drops the device lock
 * @ops: the operation of the request
 * @skb: the request
 * @info: the request info, user_ptr[0] holds the device
 *
 * Context: Runs as the genl post_doit, with the lock that
 * pon_device_get_locked() took.
 */
void
pon_device_unlock(const struct genl_split_ops *ops, struct sk_buff *skb,
		  struct genl_info *info)
{
	struct pon_dev *pdev = info->user_ptr[0];

	mutex_unlock(&pdev->lock);
}

/* Device */

/**
 * pon_nl_dev_fill() - put one device message into an skb
 * @pdev: the PON device
 * @rsp: the skb to fill
 * @info: the request, or the notification info
 *
 * Context: Called with @pdev->lock held.
 * Return: 0, or -EMSGSIZE when the message does not fit.
 */
static int
pon_nl_dev_fill(struct pon_dev *pdev, struct sk_buff *rsp,
		const struct genl_info *info)
{
	void *hdr;

	hdr = genlmsg_iput(rsp, info);
	if (!hdr)
		return -EMSGSIZE;

	if (nla_put_u32(rsp, PON_A_DEV_ID, pdev->id) ||
	    nla_put_u32(rsp, PON_A_DEV_IFINDEX, pdev->main_netdev->ifindex) ||
	    nla_put_u32(rsp, PON_A_DEV_MODE, pdev->mode) ||
	    nla_put_u32(rsp, PON_A_DEV_MODES_CAP, pdev->caps->modes) ||
	    nla_put_u32(rsp, PON_A_DEV_PLOAM_STATE, READ_ONCE(pdev->ploam)) ||
	    nla_put_u32(rsp, PON_A_DEV_MAX_TCONTS, pdev->caps->max_tconts) ||
	    nla_put_u32(rsp, PON_A_DEV_MAX_GEMS, pdev->caps->max_gems))
		goto err_cancel_msg;

	if (pdev->identity.serial_set &&
	    nla_put(rsp, PON_A_DEV_SERIAL, PON_SERIAL_LEN,
		    pdev->identity.serial))
		goto err_cancel_msg;

	if (nla_put_u8(rsp, PON_A_DEV_ENABLE, pdev->enabled))
		goto err_cancel_msg;

	genlmsg_end(rsp, hdr);
	return 0;

err_cancel_msg:
	genlmsg_cancel(rsp, hdr);
	return -EMSGSIZE;
}

/**
 * pon_nl_notify_dev() - send a device notification to the mgmt group
 * @pdev: the PON device
 * @cmd: the notification command, PON_CMD_DEV_ADD_NTF,
 *       PON_CMD_DEV_CHANGE_NTF or PON_CMD_DEV_DEL_NTF
 *
 * Does nothing when the group has no listener in the device's namespace.
 *
 * Context: Called with @pdev->lock held. May sleep.
 */
void pon_nl_notify_dev(struct pon_dev *pdev, u32 cmd)
{
	struct net *net = dev_net(pdev->main_netdev);
	struct genl_info info;
	struct sk_buff *ntf;

	lockdep_assert_held(&pdev->lock);

	if (!genl_has_listeners(&pon_nl_family, net, PON_NLGRP_MGMT))
		return;

	ntf = genlmsg_new(GENLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!ntf)
		return;

	genl_info_init_ntf(&info, &pon_nl_family, cmd);
	genl_info_net_set(&info, net);
	if (pon_nl_dev_fill(pdev, ntf, &info)) {
		nlmsg_free(ntf);
		return;
	}

	genlmsg_multicast_netns(&pon_nl_family, net, ntf, 0, PON_NLGRP_MGMT,
				GFP_KERNEL);
}

/**
 * pon_nl_dev_get_doit() - handle PON_CMD_DEV_GET for one device
 * @req: the request
 * @info: the request info, user_ptr[0] holds the device
 *
 * Context: Called with the device lock held by pon_device_get_locked().
 * Return: 0, or a negative errno.
 */
int pon_nl_dev_get_doit(struct sk_buff *req, struct genl_info *info)
{
	return pon_nl_dev_reply(info, pon_nl_dev_fill);
}

/**
 * pon_nl_dev_get_dumpit() - handle the PON_CMD_DEV_GET dump
 * @rsp: the skb to fill
 * @cb: the dump state, args[0] is the next device id
 *
 * Dumps every device in the namespace of the requesting socket.
 *
 * Context: Takes pon_devs_lock and each device lock in turn.
 * Return: 0, or -EMSGSIZE when the skb is full and the dump resumes.
 */
int pon_nl_dev_get_dumpit(struct sk_buff *rsp, struct netlink_callback *cb)
{
	struct pon_dev *pdev;
	int err = 0;

	mutex_lock(&pon_devs_lock);
	xa_for_each_start(&pon_devs, cb->args[0], pdev, cb->args[0]) {
		mutex_lock(&pdev->lock);
		if (dev_net(pdev->main_netdev) == sock_net(rsp->sk))
			err = pon_nl_dev_fill(pdev, rsp, genl_info_dump(cb));
		mutex_unlock(&pdev->lock);
		if (err)
			break;
	}
	mutex_unlock(&pon_devs_lock);

	return err;
}

/**
 * pon_nl_identity_commit() - copy accepted identity settings into the device
 * @pdev: the PON device
 * @id: the settings the driver took
 *
 * The driver took the settings. The core keeps the mode and the serial
 * number, which dev-get reports. The registration id stays with the driver.
 *
 * Context: Called with @pdev->lock held.
 */
static void pon_nl_identity_commit(struct pon_dev *pdev,
				   const struct pon_identity *id)
{
	if (id->mode_set)
		pdev->mode = id->mode;
	if (id->serial_set) {
		memcpy(pdev->identity.serial, id->serial, PON_SERIAL_LEN);
		pdev->identity.serial_set = true;
	}
}

/**
 * pon_nl_serial_same() - compare a requested serial number with the stored one
 * @pdev: the PON device
 * @id: the settings of the request, with a serial number
 *
 * Context: Called with @pdev->lock held.
 * Return: true when the device holds a serial number equal to that of @id.
 */
static bool pon_nl_serial_same(const struct pon_dev *pdev,
			       const struct pon_identity *id)
{
	return pdev->identity.serial_set &&
	       !memcmp(pdev->identity.serial, id->serial, PON_SERIAL_LEN);
}

/**
 * pon_nl_dev_set() - apply a PON_CMD_DEV_SET request
 * @info: the request info, user_ptr[0] holds the device
 * @id: zeroed space for the identity settings, which the caller clears
 *	after the call because it holds credentials
 *
 * Passes the identity settings to the set_identity op, then the enable
 * setting to the enable op. A registration id shorter than PON_REG_ID_LEN
 * reaches the op padded with 0x00 bytes, as the note of ITU-T G.9807.1 Table
 * C.11.25 recommends. An empty one is refused. A mode the device does not
 * list in its capabilities is refused with an error line through
 * pon_dev_log_level(). While the link is enabled, a mode or a serial number
 * other than the one the device holds is refused: the OLT addresses the ONU
 * by its serial number (G.9807.1 clause C.11.2.6.1). The serial number of
 * eight 0x00 bytes is refused, since G.9807.1 Table C.11.23A uses it to
 * address every ONU. A PON_CMD_DEV_CHANGE_NTF follows once the
 * identity changed, even when the enable op then fails.
 *
 * Context: Called with the device lock held by pon_device_get_locked().
 * Return: 0, or a negative errno.
 */
static int pon_nl_dev_set(struct genl_info *info, struct pon_identity *id)
{
	struct pon_dev *pdev = info->user_ptr[0];
	bool identity = false;
	int err;

	if (info->attrs[PON_A_DEV_MODE]) {
		id->mode = nla_get_u32(info->attrs[PON_A_DEV_MODE]);
		id->mode_set = true;
		if (!(pdev->caps->modes & BIT(id->mode))) {
			NL_SET_ERR_MSG(info->extack,
				       "mode not supported by the device");
			pon_dev_log_level(pdev, KERN_ERR,
					  "mode %s not supported by the device",
					  pon_nl_mode_names[id->mode]);
			return -EINVAL;
		}
		if (pdev->enabled && id->mode != pdev->mode) {
			NL_SET_ERR_MSG_ATTR(info->extack,
					    info->attrs[PON_A_DEV_MODE],
					    "the mode cannot change while the link is enabled");
			return -EBUSY;
		}
	}
	if (info->attrs[PON_A_DEV_SERIAL]) {
		nla_memcpy(id->serial, info->attrs[PON_A_DEV_SERIAL],
			   PON_SERIAL_LEN);
		id->serial_set = true;
		if (!memchr_inv(id->serial, 0, PON_SERIAL_LEN)) {
			NL_SET_ERR_MSG_ATTR(info->extack,
					    info->attrs[PON_A_DEV_SERIAL],
					    "the all-zero serial number addresses every ONU");
			return -EINVAL;
		}
		if (pdev->enabled && !pon_nl_serial_same(pdev, id)) {
			NL_SET_ERR_MSG_ATTR(info->extack,
					    info->attrs[PON_A_DEV_SERIAL],
					    "the serial number cannot change while the link is enabled");
			return -EBUSY;
		}
	}
	if (info->attrs[PON_A_DEV_REGISTRATION_ID]) {
		struct nlattr *reg_id = info->attrs[PON_A_DEV_REGISTRATION_ID];

		if (!nla_len(reg_id)) {
			NL_SET_ERR_MSG_ATTR(info->extack, reg_id,
					    "the registration id is empty");
			return -EINVAL;
		}
		nla_memcpy(id->reg_id, reg_id, PON_REG_ID_LEN);
		id->reg_id_len = PON_REG_ID_LEN;
	}

	identity = id->mode_set || id->serial_set || id->reg_id_len;

	if (!identity && !info->attrs[PON_A_DEV_ENABLE]) {
		NL_SET_ERR_MSG(info->extack, "no settings present");
		return -EINVAL;
	}

	if (identity && !pdev->ops->set_identity)
		return -EOPNOTSUPP;
	if (info->attrs[PON_A_DEV_ENABLE] && !pdev->ops->enable)
		return -EOPNOTSUPP;

	if (identity) {
		err = pdev->ops->set_identity(pdev, id, info->extack);
		if (err)
			return err;
		pon_nl_identity_commit(pdev, id);
	}

	if (info->attrs[PON_A_DEV_ENABLE]) {
		bool on = nla_get_u8(info->attrs[PON_A_DEV_ENABLE]);

		err = pdev->ops->enable(pdev, on, info->extack);
		if (err) {
			if (identity)
				pon_nl_notify_dev(pdev, PON_CMD_DEV_CHANGE_NTF);
			return err;
		}
		pdev->enabled = on;
	}

	pon_nl_notify_dev(pdev, PON_CMD_DEV_CHANGE_NTF);

	return 0;
}

/**
 * pon_nl_dev_set_doit() - handle PON_CMD_DEV_SET
 * @skb: the request
 * @info: the request info, user_ptr[0] holds the device
 *
 * Applies the request through pon_nl_dev_set() and clears the copy of the
 * identity settings on every exit, since it holds the registration id.
 *
 * Context: Called with the device lock held by pon_device_get_locked().
 * Return: 0, or a negative errno.
 */
int pon_nl_dev_set_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct pon_identity id = {};
	int err;

	err = pon_nl_dev_set(info, &id);
	memzero_explicit(&id, sizeof(id));

	return err;
}

/* State notifications. The instance lock is held and they may sleep. */

/**
 * pon_nl_notify_ploam() - send the activation state notification
 * @pdev: the PON device
 *
 * Sends PON_CMD_PLOAM_NTF with the ONU activation state of ITU-T G.9807.1
 * Table C.12.1 to the state group. pon_dev_state_report() owns the state
 * and calls this once it has validated and published it, so this only
 * formats the message.
 *
 * Context: Called with @pdev->lock held. May sleep.
 */
void pon_nl_notify_ploam(struct pon_dev *pdev)
{
	struct net *net = dev_net(pdev->main_netdev);
	struct sk_buff *ntf;
	void *hdr;

	lockdep_assert_held(&pdev->lock);

	if (!genl_has_listeners(&pon_nl_family, net, PON_NLGRP_STATE))
		return;

	ntf = genlmsg_new(GENLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!ntf)
		return;

	hdr = genlmsg_put(ntf, 0, 0, &pon_nl_family, 0, PON_CMD_PLOAM_NTF);
	if (!hdr)
		goto err_free;

	if (nla_put_u32(ntf, PON_A_DEV_ID, pdev->id) ||
	    nla_put_u32(ntf, PON_A_DEV_PLOAM_STATE, pdev->ploam))
		goto err_free;

	genlmsg_end(ntf, hdr);
	genlmsg_multicast_netns(&pon_nl_family, net, ntf, 0, PON_NLGRP_STATE,
				GFP_KERNEL);
	return;

err_free:
	nlmsg_free(ntf);
}

/**
 * pon_nl_obj_gen_inc() - move the object generation
 *
 * Called whenever an object joins or leaves a list, so that an object dump
 * that resumes on a new generation is marked as interrupted. The value
 * wraps from INT_MAX to 1 and so never becomes 0.
 */
void pon_nl_obj_gen_inc(void)
{
	int old = atomic_read(&pon_nl_obj_gen);
	int new;

	do {
		new = old == INT_MAX ? 1 : old + 1;
	} while (!atomic_try_cmpxchg(&pon_nl_obj_gen, &old, new));
}

/**
 * pon_nl_obj_dev_dump() - dump one object list of one device
 * @pdev: the PON device
 * @rsp: the skb to fill
 * @cb: the dump state, args[1] counts the objects sent
 * @head: the object list
 * @skip: the number of objects sent before this call
 * @fill: puts one object into @rsp
 *
 * Context: Called with @pdev->lock held.
 * Return: 0 once the list is done, or the error of @fill.
 */
static int pon_nl_obj_dev_dump(struct pon_dev *pdev, struct sk_buff *rsp,
			       struct netlink_callback *cb,
			       struct list_head *head, unsigned long skip,
			       pon_nl_obj_fill_t fill)
{
	struct list_head *pos;
	int err;

	lockdep_assert_held(&pdev->lock);

	cb->seq = atomic_read(&pon_nl_obj_gen);

	list_for_each(pos, head) {
		if (skip) {
			skip--;
			continue;
		}
		err = fill(pdev, pos, rsp, genl_info_dump(cb));
		if (err)
			return err;
		cb->args[1]++;
	}

	cb->args[1] = 0;
	return 0;
}

/**
 * pon_nl_obj_dumpit() - dump one object list of each visible device
 * @rsp: the skb to fill
 * @cb: the dump state, args[0] is the device, args[1] the objects sent
 * @list_offset: the offset of the list head in struct pon_dev
 * @fill: puts one object into @rsp
 *
 * Walks every device in the namespace of the requesting socket, or only
 * the device the request names. A part that put no message leaves the
 * consistency check to the NLMSG_DONE that follows, which then carries
 * NLM_F_DUMP_INTR when the generation moved since the previous part.
 *
 * Context: Takes pon_devs_lock and each device lock in turn.
 * Return: 0, the error of @fill, or -ENODEV when the named device is not
 * found.
 */
static int pon_nl_obj_dumpit(struct sk_buff *rsp, struct netlink_callback *cb,
			     size_t list_offset, pon_nl_obj_fill_t fill)
{
	const struct genl_info *info = genl_info_dump(cb);
	unsigned long resumed = cb->args[0];
	unsigned long last = ULONG_MAX;
	struct pon_dev *pdev;
	bool found = false;
	int err = 0;

	if (info->attrs[PON_A_DEV_ID]) {
		last = nla_get_u32(info->attrs[PON_A_DEV_ID]);
		cb->args[0] = last;
	}

	mutex_lock(&pon_devs_lock);
	cb->seq = atomic_read(&pon_nl_obj_gen);
	xa_for_each_range(&pon_devs, cb->args[0], pdev, cb->args[0], last) {
		struct list_head *head = (void *)pdev + list_offset;
		unsigned long skip = pdev->id == resumed ? cb->args[1] : 0;

		mutex_lock(&pdev->lock);
		if (dev_net(pdev->main_netdev) == sock_net(rsp->sk)) {
			found = true;
			err = pon_nl_obj_dev_dump(pdev, rsp, cb, head, skip,
						  fill);
		}
		mutex_unlock(&pdev->lock);
		if (err)
			break;
	}
	mutex_unlock(&pon_devs_lock);

	if (!found && last != ULONG_MAX) {
		NL_SET_ERR_MSG(cb->extack, "no such device");
		return -ENODEV;
	}

	if (rsp->len)
		nl_dump_check_consistent(cb, nlmsg_hdr(rsp));

	return err;
}

/* T-CONT */

/**
 * pon_gem_on_tcont() - check whether a GEM port is bound to a T-CONT
 * @gem: the GEM port
 * @index: the T-CONT index
 *
 * Return: true when @gem names the T-CONT @index.
 */
static bool pon_gem_on_tcont(const struct pon_gem *gem, u16 index)
{
	return gem->cfg.tcont_valid && gem->cfg.tcont_index == index;
}

/**
 * pon_tcont_in_use() - check whether a GEM port names a T-CONT
 * @pdev: the PON device
 * @index: the T-CONT index
 *
 * Return: true while a GEM port object holds @index as its T-CONT.
 */
bool pon_tcont_in_use(struct pon_dev *pdev, u16 index)
{
	struct pon_gem *gem;

	lockdep_assert_held(&pdev->lock);
	list_for_each_entry(gem, &pdev->gems, list)
		if (pon_gem_on_tcont(gem, index))
			return true;
	return false;
}

/**
 * pon_tcont_alloc_taken() - check whether another T-CONT holds an alloc-id
 * @pdev: the PON device
 * @index: the T-CONT that asks for @alloc_id
 * @alloc_id: the alloc-id
 *
 * ITU-T G.988 clause 9.2.2 relates alloc-ids and T-CONTs one to one and
 * leaves several T-CONTs on one alloc-id undefined.
 *
 * Return: true while a T-CONT other than @index holds @alloc_id.
 */
bool pon_tcont_alloc_taken(struct pon_dev *pdev, u16 index, u16 alloc_id)
{
	struct pon_tcont *tcont;

	lockdep_assert_held(&pdev->lock);
	list_for_each_entry(tcont, &pdev->tconts, list)
		if (tcont->cfg.index != index &&
		    tcont->cfg.alloc_id == alloc_id)
			return true;
	return false;
}

/**
 * pon_tcont_gems_rebind() - move the GEM ports of a T-CONT to another alloc-id
 * @pdev: the PON device
 * @index: the T-CONT index
 * @old_alloc_id: the alloc-id the GEM ports are bound to
 * @new_alloc_id: the alloc-id to bind them to
 * @extack: netlink extended ack
 *
 * Programs each GEM port of the T-CONT again through the gem_add op. If the
 * driver refuses one, the refused GEM port and the ones already moved are
 * programmed again with @old_alloc_id, so that the core and the driver agree.
 *
 * Return: 0, or the error of the refused GEM port.
 */
int pon_tcont_gems_rebind(struct pon_dev *pdev, u16 index, u16 old_alloc_id,
			  u16 new_alloc_id, struct netlink_ext_ack *extack)
{
	struct pon_gem *gem;
	int err;

	lockdep_assert_held(&pdev->lock);

	list_for_each_entry(gem, &pdev->gems, list) {
		if (!pon_gem_on_tcont(gem, index) ||
		    gem->cfg.alloc_id == new_alloc_id)
			continue;

		gem->cfg.alloc_id = new_alloc_id;
		err = pdev->ops->gem_add(pdev, &gem->cfg, extack);
		if (err)
			goto err_restore;
	}

	return 0;

err_restore:
	NL_SET_ERR_MSG_WEAK(extack,
			    "a GEM port of the T-CONT cannot move to the alloc-id");

	list_for_each_entry_from_reverse(gem, &pdev->gems, list) {
		if (!pon_gem_on_tcont(gem, index) ||
		    gem->cfg.alloc_id != new_alloc_id)
			continue;

		gem->cfg.alloc_id = old_alloc_id;
		if (pdev->ops->gem_add(pdev, &gem->cfg, NULL))
			netdev_warn(pdev->main_netdev,
				    "restoring GEM %u to alloc-id %u failed\n",
				    gem->cfg.id, old_alloc_id);
	}

	return err;
}

/**
 * pon_nl_tcont_set_doit() - handle PON_CMD_TCONT_SET
 * @skb: the request
 * @info: the request info, user_ptr[0] holds the device
 *
 * Creates the T-CONT, or binds an existing one to another alloc-id and
 * moves its GEM ports with it. The alloc-id must lie in the ranges of
 * ITU-T G.9807.1 Table C.6.5 and must not be a serial number grant. Alloc-ids
 * and T-CONTs relate one to one, as ITU-T G.988 clause 9.2.2 defines.
 *
 * Context: Called with the device lock held by pon_device_get_locked().
 * Return: 0, or a negative errno.
 */
int pon_nl_tcont_set_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct pon_dev *pdev = info->user_ptr[0];
	struct pon_tcont_cfg cfg = {};
	struct pon_tcont *tcont;
	bool changed = false;
	bool is_new;
	int err;

	if (GENL_REQ_ATTR_CHECK(info, PON_A_TCONT_INDEX) ||
	    GENL_REQ_ATTR_CHECK(info, PON_A_TCONT_ALLOC_ID))
		return -EINVAL;

	cfg.index = nla_get_u32(info->attrs[PON_A_TCONT_INDEX]);
	cfg.alloc_id = nla_get_u32(info->attrs[PON_A_TCONT_ALLOC_ID]);

	if (cfg.index >= pdev->caps->max_tconts) {
		NL_SET_ERR_MSG_ATTR(info->extack,
				    info->attrs[PON_A_TCONT_INDEX],
				    "T-CONT index out of range");
		return -ERANGE;
	}
	if (cfg.alloc_id >= PON_PLOAM_ALLOC_ID_SN_GRANT_MIN &&
	    cfg.alloc_id <= PON_PLOAM_ALLOC_ID_SN_GRANT_2G5) {
		NL_SET_ERR_MSG_ATTR(info->extack,
				    info->attrs[PON_A_TCONT_ALLOC_ID],
				    "the alloc-id is a serial number grant");
		return -EINVAL;
	}
	if (pon_tcont_alloc_taken(pdev, cfg.index, cfg.alloc_id)) {
		NL_SET_ERR_MSG_ATTR(info->extack,
				    info->attrs[PON_A_TCONT_ALLOC_ID],
				    "another T-CONT holds the alloc-id");
		return -EEXIST;
	}

	tcont = pon_tcont_find(pdev, cfg.index);
	is_new = !tcont;
	if (is_new) {
		tcont = kzalloc_obj(*tcont, GFP_KERNEL);
		if (!tcont)
			return -ENOMEM;
	} else {
		changed = tcont->cfg.alloc_id != cfg.alloc_id;
	}

	err = pdev->ops->tcont_set(pdev, &cfg, info->extack);
	if (err)
		goto err_free_tcont;

	if (changed) {
		err = pon_tcont_gems_rebind(pdev, cfg.index,
					    tcont->cfg.alloc_id, cfg.alloc_id,
					    info->extack);
		if (err)
			goto err_restore_tcont;
	}

	tcont->cfg = cfg;
	if (is_new) {
		list_add_tail(&tcont->list, &pdev->tconts);
		pon_nl_obj_gen_inc();
	}

	pon_dev_carrier_update(pdev);

	if (is_new)
		pon_nl_notify_tcont(pdev, tcont, PON_CMD_TCONT_ADD_NTF);
	else if (changed)
		pon_nl_notify_tcont(pdev, tcont, PON_CMD_TCONT_CHANGE_NTF);

	return 0;

err_restore_tcont:
	if (pdev->ops->tcont_set(pdev, &tcont->cfg, NULL))
		netdev_warn(pdev->main_netdev,
			    "restoring T-CONT %u to alloc-id %u failed\n",
			    tcont->cfg.index, tcont->cfg.alloc_id);
err_free_tcont:
	if (is_new)
		kfree(tcont);
	return err;
}

/**
 * pon_nl_tcont_fill() - put one T-CONT message into an skb
 * @pdev: the PON device
 * @tcont: the T-CONT
 * @rsp: the skb to fill
 * @info: the request, or the notification info
 *
 * Context: Called with @pdev->lock held.
 * Return: 0, or -EMSGSIZE when the message does not fit.
 */
static int
pon_nl_tcont_fill(struct pon_dev *pdev, struct pon_tcont *tcont,
		  struct sk_buff *rsp, const struct genl_info *info)
{
	struct pon_tcont_cfg *cfg = &tcont->cfg;
	void *hdr;

	hdr = genlmsg_iput(rsp, info);
	if (!hdr)
		return -EMSGSIZE;

	if (nla_put_u32(rsp, PON_A_TCONT_DEV_ID, pdev->id) ||
	    nla_put_u32(rsp, PON_A_TCONT_INDEX, cfg->index) ||
	    nla_put_u32(rsp, PON_A_TCONT_ALLOC_ID, cfg->alloc_id))
		goto err_cancel_msg;

	genlmsg_end(rsp, hdr);
	return 0;

err_cancel_msg:
	genlmsg_cancel(rsp, hdr);
	return -EMSGSIZE;
}

/**
 * pon_nl_tcont_fill_pos() - pon_nl_obj_fill_t for the T-CONT list
 * @pdev: the PON device
 * @pos: the list entry of the T-CONT
 * @rsp: the skb to fill
 * @info: the dump info
 *
 * Return: the result of pon_nl_tcont_fill().
 */
static int pon_nl_tcont_fill_pos(struct pon_dev *pdev, struct list_head *pos,
				 struct sk_buff *rsp,
				 const struct genl_info *info)
{
	return pon_nl_tcont_fill(pdev, list_entry(pos, struct pon_tcont, list),
				 rsp, info);
}

/**
 * pon_nl_notify_tcont() - send a T-CONT notification to the mgmt group
 * @pdev: the PON device
 * @tcont: the T-CONT
 * @cmd: the notification command, PON_CMD_TCONT_ADD_NTF,
 *       PON_CMD_TCONT_CHANGE_NTF or PON_CMD_TCONT_DEL_NTF
 *
 * Context: Called with @pdev->lock held. May sleep.
 */
void pon_nl_notify_tcont(struct pon_dev *pdev, struct pon_tcont *tcont, u32 cmd)
{
	pon_nl_notify_obj(pdev, &tcont->list, cmd, pon_nl_tcont_fill_pos);
}

/**
 * pon_nl_tcont_get_doit() - handle PON_CMD_TCONT_GET for one T-CONT
 * @skb: the request
 * @info: the request info, user_ptr[0] holds the device
 *
 * Context: Called with the device lock held by pon_device_get_locked().
 * Return: 0, -ENOENT when the T-CONT does not exist, or a negative errno.
 */
int pon_nl_tcont_get_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct pon_dev *pdev = info->user_ptr[0];
	struct pon_tcont *tcont;

	if (GENL_REQ_ATTR_CHECK(info, PON_A_TCONT_INDEX))
		return -EINVAL;

	tcont = pon_tcont_find(pdev,
			       nla_get_u32(info->attrs[PON_A_TCONT_INDEX]));
	if (!tcont) {
		NL_SET_ERR_MSG(info->extack, "no such T-CONT");
		return -ENOENT;
	}

	return pon_nl_obj_reply(info, &tcont->list, pon_nl_tcont_fill_pos);
}

/**
 * pon_nl_tcont_get_dumpit() - handle the PON_CMD_TCONT_GET dump
 * @rsp: the skb to fill
 * @cb: the dump state
 *
 * Return: the result of pon_nl_obj_dumpit().
 */
int pon_nl_tcont_get_dumpit(struct sk_buff *rsp, struct netlink_callback *cb)
{
	return pon_nl_obj_dumpit(rsp, cb, offsetof(struct pon_dev, tconts),
				 pon_nl_tcont_fill_pos);
}

/**
 * pon_nl_tcont_del_doit() - handle PON_CMD_TCONT_DEL
 * @skb: the request
 * @info: the request info, user_ptr[0] holds the device
 *
 * Refuses while a GEM port names the T-CONT.
 *
 * Context: Called with the device lock held by pon_device_get_locked().
 * Return: 0, -ENOENT when the T-CONT does not exist, -EBUSY while a GEM port
 * names it, or another negative errno.
 */
int pon_nl_tcont_del_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct pon_dev *pdev = info->user_ptr[0];
	struct pon_tcont *tcont;
	u16 index;
	int err;

	if (GENL_REQ_ATTR_CHECK(info, PON_A_TCONT_INDEX))
		return -EINVAL;

	index = nla_get_u32(info->attrs[PON_A_TCONT_INDEX]);
	tcont = pon_tcont_find(pdev, index);
	if (!tcont) {
		NL_SET_ERR_MSG(info->extack, "no such T-CONT");
		return -ENOENT;
	}
	if (pon_tcont_in_use(pdev, index)) {
		NL_SET_ERR_MSG(info->extack, "a GEM port names the T-CONT");
		return -EBUSY;
	}

	err = pdev->ops->tcont_clear(pdev, &tcont->cfg, info->extack);
	if (err)
		return err;

	pon_nl_notify_tcont(pdev, tcont, PON_CMD_TCONT_DEL_NTF);

	list_del(&tcont->list);
	kfree(tcont);
	pon_nl_obj_gen_inc();

	pon_dev_carrier_update(pdev);

	return 0;
}

/* GEM ports */

/**
 * pon_gems_full() - check whether the device holds all the GEM ports it can
 * @pdev: the PON device
 *
 * Return: true when the GEM port objects number the device's max_gems.
 */
bool pon_gems_full(struct pon_dev *pdev)
{
	struct pon_gem *gem;
	unsigned int n = 0;

	lockdep_assert_held(&pdev->lock);
	list_for_each_entry(gem, &pdev->gems, list)
		n++;
	return n >= pdev->caps->max_gems;
}

/**
 * pon_gem_cfg_same() - compare two GEM port configurations
 * @existing: the configuration of the GEM port the instance holds
 * @requested: the configuration a request names
 *
 * Return: true when every member that a request sets is equal.
 */
static bool pon_gem_cfg_same(const struct pon_gem_cfg *existing,
			     const struct pon_gem_cfg *requested)
{
	return existing->id == requested->id &&
	       existing->dir == requested->dir &&
	       existing->tcont_valid == requested->tcont_valid &&
	       existing->tcont_index == requested->tcont_index &&
	       existing->alloc_id == requested->alloc_id &&
	       existing->key_ring == requested->key_ring;
}

/**
 * pon_nl_gem_new_doit() - handle PON_CMD_GEM_NEW
 * @skb: the request
 * @info: the request info, user_ptr[0] holds the device
 *
 * Creates the GEM port of the GEM port network CTP of ITU-T G.988 clause
 * 9.2.3. The policy holds the GEM port ID to the assignable range of ITU-T
 * G.9807.1 Table C.6.6. The broadcast key ring of clause 9.2.3 is refused.
 * A GEM port with an upstream half names its T-CONT and takes the alloc-id of
 * it. A downstream GEM port names none. A request for a GEM port that exists
 * with the same attributes succeeds and changes nothing.
 *
 * Context: Called with the device lock held by pon_device_get_locked().
 * Return: 0, or a negative errno.
 */
int pon_nl_gem_new_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct pon_dev *pdev = info->user_ptr[0];
	struct pon_gem_cfg cfg = {};
	struct pon_gem *gem;
	int err;

	if (GENL_REQ_ATTR_CHECK(info, PON_A_GEM_ID) ||
	    GENL_REQ_ATTR_CHECK(info, PON_A_GEM_DIR))
		return -EINVAL;

	cfg.id = nla_get_u32(info->attrs[PON_A_GEM_ID]);
	cfg.dir = nla_get_u32(info->attrs[PON_A_GEM_DIR]);
	if (cfg.dir == PON_GEM_DIR_DOWNSTREAM) {
		if (info->attrs[PON_A_GEM_TCONT_INDEX]) {
			NL_SET_ERR_MSG_ATTR(info->extack,
					    info->attrs[PON_A_GEM_TCONT_INDEX],
					    "a downstream GEM port rides no T-CONT");
			return -EINVAL;
		}
	} else if (GENL_REQ_ATTR_CHECK(info, PON_A_GEM_TCONT_INDEX)) {
		NL_SET_ERR_MSG(info->extack,
			       "an upstream GEM port needs a T-CONT");
		return -EINVAL;
	}
	if (info->attrs[PON_A_GEM_TCONT_INDEX]) {
		cfg.tcont_index =
			nla_get_u32(info->attrs[PON_A_GEM_TCONT_INDEX]);
		cfg.tcont_valid = true;
	}
	if (info->attrs[PON_A_GEM_KEY_RING])
		cfg.key_ring = nla_get_u32(info->attrs[PON_A_GEM_KEY_RING]);
	if (cfg.key_ring == PON_GEM_KEY_RING_BROADCAST) {
		NL_SET_ERR_MSG_ATTR(info->extack,
				    info->attrs[PON_A_GEM_KEY_RING],
				    "broadcast keys are not supported");
		return -EOPNOTSUPP;
	}

	if (cfg.tcont_valid) {
		struct pon_tcont *tcont;

		tcont = pon_tcont_find(pdev, cfg.tcont_index);
		if (!tcont) {
			NL_SET_ERR_MSG_ATTR(info->extack,
					    info->attrs[PON_A_GEM_TCONT_INDEX],
					    "no such T-CONT");
			return -ENOENT;
		}
		cfg.alloc_id = tcont->cfg.alloc_id;
	}

	gem = pon_gem_find(pdev, cfg.id);
	if (gem) {
		if (!pon_gem_cfg_same(&gem->cfg, &cfg)) {
			NL_SET_ERR_MSG(info->extack,
				       "the GEM port exists with other attributes");
			return -EEXIST;
		}

		return 0;
	}

	if (pon_gems_full(pdev)) {
		NL_SET_ERR_MSG(info->extack,
			       "the device holds no more GEM ports");
		return -ENOSPC;
	}

	gem = kzalloc_obj(*gem, GFP_KERNEL);
	if (!gem)
		return -ENOMEM;

	err = pdev->ops->gem_add(pdev, &cfg, info->extack);
	if (err)
		goto err_free_gem;

	gem->cfg = cfg;
	list_add_tail(&gem->list, &pdev->gems);
	pon_nl_obj_gen_inc();
	pon_dev_carrier_update(pdev);
	pon_nl_notify_gem(pdev, gem, PON_CMD_GEM_ADD_NTF);

	return 0;

err_free_gem:
	kfree(gem);
	return err;
}

/**
 * pon_nl_gem_del_doit() - handle PON_CMD_GEM_DEL
 * @skb: the request
 * @info: the request info, user_ptr[0] holds the device
 *
 * Refuses while a GEM network device is attached to the GEM port.
 *
 * Context: Called with the device lock held by pon_device_get_locked().
 * Return: 0, -ENOENT when the GEM port does not exist, -EBUSY while a GEM
 * network device is attached, or another negative errno.
 */
int pon_nl_gem_del_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct pon_dev *pdev = info->user_ptr[0];
	struct pon_gem *gem;
	u16 gem_id;
	int err;

	if (GENL_REQ_ATTR_CHECK(info, PON_A_GEM_ID))
		return -EINVAL;

	gem_id = nla_get_u32(info->attrs[PON_A_GEM_ID]);
	gem = pon_gem_find(pdev, gem_id);
	if (!gem) {
		NL_SET_ERR_MSG(info->extack, "no such GEM port");
		return -ENOENT;
	}
	if (gem->gem_netdev) {
		NL_SET_ERR_MSG(info->extack,
			       "a GEM network device is attached");
		return -EBUSY;
	}

	err = pdev->ops->gem_del(pdev, gem_id, info->extack);
	if (err)
		return err;

	pon_nl_notify_gem(pdev, gem, PON_CMD_GEM_DEL_NTF);

	list_del(&gem->list);
	kfree(gem);
	pon_nl_obj_gen_inc();

	pon_dev_carrier_update(pdev);

	return 0;
}

/**
 * pon_nl_gem_fill() - put one GEM port message into an skb
 * @pdev: the PON device
 * @gem: the GEM port
 * @rsp: the skb to fill
 * @info: the request, or the notification info
 *
 * Context: Called with @pdev->lock held.
 * Return: 0, or -EMSGSIZE when the message does not fit.
 */
static int
pon_nl_gem_fill(struct pon_dev *pdev, struct pon_gem *gem,
		struct sk_buff *rsp, const struct genl_info *info)
{
	struct pon_gem_cfg *cfg = &gem->cfg;
	void *hdr;

	hdr = genlmsg_iput(rsp, info);
	if (!hdr)
		return -EMSGSIZE;

	if (nla_put_u32(rsp, PON_A_GEM_DEV_ID, pdev->id) ||
	    nla_put_u32(rsp, PON_A_GEM_ID, cfg->id) ||
	    nla_put_u32(rsp, PON_A_GEM_DIR, cfg->dir) ||
	    nla_put_u32(rsp, PON_A_GEM_KEY_RING, cfg->key_ring))
		goto err_cancel_msg;

	if (cfg->tcont_valid &&
	    nla_put_u32(rsp, PON_A_GEM_TCONT_INDEX, cfg->tcont_index))
		goto err_cancel_msg;

	genlmsg_end(rsp, hdr);
	return 0;

err_cancel_msg:
	genlmsg_cancel(rsp, hdr);
	return -EMSGSIZE;
}

/**
 * pon_nl_gem_fill_pos() - pon_nl_obj_fill_t for the GEM port list
 * @pdev: the PON device
 * @pos: the list entry of the GEM port
 * @rsp: the skb to fill
 * @info: the dump info
 *
 * Return: the result of pon_nl_gem_fill().
 */
static int pon_nl_gem_fill_pos(struct pon_dev *pdev, struct list_head *pos,
			       struct sk_buff *rsp,
			       const struct genl_info *info)
{
	return pon_nl_gem_fill(pdev, list_entry(pos, struct pon_gem, list), rsp,
			       info);
}

/**
 * pon_nl_notify_gem() - send a GEM port notification to the mgmt group
 * @pdev: the PON device
 * @gem: the GEM port
 * @cmd: the notification command, PON_CMD_GEM_ADD_NTF or
 *       PON_CMD_GEM_DEL_NTF
 *
 * Context: Called with @pdev->lock held. May sleep.
 */
void pon_nl_notify_gem(struct pon_dev *pdev, struct pon_gem *gem, u32 cmd)
{
	pon_nl_notify_obj(pdev, &gem->list, cmd, pon_nl_gem_fill_pos);
}

/**
 * pon_nl_gem_get_doit() - handle PON_CMD_GEM_GET for one GEM port
 * @skb: the request
 * @info: the request info, user_ptr[0] holds the device
 *
 * Context: Called with the device lock held by pon_device_get_locked().
 * Return: 0, -ENOENT when the GEM port does not exist, or a negative errno.
 */
int pon_nl_gem_get_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct pon_dev *pdev = info->user_ptr[0];
	struct pon_gem *gem;

	if (GENL_REQ_ATTR_CHECK(info, PON_A_GEM_ID))
		return -EINVAL;

	gem = pon_gem_find(pdev, nla_get_u32(info->attrs[PON_A_GEM_ID]));
	if (!gem) {
		NL_SET_ERR_MSG(info->extack, "no such GEM port");
		return -ENOENT;
	}

	return pon_nl_obj_reply(info, &gem->list, pon_nl_gem_fill_pos);
}

/**
 * pon_nl_gem_get_dumpit() - handle the PON_CMD_GEM_GET dump
 * @rsp: the skb to fill
 * @cb: the dump state
 *
 * Return: the result of pon_nl_obj_dumpit().
 */
int pon_nl_gem_get_dumpit(struct sk_buff *rsp, struct netlink_callback *cb)
{
	return pon_nl_obj_dumpit(rsp, cb, offsetof(struct pon_dev, gems),
				 pon_nl_gem_fill_pos);
}

/* GEM upstream classifier */

/**
 * pon_nl_gem_map_parse() - read a classifier rule from a request
 * @info: the request info
 * @cfg: receives the rule, members without an attribute stay unset
 */
static void pon_nl_gem_map_parse(struct genl_info *info,
				 struct pon_gem_map_cfg *cfg)
{
	cfg->gem_id = nla_get_u32(info->attrs[PON_A_GEM_MAP_GEM_ID]);
	if (info->attrs[PON_A_GEM_MAP_TAG]) {
		cfg->tag_valid = true;
		cfg->tagged = nla_get_u32(info->attrs[PON_A_GEM_MAP_TAG]) ==
			      PON_GEM_MAP_TAG_TAGGED;
	}
	if (info->attrs[PON_A_GEM_MAP_VID]) {
		cfg->vid_valid = true;
		cfg->vid = nla_get_u32(info->attrs[PON_A_GEM_MAP_VID]);
	}
	if (info->attrs[PON_A_GEM_MAP_PBIT]) {
		cfg->pbit_valid = true;
		cfg->pbit = nla_get_u32(info->attrs[PON_A_GEM_MAP_PBIT]);
	}
	if (info->attrs[PON_A_GEM_MAP_DSCP]) {
		cfg->dscp_valid = true;
		cfg->dscp = nla_get_u32(info->attrs[PON_A_GEM_MAP_DSCP]);
	}
}

/**
 * pon_nl_gem_map_new_doit() - handle PON_CMD_GEM_MAP_NEW
 * @skb: the request
 * @info: the request info, user_ptr[0] holds the device
 *
 * Programs an upstream classifier rule through the gem_map_set op. The rule
 * joins the list only once the driver has taken it. A rule that exists is
 * programmed again and sends no notification.
 *
 * Context: Called with the device lock held by pon_device_get_locked().
 * Return: 0, -EOPNOTSUPP without a classifier in the driver, or a negative
 * errno.
 */
int pon_nl_gem_map_new_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct pon_dev *pdev = info->user_ptr[0];
	struct pon_gem_map_cfg cfg = {};
	struct pon_gem_map *map;
	bool is_new;
	int err;

	if (GENL_REQ_ATTR_CHECK(info, PON_A_GEM_MAP_GEM_ID))
		return -EINVAL;

	if (!pdev->ops->gem_map_set) {
		NL_SET_ERR_MSG(info->extack,
			       "the driver carries no GEM classifier");
		return -EOPNOTSUPP;
	}

	pon_nl_gem_map_parse(info, &cfg);

	map = pon_gem_map_find(pdev, &cfg);
	is_new = !map;
	if (is_new) {
		map = kzalloc_obj(*map, GFP_KERNEL);
		if (!map)
			return -ENOMEM;
	}

	err = pdev->ops->gem_map_set(pdev, &cfg, info->extack);
	if (err)
		goto err_free_map;

	map->cfg = cfg;
	if (is_new) {
		list_add_tail(&map->list, &pdev->gem_maps);
		pon_nl_obj_gen_inc();
		pon_nl_notify_gem_map(pdev, map, PON_CMD_GEM_MAP_ADD_NTF);
	}

	return 0;

err_free_map:
	if (is_new)
		kfree(map);
	return err;
}

/**
 * pon_nl_gem_map_del_doit() - handle PON_CMD_GEM_MAP_DEL
 * @skb: the request
 * @info: the request info, user_ptr[0] holds the device
 *
 * Context: Called with the device lock held by pon_device_get_locked().
 * Return: 0, -EOPNOTSUPP without a classifier in the driver, -ENOENT when
 * the rule does not exist, or another negative errno.
 */
int pon_nl_gem_map_del_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct pon_dev *pdev = info->user_ptr[0];
	struct pon_gem_map_cfg cfg = {};
	struct pon_gem_map *map;
	int err;

	if (GENL_REQ_ATTR_CHECK(info, PON_A_GEM_MAP_GEM_ID))
		return -EINVAL;

	if (!pdev->ops->gem_map_del) {
		NL_SET_ERR_MSG(info->extack,
			       "the driver carries no GEM classifier");
		return -EOPNOTSUPP;
	}

	pon_nl_gem_map_parse(info, &cfg);

	map = pon_gem_map_find(pdev, &cfg);
	if (!map) {
		NL_SET_ERR_MSG(info->extack, "no such classifier rule");
		return -ENOENT;
	}

	err = pdev->ops->gem_map_del(pdev, &cfg, info->extack);
	if (err)
		return err;

	pon_nl_notify_gem_map(pdev, map, PON_CMD_GEM_MAP_DEL_NTF);
	list_del(&map->list);
	kfree(map);
	pon_nl_obj_gen_inc();

	return 0;
}

/**
 * pon_nl_omci_register_doit() - handle PON_CMD_OMCI_REGISTER
 * @skb: the request
 * @info: the request info, user_ptr[0] holds the device
 *
 * Makes the requesting socket the owner of the OMCI channel of the device.
 * The received OMCI PDUs go to the owner as PON_CMD_OMCI_NTF. A second
 * request from the owner succeeds.
 *
 * Context: Called with the device lock held by pon_device_get_locked().
 * Return: 0, or -EBUSY while another socket owns the channel.
 */
int pon_nl_omci_register_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct pon_dev *pdev = info->user_ptr[0];
	int err;

	err = pon_omci_register(pdev, info->snd_portid);
	if (err)
		NL_SET_ERR_MSG(info->extack,
			       "another socket owns the OMCI channel");

	return err;
}

/**
 * pon_nl_omci_tx_doit() - handle PON_CMD_OMCI_TX
 * @skb: the request
 * @info: the request info, user_ptr[0] holds the device
 *
 * Sends one OMCI PDU without its MIC. Only the owner of the OMCI channel
 * sends. The PDU holds at least the 10 byte header of the extended message
 * format of ITU-T G.988 Table 11.2-2. The policy limits it to the 1980 byte
 * PDU of G.988 clause 11.2.5 less the 4 byte MIC. pon_omci_xmit() refuses a
 * PDU whose length does not match its format and sets the extended ack text
 * for that refusal only.
 *
 * Context: Called with the device lock held by pon_device_get_locked().
 * Return: 0, -EPERM when the socket does not own the channel, -EINVAL for
 * a PDU that is too short or does not match its format, or the error of
 * pon_omci_xmit().
 */
int pon_nl_omci_tx_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct pon_dev *pdev = info->user_ptr[0];
	struct nlattr *pdu;

	if (GENL_REQ_ATTR_CHECK(info, PON_A_OMCI_PDU))
		return -EINVAL;

	if (READ_ONCE(pdev->omci_portid) != info->snd_portid) {
		NL_SET_ERR_MSG(info->extack,
			       "the socket does not own the OMCI channel");
		return -EPERM;
	}

	pdu = info->attrs[PON_A_OMCI_PDU];

	return pon_omci_xmit(pdev, nla_data(pdu), nla_len(pdu), info->extack);
}

/**
 * pon_nl_omci_ntf() - pass a received OMCI PDU to the channel owner
 * @pdev: the PON device
 * @skb: the verified PDU
 *
 * Sends PON_CMD_OMCI_NTF as a unicast to the socket that owns the OMCI
 * channel. The caller keeps @skb.
 *
 * Context: Called with @pdev->lock held. May sleep.
 * Return: 0, -ENOTCONN without an owner, -ENOMEM, -EMSGSIZE, or the error
 * of genlmsg_unicast().
 */
int pon_nl_omci_ntf(struct pon_dev *pdev, const struct sk_buff *skb)
{
	u32 portid = READ_ONCE(pdev->omci_portid);
	struct sk_buff *ntf;
	struct nlattr *pdu;
	void *hdr;

	lockdep_assert_held(&pdev->lock);

	if (!portid)
		return -ENOTCONN;

	ntf = genlmsg_new(nla_total_size(sizeof(u32)) +
			  nla_total_size(skb->len), GFP_KERNEL);
	if (!ntf)
		return -ENOMEM;

	hdr = genlmsg_put(ntf, 0, 0, &pon_nl_family, 0, PON_CMD_OMCI_NTF);
	if (!hdr)
		goto err_free;

	if (nla_put_u32(ntf, PON_A_OMCI_DEV_ID, pdev->id))
		goto err_free;

	pdu = nla_reserve(ntf, PON_A_OMCI_PDU, skb->len);
	if (!pdu || skb_copy_bits(skb, 0, nla_data(pdu), skb->len))
		goto err_free;

	genlmsg_end(ntf, hdr);

	return genlmsg_unicast(dev_net(pdev->main_netdev), ntf, portid);

err_free:
	nlmsg_free(ntf);
	return -EMSGSIZE;
}

/* Upstream classifier rules */

/**
 * pon_nl_gem_map_fill() - put one classifier rule message into an skb
 * @pdev: the PON device
 * @map: the rule
 * @rsp: the skb to fill
 * @info: the request, or the notification info
 *
 * Context: Called with @pdev->lock held.
 * Return: 0, or -EMSGSIZE when the message does not fit.
 */
static int pon_nl_gem_map_fill(struct pon_dev *pdev, struct pon_gem_map *map,
			       struct sk_buff *rsp,
			       const struct genl_info *info)
{
	struct pon_gem_map_cfg *cfg = &map->cfg;
	void *hdr;

	hdr = genlmsg_iput(rsp, info);
	if (!hdr)
		return -EMSGSIZE;

	if (nla_put_u32(rsp, PON_A_GEM_MAP_DEV_ID, pdev->id) ||
	    nla_put_u32(rsp, PON_A_GEM_MAP_GEM_ID, cfg->gem_id))
		goto err_cancel_msg;

	if (cfg->tag_valid &&
	    nla_put_u32(rsp, PON_A_GEM_MAP_TAG, cfg->tagged))
		goto err_cancel_msg;
	if (cfg->vid_valid && nla_put_u32(rsp, PON_A_GEM_MAP_VID, cfg->vid))
		goto err_cancel_msg;
	if (cfg->pbit_valid && nla_put_u32(rsp, PON_A_GEM_MAP_PBIT, cfg->pbit))
		goto err_cancel_msg;
	if (cfg->dscp_valid && nla_put_u32(rsp, PON_A_GEM_MAP_DSCP, cfg->dscp))
		goto err_cancel_msg;

	genlmsg_end(rsp, hdr);
	return 0;

err_cancel_msg:
	genlmsg_cancel(rsp, hdr);
	return -EMSGSIZE;
}

/**
 * pon_nl_gem_map_fill_pos() - pon_nl_obj_fill_t for the classifier rules
 * @pdev: the PON device
 * @pos: the list entry of the rule
 * @rsp: the skb to fill
 * @info: the dump info
 *
 * Return: the result of pon_nl_gem_map_fill().
 */
static int pon_nl_gem_map_fill_pos(struct pon_dev *pdev,
				   struct list_head *pos, struct sk_buff *rsp,
				   const struct genl_info *info)
{
	return pon_nl_gem_map_fill(pdev,
				   list_entry(pos, struct pon_gem_map, list),
				   rsp, info);
}

/**
 * pon_nl_notify_gem_map() - send a classifier rule notification
 * @pdev: the PON device
 * @map: the rule
 * @cmd: the notification command, PON_CMD_GEM_MAP_ADD_NTF or
 *       PON_CMD_GEM_MAP_DEL_NTF
 *
 * Context: Called with @pdev->lock held. May sleep.
 */
void pon_nl_notify_gem_map(struct pon_dev *pdev, struct pon_gem_map *map,
			   u32 cmd)
{
	pon_nl_notify_obj(pdev, &map->list, cmd, pon_nl_gem_map_fill_pos);
}

/**
 * pon_nl_gem_map_get_dumpit() - handle the PON_CMD_GEM_MAP_GET dump
 * @rsp: the skb to fill
 * @cb: the dump state
 *
 * Return: the result of pon_nl_obj_dumpit().
 */
int pon_nl_gem_map_get_dumpit(struct sk_buff *rsp, struct netlink_callback *cb)
{
	return pon_nl_obj_dumpit(rsp, cb, offsetof(struct pon_dev, gem_maps),
				 pon_nl_gem_map_fill_pos);
}
