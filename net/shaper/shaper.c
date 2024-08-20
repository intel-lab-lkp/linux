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

	/* Serialize write ops and protects node_ids updates. */
	struct mutex lock;
	struct idr node_ids;
};

struct net_shaper_nl_ctx {
	u32 start_index;
};

/* Count the number of [multi] attributes of the given type. */
static int net_shaper_list_len(struct genl_info *info, int type)
{
	struct nlattr *attr;
	int rem, cnt = 0;

	nla_for_each_attr_type(attr, type, genlmsg_data(info->genlhdr),
			       genlmsg_len(info->genlhdr), rem)
		cnt++;
	return cnt;
}

static int net_shaper_handle_size(void)
{
	return nla_total_size(nla_total_size(sizeof(u32)) +
			      nla_total_size(sizeof(u32)));
}

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
static int net_shaper_fetch_dev(const struct genl_info *info, int type,
				struct net_device **pdev)
{
	struct net *ns = genl_info_net(info);
	struct net_device *dev;
	int ifindex;

	if (GENL_REQ_ATTR_CHECK(info, type))
		return -EINVAL;

	ifindex = nla_get_u32(info->attrs[type]);
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

static void net_shaper_default_parent(const struct net_shaper_handle *handle,
				      struct net_shaper_handle *parent)
{
	switch (handle->scope) {
	case NET_SHAPER_SCOPE_UNSPEC:
	case NET_SHAPER_SCOPE_NETDEV:
	case __NET_SHAPER_SCOPE_MAX:
		parent->scope = NET_SHAPER_SCOPE_UNSPEC;
		break;

	case NET_SHAPER_SCOPE_QUEUE:
	case NET_SHAPER_SCOPE_NODE:
		parent->scope = NET_SHAPER_SCOPE_NETDEV;
		break;
	}
	parent->id = 0;
}

#define NET_SHAPER_CACHE_NOT_VALID XA_MARK_0

static struct xarray *net_shaper_cache_container(struct net_device *dev)
{
	/* The barrier pairs with cmpxchg on init. */
	struct net_shaper_data *data = READ_ONCE(dev->net_shaper_data);

	return data ? &data->shapers : NULL;
}

static struct mutex *net_shaper_cache_lock(struct net_device *dev)
{
	return dev->net_shaper_data ? &dev->net_shaper_data->lock : NULL;
}

/* Lookup the given shaper inside the cache. */
static struct net_shaper_info *
net_shaper_cache_lookup(struct net_device *dev,
			const struct net_shaper_handle *handle)
{
	struct xarray *xa = net_shaper_cache_container(dev);
	u32 index = net_shaper_handle_to_index(handle);

	if (!xa || xa_get_mark(xa, index, NET_SHAPER_CACHE_NOT_VALID))
		return NULL;

	return xa_load(xa, index);
}

/* Allocate on demand the per device shaper's cache. */
static struct mutex *net_shaper_cache_init(struct net_device *dev,
					   struct netlink_ext_ack *extack)
{
	struct net_shaper_data *new, *data = READ_ONCE(dev->net_shaper_data);

	if (!data) {
		new = kmalloc(sizeof(*dev->net_shaper_data), GFP_KERNEL);
		if (!new) {
			NL_SET_ERR_MSG(extack, "Can't allocate memory for shaper data");
			return NULL;
		}

		mutex_init(&new->lock);
		xa_init(&new->shapers);
		idr_init(&new->node_ids);

		/* No lock acquired yet, we can race with other operations. */
		data = cmpxchg(&dev->net_shaper_data, NULL, new);
		if (!data)
			data = new;
		else
			kfree(new);
	}
	return &data->lock;
}

/* Prepare the cache to actually insert the given shaper, doing
 * in advance the needed allocations.
 */
static int net_shaper_cache_pre_insert(struct net_device *dev,
				       struct net_shaper_handle *handle,
				       struct netlink_ext_ack *extack)
{
	struct xarray *xa = net_shaper_cache_container(dev);
	struct net_shaper_info *prev, *cur;
	bool id_allocated = false;
	int ret, id, index;

	if (!xa)
		return -ENOMEM;

	index = net_shaper_handle_to_index(handle);
	cur = xa_load(xa, index);
	if (cur)
		return 0;

	/* Allocated a new id, if needed. */
	if (handle->scope == NET_SHAPER_SCOPE_NODE &&
	    handle->id == NET_SHAPER_ID_UNSPEC) {
		id = idr_alloc(&dev->net_shaper_data->node_ids, NULL,
			       0, NET_SHAPER_ID_UNSPEC, GFP_ATOMIC);

		if (id < 0) {
			NL_SET_ERR_MSG(extack, "Can't allocate new id for NODE shaper");
			return id;
		}

		handle->id = id;
		index = net_shaper_handle_to_index(handle);
		id_allocated = true;
	}

	cur = kmalloc(sizeof(*cur), GFP_KERNEL | __GFP_ZERO);
	if (!cur) {
		NL_SET_ERR_MSG(extack, "Can't allocate memory for cached shaper");
		ret = -ENOMEM;
		goto free_id;
	}

	/* Mark 'tentative' shaper inside the cache. */
	xa_lock(xa);
	prev = __xa_store(xa, index, cur, GFP_KERNEL);
	__xa_set_mark(xa, index, NET_SHAPER_CACHE_NOT_VALID);
	xa_unlock(xa);
	if (xa_err(prev)) {
		NL_SET_ERR_MSG(extack, "Can't insert shaper into cache");
		kfree(cur);
		ret = xa_err(prev);
		goto free_id;
	}
	return 0;

free_id:
	if (id_allocated)
		idr_remove(&dev->net_shaper_data->node_ids, handle->id);
	return ret;
}

/* Commit the tentative insert with the actual values.
 * Must be called only after a successful net_shaper_pre_insert().
 */
static void net_shaper_cache_commit(struct net_device *dev, int nr_shapers,
				    const struct net_shaper_handle *handle,
				    const struct net_shaper_info *shapers)
{
	struct xarray *xa = net_shaper_cache_container(dev);
	struct net_shaper_info *cur;
	int index;
	int i;

	xa_lock(xa);
	for (i = 0; i < nr_shapers; ++i) {
		index = net_shaper_handle_to_index(&handle[i]);

		cur = xa_load(xa, index);
		if (WARN_ON_ONCE(!cur))
			continue;

		/* Successful update: drop the tentative mark
		 * and update the cache.
		 */
		__xa_clear_mark(xa, index, NET_SHAPER_CACHE_NOT_VALID);
		*cur = shapers[i];
	}
	xa_unlock(xa);
}

/* Rollback all the tentative inserts from the shaper cache. */
static void net_shaper_cache_rollback(struct net_device *dev)
{
	struct xarray *xa = net_shaper_cache_container(dev);
	struct net_shaper_handle handle;
	struct net_shaper_info *cur;
	unsigned long index;

	if (!xa)
		return;

	xa_lock(xa);
	xa_for_each_marked(xa, index, cur, NET_SHAPER_CACHE_NOT_VALID) {
		net_shaper_index_to_handle(index, &handle);
		if (handle.scope == NET_SHAPER_SCOPE_NODE)
			idr_remove(&dev->net_shaper_data->node_ids, handle.id);
		__xa_erase(xa, index);
		kfree(cur);
	}
	xa_unlock(xa);
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

static int net_shaper_parse_info(struct net_device *dev, struct nlattr **tb,
				 const struct genl_info *info,
				 struct net_shaper_handle *handle,
				 struct net_shaper_info *shaper)
{
	struct net_shaper_info *old;
	int ret;

	/* The shaper handle is the only mandatory attribute. */
	if (NL_REQ_ATTR_CHECK(info->extack, NULL, tb, NET_SHAPER_A_HANDLE))
		return -EINVAL;

	ret = net_shaper_parse_handle(tb[NET_SHAPER_A_HANDLE], info, handle);
	if (ret)
		return ret;

	/* Fetch existing data, if any, so that user provide info will
	 * incrementally update the existing shaper configuration.
	 */
	old = net_shaper_cache_lookup(dev, handle);
	if (old)
		*shaper = *old;
	else
		net_shaper_default_parent(handle, &shaper->parent);

	if (tb[NET_SHAPER_A_METRIC])
		shaper->metric = nla_get_u32(tb[NET_SHAPER_A_METRIC]);

	if (tb[NET_SHAPER_A_BW_MIN])
		shaper->bw_min = nla_get_uint(tb[NET_SHAPER_A_BW_MIN]);

	if (tb[NET_SHAPER_A_BW_MAX])
		shaper->bw_max = nla_get_uint(tb[NET_SHAPER_A_BW_MAX]);

	if (tb[NET_SHAPER_A_BURST])
		shaper->burst = nla_get_uint(tb[NET_SHAPER_A_BURST]);

	if (tb[NET_SHAPER_A_PRIORITY])
		shaper->priority = nla_get_u32(tb[NET_SHAPER_A_PRIORITY]);

	if (tb[NET_SHAPER_A_WEIGHT])
		shaper->weight = nla_get_u32(tb[NET_SHAPER_A_WEIGHT]);
	return 0;
}

/* Fetch the cached shaper info and update them with the user-provided
 * attributes.
 */
static int net_shaper_parse_info_nest(struct net_device *dev,
				      const struct nlattr *attr,
				      const struct genl_info *info,
				      struct net_shaper_handle *handle,
				      struct net_shaper_info *shaper)
{
	struct nlattr *tb[NET_SHAPER_A_WEIGHT + 1];
	int ret;

	ret = nla_parse_nested(tb, NET_SHAPER_A_WEIGHT, attr,
			       net_shaper_info_nl_policy, info->extack);
	if (ret < 0)
		return ret;

	return net_shaper_parse_info(dev, tb, info, handle, shaper);
}

/* Alike net_parse_shaper_info(), but additionally allow the user specifying
 * the shaper's parent handle.
 */
static int net_shaper_parse_root(struct net_device *dev,
				 const struct nlattr *attr,
				 const struct genl_info *info,
				 struct net_shaper_handle *handle,
				 struct net_shaper_info *shaper)
{
	struct nlattr *tb[NET_SHAPER_A_PARENT + 1];
	int ret;

	ret = nla_parse_nested(tb, NET_SHAPER_A_PARENT, attr,
			       net_shaper_root_info_nl_policy,
			       info->extack);
	if (ret < 0)
		return ret;

	ret = net_shaper_parse_info(dev, tb, info, handle, shaper);
	if (ret)
		return ret;

	if (tb[NET_SHAPER_A_PARENT]) {
		ret = net_shaper_parse_handle(tb[NET_SHAPER_A_PARENT], info,
					      &shaper->parent);
		if (ret)
			return ret;
	}
	return 0;
}

int net_shaper_nl_pre_doit(const struct genl_split_ops *ops,
			   struct sk_buff *skb, struct genl_info *info)
{
	struct net_device *dev;
	int ret;

	ret = net_shaper_fetch_dev(info, NET_SHAPER_A_IFINDEX, &dev);
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

int net_shaper_nl_cap_pre_doit(const struct genl_split_ops *ops,
			       struct sk_buff *skb, struct genl_info *info)
{
	struct net_device *dev;
	int ret;

	ret = net_shaper_fetch_dev(info, NET_SHAPER_A_CAPABILITIES_IFINDEX, &dev);
	if (ret)
		return ret;

	info->user_ptr[0] = dev;
	return 0;
}

void net_shaper_nl_cap_post_doit(const struct genl_split_ops *ops,
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

	ret = net_shaper_fetch_dev(info, NET_SHAPER_A_IFINDEX, &dev);
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

/* Update the H/W and on success update the local cache, too. */
static int net_shaper_set(struct net_device *dev,
			  const struct net_shaper_handle *h,
			  const struct net_shaper_info *shaper,
			  struct netlink_ext_ack *extack)
{
	struct mutex *lock = net_shaper_cache_init(dev, extack);
	struct net_shaper_handle handle = *h;
	int ret;

	if (!lock)
		return -ENOMEM;

	if (handle.scope == NET_SHAPER_SCOPE_UNSPEC) {
		NL_SET_ERR_MSG_FMT(extack, "Can't set shaper with unspec scope");
		return -EINVAL;
	}

	mutex_lock(lock);
	if (handle.scope == NET_SHAPER_SCOPE_NODE &&
	    net_shaper_cache_lookup(dev, &handle)) {
		ret = -ENOENT;
		goto unlock;
	}

	ret = net_shaper_cache_pre_insert(dev, &handle, extack);
	if (ret)
		goto unlock;

	ret = dev->netdev_ops->net_shaper_ops->set(dev, &handle, shaper, extack);
	net_shaper_cache_commit(dev, 1, &handle, shaper);

unlock:
	mutex_unlock(lock);
	return ret;
}

int net_shaper_nl_set_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct net_device *dev = info->user_ptr[0];
	struct net_shaper_handle handle;
	struct net_shaper_info shaper;
	struct nlattr *attr;
	int ret;

	if (GENL_REQ_ATTR_CHECK(info, NET_SHAPER_A_SHAPER))
		return -EINVAL;

	attr = info->attrs[NET_SHAPER_A_SHAPER];
	ret = net_shaper_parse_info_nest(dev, attr, info, &handle, &shaper);
	if (ret)
		return ret;

	return net_shaper_set(dev, &handle, &shaper, info->extack);
}

static int __net_shaper_delete(struct net_device *dev,
			       const struct net_shaper_handle *h,
			       struct net_shaper_info *shaper,
			       struct netlink_ext_ack *extack)
{
	struct net_shaper_handle parent_handle, handle = *h;
	struct xarray *xa = net_shaper_cache_container(dev);
	int ret;

	/* Should never happen: we are under the cache lock, the cache
	 * is already initialized.
	 */
	if (WARN_ON_ONCE(!xa))
		return -EINVAL;

again:
	parent_handle = shaper->parent;

	ret = dev->netdev_ops->net_shaper_ops->delete(dev, &handle, extack);
	if (ret < 0)
		return ret;

	xa_erase(xa, net_shaper_handle_to_index(&handle));
	if (handle.scope == NET_SHAPER_SCOPE_NODE)
		idr_remove(&dev->net_shaper_data->node_ids, handle.id);
	kfree(shaper);

	/* Eventually delete the parent, if it is left over with no leaves. */
	if (parent_handle.scope == NET_SHAPER_SCOPE_NODE) {
		shaper = net_shaper_cache_lookup(dev, &parent_handle);
		if (shaper && !--shaper->leaves) {
			handle = parent_handle;
			goto again;
		}
	}
	return 0;
}

static int __net_shaper_group(struct net_device *dev,
			      bool cache_root, int leaves_count,
			      const struct net_shaper_handle *leaves_handles,
			      struct net_shaper_info *leaves,
			      struct net_shaper_handle *root_handle,
			      struct net_shaper_info *root,
			      struct netlink_ext_ack *extack)
{
	struct net_shaper_info *parent = NULL;
	struct net_shaper_handle leaf_handle;
	int i, ret;

	if (root_handle->scope == NET_SHAPER_SCOPE_NODE) {
		if (root_handle->id != NET_SHAPER_ID_UNSPEC &&
		    !net_shaper_cache_lookup(dev, root_handle)) {
			NL_SET_ERR_MSG_FMT(extack, "Root shaper %d:%d does not exists",
					   root_handle->scope, root_handle->id);
			return -ENOENT;
		}
		if (root->parent.scope != NET_SHAPER_SCOPE_NODE &&
		    root->parent.scope != NET_SHAPER_SCOPE_NETDEV) {
			NL_SET_ERR_MSG_FMT(extack, "Invalid scope %d for root parent shaper",
					   root->parent.scope);
			return -EINVAL;
		}
	}

	if (root->parent.scope == NET_SHAPER_SCOPE_NODE) {
		parent = net_shaper_cache_lookup(dev, &root->parent);
		if (!parent) {
			NL_SET_ERR_MSG_FMT(extack, "Root parent shaper %d:%d does not exists",
					   root->parent.scope, root->parent.id);
			return -ENOENT;
		}
	}

	if (cache_root) {
		/* For newly created node scope shaper, the following will
		 * update the handle, due to id allocation.
		 */
		ret = net_shaper_cache_pre_insert(dev, root_handle, extack);
		if (ret)
			return ret;
	}

	for (i = 0; i < leaves_count; ++i) {
		leaf_handle = leaves_handles[i];
		if (leaf_handle.scope != NET_SHAPER_SCOPE_QUEUE) {
			ret = -EINVAL;
			NL_SET_ERR_MSG_FMT(extack, "Invalid scope %d for leaf shaper %d",
					   leaf_handle.scope, i);
			goto rollback;
		}

		ret = net_shaper_cache_pre_insert(dev, &leaf_handle, extack);
		if (ret)
			goto rollback;

		if (leaves[i].parent.scope == root_handle->scope &&
		    leaves[i].parent.id == root_handle->id)
			continue;

		/* The leaves shapers will be nested to the root, update the
		 * linking accordingly.
		 */
		leaves[i].parent = *root_handle;
		root->leaves++;
	}

	ret = dev->netdev_ops->net_shaper_ops->group(dev, leaves_count,
						     leaves_handles, leaves,
						     root_handle, root,
						     extack);
	if (ret < 0)
		goto rollback;

	if (parent)
		parent->leaves++;
	if (cache_root)
		net_shaper_cache_commit(dev, 1, root_handle, root);
	net_shaper_cache_commit(dev, leaves_count, leaves_handles, leaves);
	return 0;

rollback:
	net_shaper_cache_rollback(dev);
	return ret;
}

static int __net_shaper_pre_del_node(struct net_device *dev,
				     const struct net_shaper_handle *handle,
				     const struct net_shaper_info *shaper,
				     struct netlink_ext_ack *extack)
{
	struct net_shaper_handle *leaves_handles, root_handle;
	struct xarray *xa = net_shaper_cache_container(dev);
	struct net_shaper_info *cur, *leaves, root = {};
	int ret, leaves_count = 0;
	unsigned long index;
	bool cache_root;

	if (!shaper->leaves)
		return 0;

	if (WARN_ON_ONCE(!xa))
		return -EINVAL;

	/* Fetch the new root information. */
	root_handle = shaper->parent;
	cur = net_shaper_cache_lookup(dev, &root_handle);
	if (cur) {
		root = *cur;
	} else {
		/* A scope NODE shaper can be nested only to the NETDEV scope
		 * shaper without creating the latter, this check may fail only
		 * if the cache is in inconsistent status.
		 */
		if (WARN_ON_ONCE(root_handle.scope != NET_SHAPER_SCOPE_NETDEV))
			return -EINVAL;
	}

	leaves = kcalloc(shaper->leaves,
			 sizeof(struct net_shaper_info) +
			 sizeof(struct net_shaper_handle), GFP_KERNEL);
	if (!leaves)
		return -ENOMEM;

	leaves_handles = (struct net_shaper_handle *)&leaves[shaper->leaves];

	/* Build the leaves arrays. */
	xa_for_each(xa, index, cur) {
		if (cur->parent.scope != handle->scope ||
		    cur->parent.id != handle->id)
			continue;

		if (WARN_ON_ONCE(leaves_count == shaper->leaves)) {
			ret = -EINVAL;
			goto free;
		}

		net_shaper_index_to_handle(index,
					   &leaves_handles[leaves_count]);
		leaves[leaves_count++] = *cur;
	}

	/* When re-linking to the netdev shaper, avoid the eventual, implicit,
	 * creation of the new root, would be surprising since the user is
	 * doing a delete operation.
	 */
	cache_root = root_handle.scope != NET_SHAPER_SCOPE_NETDEV;
	ret = __net_shaper_group(dev, cache_root, leaves_count, leaves_handles,
				 leaves, &root_handle, &root, extack);

free:
	kfree(leaves);
	return ret;
}

static int net_shaper_delete(struct net_device *dev,
			     const struct net_shaper_handle *handle,
			     struct netlink_ext_ack *extack)
{
	struct mutex *lock = net_shaper_cache_lock(dev);
	struct net_shaper_info *shaper;
	int ret;

	/* The lock is null when the cache is not initialized, and thus
	 * no shaper has been created yet.
	 */
	if (!lock)
		return -ENOENT;

	mutex_lock(lock);
	shaper = net_shaper_cache_lookup(dev, handle);
	if (!shaper) {
		ret = -ENOENT;
		goto unlock;
	}

	if (handle->scope == NET_SHAPER_SCOPE_NODE) {
		ret = __net_shaper_pre_del_node(dev, handle, shaper, extack);
		if (ret)
			goto unlock;
	}

	ret = __net_shaper_delete(dev, handle, shaper, extack);

unlock:
	mutex_unlock(lock);
	return ret;
}

int net_shaper_nl_delete_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct net_device *dev = info->user_ptr[0];
	struct net_shaper_handle handle;
	int ret;

	if (GENL_REQ_ATTR_CHECK(info, NET_SHAPER_A_HANDLE))
		return -EINVAL;

	ret = net_shaper_parse_handle(info->attrs[NET_SHAPER_A_HANDLE], info,
				      &handle);
	if (ret)
		return ret;

	return net_shaper_delete(dev, &handle, info->extack);
}

/* Update the H/W and on success update the local cache, too */
static int net_shaper_group(struct net_device *dev, int leaves_count,
			    const struct net_shaper_handle *leaves_handles,
			    struct net_shaper_info *leaves,
			    struct net_shaper_handle *root_handle,
			    struct net_shaper_info *root,
			    struct netlink_ext_ack *extack)
{
	struct mutex *lock = net_shaper_cache_init(dev, extack);
	struct net_shaper_handle *old_roots;
	int i, ret, old_roots_count = 0;

	if (!lock)
		return -ENOMEM;

	if (root_handle->scope != NET_SHAPER_SCOPE_NODE &&
	    root_handle->scope != NET_SHAPER_SCOPE_NETDEV) {
		NL_SET_ERR_MSG_FMT(extack, "Invalid scope %d for root shaper",
				   root_handle->scope);
		return -EINVAL;
	}

	old_roots = kcalloc(leaves_count, sizeof(struct net_shaper_handle),
			    GFP_KERNEL);
	if (!old_roots)
		return -ENOMEM;

	for (i = 0; i < leaves_count; i++)
		if (leaves[i].parent.scope == NET_SHAPER_SCOPE_NODE &&
		    (leaves[i].parent.scope != root_handle->scope ||
		     leaves[i].parent.id != root_handle->id))
			old_roots[old_roots_count++] = leaves[i].parent;

	mutex_lock(lock);
	ret = __net_shaper_group(dev, true, leaves_count, leaves_handles,
				 leaves, root_handle, root, extack);

	/* Check if we need to delete any NODE left alone by the new leaves
	 * linkage.
	 */
	for (i = 0; i < old_roots_count; ++i) {
		root = net_shaper_cache_lookup(dev, &old_roots[i]);
		if (!root)
			continue;

		if (--root->leaves > 0)
			continue;

		/* Errors here are not fatal: the grouping operation is
		 * completed, and user-space can still explicitly clean-up
		 * left-over nodes.
		 */
		__net_shaper_delete(dev, &old_roots[i], root, extack);
	}

	mutex_unlock(lock);

	kfree(old_roots);
	return ret;
}

static int net_shaper_group_send_reply(struct genl_info *info,
				       struct net_shaper_handle *handle)
{
	struct net_device *dev = info->user_ptr[0];
	struct nlattr *handle_attr;
	struct sk_buff *msg;
	int ret = -EMSGSIZE;
	void *hdr;

	/* Prepare the msg reply in advance, to avoid device operation
	 * rollback.
	 */
	msg = genlmsg_new(net_shaper_handle_size(), GFP_KERNEL);
	if (!msg)
		return ret;

	hdr = genlmsg_iput(msg, info);
	if (!hdr)
		goto free_msg;

	if (nla_put_u32(msg, NET_SHAPER_A_IFINDEX, dev->ifindex))
		goto free_msg;

	handle_attr = nla_nest_start(msg, NET_SHAPER_A_HANDLE);
	if (!handle_attr)
		goto free_msg;

	if (nla_put_u32(msg, NET_SHAPER_A_SCOPE, handle->scope))
		goto free_msg;

	if (nla_put_u32(msg, NET_SHAPER_A_ID, handle->id))
		goto free_msg;

	nla_nest_end(msg, handle_attr);
	genlmsg_end(msg, hdr);

	ret = genlmsg_reply(msg, info);
	if (ret)
		goto free_msg;

	return ret;

free_msg:
	nlmsg_free(msg);
	return ret;
}

int net_shaper_nl_group_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct net_shaper_handle *leaves_handles, root_handle;
	struct net_device *dev = info->user_ptr[0];
	struct net_shaper_info *leaves, root;
	int i, ret, rem, leaves_count;
	struct nlattr *attr;

	if (GENL_REQ_ATTR_CHECK(info, NET_SHAPER_A_LEAVES) ||
	    GENL_REQ_ATTR_CHECK(info, NET_SHAPER_A_ROOT))
		return -EINVAL;

	leaves_count = net_shaper_list_len(info, NET_SHAPER_A_LEAVES);
	leaves = kcalloc(leaves_count, sizeof(struct net_shaper_info) +
			 sizeof(struct net_shaper_handle), GFP_KERNEL);
	if (!leaves) {
		GENL_SET_ERR_MSG_FMT(info, "Can't allocate memory for %d leaves shapers",
				     leaves_count);
		return -ENOMEM;
	}
	leaves_handles = (struct net_shaper_handle *)&leaves[leaves_count];

	ret = net_shaper_parse_root(dev, info->attrs[NET_SHAPER_A_ROOT],
				    info, &root_handle, &root);
	if (ret)
		goto free_shapers;

	i = 0;
	nla_for_each_attr_type(attr, NET_SHAPER_A_LEAVES,
			       genlmsg_data(info->genlhdr),
			       genlmsg_len(info->genlhdr), rem) {
		if (WARN_ON_ONCE(i >= leaves_count))
			goto free_shapers;

		ret = net_shaper_parse_info_nest(dev, attr, info,
						 &leaves_handles[i],
						 &leaves[i]);
		if (ret)
			goto free_shapers;
		i++;
	}

	ret = net_shaper_group(dev, leaves_count, leaves_handles, leaves,
			       &root_handle, &root, info->extack);
	if (ret < 0)
		goto free_shapers;

	ret = net_shaper_group_send_reply(info, &root_handle);
	if (ret) {
		/* Error on reply is not fatal to avoid rollback a successful
		 * configuration.
		 */
		GENL_SET_ERR_MSG_FMT(info, "Can't send reply %d", ret);
		ret = 0;
	}

free_shapers:
	kfree(leaves);
	return ret;
}

static int
net_shaper_cap_fill_one(struct sk_buff *msg, int ifindex,
			enum net_shaper_scope scope, unsigned long flags,
			const struct genl_info *info)
{
	unsigned long cur;
	void *hdr;

	hdr = genlmsg_iput(msg, info);
	if (!hdr)
		return -EMSGSIZE;

	if (nla_put_u32(msg, NET_SHAPER_A_CAPABILITIES_IFINDEX, ifindex) ||
	    nla_put_u32(msg, NET_SHAPER_A_CAPABILITIES_SCOPE, scope))
		goto nla_put_failure;

	for (cur = NET_SHAPER_A_CAPABILITIES_SUPPORT_METRIC_BPS;
	     cur <= NET_SHAPER_A_CAPABILITIES_MAX; ++cur) {
		if (flags & BIT(cur) && nla_put_flag(msg, cur))
			goto nla_put_failure;
	}

	genlmsg_end(msg, hdr);

	return 0;

nla_put_failure:
	genlmsg_cancel(msg, hdr);
	return -EMSGSIZE;
}

int net_shaper_nl_cap_get_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct net_device *dev = info->user_ptr[0];
	const struct net_shaper_ops *ops;
	enum net_shaper_scope scope;
	struct sk_buff *msg;
	unsigned long flags;
	int ret;

	if (GENL_REQ_ATTR_CHECK(info, NET_SHAPER_A_CAPABILITIES_SCOPE))
		return -EINVAL;

	scope = nla_get_u32(info->attrs[NET_SHAPER_A_CAPABILITIES_SCOPE]);
	ops = dev->netdev_ops->net_shaper_ops;
	ret = ops->capabilities(dev, scope, &flags);
	if (ret)
		return ret;

	msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	ret = net_shaper_cap_fill_one(msg, dev->ifindex, scope, flags, info);
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

int net_shaper_nl_cap_get_dumpit(struct sk_buff *skb,
				 struct netlink_callback *cb)
{
	const struct genl_info *info = genl_info_dump(cb);
	const struct net_shaper_ops *ops;
	enum net_shaper_scope scope;
	struct net_device *dev;
	unsigned long flags;
	int ret;

	ret = net_shaper_fetch_dev(info, NET_SHAPER_A_CAPABILITIES_IFINDEX, &dev);
	if (ret)
		return ret;

	ops = dev->netdev_ops->net_shaper_ops;
	for (scope = 0; scope <= NET_SHAPER_SCOPE_MAX; ++scope) {
		if (ops->capabilities(dev, scope, &flags))
			continue;

		ret = net_shaper_cap_fill_one(skb, dev->ifindex, scope, flags,
					      info);
		if (ret)
			goto put;
	}

put:
	netdev_put(dev, NULL);
	return ret;
}

void net_shaper_flush(struct net_device *dev)
{
	struct xarray *xa = net_shaper_cache_container(dev);
	struct mutex *lock = net_shaper_cache_lock(dev);
	struct net_shaper_info *cur;
	unsigned long index;

	if (!xa || !lock)
		return;

	mutex_lock(lock);
	xa_lock(xa);
	xa_for_each(xa, index, cur) {
		__xa_erase(xa, index);
		kfree(cur);
	}
	xa_unlock(xa);
	idr_destroy(&dev->net_shaper_data->node_ids);
	mutex_unlock(lock);

	kfree(dev->net_shaper_data);
}

static int __init shaper_init(void)
{
	return genl_register_family(&net_shaper_nl_family);
}

subsys_initcall(shaper_init);
