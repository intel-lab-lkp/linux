// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/kernel.h>
#include <linux/bits.h>
#include <linux/bitfield.h>
#include <linux/idr.h>
#include <linux/netdevice.h>
#include <linux/netlink.h>
#include <linux/skbuff.h>
#include <linux/xarray.h>
#include <net/net_shaper.h>

#include "shaper_nl_gen.h"

#include "../core/dev.h"

#define NET_SHAPER_SCOPE_SHIFT	26
#define NET_SHAPER_ID_MASK	GENMASK(NET_SHAPER_SCOPE_SHIFT - 1, 0)
#define NET_SHAPER_SCOPE_MASK	GENMASK(31, NET_SHAPER_SCOPE_SHIFT)

#define NET_SHAPER_ID_UNSPEC NET_SHAPER_ID_MASK

struct net_shaper_data {
	struct xarray shapers;
};

struct net_shaper_nl_ctx {
	u32 start_index;
};

static int net_shaper_fill_handle(struct sk_buff *msg,
				  const struct net_shaper_handle *handle,
				  u32 type, const struct genl_info *info)
{
	struct nlattr *handle_attr;

	if (handle->scope == NET_SHAPER_SCOPE_UNSPEC)
		return 0;

	handle_attr = nla_nest_start_noflag(msg, type);
	if (!handle_attr)
		return -EMSGSIZE;

	if (nla_put_u32(msg, NET_SHAPER_A_SCOPE, handle->scope) ||
	    (handle->scope >= NET_SHAPER_SCOPE_QUEUE &&
	     nla_put_u32(msg, NET_SHAPER_A_ID, handle->id)))
		goto handle_nest_cancel;

	nla_nest_end(msg, handle_attr);
	return 0;

handle_nest_cancel:
	nla_nest_cancel(msg, handle_attr);
	return -EMSGSIZE;
}

static int
net_shaper_fill_one(struct sk_buff *msg,
		    const struct net_shaper_handle *handle,
		    const struct net_shaper_info *shaper,
		    const struct genl_info *info)
{
	void *hdr;

	hdr = genlmsg_iput(msg, info);
	if (!hdr)
		return -EMSGSIZE;

	if (net_shaper_fill_handle(msg, &shaper->parent, NET_SHAPER_A_PARENT,
				   info) ||
	    net_shaper_fill_handle(msg, handle, NET_SHAPER_A_HANDLE, info) ||
	    ((shaper->bw_min || shaper->bw_max || shaper->burst) &&
	     nla_put_u32(msg, NET_SHAPER_A_METRIC, shaper->metric)) ||
	    (shaper->bw_min &&
	     nla_put_uint(msg, NET_SHAPER_A_BW_MIN, shaper->bw_min)) ||
	    (shaper->bw_max &&
	     nla_put_uint(msg, NET_SHAPER_A_BW_MAX, shaper->bw_max)) ||
	    (shaper->burst &&
	     nla_put_uint(msg, NET_SHAPER_A_BURST, shaper->burst)) ||
	    (shaper->priority &&
	     nla_put_u32(msg, NET_SHAPER_A_PRIORITY, shaper->priority)) ||
	    (shaper->weight &&
	     nla_put_u32(msg, NET_SHAPER_A_WEIGHT, shaper->weight)))
		goto nla_put_failure;

	genlmsg_end(msg, hdr);

	return 0;

nla_put_failure:
	genlmsg_cancel(msg, hdr);
	return -EMSGSIZE;
}

/* On success sets pdev to the relevant device and acquires a reference
 * to it.
 */
static int net_shaper_fetch_dev(const struct genl_info *info,
				struct net_device **pdev)
{
	struct net *ns = genl_info_net(info);
	struct net_device *dev;
	int ifindex;

	if (GENL_REQ_ATTR_CHECK(info, NET_SHAPER_A_IFINDEX))
		return -EINVAL;

	ifindex = nla_get_u32(info->attrs[NET_SHAPER_A_IFINDEX]);
	dev = dev_get_by_index(ns, ifindex);
	if (!dev) {
		GENL_SET_ERR_MSG_FMT(info, "device %d not found", ifindex);
		return -EINVAL;
	}

	if (!dev->netdev_ops->net_shaper_ops) {
		GENL_SET_ERR_MSG_FMT(info, "device %s does not support H/W shaper",
				     dev->name);
		netdev_put(dev, NULL);
		return -EOPNOTSUPP;
	}

	*pdev = dev;
	return 0;
}

static inline u32
net_shaper_handle_to_index(const struct net_shaper_handle *handle)
{
	return FIELD_PREP(NET_SHAPER_SCOPE_MASK, handle->scope) |
		FIELD_PREP(NET_SHAPER_ID_MASK, handle->id);
}

static void net_shaper_index_to_handle(u32 index,
				       struct net_shaper_handle *handle)
{
	handle->scope = FIELD_GET(NET_SHAPER_SCOPE_MASK, index);
	handle->id = FIELD_GET(NET_SHAPER_ID_MASK, index);
}

static struct xarray *net_shaper_cache_container(struct net_device *dev)
{
	/* The barrier pairs with cmpxchg on init. */
	struct net_shaper_data *data = READ_ONCE(dev->net_shaper_data);

	return data ? &data->shapers : NULL;
}

/* Lookup the given shaper inside the cache. */
static struct net_shaper_info *
net_shaper_cache_lookup(struct net_device *dev,
			const struct net_shaper_handle *handle)
{
	struct xarray *xa = net_shaper_cache_container(dev);
	u32 index = net_shaper_handle_to_index(handle);

	return xa ? xa_load(xa, index) : NULL;
}

static int net_shaper_parse_handle(const struct nlattr *attr,
				   const struct genl_info *info,
				   struct net_shaper_handle *handle)
{
	struct nlattr *tb[NET_SHAPER_A_ID + 1];
	struct nlattr *scope_attr, *id_attr;
	u32 id = 0;
	int ret;

	ret = nla_parse_nested(tb, NET_SHAPER_A_ID, attr,
			       net_shaper_handle_nl_policy, info->extack);
	if (ret < 0)
		return ret;

	scope_attr = tb[NET_SHAPER_A_SCOPE];
	if (!scope_attr) {
		NL_SET_BAD_ATTR(info->extack, tb[NET_SHAPER_A_SCOPE]);
		return -EINVAL;
	}

	handle->scope = nla_get_u32(scope_attr);

	/* The default id for NODE scope shapers is an invalid one
	 * to help the 'group' operation discriminate between new
	 * NODE shaper creation (ID_UNSPEC) and reuse of existing
	 * shaper (any other value).
	 */
	id_attr = tb[NET_SHAPER_A_ID];
	if (id_attr)
		id = nla_get_u32(id_attr);
	else if (handle->scope == NET_SHAPER_SCOPE_NODE)
		id = NET_SHAPER_ID_UNSPEC;

	handle->id = id;
	return 0;
}

int net_shaper_nl_pre_doit(const struct genl_split_ops *ops,
			   struct sk_buff *skb, struct genl_info *info)
{
	struct net_device *dev;
	int ret;

	ret = net_shaper_fetch_dev(info, &dev);
	if (ret)
		return ret;

	info->user_ptr[0] = dev;
	return 0;
}

void net_shaper_nl_post_doit(const struct genl_split_ops *ops,
			     struct sk_buff *skb, struct genl_info *info)
{
	struct net_device *dev = info->user_ptr[0];

	netdev_put(dev, NULL);
}

int net_shaper_nl_get_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct net_device *dev = info->user_ptr[0];
	struct net_shaper_handle handle;
	struct net_shaper_info *shaper;
	struct sk_buff *msg;
	int ret;

	if (GENL_REQ_ATTR_CHECK(info, NET_SHAPER_A_HANDLE))
		return -EINVAL;

	ret = net_shaper_parse_handle(info->attrs[NET_SHAPER_A_HANDLE], info,
				      &handle);
	if (ret < 0)
		return ret;

	shaper = net_shaper_cache_lookup(dev, &handle);
	if (!shaper) {
		NL_SET_BAD_ATTR(info->extack,
				info->attrs[NET_SHAPER_A_HANDLE]);
		return -ENOENT;
	}

	msg = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	ret = net_shaper_fill_one(msg, &handle, shaper, info);
	if (ret)
		goto free_msg;

	ret =  genlmsg_reply(msg, info);
	if (ret)
		goto free_msg;

	return 0;

free_msg:
	nlmsg_free(msg);
	return ret;
}

int net_shaper_nl_get_dumpit(struct sk_buff *skb,
			     struct netlink_callback *cb)
{
	struct net_shaper_nl_ctx *ctx = (struct net_shaper_nl_ctx *)cb->ctx;
	const struct genl_info *info = genl_info_dump(cb);
	struct net_shaper_handle handle;
	struct net_shaper_info *shaper;
	struct net_device *dev;
	unsigned long index;
	int ret;

	ret = net_shaper_fetch_dev(info, &dev);
	if (ret)
		return ret;

	BUILD_BUG_ON(sizeof(struct net_shaper_nl_ctx) > sizeof(cb->ctx));

	/* Don't error out dumps performed before any set operation. */
	if (!dev->net_shaper_data) {
		ret = 0;
		goto put;
	}

	xa_for_each_range(&dev->net_shaper_data->shapers, index, shaper,
			  ctx->start_index, U32_MAX) {
		net_shaper_index_to_handle(index, &handle);
		ret = net_shaper_fill_one(skb, &handle, shaper, info);
		if (ret)
			goto put;

		ctx->start_index = index;
	}

put:
	netdev_put(dev, NULL);
	return ret;
}

int net_shaper_nl_set_doit(struct sk_buff *skb, struct genl_info *info)
{
	return -EOPNOTSUPP;
}

int net_shaper_nl_delete_doit(struct sk_buff *skb, struct genl_info *info)
{
	return -EOPNOTSUPP;
}

int net_shaper_nl_group_doit(struct sk_buff *skb, struct genl_info *info)
{
	return -EOPNOTSUPP;
}

void net_shaper_flush(struct net_device *dev)
{
	struct xarray *xa = net_shaper_cache_container(dev);
	struct net_shaper_info *cur;
	unsigned long index;

	if (!xa)
		return;

	xa_lock(xa);
	xa_for_each(xa, index, cur) {
		__xa_erase(xa, index);
		kfree(cur);
	}
	xa_unlock(xa);
	kfree(dev->net_shaper_data);
}

static int __init shaper_init(void)
{
	return genl_register_family(&net_shaper_nl_family);
}

subsys_initcall(shaper_init);
