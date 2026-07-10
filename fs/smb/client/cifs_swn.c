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
	if (swnreg->net_name != NULL && strlen(swnreg->net_name) == host_len &&
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
	if (swnreg->net_name_notify && !cifs_swn_reg_net_name_matches(swnreg, tcon))
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

static struct cifs_tcon *
cifs_swn_reg_get_tcon(const struct cifs_swn_reg *swnreg)
{
	struct TCP_Server_Info *server;
	struct cifs_ses *ses;
	struct cifs_tcon *tcon;

	spin_lock(&cifs_tcp_ses_lock);
	list_for_each_entry(server, &cifs_tcp_ses_list, tcp_ses_list) {
		if (SERVER_IS_CHAN(server))
			continue;

		list_for_each_entry(ses, &server->smb_ses_list, smb_ses_list) {
			list_for_each_entry(tcon, &ses->tcon_list, tcon_list) {
				spin_lock(&tcon->tc_lock);
				if (tcon->status == TID_EXITING ||
				    !cifs_swn_reg_tcon_matches(swnreg, tcon)) {
					spin_unlock(&tcon->tc_lock);
					continue;
				}
				++tcon->tc_count;
				trace_smb3_tcon_ref(
					tcon->debug_id, tcon->tc_count,
					netfs_trace_tcon_ref_get_swn);
				spin_unlock(&tcon->tc_lock);
				spin_unlock(&cifs_tcp_ses_lock);
				return tcon;
			}
		}
	}
	spin_unlock(&cifs_tcp_ses_lock);
	return ERR_PTR(-ENOENT);
}

/*
 * Sends a register message to the userspace daemon based on the registration.
 * The authentication information to connect to the witness service is bundled
 * into the message. This function can sleep while allocating the genlmsg so
 * it must be called after taking a swnreg reference and release the lock.
 */
static int cifs_swn_send_register_message(struct cifs_swn_reg *swnreg)
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

	ret = nla_put(skb, CIFS_GENL_ATTR_SWN_IP,
		      sizeof(struct sockaddr_storage), &swnreg->addr);
	if (ret < 0)
		goto nlmsg_fail;

	if (swnreg->net_name_notify) {
		ret = nla_put_string(skb, CIFS_GENL_ATTR_SWN_NET_NAME,
				     swnreg->net_name);
		if (ret < 0)
			goto nlmsg_fail;

		ret = nla_put_flag(skb, CIFS_GENL_ATTR_SWN_NET_NAME_NOTIFY);
		if (ret < 0)
			goto nlmsg_fail;
	}

	if (swnreg->share_name_notify) {
		ret = nla_put_string(skb, CIFS_GENL_ATTR_SWN_SHARE_NAME,
				     swnreg->share_name);
		if (ret < 0)
			goto nlmsg_fail;

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

	cifs_dbg(FYI, "%s: Message to register for network name '%s' with id %d sent\n", __func__,
			swnreg->net_name ? swnreg->net_name : "", swnreg->id);

	return 0;

nlmsg_fail:
	genlmsg_cancel(skb, hdr);
	nlmsg_free(skb);
	return ret;
}

/*
 * Sends an unregister message to the userspace daemon based on the registration.
 * This function can sleep while allocating the genlmsg so it must be called after
 * taking a swnreg reference and release the lock.
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

	ret = nla_put(skb, CIFS_GENL_ATTR_SWN_IP,
		      sizeof(struct sockaddr_storage), &swnreg->addr);
	if (ret < 0)
		goto nlmsg_fail;

	if (swnreg->net_name_notify) {
		ret = nla_put_string(skb, CIFS_GENL_ATTR_SWN_NET_NAME,
				     swnreg->net_name);
		if (ret < 0)
			goto nlmsg_fail;

		ret = nla_put_flag(skb, CIFS_GENL_ATTR_SWN_NET_NAME_NOTIFY);
		if (ret < 0)
			goto nlmsg_fail;
	}

	if (swnreg->share_name_notify) {
		ret = nla_put_string(skb, CIFS_GENL_ATTR_SWN_SHARE_NAME,
				     swnreg->share_name);
		if (ret < 0)
			goto nlmsg_fail;

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

	cifs_dbg(FYI, "%s: Message to unregister for network name '%s' with id %d sent\n", __func__,
			swnreg->net_name ? swnreg->net_name : "", swnreg->id);

	return 0;

nlmsg_fail:
	genlmsg_cancel(skb, hdr);
	nlmsg_free(skb);
	return ret;
}

/*
 * Release a registration. Must be called with the last reference dropped (the
 * refcount has reached zero) and with the registration already removed from the
 * IDR under cifs_swnreg_idr_mutex, so it is no longer discoverable.
 */
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
	struct cifs_tcon *tcon;
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

	tcon = cifs_swn_reg_get_tcon(swnreg);
	if (IS_ERR(tcon)) {
		ret = PTR_ERR(tcon);
		cifs_dbg(FYI, "No matching tcon for registration id %d: %d\n",
			 swnreg->id, ret);

		/*
		 * There is no point in keeping a live registration if there
		 * are no matching tcons. Drop the reference that this fn
		 * took at the top.
		 */
		mutex_lock(&cifs_swnreg_idr_mutex);
		if (kref_put(&swnreg->ref_count, cifs_swn_reg_idr_remove)) {
			mutex_unlock(&cifs_swnreg_idr_mutex);
			cifs_swn_reg_release(swnreg);
			return;
		}

		/*
		 * Again, to drop the birth reference created via kref_init().
		 * If there are other live references, next check run might release it.
		 */
		if (kref_put(&swnreg->ref_count, cifs_swn_reg_idr_remove)) {
			mutex_unlock(&cifs_swnreg_idr_mutex);
			cifs_swn_reg_release(swnreg);
			return;
		}
		mutex_unlock(&cifs_swnreg_idr_mutex);

		/* References remain: retry later. */
		queue_delayed_work(cifsiod_wq, &swnreg->check,
				   swnreg->check_interval);
		return;
	}
	cifs_put_tcon(tcon, netfs_trace_tcon_ref_put_swn);

	/*
	 * It is safe to send the registration message multiple times.
	 * The userspace client library tracks if registered or not
	 * using the swnreg->id.
	 */
	ret = cifs_swn_send_register_message(swnreg);
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

static bool cifs_swn_resource_state_changed(struct cifs_tcon *tcon,
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

	/* Address has not changed */
	return false;
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

static bool cifs_swn_client_move(struct cifs_tcon *tcon,
				 struct sockaddr_storage *addr)
{
	int ret;

	if (cifs_match_ipaddr((struct sockaddr *)&tcon->ses->server->dstaddr,
			      (struct sockaddr *)addr)) {
		/* no-op */
		return false;
	}

	cifs_dbg(FYI, "%s: move to %pISc\n", __func__, addr);

	/* Store the reconnect address */
	ret = cifs_swn_store_swn_addr(addr, &tcon->ses->server->dstaddr,
				      &tcon->ses->server->swn_dstaddr);
	if (ret < 0) {
		cifs_dbg(VFS, "%s: failed to store address: %d\n", __func__,
			 ret);
		return false;
	}
	tcon->ses->server->use_swn_dstaddr = true;

	cifs_signal_cifsd_for_reconnect(tcon->ses->server, false);

	/* Address changed */
	return true;
}

/*
 * This function applies the notification to a matching tcon.
 * It is called holding a spinlock. Just sets the reconnect flag and
 * store the reconnect address if necessary.
 */
static bool
cifs_swn_handle_notification_tcon(const struct cifs_swn_notification *not,
				  struct cifs_tcon *tcon)
{
	switch (not->type) {
	case CIFS_SWN_NOTIFICATION_RESOURCE_CHANGE:
		return cifs_swn_resource_state_changed(
			tcon, not->data.resource_name_changed.name,
			not->data.resource_name_changed.state);
	case CIFS_SWN_NOTIFICATION_CLIENT_MOVE:
		return cifs_swn_client_move(tcon, not->data.client_move.addr);
	default:
		cifs_dbg(FYI, "%s: unknown notification type %d\n", __func__,
			 not->type);
		break;
	}
	return false;
}

/*
 * This function process a notification received for a registration. It
 * unregisters/registers as necessary and applies the notification to all
 * matching tcons.
 */
static int cifs_swn_handle_notification(const struct cifs_swn_notification *not)
{
	struct TCP_Server_Info *server;
	struct cifs_ses *ses;
	struct cifs_tcon *tcon;
	bool addr_changed = false;
	int ret;

	/* Walk the tcons and apply the notification to matching ones */
	spin_lock(&cifs_tcp_ses_lock);
	list_for_each_entry(server, &cifs_tcp_ses_list, tcp_ses_list) {
		if (SERVER_IS_CHAN(server))
			continue;

		cifs_server_lock(server);
		list_for_each_entry(ses, &server->smb_ses_list, smb_ses_list) {
			list_for_each_entry(tcon, &ses->tcon_list, tcon_list) {
				spin_lock(&tcon->tc_lock);
				if (tcon->status == TID_EXITING ||
				    !cifs_swn_reg_tcon_matches(not->swnreg,
							       tcon)) {
					spin_unlock(&tcon->tc_lock);
					continue;
				}
				addr_changed |=
					cifs_swn_handle_notification_tcon(not,
									  tcon);
				spin_unlock(&tcon->tc_lock);
			}
		}
		cifs_server_unlock(server);
	}
	spin_unlock(&cifs_tcp_ses_lock);

	if (!addr_changed)
		return 0;

	/*
	 * If the address has changed, unregister from the previous and
	 * register for the new one
	 */
	ret = cifs_swn_send_unregister_message(not->swnreg);
	if (ret < 0) {
		cifs_dbg(VFS,
			"%s: Failed to unregister for witness notifications: %d\n",
			__func__, ret);
	}

	/* Store the new address */
	not->swnreg->addr = *not->data.client_move.addr;

	/* Register for this new address */
	ret = cifs_swn_send_register_message(not->swnreg);
	if (ret < 0) {
		cifs_dbg(VFS,
			"%s: Failed to register for witness notifications: %d\n",
			__func__, ret);
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

	mutex_lock(&cifs_swnreg_idr_mutex);

	swnreg = cifs_find_swn_reg(tcon);
	if (!IS_ERR(swnreg)) {
		/*
		 * There is a registration matching this tcon, could be a second mount of
		 * the same share, increment the refcount.
		 */
		if (kref_get_unless_zero(&swnreg->ref_count)) {
			mutex_unlock(&cifs_swnreg_idr_mutex);
			return 0;
		}
		/* Else it is being released, allocate new one */
	} else if (PTR_ERR(swnreg) != -ENOENT) {
		mutex_unlock(&cifs_swnreg_idr_mutex);
		return PTR_ERR(swnreg);
	}

	/* Allocate new registration */
	swnreg = kzalloc_obj(struct cifs_swn_reg, GFP_KERNEL);
	if (swnreg == NULL) {
		mutex_unlock(&cifs_swnreg_idr_mutex);
		return -ENOMEM;
	}

	kref_init(&swnreg->ref_count);

	swnreg->id = idr_alloc(&cifs_swnreg_idr, swnreg, 1, 0, GFP_KERNEL);
	if (swnreg->id < 0) {
		ret = swnreg->id;
		cifs_dbg(FYI, "%s: failed to allocate registration id\n",
			 __func__);
		kfree(swnreg);
		mutex_unlock(&cifs_swnreg_idr_mutex);
		return ret;
	}

	swnreg->net_name_notify = true;
	swnreg->share_name_notify =
		(tcon->capabilities & SMB2_SHARE_CAP_ASYMMETRIC);
	swnreg->ip_notify = false;

	if (swnreg->net_name_notify) {
		swnreg->net_name = extract_hostname(tcon->tree_name);
		if (IS_ERR(swnreg->net_name)) {
			ret = PTR_ERR(swnreg->net_name);
			cifs_dbg(
				VFS,
				"%s: failed to extract host name from target: %d\n",
				__func__, ret);
			idr_remove(&cifs_swnreg_idr, swnreg->id);
			kfree(swnreg);
			mutex_unlock(&cifs_swnreg_idr_mutex);
			return ret;
		}
	}

	if (swnreg->share_name_notify) {
		swnreg->share_name = extract_sharename(tcon->tree_name);
		if (IS_ERR(swnreg->share_name)) {
			ret = PTR_ERR(swnreg->share_name);
			cifs_dbg(
				VFS,
				"%s: failed to extract share name from target: %d\n",
				__func__, ret);
			kfree(swnreg->net_name);
			idr_remove(&cifs_swnreg_idr, swnreg->id);
			kfree(swnreg);
			mutex_unlock(&cifs_swnreg_idr_mutex);
			return ret;
		}
	}

	ret = cifs_swn_reg_set_auth(swnreg, tcon);
	if (ret != 0) {
		cifs_dbg(VFS, "%s: failed to set auth info: %d\n", __func__,
			 ret);
		kfree(swnreg->net_name);
		kfree(swnreg->share_name);
		idr_remove(&cifs_swnreg_idr, swnreg->id);
		kfree(swnreg);
		mutex_unlock(&cifs_swnreg_idr_mutex);
		return ret;
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

	swnreg->check_interval = tcon->ses->server->echo_interval;
	INIT_DELAYED_WORK(&swnreg->check, cifs_swn_reg_check);

	/* Queue a immediate run to send the netlink message */
	queue_delayed_work(cifsiod_wq, &swnreg->check, 0);

	mutex_unlock(&cifs_swnreg_idr_mutex);

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
			id, kref_read(&swnreg->ref_count),
			swnreg->net_name ? swnreg->net_name : "",
			swnreg->net_name_notify ? "(y)" : "(n)",
			swnreg->share_name ? swnreg->share_name : "",
			swnreg->share_name_notify ? "(y)" : "(n)",
			&swnreg->addr, swnreg->ip_notify ? "(y)" : "(n)");
	}
	mutex_unlock(&cifs_swnreg_idr_mutex);
	seq_puts(m, "\n");
}
