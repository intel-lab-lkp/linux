// SPDX-License-Identifier: GPL-2.0-only
/* Included by nfnetlink_queue.c for bounded packet-ID and batch tests. */
#include <kunit/test.h>
#include <linux/bits.h>
#include <linux/err.h>
#include <linux/net.h>
#include <linux/sched.h>
#include <linux/user_namespace.h>
#include <net/net_namespace.h>

#define NFQNL_TEST_QUEUE 65530
#define NFQNL_TEST_PORTID 0x1234abcd
#define NFQNL_TEST_MARK 0x12345678U
#define NFQNL_TEST_PRIORITY 0x87654321
#define NFQNL_TEST_MAX_ENTRIES 4
#define NFQNL_TEST_DROPPED 7
#define NFQNL_TEST_USER_DROPPED 11

struct nfqnl_test_result {
	u64 sequence;
	u32 id;
	u32 mark;
	u32 priority;
};

struct nfqnl_test_ctx {
	struct net *net;
	struct nfqnl_instance *queue;
	struct nfqnl_test_result freed[NFQNL_TEST_MAX_ENTRIES];
	unsigned int freed_count;
	struct socket *socket;
	bool hook_registered;
	unsigned int accepted_count;
	u64 accepted_sequence;
};

struct nfqnl_test_skb_cb {
	struct nfqnl_test_ctx *ctx;
	u64 sequence;
	u32 id;
};

struct nfqnl_batch_case {
	const char *name;
	u32 ids[NFQNL_TEST_MAX_ENTRIES];
	u32 boundary;
	unsigned int count;
	unsigned int selected;
	unsigned int remove_mask;
	u64 sequence_offset;
	u64 issued_advance;
	int error;
};

static const struct nfqnl_batch_case nfqnl_batch_cases[] = {
	{
		.name = "empty",
		.boundary = 42,
	},
	{
		.name = "ordinary_prefix",
		.ids = { 10, 11, 12 },
		.boundary = 11,
		.count = 3,
		.selected = 2,
	},
	{
		.name = "ordinary_wrap",
		.ids = { 0xfffffffe, 0xffffffff, 0, 1 },
		.boundary = 0,
		.count = 4,
		.selected = 3,
	},
	{
		.name = "zero_boundary",
		.ids = { 0, 1 },
		.boundary = 0,
		.count = 2,
		.selected = 1,
	},
	{
		.name = "small_gap",
		.ids = { 10, 15, 18 },
		.boundary = 15,
		.count = 3,
		.selected = 2,
	},
	{
		.name = "removed_boundary",
		.ids = { 10, 12, 14 },
		.boundary = 13,
		.count = 3,
		.selected = 2,
	},
	{
		.name = "removed_wrap_boundary",
		.ids = { 0xfffffffe, 0, 2 },
		.boundary = 1,
		.count = 3,
		.selected = 2,
	},
	{
		.name = "before_all",
		.ids = { 10, 12 },
		.boundary = 9,
		.count = 2,
	},
	{
		.name = "after_all",
		.ids = { 10, 12 },
		.boundary = 13,
		.count = 2,
		.selected = 2,
	},
	{
		.name = "old_head_below_half",
		.ids = { 1, 0x80000000, 0x80000001 },
		.boundary = 0x80000000,
		.count = 3,
		.selected = 2,
	},
	{
		.name = "old_head_at_half",
		.ids = { 1, 0x80000001, 0x80000002 },
		.boundary = 0x80000001,
		.count = 3,
		.selected = 2,
	},
	{
		.name = "old_head_over_half",
		.ids = { 1, 0x80000002, 0x80000003 },
		.boundary = 0x80000002,
		.count = 3,
		.selected = 2,
	},
	{
		.name = "new_tail_below_half",
		.ids = { 1, 0x80000000 },
		.boundary = 1,
		.count = 2,
		.selected = 1,
	},
	{
		.name = "new_tail_at_half",
		.ids = { 1, 0x80000001 },
		.boundary = 1,
		.count = 2,
		.selected = 1,
	},
	{
		.name = "new_tail_over_half",
		.ids = { 1, 0x80000002 },
		.boundary = 1,
		.count = 2,
		.selected = 1,
	},
	{
		.name = "removed_early_large_span",
		.ids = { 1, 2, 0x80000003 },
		.boundary = 2,
		.count = 3,
		.selected = 1,
		.remove_mask = BIT(1),
	},
	{
		.name = "removed_late_large_span",
		.ids = { 1, 0x80000002, 0x80000003 },
		.boundary = 0x80000002,
		.count = 3,
		.selected = 1,
		.remove_mask = BIT(1),
	},
	{
		.name = "missing_inside_large_span",
		.ids = { 1, 0x80000003 },
		.boundary = 0x80000001,
		.count = 2,
		.selected = 1,
	},
	{
		.name = "missing_before_large_span",
		.ids = { 1, 0x80000003 },
		.boundary = 0,
		.count = 2,
		.error = -ERANGE,
	},
	{
		.name = "missing_after_large_span",
		.ids = { 1, 0x80000003 },
		.boundary = 0x80000004,
		.count = 2,
		.error = -ERANGE,
	},
	{
		.name = "missing_half_from_head",
		.ids = { 1, 2 },
		.boundary = 0x80000001,
		.count = 2,
		.error = -ERANGE,
	},
	{
		.name = "missing_half_from_issued",
		.ids = { 1, 2 },
		.boundary = 0x80000002,
		.count = 2,
		.error = -ERANGE,
	},
	{
		.name = "missing_future_below_half",
		.ids = { 1, 2 },
		.boundary = 0x80000000,
		.count = 2,
		.selected = 2,
	},
	{
		.name = "missing_past_below_half",
		.ids = { 1, 2 },
		.boundary = 0x80000003,
		.count = 2,
	},
	{
		.name = "removed_wrap_large_span",
		.ids = { 0xfffffffe, 1, 0x80000002 },
		.boundary = 1,
		.count = 3,
		.selected = 1,
		.remove_mask = BIT(1),
	},
	{
		.name = "missing_full_cycle",
		.ids = { 1, 0x80000002, 2 },
		.boundary = 0,
		.count = 3,
		.error = -ERANGE,
	},
	{
		.name = "present_full_cycle",
		.ids = { 1, 0x80000002, 2 },
		.boundary = 1,
		.count = 3,
		.selected = 1,
	},
	{
		.name = "maximum_missing_window",
		.ids = { 1, 3 },
		.boundary = 0,
		.count = 2,
		.selected = 2,
		.issued_advance = U32_MAX - 2,
	},
	{
		.name = "assigned_ids_expire_window",
		.ids = { 1, 2 },
		.boundary = 3,
		.count = 2,
		.issued_advance = U32_MAX,
		.error = -ERANGE,
	},
	{
		.name = "assigned_ids_extend_window",
		.ids = { 1, 2 },
		.boundary = 0x80000001,
		.count = 2,
		.selected = 2,
		.issued_advance = 0x80000000,
	},
	{
		.name = "removal_restores_window",
		.ids = { 1, 0x80000002, 2 },
		.boundary = 0,
		.count = 3,
		.selected = 1,
		.remove_mask = BIT(0),
	},
	{
		.name = "counter_wrap_missing",
		.ids = { 0xfffffffe, 0, 2 },
		.boundary = 1,
		.count = 3,
		.selected = 2,
		.sequence_offset = U64_MAX - U32_MAX,
	},
	{
		.name = "removal_empties_aged_queue",
		.ids = { 1, 2 },
		.boundary = 3,
		.count = 2,
		.remove_mask = BIT(0) | BIT(1),
		.issued_advance = U32_MAX,
	},
};

static void nfqnl_batch_case_desc(const struct nfqnl_batch_case *param,
				  char *desc)
{
	strscpy(desc, param->name, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(nfqnl_batch, nfqnl_batch_cases, nfqnl_batch_case_desc);

static void nfqnl_test_skb_destructor(struct sk_buff *skb)
{
	struct nfqnl_test_skb_cb *cb = (void *)skb->cb;
	struct nfqnl_test_ctx *ctx = cb->ctx;
	unsigned int i = ctx->freed_count++;

	if (i >= ARRAY_SIZE(ctx->freed))
		return;
	ctx->freed[i].sequence = cb->sequence;
	ctx->freed[i].id = cb->id;
	ctx->freed[i].mark = skb->mark;
	ctx->freed[i].priority = skb->priority;
}

static struct nf_queue_entry *nfqnl_test_alloc(struct nfqnl_test_ctx *ctx,
					       u64 sequence)
{
	struct nfqnl_test_skb_cb *cb;
	struct nf_queue_entry *entry;

	entry = kzalloc_obj(*entry);
	if (!entry)
		return NULL;
	entry->skb = alloc_skb(0, GFP_KERNEL);
	if (!entry->skb) {
		kfree(entry);
		return NULL;
	}
	entry->id = (u32)sequence;
	entry->id_epoch = (u32)(sequence >> 32);
	entry->size = sizeof(*entry);
	entry->state.net = ctx->net;
	entry->state.pf = AF_INET;
	entry->state.hook = NF_INET_PRE_ROUTING;
	entry->skb->mark = 10;
	entry->skb->priority = 20;
	cb = (void *)entry->skb->cb;
	cb->ctx = ctx;
	cb->sequence = sequence;
	cb->id = entry->id;
	entry->skb->destructor = nfqnl_test_skb_destructor;
	return entry;
}

static int nfqnl_test_enqueue(struct nfqnl_test_ctx *ctx, u64 sequence)
{
	struct nf_queue_entry *entry = nfqnl_test_alloc(ctx, sequence);
	int err;

	if (!entry)
		return -ENOMEM;
	spin_lock_bh(&ctx->queue->lock);
	ctx->queue->id_sequence = sequence;
	err = __enqueue_entry(ctx->queue, entry);
	spin_unlock_bh(&ctx->queue->lock);
	if (err) {
		kfree_skb(entry->skb);
		nf_queue_entry_free(entry);
	}
	return err;
}

static int nfqnl_test_verdict(struct nfqnl_test_ctx *ctx, u32 boundary)
{
	struct nfqnl_msg_verdict_hdr vhdr = {
		.verdict = htonl(NF_DROP),
		.id = htonl(boundary),
	};
	struct nlattr *attrs[NFQA_MAX + 1];
	struct nfnl_info info = { .net = ctx->net };
	struct nlmsghdr *nlh;
	struct sk_buff *skb;
	u16 type;
	int err;

	skb = alloc_skb(256, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;
	type = nfnl_msg_type(NFNL_SUBSYS_QUEUE, NFQNL_MSG_VERDICT_BATCH);
	nlh = nfnl_msg_put(skb, NFQNL_TEST_PORTID, 1, type, NLM_F_REQUEST,
			   AF_INET, NFNETLINK_V0, htons(NFQNL_TEST_QUEUE));
	if (!nlh || nla_put(skb, NFQA_VERDICT_HDR, sizeof(vhdr), &vhdr) ||
	    nla_put_be32(skb, NFQA_MARK, htonl(NFQNL_TEST_MARK)) ||
	    nla_put_be32(skb, NFQA_PRIORITY, htonl(NFQNL_TEST_PRIORITY))) {
		err = -EMSGSIZE;
		goto out;
	}
	nlmsg_end(skb, nlh);
	err = nlmsg_parse_deprecated(nlh, sizeof(struct nfgenmsg), attrs,
				     NFQA_MAX, nfqa_verdict_batch_policy, NULL);
	if (err)
		goto out;
	NETLINK_CB(skb).portid = NFQNL_TEST_PORTID;
	info.nlh = nlh;
	info.nfmsg = nlmsg_data(nlh);
	rcu_read_lock();
	err = nfqnl_recv_verdict_batch(skb, &info,
				       (const struct nlattr *const *)attrs);
	rcu_read_unlock();
out:
	kfree_skb(skb);
	return err;
}

static void nfqnl_test_expect_queue(struct kunit *test,
				    const struct nfqnl_batch_case *param,
				    unsigned int removed_mask, u64 sequence)
{
	struct nfqnl_test_result remaining[NFQNL_TEST_MAX_ENTRIES];
	bool in_hash[NFQNL_TEST_MAX_ENTRIES];
	struct nfqnl_test_ctx *ctx = test->priv;
	struct nf_queue_entry *entry;
	unsigned int i, expected = 0, count = 0, queue_total;
	unsigned int dropped, user_dropped;
	u64 actual_sequence;

	rcu_read_lock();
	spin_lock_bh(&ctx->queue->lock);
	queue_total = ctx->queue->queue_total;
	dropped = ctx->queue->queue_dropped;
	user_dropped = ctx->queue->queue_user_dropped;
	actual_sequence = ctx->queue->id_sequence;
	list_for_each_entry(entry, &ctx->queue->queue_list, list) {
		if (count == ARRAY_SIZE(remaining))
			break;
		remaining[count].id = entry->id;
		remaining[count].mark = entry->skb->mark;
		remaining[count].priority = entry->skb->priority;
		count++;
	}
	for (i = 0; i < param->count; i++)
		in_hash[i] =
			rhashtable_lookup_fast(&ctx->queue->nfqnl_packet_map,
					       &param->ids[i],
					       nfqnl_rhashtable_params) != NULL;
	spin_unlock_bh(&ctx->queue->lock);
	rcu_read_unlock();

	KUNIT_EXPECT_EQ(test, dropped, NFQNL_TEST_DROPPED);
	KUNIT_EXPECT_EQ(test, user_dropped, NFQNL_TEST_USER_DROPPED);
	KUNIT_EXPECT_EQ(test, actual_sequence, sequence);
	for (i = 0; i < param->count; i++) {
		bool present = !(removed_mask & BIT(i));

		KUNIT_EXPECT_EQ(test, in_hash[i], present);
		if (!present)
			continue;
		if (expected < count)
			KUNIT_EXPECT_EQ(test, remaining[expected].id,
					param->ids[i]);
		expected++;
	}
	KUNIT_EXPECT_EQ(test, queue_total, expected);
	KUNIT_EXPECT_EQ(test, count, expected);
	for (i = 0; i < count; i++) {
		KUNIT_EXPECT_EQ(test, remaining[i].mark, 10);
		KUNIT_EXPECT_EQ(test, remaining[i].priority, 20);
	}
}

static int nfqnl_test_remove(struct nfqnl_test_ctx *ctx, u32 id)
{
	struct nf_queue_entry *entry;
	int err;

	rcu_read_lock();
	entry = find_dequeue_entry(ctx->queue, id);
	err = entry ? 0 : -ENOENT;
	/* find_dequeue_entry() releases the queue lock before reinjection. */
	if (entry)
		nfqnl_reinject(entry, NF_DROP);
	rcu_read_unlock();
	return err;
}

static void nfqnl_batch_selection_test(struct kunit *test)
{
	const struct nfqnl_batch_case *param = test->param_value;
	struct nfqnl_test_ctx *ctx = test->priv;
	unsigned int removed_mask = 0, removed_count = 0, selected = 0;
	u64 sequence = param->sequence_offset;
	unsigned int i;
	int err;

	/* Seed chronological positions without generating billions of packets. */
	for (i = 0; i < param->count; i++) {
		sequence += i ? (u32)(param->ids[i] - param->ids[i - 1]) :
				param->ids[i];
		err = nfqnl_test_enqueue(ctx, sequence);
		KUNIT_ASSERT_EQ(test, err, 0);
	}

	/* Failed notifications can assign IDs without adding queued entries. */
	sequence += param->issued_advance;
	spin_lock_bh(&ctx->queue->lock);
	ctx->queue->id_sequence = sequence;
	spin_unlock_bh(&ctx->queue->lock);
	nfqnl_test_expect_queue(test, param, removed_mask, sequence);

	for (i = 0; i < param->count; i++) {
		if (!(param->remove_mask & BIT(i)))
			continue;
		err = nfqnl_test_remove(ctx, param->ids[i]);
		KUNIT_ASSERT_EQ(test, err, 0);
		removed_mask |= BIT_U32(i);
		KUNIT_ASSERT_EQ(test, ctx->freed_count, removed_count + 1);
		KUNIT_EXPECT_EQ(test, ctx->freed[removed_count].id,
				param->ids[i]);
		KUNIT_EXPECT_EQ(test, ctx->freed[removed_count].mark, 10);
		KUNIT_EXPECT_EQ(test, ctx->freed[removed_count].priority, 20);
		removed_count++;
		nfqnl_test_expect_queue(test, param, removed_mask, sequence);
	}

	err = nfqnl_test_verdict(ctx, param->boundary);
	KUNIT_EXPECT_EQ(test, err,
			param->error ?: (param->selected ? 0 : -ENOENT));
	KUNIT_EXPECT_EQ(test, ctx->freed_count,
			removed_count + param->selected);
	for (i = 0; i < param->count && selected < param->selected; i++) {
		unsigned int freed = removed_count + selected;

		if (removed_mask & BIT(i))
			continue;
		removed_mask |= BIT_U32(i);
		selected++;
		if (freed >= ctx->freed_count)
			continue;
		KUNIT_EXPECT_EQ(test, ctx->freed[freed].id, param->ids[i]);
		KUNIT_EXPECT_EQ(test, ctx->freed[freed].mark, NFQNL_TEST_MARK);
		KUNIT_EXPECT_EQ(test, ctx->freed[freed].priority,
				NFQNL_TEST_PRIORITY);
	}
	nfqnl_test_expect_queue(test, param, removed_mask, sequence);
}

static void nfqnl_test_expect_entries(struct kunit *test, const u64 *sequences,
				      unsigned int expected, u64 issued,
				      unsigned int expected_dropped)
{
	struct nfqnl_test_result remaining[NFQNL_TEST_MAX_ENTRIES];
	bool in_hash[NFQNL_TEST_MAX_ENTRIES];
	struct nfqnl_test_ctx *ctx = test->priv;
	struct nf_queue_entry *entry, *found;
	unsigned int count = 0, i, total, dropped, user_dropped;
	u64 actual_issued;
	int mapped;

	rcu_read_lock();
	spin_lock_bh(&ctx->queue->lock);
	total = ctx->queue->queue_total;
	mapped = atomic_read(&ctx->queue->nfqnl_packet_map.nelems);
	dropped = ctx->queue->queue_dropped;
	user_dropped = ctx->queue->queue_user_dropped;
	actual_issued = ctx->queue->id_sequence;
	list_for_each_entry(entry, &ctx->queue->queue_list, list) {
		if (count == ARRAY_SIZE(remaining))
			break;
		remaining[count].sequence = ((u64)entry->id_epoch << 32) |
					    entry->id;
		remaining[count].id = entry->id;
		remaining[count].mark = entry->skb->mark;
		remaining[count].priority = entry->skb->priority;
		found = rhashtable_lookup_fast(&ctx->queue->nfqnl_packet_map,
					       &entry->id,
					       nfqnl_rhashtable_params);
		in_hash[count] = found == entry;
		count++;
	}
	spin_unlock_bh(&ctx->queue->lock);
	rcu_read_unlock();

	KUNIT_EXPECT_EQ(test, total, expected);
	KUNIT_EXPECT_EQ(test, mapped, (int)expected);
	KUNIT_EXPECT_EQ(test, count, expected);
	KUNIT_EXPECT_EQ(test, actual_issued, issued);
	KUNIT_EXPECT_EQ(test, dropped, expected_dropped);
	KUNIT_EXPECT_EQ(test, user_dropped, NFQNL_TEST_USER_DROPPED);
	for (i = 0; i < min(count, expected); i++) {
		KUNIT_EXPECT_EQ(test, remaining[i].sequence, sequences[i]);
		KUNIT_EXPECT_EQ(test, remaining[i].id, (u32)sequences[i]);
		KUNIT_EXPECT_EQ(test, remaining[i].mark, 10);
		KUNIT_EXPECT_EQ(test, remaining[i].priority, 20);
		KUNIT_EXPECT_TRUE(test, in_hash[i]);
	}
}

static void nfqnl_test_expect_freed(struct kunit *test, unsigned int index,
				    u64 sequence, bool batch)
{
	struct nfqnl_test_ctx *ctx = test->priv;

	KUNIT_ASSERT_LT(test, index, ctx->freed_count);
	KUNIT_EXPECT_EQ(test, ctx->freed[index].sequence, sequence);
	KUNIT_EXPECT_EQ(test, ctx->freed[index].id, (u32)sequence);
	KUNIT_EXPECT_EQ(test, ctx->freed[index].mark,
			batch ? NFQNL_TEST_MARK : 10);
	KUNIT_EXPECT_EQ(test, ctx->freed[index].priority,
			batch ? NFQNL_TEST_PRIORITY : 20);
}

struct nfqnl_collision_case {
	const char *name;
	u32 id;
	bool batch;
};

static const struct nfqnl_collision_case nfqnl_collision_cases[] = {
	{ "individual", 1, false },
	{ "batch", 1, true },
	{ "individual_zero", 0, false },
	{ "batch_zero", 0, true },
};

static void nfqnl_collision_case_desc(const struct nfqnl_collision_case *param,
				      char *desc)
{
	strscpy(desc, param->name, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(nfqnl_collision, nfqnl_collision_cases,
		  nfqnl_collision_case_desc);

static void nfqnl_id_collision_test(struct kunit *test)
{
	const struct nfqnl_collision_case *param = test->param_value;
	struct nfqnl_test_ctx *ctx = test->priv;
	u64 old = param->id, collision = (1ULL << 32) + old;
	u64 reused[] = { (2ULL << 32) + old, (2ULL << 32) + old + 1 };
	int err;

	err = nfqnl_test_enqueue(ctx, old);
	KUNIT_ASSERT_EQ(test, err, 0);
	err = nfqnl_test_enqueue(ctx, collision);
	KUNIT_ASSERT_EQ(test, err, -EEXIST);
	KUNIT_ASSERT_EQ(test, ctx->freed_count, 1);
	nfqnl_test_expect_freed(test, 0, collision, false);
	nfqnl_test_expect_entries(test, &old, 1, collision, NFQNL_TEST_DROPPED);

	/* Both verdict forms still identify the retained original object. */
	err = param->batch ? nfqnl_test_verdict(ctx, param->id) :
			     nfqnl_test_remove(ctx, param->id);
	KUNIT_ASSERT_EQ(test, err, 0);
	KUNIT_ASSERT_EQ(test, ctx->freed_count, 2);
	nfqnl_test_expect_freed(test, 1, old, param->batch);
	nfqnl_test_expect_entries(test, NULL, 0, collision, NFQNL_TEST_DROPPED);

	/* Freed IDs can be reused; the following distinct ID is admitted too. */
	err = nfqnl_test_enqueue(ctx, reused[0]);
	KUNIT_ASSERT_EQ(test, err, 0);
	err = nfqnl_test_enqueue(ctx, reused[1]);
	KUNIT_ASSERT_EQ(test, err, 0);
	nfqnl_test_expect_entries(test, reused, 2, reused[1],
				  NFQNL_TEST_DROPPED);
	err = nfqnl_test_verdict(ctx, (u32)reused[1]);
	KUNIT_ASSERT_EQ(test, err, 0);
	KUNIT_ASSERT_EQ(test, ctx->freed_count, 4);
	nfqnl_test_expect_freed(test, 2, reused[0], true);
	nfqnl_test_expect_freed(test, 3, reused[1], true);
	nfqnl_test_expect_entries(test, NULL, 0, reused[1], NFQNL_TEST_DROPPED);
}

static unsigned int nfqnl_test_hook(void *priv, struct sk_buff *skb,
				    const struct nf_hook_state *state)
{
	(void)priv;
	(void)skb;
	(void)state;
	return NF_ACCEPT;
}

static const struct nf_hook_ops nfqnl_test_hook_ops = {
	.hook = nfqnl_test_hook,
	.pf = NFPROTO_IPV4,
	.hooknum = NF_INET_PRE_ROUTING,
	.priority = NF_IP_PRI_LAST,
};

static int nfqnl_test_accept(struct net *net, struct sock *sk,
			     struct sk_buff *skb)
{
	struct nfqnl_test_skb_cb *cb = (void *)skb->cb;

	(void)net;
	(void)sk;
	cb->ctx->accepted_count++;
	cb->ctx->accepted_sequence = cb->sequence;
	kfree_skb(skb);
	return 0;
}

static int nfqnl_test_packet_enqueue(struct nfqnl_test_ctx *ctx, u64 sequence)
{
	struct nf_queue_entry *entry = nfqnl_test_alloc(ctx, sequence);
	const struct nf_hook_entries *hooks;
	struct sk_buff *skb;
	unsigned int i;
	int err;

	if (!entry)
		return -ENOMEM;
	skb = entry->skb;
	/* Verify production assignment, independently of fixture seeding. */
	entry->id = ~(u32)sequence;
	entry->id_epoch = ~(u32)(sequence >> 32);
	entry->state.okfn = nfqnl_test_accept;
	rcu_read_lock();
	hooks = nf_hook_entries_head(ctx->net, NFPROTO_IPV4,
				     NF_INET_PRE_ROUTING);
	for (i = 0; hooks && i < hooks->num_hook_entries; i++) {
		if (hooks->hooks[i].hook == nfqnl_test_hook)
			break;
	}
	if (!hooks || i == hooks->num_hook_entries) {
		err = -ENOENT;
	} else {
		entry->hook_index = i;
		err = __nfqnl_enqueue_packet(ctx->net, ctx->queue, entry);
	}
	rcu_read_unlock();
	/* Mirror the NFQUEUE caller's ownership on an enqueue error. */
	if (err < 0) {
		nf_queue_entry_free(entry);
		kfree_skb(skb);
	}
	return err;
}

static void nfqnl_test_notification(struct kunit *test, bool expected, u32 id)
{
	struct nfqnl_test_ctx *ctx = test->priv;
	struct nlattr *attrs[NFQA_MAX + 1];
	struct nfqnl_msg_packet_hdr *phdr;
	struct sk_buff *skb;
	int err;

	skb = skb_recv_datagram(ctx->socket->sk, MSG_DONTWAIT, &err);
	if (!expected) {
		KUNIT_EXPECT_PTR_EQ(test, skb, NULL);
		KUNIT_EXPECT_EQ(test, err, -EAGAIN);
		if (skb)
			skb_free_datagram(ctx->socket->sk, skb);
		return;
	}
	KUNIT_ASSERT_NOT_NULL(test, skb);
	err = nlmsg_parse_deprecated(nlmsg_hdr(skb), sizeof(struct nfgenmsg),
				     attrs, NFQA_MAX, NULL, NULL);
	KUNIT_EXPECT_EQ(test, err, 0);
	if (!err) {
		KUNIT_EXPECT_NOT_NULL(test, attrs[NFQA_PACKET_HDR]);
		if (attrs[NFQA_PACKET_HDR]) {
			phdr = nla_data(attrs[NFQA_PACKET_HDR]);
			KUNIT_EXPECT_EQ(test, ntohl(phdr->packet_id), id);
		}
	}
	skb_free_datagram(ctx->socket->sk, skb);
}

struct nfqnl_enqueue_case {
	const char *name;
	bool failopen;
	u64 offset;
};

static const struct nfqnl_enqueue_case nfqnl_enqueue_cases[] = {
	{ "default", false, 0 },
	{ "fail_open", true, 0 },
	{ "high_epoch_default", false, 0x8000000000000000ULL },
	{ "high_epoch_fail_open", true, 0x8000000000000000ULL },
	{ "last_epoch_default", false, 0xfffffffe00000000ULL },
	{ "last_epoch_fail_open", true, 0xfffffffe00000000ULL },
	{ "counter_wrap_default", false, 0xffffffff00000000ULL },
	{ "counter_wrap_fail_open", true, 0xffffffff00000000ULL },
};

static void nfqnl_enqueue_case_desc(const struct nfqnl_enqueue_case *param,
				    char *desc)
{
	strscpy(desc, param->name, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(nfqnl_enqueue, nfqnl_enqueue_cases, nfqnl_enqueue_case_desc);

static void nfqnl_collision_enqueue_test(struct kunit *test)
{
	struct nfqnl_test_ctx *ctx = test->priv;
	const struct nfqnl_enqueue_case *param = test->param_value;
	bool failopen = param->failopen;
	u64 offset = param->offset;
	struct sockaddr_nl addr = {
		.nl_family = AF_NETLINK,
		.nl_pid = NFQNL_TEST_PORTID,
	};
	u64 sequences[] = { offset + 1, offset + (1ULL << 32) + 2 };
	u64 collision = offset + (1ULL << 32) + 1;
	unsigned int dropped = NFQNL_TEST_DROPPED + !failopen;
	int err;

	err = sock_create_kern(ctx->net, AF_NETLINK, SOCK_RAW,
			       NETLINK_NETFILTER, &ctx->socket);
	KUNIT_ASSERT_EQ(test, err, 0);
	err = kernel_bind(ctx->socket, (struct sockaddr_unsized *)&addr,
			  sizeof(addr));
	KUNIT_ASSERT_EQ(test, err, 0);
	err = nf_register_net_hook(ctx->net, &nfqnl_test_hook_ops);
	KUNIT_ASSERT_EQ(test, err, 0);
	ctx->hook_registered = true;
	ctx->queue->copy_mode = NFQNL_COPY_META;
	ctx->queue->flags = failopen ? NFQA_CFG_F_FAIL_OPEN : 0;

	spin_lock_bh(&ctx->queue->lock);
	ctx->queue->id_sequence = offset;
	spin_unlock_bh(&ctx->queue->lock);

	/* Prove the local notification socket works before testing suppression. */
	err = nfqnl_test_packet_enqueue(ctx, sequences[0]);
	KUNIT_ASSERT_EQ(test, err, 0);
	nfqnl_test_notification(test, true, 1);
	nfqnl_test_notification(test, false, 0);
	nfqnl_test_expect_entries(test, sequences, 1, sequences[0],
				  NFQNL_TEST_DROPPED);

	spin_lock_bh(&ctx->queue->lock);
	ctx->queue->id_sequence = offset + (1ULL << 32);
	spin_unlock_bh(&ctx->queue->lock);
	err = nfqnl_test_packet_enqueue(ctx, collision);
	KUNIT_ASSERT_EQ(test, err, failopen ? 0 : -EEXIST);
	KUNIT_ASSERT_EQ(test, ctx->freed_count, 1);
	nfqnl_test_expect_freed(test, 0, collision, false);
	KUNIT_EXPECT_EQ(test, ctx->accepted_count, failopen ? 1U : 0U);
	if (failopen)
		KUNIT_EXPECT_EQ(test, ctx->accepted_sequence, collision);
	nfqnl_test_expect_entries(test, sequences, 1, collision, dropped);
	nfqnl_test_notification(test, false, 0);

	/* Collision refusal advances issuance; the next ID can be notified. */
	err = nfqnl_test_packet_enqueue(ctx, sequences[1]);
	KUNIT_ASSERT_EQ(test, err, 0);
	nfqnl_test_notification(test, true, 2);
	nfqnl_test_notification(test, false, 0);
	nfqnl_test_expect_entries(test, sequences, 2, sequences[1], dropped);
	err = nfqnl_test_verdict(ctx, 2);
	KUNIT_ASSERT_EQ(test, err, 0);
	KUNIT_ASSERT_EQ(test, ctx->freed_count, 3);
	nfqnl_test_expect_freed(test, 1, sequences[0], true);
	nfqnl_test_expect_freed(test, 2, sequences[1], true);
	nfqnl_test_expect_entries(test, NULL, 0, sequences[1], dropped);
	KUNIT_EXPECT_EQ(test, ctx->accepted_count, failopen ? 1U : 0U);
}

static int nfqnl_batch_test_init(struct kunit *test)
{
	struct nfqnl_test_ctx *ctx;
	int err;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	/* Hook teardown must not flush queues outside this test. */
	ctx->net = copy_net_ns(CLONE_NEWNET, &init_user_ns, &init_net);
	if (IS_ERR(ctx->net))
		return PTR_ERR(ctx->net);
	ctx->queue = instance_create(nfnl_queue_pernet(ctx->net),
				     NFQNL_TEST_QUEUE, NFQNL_TEST_PORTID);
	err = PTR_ERR_OR_ZERO(ctx->queue);
	if (err) {
		put_net(ctx->net);
		return err;
	}
	ctx->queue->queue_dropped = NFQNL_TEST_DROPPED;
	ctx->queue->queue_user_dropped = NFQNL_TEST_USER_DROPPED;
	test->priv = ctx;
	return 0;
}

static void nfqnl_batch_test_exit(struct kunit *test)
{
	struct nfqnl_test_ctx *ctx = test->priv;

	if (ctx->hook_registered)
		nf_unregister_net_hook(ctx->net, &nfqnl_test_hook_ops);
	instance_destroy(nfnl_queue_pernet(ctx->net), ctx->queue);
	/* Drain queued-packet destructors before releasing the test context. */
	rcu_barrier();
	flush_workqueue(nfq_cleanup_wq);
	if (ctx->socket)
		sock_release(ctx->socket);
	put_net(ctx->net);
}

static struct kunit_case nfqnl_batch_test_cases[] = {
	KUNIT_CASE_PARAM(nfqnl_batch_selection_test, nfqnl_batch_gen_params),
	KUNIT_CASE_PARAM(nfqnl_id_collision_test, nfqnl_collision_gen_params),
	KUNIT_CASE_PARAM(nfqnl_collision_enqueue_test,
			 nfqnl_enqueue_gen_params),
	{}
};

static struct kunit_suite nfqnl_batch_test_suite = {
	.name = "nfnetlink_queue_batch",
	.init = nfqnl_batch_test_init,
	.exit = nfqnl_batch_test_exit,
	.test_cases = nfqnl_batch_test_cases,
};

kunit_test_suite(nfqnl_batch_test_suite);
