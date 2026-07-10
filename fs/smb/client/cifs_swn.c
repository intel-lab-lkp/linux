// SPDX-License-Identifier: GPL-2.0
/*
 * Witness Service client for CIFS
 *
 * Copyright (c) 2020 Samuel Cabrero <scabrero@suse.de>
 */

#include <linux/kref.h>
#include <net/genetlink.h>
#include <uapi/linux/cifs/cifs_netlink.h>

#include "cifs_swn.h"
#include "cifsglob.h"
#include "cifsproto.h"
#include "fscache.h"
#include "cifs_debug.h"
#include "netlink.h"

static DEFINE_IDR(cifs_swnreg_idr);
static DEFINE_MUTEX(cifs_swnreg_idr_mutex);

struct cifs_swn_reg {
	int id;
	struct kref ref_count;

	const char *net_name;
	const char *share_name;
	struct sockaddr_storage addr;
	bool net_name_notify;
	bool share_name_notify;
	bool ip_notify;

	/* Need to keep this information to re-register when reconnecting */
	enum securityEnum auth_type;
	union {
		struct {
		} kerberos;
		struct {
			const char *domain_name;
			const char *user_name;
			const char *password;
		} ntlm;
	} authinfo;

	struct cifs_tcon *tcon;

	unsigned long check_interval;
	struct delayed_work check;
};

struct cifs_swn_notification {
	int type;
	union {
		struct {
			const char *name;
			int state;
		} resource_name_changed;
		struct {
			struct sockaddr_storage *addr;
		} client_move;
		struct {
			struct sockaddr_storage *addr;
		} share_move;
	} data;

	struct cifs_swn_reg *swnreg;
};

static int cifs_swn_reg_set_auth(struct cifs_swn_reg *swnreg,
				 const struct cifs_tcon *tcon)
{
	swnreg->auth_type =
		cifs_select_sectype(tcon->ses->server, tcon->ses->sectype);
	switch (swnreg->auth_type) {
	case Kerberos:
		break;
	case NTLMv2:
	case RawNTLMSSP:
		if (tcon->ses->user_name != NULL) {
			swnreg->authinfo.ntlm.user_name =
				kstrdup(tcon->ses->user_name, GFP_KERNEL);
			if (swnreg->authinfo.ntlm.user_name == NULL)
				return -ENOMEM;
		}
		if (tcon->ses->domainName != NULL) {
			swnreg->authinfo.ntlm.domain_name =
				kstrdup(tcon->ses->domainName, GFP_KERNEL);
			if (swnreg->authinfo.ntlm.domain_name == NULL) {
				kfree(swnreg->authinfo.ntlm.user_name);
				return -ENOMEM;
			}
		}
		if (tcon->ses->password != NULL) {
			swnreg->authinfo.ntlm.password =
				kstrdup(tcon->ses->password, GFP_KERNEL);
			if (swnreg->authinfo.ntlm.password == NULL) {
				kfree(swnreg->authinfo.ntlm.user_name);
				kfree(swnreg->authinfo.ntlm.domain_name);
				return -ENOMEM;
			}
		}
		break;
	default:
		cifs_dbg(VFS, "%s: secType %d not supported!\n", __func__,
			 swnreg->auth_type);
		return -EINVAL;
	}
	return 0;
}

static bool cifs_swn_str_equal(const char *a, const char *b)
{
	if (a == b)
		return true;

	if (a == NULL || b == NULL)
		return false;

	return strcmp(a, b) == 0;
}

static bool cifs_swn_auth_info_equal(const struct cifs_swn_reg *swnreg,
				     const struct cifs_tcon *tcon)
{
	enum securityEnum auth_type =
		cifs_select_sectype(tcon->ses->server, tcon->ses->sectype);
	if (swnreg->auth_type != auth_type)
		return false;

	switch (auth_type) {
	case Kerberos:
		break;
	case NTLMv2:
	case RawNTLMSSP:
		if (!cifs_swn_str_equal(tcon->ses->user_name,
					swnreg->authinfo.ntlm.user_name))
			return false;
		if (!cifs_swn_str_equal(tcon->ses->domainName,
					swnreg->authinfo.ntlm.domain_name))
			return false;
		if (!cifs_swn_str_equal(tcon->ses->password,
					swnreg->authinfo.ntlm.password))
			return false;
		break;
	default:
		cifs_dbg(VFS, "%s: secType %d not supported!\n", __func__,
			 auth_type);
		return false;
	}

	return true;
}

static int cifs_swn_auth_info_krb(struct sk_buff *skb)
{
	int ret;

	ret = nla_put_flag(skb, CIFS_GENL_ATTR_SWN_KRB_AUTH);
	if (ret < 0)
		return ret;

	return 0;
}

static int cifs_swn_auth_info_ntlm(struct cifs_swn_reg *swnreg,
				   struct sk_buff *skb)
{
	int ret;

	if (swnreg->authinfo.ntlm.user_name != NULL) {
		ret = nla_put_string(skb, CIFS_GENL_ATTR_SWN_USER_NAME,
				     swnreg->authinfo.ntlm.user_name);
		if (ret < 0)
			return ret;
	}

	if (swnreg->authinfo.ntlm.password != NULL) {
		ret = nla_put_string(skb, CIFS_GENL_ATTR_SWN_PASSWORD,
				     swnreg->authinfo.ntlm.password);
		if (ret < 0)
			return ret;
	}

	if (swnreg->authinfo.ntlm.domain_name != NULL) {
		ret = nla_put_string(skb, CIFS_GENL_ATTR_SWN_DOMAIN_NAME,
				     swnreg->authinfo.ntlm.domain_name);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static bool cifs_swn_reg_net_name_matches(const struct cifs_swn_reg *swnreg,
					  const struct cifs_tcon *tcon)
{
	const char *unc = tcon->tree_name;
	const char *host, *delim;
	size_t host_len;

	/* extract hostname, requires strlen(unc) >= 3 (\\a)*/
	if (strnlen(unc, 3) < 3)
		return false;

	/* extract_hostname: skip all leading '\' characters */
	for (host = unc; *host && *host == '\\'; host++)
		;

	if (!*host)
		return false;

	delim = strchr(host, '\\');
	if (!delim)
		return false;

	host_len = delim - host;
	if (strlen(swnreg->net_name) == host_len &&
	    !strncasecmp(swnreg->net_name, host, host_len)) {
		return true;
	}
	return false;
}

static bool cifs_swn_reg_share_name_matches(const struct cifs_swn_reg *swnreg,
					    const struct cifs_tcon *tcon)
{
	const char *unc = tcon->tree_name;
	const char *share, *delim;
	size_t share_len;

	/* extract share name, requires strlen(unc) >= 5 (\\a\b) */
	if (strnlen(unc, 5) < 5)
		return false;

	/* extract share name, start at unc + 2, then first '\' onward */
	share = unc + 2;
	delim = strchr(share, '\\');
	if (!delim)
		return false;

	share = delim + 1;
	share_len = strlen(share);

	if (strlen(swnreg->share_name) == share_len &&
	    !strncasecmp(swnreg->share_name, share, share_len)) {
		return true;
	}
	return false;
}

static bool cifs_swn_reg_tcon_matches(const struct cifs_swn_reg *swnreg,
				      const struct cifs_tcon *tcon)
{
	struct sockaddr_storage *tcon_dstaddr;

	if (!tcon->use_witness)
		return false;

	if (tcon->ses->server->use_swn_dstaddr)
		tcon_dstaddr = &tcon->ses->server->swn_dstaddr;
	else
		tcon_dstaddr = &tcon->ses->server->dstaddr;

	/* Auth info must match */
	if (!cifs_swn_auth_info_equal(swnreg, tcon))
		return false;

	/* Address must always match */
	if (!cifs_match_ipaddr((struct sockaddr *)&swnreg->addr,
			       (struct sockaddr *)tcon_dstaddr))
		return false;

	/* The network name notification is always enabled */
	if (!cifs_swn_reg_net_name_matches(swnreg, tcon))
		return false;

	/*
	 * Share name notifications is enabled only if asymmetric
	 * capability enabled otherwise ignored
	 */
	if (swnreg->share_name_notify !=
	    (tcon->capabilities & SMB2_SHARE_CAP_ASYMMETRIC))
		return false;
	else if (swnreg->share_name_notify &&
		 !cifs_swn_reg_share_name_matches(swnreg, tcon))
		return false;

	return true;
}

/*
 * Sends a register message to the userspace daemon based on the registration.
 * The authentication information to connect to the witness service is bundled
 * into the message.
 */
static int cifs_swn_send_register_message(struct cifs_swn_reg *swnreg,
					  struct cifs_tcon *tcon)
{
	struct sk_buff *skb;
	struct genlmsghdr *hdr;
	int ret;

	skb = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	hdr = genlmsg_put(skb, 0, 0, &cifs_genl_family, 0, CIFS_GENL_CMD_SWN_REGISTER);
	if (hdr == NULL) {
		ret = -ENOMEM;
		goto nlmsg_fail;
	}

	ret = nla_put_u32(skb, CIFS_GENL_ATTR_SWN_REGISTRATION_ID, swnreg->id);
	if (ret < 0)
		goto nlmsg_fail;

	ret = nla_put_string(skb, CIFS_GENL_ATTR_SWN_NET_NAME, swnreg->net_name);
	if (ret < 0)
		goto nlmsg_fail;

	ret = nla_put_string(skb, CIFS_GENL_ATTR_SWN_SHARE_NAME, swnreg->share_name);
	if (ret < 0)
		goto nlmsg_fail;

	ret = nla_put(skb, CIFS_GENL_ATTR_SWN_IP,
		      sizeof(struct sockaddr_storage), &swnreg->addr);
	if (ret < 0)
		goto nlmsg_fail;

	if (swnreg->net_name_notify) {
		ret = nla_put_flag(skb, CIFS_GENL_ATTR_SWN_NET_NAME_NOTIFY);
		if (ret < 0)
			goto nlmsg_fail;
	}

	if (swnreg->share_name_notify) {
		ret = nla_put_flag(skb, CIFS_GENL_ATTR_SWN_SHARE_NAME_NOTIFY);
		if (ret < 0)
			goto nlmsg_fail;
	}

	if (swnreg->ip_notify) {
		ret = nla_put_flag(skb, CIFS_GENL_ATTR_SWN_IP_NOTIFY);
		if (ret < 0)
			goto nlmsg_fail;
	}

	switch (swnreg->auth_type) {
	case Kerberos:
		ret = cifs_swn_auth_info_krb(skb);
		if (ret < 0) {
			cifs_dbg(VFS, "%s: Failed to get kerberos auth info: %d\n", __func__, ret);
			goto nlmsg_fail;
		}
		break;
	case NTLMv2:
	case RawNTLMSSP:
		ret = cifs_swn_auth_info_ntlm(swnreg, skb);
		if (ret < 0) {
			cifs_dbg(VFS, "%s: Failed to get NTLM auth info: %d\n", __func__, ret);
			goto nlmsg_fail;
		}
		break;
	default:
		cifs_dbg(VFS, "%s: secType %d not supported!\n", __func__,
			 swnreg->auth_type);
		ret = -EINVAL;
		goto nlmsg_fail;
	}

	genlmsg_end(skb, hdr);
	genlmsg_multicast(&cifs_genl_family, skb, 0, CIFS_GENL_MCGRP_SWN, GFP_ATOMIC);

	cifs_dbg(FYI, "%s: Message to register for network name %s with id %d sent\n", __func__,
			swnreg->net_name, swnreg->id);

	return 0;

nlmsg_fail:
	genlmsg_cancel(skb, hdr);
	nlmsg_free(skb);
	return ret;
}

/*
 * Sends an uregister message to the userspace daemon based on the registration
 */
static int cifs_swn_send_unregister_message(struct cifs_swn_reg *swnreg)
{
	struct sk_buff *skb;
	struct genlmsghdr *hdr;
	int ret;

	skb = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (skb == NULL)
		return -ENOMEM;

	hdr = genlmsg_put(skb, 0, 0, &cifs_genl_family, 0, CIFS_GENL_CMD_SWN_UNREGISTER);
	if (hdr == NULL) {
		ret = -ENOMEM;
		goto nlmsg_fail;
	}

	ret = nla_put_u32(skb, CIFS_GENL_ATTR_SWN_REGISTRATION_ID, swnreg->id);
	if (ret < 0)
		goto nlmsg_fail;

	ret = nla_put_string(skb, CIFS_GENL_ATTR_SWN_NET_NAME, swnreg->net_name);
	if (ret < 0)
		goto nlmsg_fail;

	ret = nla_put_string(skb, CIFS_GENL_ATTR_SWN_SHARE_NAME, swnreg->share_name);
	if (ret < 0)
		goto nlmsg_fail;

	ret = nla_put(skb, CIFS_GENL_ATTR_SWN_IP,
		      sizeof(struct sockaddr_storage), &swnreg->addr);
	if (ret < 0)
		goto nlmsg_fail;

	if (swnreg->net_name_notify) {
		ret = nla_put_flag(skb, CIFS_GENL_ATTR_SWN_NET_NAME_NOTIFY);
		if (ret < 0)
			goto nlmsg_fail;
	}

	if (swnreg->share_name_notify) {
		ret = nla_put_flag(skb, CIFS_GENL_ATTR_SWN_SHARE_NAME_NOTIFY);
		if (ret < 0)
			goto nlmsg_fail;
	}

	if (swnreg->ip_notify) {
		ret = nla_put_flag(skb, CIFS_GENL_ATTR_SWN_IP_NOTIFY);
		if (ret < 0)
			goto nlmsg_fail;
	}

	genlmsg_end(skb, hdr);
	genlmsg_multicast(&cifs_genl_family, skb, 0, CIFS_GENL_MCGRP_SWN, GFP_ATOMIC);

	cifs_dbg(FYI, "%s: Message to unregister for network name %s with id %d sent\n", __func__,
			swnreg->net_name, swnreg->id);

	return 0;

nlmsg_fail:
	genlmsg_cancel(skb, hdr);
	nlmsg_free(skb);
	return ret;
}

static void cifs_swn_reg_release(struct cifs_swn_reg *swnreg)
{
	int ret;

	ret = cifs_swn_send_unregister_message(swnreg);
	if (ret < 0)
		cifs_dbg(VFS, "%s: Failed to send unregister message: %d\n", __func__, ret);

	switch (swnreg->auth_type) {
	case NTLMv2:
	case RawNTLMSSP:
		kfree(swnreg->authinfo.ntlm.user_name);
		kfree(swnreg->authinfo.ntlm.domain_name);
		kfree(swnreg->authinfo.ntlm.password);
		break;
	default:
		break;
	}

	kfree(swnreg->net_name);
	kfree(swnreg->share_name);
	kfree(swnreg);
}

static void cifs_swn_reg_idr_remove(struct kref *ref)
{
	struct cifs_swn_reg *swnreg = container_of(ref, struct cifs_swn_reg, ref_count);

	idr_remove(&cifs_swnreg_idr, swnreg->id);
}

/*
 * Periodic task to enforce registration even when the userspace daemon is
 * started after mounting the share.
 */
static void cifs_swn_reg_check(struct work_struct *work)
{
	struct cifs_swn_reg *swnreg =
		container_of(work, struct cifs_swn_reg, check.work);
	int ret;

	/*
	 * First take a reference to avoid other thread releasing the swnreg
	 * on concurrent cifs_swn_unregister().
	 *
	 * Do not resurrect dead registrations, a concurrent cifs_swn_unregister
	 * can drop refcount to 0 and remove this swnreg from the IDR before
	 * releasing the mutex, but it will then wait for this callback to end
	 * before releasing the swnreg.
	 */
	mutex_lock(&cifs_swnreg_idr_mutex);
	if (!kref_get_unless_zero(&swnreg->ref_count)) {
		mutex_unlock(&cifs_swnreg_idr_mutex);
		return;
	}
	mutex_unlock(&cifs_swnreg_idr_mutex);

	/*
	 * It is safe to send the registration message multiple times.
	 * The userspace client library tracks if registered or not
	 * using the swnreg->id.
	 */
	ret = cifs_swn_send_register_message(swnreg, swnreg->tcon);
	if (ret < 0)
		cifs_dbg(FYI, "%s: Failed to send register message: %d\n",
			 __func__, ret);

	/* Release our reference */
	mutex_lock(&cifs_swnreg_idr_mutex);
	if (kref_put(&swnreg->ref_count, cifs_swn_reg_idr_remove)) {
		mutex_unlock(&cifs_swnreg_idr_mutex);
		cifs_swn_reg_release(swnreg);
		return;
	}
	mutex_unlock(&cifs_swnreg_idr_mutex);

	queue_delayed_work(cifsiod_wq, &swnreg->check, swnreg->check_interval);
}

/*
 * Try to find a matching registration for the tcon's server name and share name.
 * Calls to this function must be protected by cifs_swnreg_idr_mutex.
 */
static struct cifs_swn_reg *cifs_find_swn_reg(struct cifs_tcon *tcon)
{
	struct cifs_swn_reg *swnreg;
	int id;

	idr_for_each_entry(&cifs_swnreg_idr, swnreg, id) {
		if (cifs_swn_reg_tcon_matches(swnreg, tcon))
			return swnreg;
	}

	return ERR_PTR(-ENOENT);
}

/*
 * Get a registration for the tcon's server and share name, allocating a new one if it does not
 * exists
 */
static struct cifs_swn_reg *cifs_get_swn_reg(struct cifs_tcon *tcon)
{
	struct cifs_swn_reg *swnreg = NULL;
	int ret;

	mutex_lock(&cifs_swnreg_idr_mutex);

	/* Check if we are already registered for this network and share names */
	swnreg = cifs_find_swn_reg(tcon);
	if (!IS_ERR(swnreg)) {
		kref_get(&swnreg->ref_count);
		goto unlock;
	} else if (PTR_ERR(swnreg) != -ENOENT) {
		goto unlock;
	}

	swnreg = kmalloc_obj(struct cifs_swn_reg, GFP_ATOMIC);
	if (swnreg == NULL) {
		ret = -ENOMEM;
		goto fail_unlock;
	}

	kref_init(&swnreg->ref_count);

	swnreg->id = idr_alloc(&cifs_swnreg_idr, swnreg, 1, 0, GFP_ATOMIC);
	if (swnreg->id < 0) {
		cifs_dbg(FYI, "%s: failed to allocate registration id\n", __func__);
		ret = swnreg->id;
		goto fail;
	}

	swnreg->net_name = extract_hostname(tcon->tree_name);
	if (IS_ERR(swnreg->net_name)) {
		ret = PTR_ERR(swnreg->net_name);
		cifs_dbg(VFS, "%s: failed to extract host name from target: %d\n", __func__, ret);
		goto fail_idr;
	}

	swnreg->share_name = extract_sharename(tcon->tree_name);
	if (IS_ERR(swnreg->share_name)) {
		ret = PTR_ERR(swnreg->share_name);
		cifs_dbg(VFS, "%s: failed to extract share name from target: %d\n", __func__, ret);
		goto fail_net_name;
	}

	ret = cifs_swn_reg_set_auth(swnreg, tcon);
	if (ret != 0) {
		cifs_dbg(VFS, "%s: failed to set auth info: %d\n", __func__,
			 ret);
		goto fail_share_name;
	}

	/*
	 * If there is an address stored use it instead of the server address, because we are
	 * in the process of reconnecting to it after a share has been moved or we have been
	 * told to switch to it (client move message). In these cases we unregister from the
	 * server address and register to the new address when we receive the notification.
	 */
	if (tcon->ses->server->use_swn_dstaddr)
		swnreg->addr = tcon->ses->server->swn_dstaddr;
	else
		swnreg->addr = tcon->ses->server->dstaddr;

	swnreg->net_name_notify = true;
	swnreg->share_name_notify =
		(tcon->capabilities & SMB2_SHARE_CAP_ASYMMETRIC);
	swnreg->ip_notify = false;

	swnreg->tcon = tcon;

	swnreg->check_interval = tcon->ses->server->echo_interval;
	INIT_DELAYED_WORK(&swnreg->check, cifs_swn_reg_check);

	queue_delayed_work(cifsiod_wq, &swnreg->check, swnreg->check_interval);
unlock:
	mutex_unlock(&cifs_swnreg_idr_mutex);

	return swnreg;
fail_share_name:
	kfree(swnreg->share_name);
fail_net_name:
	kfree(swnreg->net_name);
fail_idr:
	idr_remove(&cifs_swnreg_idr, swnreg->id);
fail:
	kfree(swnreg);
fail_unlock:
	mutex_unlock(&cifs_swnreg_idr_mutex);
	return ERR_PTR(ret);
}

static int cifs_swn_resource_state_changed(struct cifs_tcon *tcon,
					   const char *name, int state)
{
	switch (state) {
	case CIFS_SWN_RESOURCE_STATE_UNAVAILABLE:
		cifs_dbg(FYI, "%s: resource name '%s' become unavailable\n", __func__, name);
		cifs_signal_cifsd_for_reconnect(tcon->ses->server, true);
		break;
	case CIFS_SWN_RESOURCE_STATE_AVAILABLE:
		cifs_dbg(FYI, "%s: resource name '%s' become available\n", __func__, name);
		cifs_signal_cifsd_for_reconnect(tcon->ses->server, true);
		break;
	case CIFS_SWN_RESOURCE_STATE_UNKNOWN:
		cifs_dbg(FYI, "%s: resource name '%s' changed to unknown state\n", __func__, name);
		break;
	}
	return 0;
}

static int cifs_swn_store_swn_addr(const struct sockaddr_storage *new,
				   const struct sockaddr_storage *old,
				   struct sockaddr_storage *dst)
{
	__be16 port = cpu_to_be16(CIFS_PORT);

	if (old->ss_family == AF_INET) {
		struct sockaddr_in *ipv4 = (struct sockaddr_in *)old;

		port = ipv4->sin_port;
	} else if (old->ss_family == AF_INET6) {
		struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)old;

		port = ipv6->sin6_port;
	}

	if (new->ss_family == AF_INET) {
		struct sockaddr_in *ipv4 = (struct sockaddr_in *)new;

		ipv4->sin_port = port;
	} else if (new->ss_family == AF_INET6) {
		struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)new;

		ipv6->sin6_port = port;
	}

	*dst = *new;

	return 0;
}

static int cifs_swn_reconnect(struct cifs_tcon *tcon, struct sockaddr_storage *addr)
{
	int ret = 0;

	cifs_server_lock(tcon->ses->server);

	if (cifs_match_ipaddr((struct sockaddr *)&tcon->ses->server->dstaddr,
			      (struct sockaddr *)addr)) {
		/* no-op */
		goto unlock;
	}

	/*
	 * Unregister to stop receiving notifications for the old IP address.
	 */
	ret = cifs_swn_unregister(tcon);
	if (ret < 0) {
		cifs_dbg(VFS, "%s: Failed to unregister for witness notifications: %d\n",
			 __func__, ret);
		/*
		 * Do not jump return on error, continue storing and registering for
		 * notifications for the new address. There will be a stale registration
		 * around running its periodic check task, which should cancel itself
		 * if no matching any tcon.
		 */
	}

	/* Store the reconnect address */
	ret = cifs_swn_store_swn_addr(addr, &tcon->ses->server->dstaddr,
				      &tcon->ses->server->swn_dstaddr);
	if (ret < 0) {
		cifs_dbg(VFS, "%s: failed to store address: %d\n", __func__,
			 ret);
		goto unlock;
	}
	tcon->ses->server->use_swn_dstaddr = true;

	/*
	 * And register to receive notifications for the new IP address now that we have
	 * stored the new address.
	 */
	ret = cifs_swn_register(tcon);
	if (ret < 0) {
		cifs_dbg(VFS, "%s: Failed to register for witness notifications: %d\n",
			 __func__, ret);
		goto unlock;
	}

	cifs_signal_cifsd_for_reconnect(tcon->ses->server, false);

unlock:
	cifs_server_unlock(tcon->ses->server);

	return ret;
}

static int cifs_swn_client_move(struct cifs_tcon *tcon,
				struct sockaddr_storage *addr)
{
	struct sockaddr_in *ipv4 = (struct sockaddr_in *)addr;
	struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)addr;

	if (addr->ss_family == AF_INET)
		cifs_dbg(FYI, "%s: move to %pI4\n", __func__, &ipv4->sin_addr);
	else if (addr->ss_family == AF_INET6)
		cifs_dbg(FYI, "%s: move to %pI6\n", __func__, &ipv6->sin6_addr);

	return cifs_swn_reconnect(tcon, addr);
}

static int cifs_swn_handle_notification(const struct cifs_swn_notification *not)
{
	switch (not->type) {
	case CIFS_SWN_NOTIFICATION_RESOURCE_CHANGE:
		return cifs_swn_resource_state_changed(
			not->swnreg->tcon, not->data.resource_name_changed.name,
			not->data.resource_name_changed.state);
	case CIFS_SWN_NOTIFICATION_CLIENT_MOVE:
		return cifs_swn_client_move(not->swnreg->tcon,
					    not->data.client_move.addr);
	default:
		cifs_dbg(FYI, "%s: unknown notification type %d\n", __func__,
			 not->type);
		break;
	}
	return 0;
}

int cifs_swn_notify(struct sk_buff *skb, struct genl_info *info)
{
	int swnreg_id;
	struct cifs_swn_notification not;
	int ret;

	/* Get the registration ID */
	if (info->attrs[CIFS_GENL_ATTR_SWN_REGISTRATION_ID]) {
		swnreg_id = nla_get_u32(
			info->attrs[CIFS_GENL_ATTR_SWN_REGISTRATION_ID]);
	} else {
		cifs_dbg(FYI, "%s: missing registration id attribute\n", __func__);
		return -EINVAL;
	}

	/* Fill the notification struct */
	if (info->attrs[CIFS_GENL_ATTR_SWN_NOTIFICATION_TYPE]) {
		not.type = nla_get_u32(
			info->attrs[CIFS_GENL_ATTR_SWN_NOTIFICATION_TYPE]);
	} else {
		cifs_dbg(FYI, "%s: missing notification type attribute\n", __func__);
		return -EINVAL;
	}

	switch (not.type) {
	case CIFS_SWN_NOTIFICATION_RESOURCE_CHANGE: {
		if (info->attrs[CIFS_GENL_ATTR_SWN_RESOURCE_NAME]) {
			not.data.resource_name_changed
				.name = (const char *)nla_data(
				info->attrs[CIFS_GENL_ATTR_SWN_RESOURCE_NAME]);
		} else {
			cifs_dbg(FYI, "%s: missing resource name attribute\n", __func__);
			return -EINVAL;
		}

		if (info->attrs[CIFS_GENL_ATTR_SWN_RESOURCE_STATE]) {
			not.data.resource_name_changed.state = nla_get_u32(
				info->attrs[CIFS_GENL_ATTR_SWN_RESOURCE_STATE]);
		} else {
			cifs_dbg(FYI, "%s: missing resource state attribute\n", __func__);
			return -EINVAL;
		}
	} break;
	case CIFS_SWN_NOTIFICATION_CLIENT_MOVE: {
		if (info->attrs[CIFS_GENL_ATTR_SWN_IP]) {
			not.data.client_move.addr =
				nla_data(info->attrs[CIFS_GENL_ATTR_SWN_IP]);
		} else {
			cifs_dbg(FYI, "%s: missing IP address attribute\n", __func__);
			return -EINVAL;
		}
	} break;
	default:
		cifs_dbg(FYI, "%s: unknown notification type %d\n", __func__,
			 not.type);
		return 0;
	}

	/*
	 * Get the registration.
	 */
	mutex_lock(&cifs_swnreg_idr_mutex);
	not.swnreg = idr_find(&cifs_swnreg_idr, swnreg_id);
	if (not.swnreg == NULL) {
		mutex_unlock(&cifs_swnreg_idr_mutex);
		cifs_dbg(FYI, "%s: registration id %d not found\n", __func__,
			 swnreg_id);
		return 0;
	}

	/*
	 * Increment the refcount under the lock while processing the notification
	 * and release the swnreg_idr_mutex because processing will take
	 * cifs_tcp_ses_lock.
	 */
	if (!kref_get_unless_zero(&not.swnreg->ref_count)) {
		/* The registration is being released, ignore the notificaiton */
		mutex_unlock(&cifs_swnreg_idr_mutex);
		return 0;
	}
	mutex_unlock(&cifs_swnreg_idr_mutex);

	ret = cifs_swn_handle_notification(&not);
	if (ret) {
		cifs_dbg(FYI,
			"%s: Failed to process notification for registration id %d: %d\n",
			__func__, swnreg_id, ret);
	}

	mutex_lock(&cifs_swnreg_idr_mutex);
	if (kref_put(&not.swnreg->ref_count, cifs_swn_reg_idr_remove)) {
		mutex_unlock(&cifs_swnreg_idr_mutex);
		cancel_delayed_work_sync(&not.swnreg->check);
		cifs_swn_reg_release(not.swnreg);
		return 0;
	}
	mutex_unlock(&cifs_swnreg_idr_mutex);

	return 0;
}

int cifs_swn_register(struct cifs_tcon *tcon)
{
	struct cifs_swn_reg *swnreg;
	int ret;

	swnreg = cifs_get_swn_reg(tcon);
	if (IS_ERR(swnreg))
		return PTR_ERR(swnreg);

	ret = cifs_swn_send_register_message(swnreg, tcon);
	if (ret < 0) {
		cifs_dbg(VFS, "%s: Failed to send swn register message: %d\n", __func__, ret);
		/* Do not put the swnreg or return error, the check task will retry */
	}

	return 0;
}

int cifs_swn_unregister(struct cifs_tcon *tcon)
{
	struct cifs_swn_reg *swnreg;

	mutex_lock(&cifs_swnreg_idr_mutex);
	swnreg = cifs_find_swn_reg(tcon);
	if (IS_ERR(swnreg)) {
		mutex_unlock(&cifs_swnreg_idr_mutex);
		return PTR_ERR(swnreg);
	}
	if (kref_put(&swnreg->ref_count, cifs_swn_reg_idr_remove)) {
		mutex_unlock(&cifs_swnreg_idr_mutex);
		cancel_delayed_work_sync(&swnreg->check);
		cifs_swn_reg_release(swnreg);
		return 0;
	}

	mutex_unlock(&cifs_swnreg_idr_mutex);

	return 0;
}

void cifs_swn_dump(struct seq_file *m)
{
	struct cifs_swn_reg *swnreg;
	int id;

	seq_puts(m, "Witness registrations:");

	mutex_lock(&cifs_swnreg_idr_mutex);
	idr_for_each_entry(&cifs_swnreg_idr, swnreg, id) {
		seq_printf(
			m,
			"\nId: %d Refs: %u Network name: '%s'%s Share name: '%s'%s Ip address: '%pISc'%s",
			id, kref_read(&swnreg->ref_count), swnreg->net_name,
			swnreg->net_name_notify ? "(y)" : "(n)",
			swnreg->share_name,
			swnreg->share_name_notify ? "(y)" : "(n)",
			&swnreg->addr, swnreg->ip_notify ? "(y)" : "(n)");
	}
	mutex_unlock(&cifs_swnreg_idr_mutex);
	seq_puts(m, "\n");
}
