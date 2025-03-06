// SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause)

#include <net/ultraeth/uet_context.h>
#include "uet_netlink.h"

#define MAX_CONTEXT_ID 256
static DECLARE_BITMAP(uet_context_ids, MAX_CONTEXT_ID);
static LIST_HEAD(uet_context_list);
static DEFINE_MUTEX(uet_context_lock);

static int uet_context_get_new_id(int id)
{
	if (WARN_ON(id < -1 || id >= MAX_CONTEXT_ID))
		return -EINVAL;

	mutex_lock(&uet_context_lock);
	if (id == -1)
		id = find_first_zero_bit(uet_context_ids, MAX_CONTEXT_ID);
	if (id < MAX_CONTEXT_ID) {
		if (test_and_set_bit(id, uet_context_ids))
			id = -EBUSY;
	} else {
		id = -ENOSPC;
	}
	mutex_unlock(&uet_context_lock);

	return id;
}

static void uet_context_put_id(struct uet_context *ctx)
{
	clear_bit(ctx->id, uet_context_ids);
}

static void uet_context_link(struct uet_context *ctx)
{
	WARN_ON(!list_empty(&ctx->list));
	list_add(&ctx->list, &uet_context_list);
}

static void uet_context_unlink(struct uet_context *ctx)
{
	list_del_init(&ctx->list);
	if (refcount_dec_and_test(&ctx->refcnt))
		return;

	mutex_unlock(&uet_context_lock);
	wait_event(ctx->refcnt_wait, refcount_read(&ctx->refcnt) == 0);
	mutex_lock(&uet_context_lock);
	WARN_ON(refcount_read(&ctx->refcnt) > 0);
}

static struct uet_context *uet_context_find(int id)
{
	struct uet_context *ctx;

	if (!test_bit(id, uet_context_ids))
		return NULL;

	list_for_each_entry(ctx, &uet_context_list, list)
		if (ctx->id == id)
			return ctx;

	return NULL;
}

struct uet_context *uet_context_get_by_id(int id)
{
	struct uet_context *ctx;

	mutex_lock(&uet_context_lock);
	ctx = uet_context_find(id);
	if (ctx)
		refcount_inc(&ctx->refcnt);
	mutex_unlock(&uet_context_lock);

	return ctx;
}

void uet_context_put(struct uet_context *ctx)
{
	if (refcount_dec_and_test(&ctx->refcnt))
		wake_up(&ctx->refcnt_wait);
}

int uet_context_create(int id)
{
	struct uet_context *ctx;
	int err = -ENOMEM;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return err;

	INIT_LIST_HEAD(&ctx->list);
	init_waitqueue_head(&ctx->refcnt_wait);
	refcount_set(&ctx->refcnt, 1);

	ctx->id = uet_context_get_new_id(id);
	if (ctx->id < 0) {
		err = ctx->id;
		goto ctx_id_err;
	}

	uet_context_link(ctx);

	return 0;

ctx_id_err:
	kfree(ctx);

	return err;
}

static void __uet_context_destroy(struct uet_context *ctx)
{
	uet_context_unlink(ctx);
	uet_context_put_id(ctx);
	kfree(ctx);
}

bool uet_context_destroy(int id)
{
	struct uet_context *ctx;
	bool found = false;

	mutex_lock(&uet_context_lock);
	ctx = uet_context_find(id);
	if (ctx) {
		__uet_context_destroy(ctx);
		found = true;
	}
	mutex_unlock(&uet_context_lock);

	return found;
}

void uet_context_destroy_all(void)
{
	struct uet_context *ctx;

	mutex_lock(&uet_context_lock);
	while ((ctx = list_first_entry_or_null(&uet_context_list,
						 struct uet_context,
						 list)))
		__uet_context_destroy(ctx);

	WARN_ON(!list_empty(&uet_context_list));
	mutex_unlock(&uet_context_lock);
}

static int __nl_ctx_fill_one(struct sk_buff *skb,
				const struct uet_context *ctx,
				int cmd, u32 flags, u32 seq, u32 portid)
{
	void *hdr;

	hdr = genlmsg_put(skb, portid, seq, &ultraeth_nl_family, flags, cmd);
	if (!hdr)
		return -EMSGSIZE;

	if (nla_put_s32(skb, ULTRAETH_A_CONTEXT_ID, ctx->id))
		goto out_err;

	genlmsg_end(skb, hdr);
	return 0;

out_err:
	genlmsg_cancel(skb, hdr);
	return -EMSGSIZE;
}

int ultraeth_nl_context_get_dumpit(struct sk_buff *skb, struct netlink_callback *cb)
{
	int idx = 0, s_idx = cb->args[0], err;
	struct uet_context *ctx;

	mutex_lock(&uet_context_lock);
	list_for_each_entry(ctx, &uet_context_list, list) {
		if (idx < s_idx) {
			idx++;
			continue;
		}
		err = __nl_ctx_fill_one(skb, ctx, ULTRAETH_CMD_CONTEXT_GET,
					  NLM_F_MULTI, cb->nlh->nlmsg_seq,
					  NETLINK_CB(cb->skb).portid);
		if (err)
			break;
		idx++;
	}
	cb->args[0] = idx;
	mutex_unlock(&uet_context_lock);

	return err ? err : skb->len;
}

int ultraeth_nl_context_new_doit(struct sk_buff *skb, struct genl_info *info)
{
	int id = -1;

	if (info->attrs[ULTRAETH_A_CONTEXT_ID])
		id = nla_get_s32(info->attrs[ULTRAETH_A_CONTEXT_ID]);

	return uet_context_create(id);
}

int ultraeth_nl_context_del_doit(struct sk_buff *skb, struct genl_info *info)
{
	bool destroyed = false;
	int id;

	if (!info->attrs[ULTRAETH_A_CONTEXT_ID]) {
		NL_SET_ERR_MSG(info->extack, "UET context id must be specified");
		return -EINVAL;
	}

	id = nla_get_s32(info->attrs[ULTRAETH_A_CONTEXT_ID]);
	destroyed = uet_context_destroy(id);

	return destroyed ? 0 : -ENOENT;
}
