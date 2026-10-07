// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for empty and skb-backed netvsc completion handling.
 *
 * Built into the hv_netvsc object rather than a separate module so the
 * cases can reach the internal helpers declared in hyperv_net.h
 * without exporting them.
 */
#include <kunit/test.h>
#include <linux/etherdevice.h>
#include <linux/hyperv.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/wait.h>

#include "hyperv_net.h"

/*
 * Scripted requestor. request_addr_callback() only receives the channel
 * and the transaction id, so the fixture is hung off a file-scope
 * pointer. KUnit runs suite cases serially; one active fixture is
 * enough and keeps the callback signature untouched.
 */
struct netvsc_completion_fixture {
	struct net_device *ndev;
	struct netvsc_device *nvdev;
	struct vmbus_channel *channel;
	/* Address returned on first use, then VMBUS_NO_RQSTOR: the id is
	 * consumed exactly once, as vmbus_request_addr_match() does.
	 */
	u64 once_addr;
	unsigned int calls;
	u64 seen_ids[8];
	unsigned int skb_frees;
};

static struct netvsc_completion_fixture *active_fx;

static u64 test_request_addr(struct vmbus_channel *channel, u64 rqst_id)
{
	struct netvsc_completion_fixture *fx = active_fx;

	if (fx->calls < ARRAY_SIZE(fx->seen_ids))
		fx->seen_ids[fx->calls] = rqst_id;
	fx->calls++;

	if (fx->calls == 1)
		return fx->once_addr;
	return VMBUS_NO_RQSTOR;
}

/*
 * A completion carrying no payload: hv_pkt_datalen() == 0. The
 * empty-completion path looks the request up by trans_id and reads its
 * message type, so the request itself is a separate object the fixture
 * hands back through request_addr_callback().
 */
struct netvsc_empty_desc {
	struct vmpacket_descriptor desc;
} __packed;

static void make_empty_desc(struct netvsc_empty_desc *pkt, u64 trans_id)
{
	memset(pkt, 0, sizeof(*pkt));
	pkt->desc.offset8 = sizeof(pkt->desc) / 8;
	pkt->desc.len8 = pkt->desc.offset8;
	pkt->desc.trans_id = trans_id;
}

/*
 * The production path never dereferences a net_device queue while
 * tx_disable is set: netif_tx_queue_stopped() && !tx_disable
 * short-circuits before hv_get_avail_to_write_percent() reads the
 * ring. Set it here so a test needs no live outbound ring.
 */
static int netvsc_completion_fixture_init(struct netvsc_completion_fixture *fx)
{
	fx->ndev = alloc_netdev_mqs(sizeof(struct net_device_context),
				    "hvcompl%d", NET_NAME_UNKNOWN,
				    ether_setup, 2, 2);
	if (!fx->ndev)
		return -ENOMEM;

	fx->nvdev = kzalloc_obj(*fx->nvdev, GFP_KERNEL);
	if (!fx->nvdev) {
		free_netdev(fx->ndev);
		return -ENOMEM;
	}

	fx->channel = kzalloc_obj(*fx->channel, GFP_KERNEL);
	if (!fx->channel) {
		kfree(fx->nvdev);
		free_netdev(fx->ndev);
		return -ENOMEM;
	}

	init_waitqueue_head(&fx->nvdev->wait_drain);
	init_completion(&fx->nvdev->channel_init_wait);
	fx->nvdev->tx_disable = true;
	fx->channel->request_addr_callback = test_request_addr;
	fx->once_addr = 0;
	fx->calls = 0;
	memset(fx->seen_ids, 0, sizeof(fx->seen_ids));
	active_fx = fx;
	return 0;
}

static void netvsc_completion_fixture_exit(struct netvsc_completion_fixture *fx)
{
	active_fx = NULL;
	kfree(fx->channel);
	kfree(fx->nvdev);
	free_netdev(fx->ndev);
}

static int netvsc_send_sends(struct netvsc_completion_fixture *fx, u16 q_idx)
{
	return atomic_read(&fx->nvdev->chan_table[q_idx].queue_sends);
}

/* Empty completion, request address NULL: owns the send accounting. */
static void netvsc_completion_null_address_acct_test(struct kunit *test)
{
	struct netvsc_completion_fixture fx = {};
	struct netvsc_empty_desc pkt;

	KUNIT_ASSERT_EQ(test, netvsc_completion_fixture_init(&fx), 0);
	atomic_set(&fx.nvdev->chan_table[0].queue_sends, 3);
	make_empty_desc(&pkt, 0x11);
	/* once_addr = 0 is the control-path NULL skb case. */
	fx.once_addr = 0;

	netvsc_send_completion(fx.ndev, fx.nvdev, fx.channel, &pkt.desc, 0);

	KUNIT_EXPECT_EQ(test, netvsc_send_sends(&fx, 0), 2);
	KUNIT_EXPECT_FALSE(test, completion_done(&fx.nvdev->channel_init_wait));
	KUNIT_EXPECT_EQ(test, fx.calls, 1U);
	KUNIT_EXPECT_EQ(test, fx.seen_ids[0], 0x11U);

	netvsc_completion_fixture_exit(&fx);
}

/* VMBUS_RQST_ERROR is rejected: no accounting, no completion. */
static void netvsc_completion_rqst_error_sentinel_test(struct kunit *test)
{
	struct netvsc_completion_fixture fx = {};
	struct netvsc_empty_desc pkt;

	KUNIT_ASSERT_EQ(test, netvsc_completion_fixture_init(&fx), 0);
	atomic_set(&fx.nvdev->chan_table[0].queue_sends, 3);
	make_empty_desc(&pkt, 0x22);
	fx.once_addr = VMBUS_RQST_ERROR;

	netvsc_send_completion(fx.ndev, fx.nvdev, fx.channel, &pkt.desc, 0);

	KUNIT_EXPECT_EQ(test, netvsc_send_sends(&fx, 0), 3);
	KUNIT_EXPECT_FALSE(test, completion_done(&fx.nvdev->channel_init_wait));

	netvsc_completion_fixture_exit(&fx);
}

/* VMBUS_NO_RQSTOR is rejected the same way. */
static void netvsc_completion_no_rqstor_sentinel_test(struct kunit *test)
{
	struct netvsc_completion_fixture fx = {};
	struct netvsc_empty_desc pkt;

	KUNIT_ASSERT_EQ(test, netvsc_completion_fixture_init(&fx), 0);
	atomic_set(&fx.nvdev->chan_table[0].queue_sends, 3);
	make_empty_desc(&pkt, 0x33);
	fx.once_addr = VMBUS_NO_RQSTOR;

	netvsc_send_completion(fx.ndev, fx.nvdev, fx.channel, &pkt.desc, 0);

	KUNIT_EXPECT_EQ(test, netvsc_send_sends(&fx, 0), 3);
	KUNIT_EXPECT_FALSE(test, completion_done(&fx.nvdev->channel_init_wait));

	netvsc_completion_fixture_exit(&fx);
}

/*
 * A real request address whose message type is SWITCH_DATA_PATH
 * completes the channel-init wait. It is not send accounting: the
 * control request was not a send.
 */
static void netvsc_completion_switch_data_path_test(struct kunit *test)
{
	struct netvsc_completion_fixture fx = {};
	struct netvsc_empty_desc pkt;
	struct nvsp_message req = {};

	KUNIT_ASSERT_EQ(test, netvsc_completion_fixture_init(&fx), 0);
	atomic_set(&fx.nvdev->chan_table[0].queue_sends, 3);
	make_empty_desc(&pkt, 0x44);
	req.hdr.msg_type = NVSP_MSG4_TYPE_SWITCH_DATA_PATH;
	fx.once_addr = (u64)(unsigned long)&req;

	netvsc_send_completion(fx.ndev, fx.nvdev, fx.channel, &pkt.desc, 0);

	KUNIT_EXPECT_TRUE(test, completion_done(&fx.nvdev->channel_init_wait));
	KUNIT_EXPECT_EQ(test, netvsc_send_sends(&fx, 0), 3);

	netvsc_completion_fixture_exit(&fx);
}

/* An unexpected message type takes neither the complete nor the acct path. */
static void netvsc_completion_unknown_msg_type_test(struct kunit *test)
{
	struct netvsc_completion_fixture fx = {};
	struct netvsc_empty_desc pkt;
	struct nvsp_message req = {};

	KUNIT_ASSERT_EQ(test, netvsc_completion_fixture_init(&fx), 0);
	atomic_set(&fx.nvdev->chan_table[0].queue_sends, 3);
	make_empty_desc(&pkt, 0x55);
	req.hdr.msg_type = 0xdead;
	fx.once_addr = (u64)(unsigned long)&req;

	netvsc_send_completion(fx.ndev, fx.nvdev, fx.channel, &pkt.desc, 0);

	KUNIT_EXPECT_FALSE(test, completion_done(&fx.nvdev->channel_init_wait));
	KUNIT_EXPECT_EQ(test, netvsc_send_sends(&fx, 0), 3);

	netvsc_completion_fixture_exit(&fx);
}

/*
 * A repeated completion for an already-consumed transaction id must not
 * account twice. The requestor returns VMBUS_NO_RQSTOR on the second
 * use and the empty-completion path rejects it.
 */
static void netvsc_completion_duplicate_completion_test(struct kunit *test)
{
	struct netvsc_completion_fixture fx = {};
	struct netvsc_empty_desc pkt;

	KUNIT_ASSERT_EQ(test, netvsc_completion_fixture_init(&fx), 0);
	atomic_set(&fx.nvdev->chan_table[0].queue_sends, 3);
	make_empty_desc(&pkt, 0x66);
	fx.once_addr = 0;

	netvsc_send_completion(fx.ndev, fx.nvdev, fx.channel, &pkt.desc, 0);
	KUNIT_EXPECT_EQ(test, netvsc_send_sends(&fx, 0), 2);
	KUNIT_EXPECT_EQ(test, fx.calls, 1U);

	/* Second completion for the same id: already consumed. */
	netvsc_send_completion(fx.ndev, fx.nvdev, fx.channel, &pkt.desc, 0);
	KUNIT_EXPECT_EQ(test, netvsc_send_sends(&fx, 0), 2);
	KUNIT_EXPECT_EQ(test, fx.calls, 2U);
	KUNIT_EXPECT_EQ(test, fx.seen_ids[0], 0x66U);
	KUNIT_EXPECT_EQ(test, fx.seen_ids[1], 0x66U);

	netvsc_completion_fixture_exit(&fx);
}

/* N completions decrement the queue exactly N times. */
static void netvsc_completion_single_decrement_test(struct kunit *test)
{
	struct netvsc_completion_fixture fx = {};
	struct netvsc_empty_desc pkt;
	int i;

	KUNIT_ASSERT_EQ(test, netvsc_completion_fixture_init(&fx), 0);
	atomic_set(&fx.nvdev->chan_table[0].queue_sends, 5);

	for (i = 0; i < 5; i++) {
		make_empty_desc(&pkt, 0x100 + i);
		fx.once_addr = 0;
		fx.calls = 0;
		netvsc_send_completion(fx.ndev, fx.nvdev, fx.channel,
				       &pkt.desc, 0);
		KUNIT_EXPECT_EQ(test, netvsc_send_sends(&fx, 0), 4 - i);
		KUNIT_EXPECT_EQ(test, fx.calls, 1U);
	}
	KUNIT_EXPECT_EQ(test, netvsc_send_sends(&fx, 0), 0);

	netvsc_completion_fixture_exit(&fx);
}

static int netvsc_test_wake(struct wait_queue_entry *wq_entry,
			    unsigned int mode, int flags, void *key)
{
	unsigned int *woken = wq_entry->private;

	(*woken)++;
	return 0;
}

/* destroy + a decrement that reaches zero wakes the drain wait. */
static void netvsc_send_acct_drain_wake_test(struct kunit *test)
{
	struct netvsc_completion_fixture fx = {};
	struct wait_queue_entry entry;
	unsigned int woken = 0;

	KUNIT_ASSERT_EQ(test, netvsc_completion_fixture_init(&fx), 0);
	fx.nvdev->destroy = true;

	memset(&entry, 0, sizeof(entry));
	entry.func = netvsc_test_wake;
	entry.private = &woken;
	add_wait_queue(&fx.nvdev->wait_drain, &entry);

	/* Non-zero remainder: the drain wait is not woken. */
	atomic_set(&fx.nvdev->chan_table[0].queue_sends, 2);
	netvsc_send_acct(fx.ndev, fx.nvdev, fx.channel, 0);
	KUNIT_EXPECT_EQ(test, woken, 0U);
	KUNIT_EXPECT_EQ(test, netvsc_send_sends(&fx, 0), 1);

	/* Exactly zero: the drain wait is woken once. */
	netvsc_send_acct(fx.ndev, fx.nvdev, fx.channel, 0);
	KUNIT_EXPECT_EQ(test, woken, 1U);
	KUNIT_EXPECT_EQ(test, netvsc_send_sends(&fx, 0), 0);

	remove_wait_queue(&fx.nvdev->wait_drain, &entry);
	netvsc_completion_fixture_exit(&fx);
}

/*
 * netvsc_send_tx_complete() applies the same sentinel rejection and
 * still accounts a NULL skb.
 */
static void netvsc_tx_complete_null_skb_acct_test(struct kunit *test)
{
	struct netvsc_completion_fixture fx = {};
	struct netvsc_empty_desc pkt;

	KUNIT_ASSERT_EQ(test, netvsc_completion_fixture_init(&fx), 0);
	atomic_set(&fx.nvdev->chan_table[0].queue_sends, 3);
	make_empty_desc(&pkt, 0x77);
	fx.once_addr = 0;

	netvsc_send_tx_complete(fx.ndev, fx.nvdev, fx.channel, &pkt.desc, 0);
	KUNIT_EXPECT_EQ(test, netvsc_send_sends(&fx, 0), 2);

	/* The sentinel is rejected before any accounting. */
	make_empty_desc(&pkt, 0x78);
	fx.once_addr = VMBUS_RQST_ERROR;
	fx.calls = 0;
	netvsc_send_tx_complete(fx.ndev, fx.nvdev, fx.channel, &pkt.desc, 0);
	KUNIT_EXPECT_EQ(test, netvsc_send_sends(&fx, 0), 2);

	netvsc_completion_fixture_exit(&fx);
}

static void netvsc_test_skb_destructor(struct sk_buff *skb)
{
	active_fx->skb_frees++;
}

static void netvsc_tx_complete_skb(struct kunit *test, u32 status)
{
	struct netvsc_completion_fixture fx = {};
	struct {
		struct vmpacket_descriptor desc;
		struct nvsp_message message;
	} pkt = {};
	unsigned long send_slots = BIT(3);
	struct hv_netvsc_packet *packet;
	struct netvsc_stats_tx *stats;
	struct sk_buff *skb;

	KUNIT_ASSERT_EQ(test, netvsc_completion_fixture_init(&fx), 0);
	skb = alloc_skb(64, GFP_KERNEL);
	if (!skb) {
		KUNIT_FAIL(test, "skb allocation failed");
		goto out;
	}
	skb->destructor = netvsc_test_skb_destructor;
	packet = (struct hv_netvsc_packet *)skb->cb;
	memset(packet, 0, sizeof(*packet));
	packet->send_buf_index = 3;
	packet->q_idx = 1;
	packet->total_packets = 2;
	packet->total_bytes = 64;
	fx.nvdev->send_section_map = &send_slots;
	stats = &fx.nvdev->chan_table[1].tx_stats;
	u64_stats_init(&stats->syncp);
	atomic_set(&fx.nvdev->chan_table[0].queue_sends, 7);
	atomic_set(&fx.nvdev->chan_table[1].queue_sends, 3);
	fx.once_addr = (u64)(unsigned long)skb;
	pkt.desc.offset8 = sizeof(pkt.desc) / 8;
	pkt.desc.len8 = DIV_ROUND_UP(sizeof(pkt), 8);
	pkt.desc.trans_id = 0x79;
	pkt.message.hdr.msg_type = NVSP_MSG1_TYPE_SEND_RNDIS_PKT_COMPLETE;
	pkt.message.msg.v1_msg.send_rndis_pkt_complete.status = status;

	/* Budget zero consumes the real skb synchronously, outside NAPI. */
	netvsc_send_completion(fx.ndev, fx.nvdev, fx.channel, &pkt.desc, 0);
	KUNIT_EXPECT_EQ(test, send_slots, 0UL);
	KUNIT_EXPECT_EQ(test, fx.skb_frees, 1U);
	KUNIT_EXPECT_EQ(test, stats->packets, 2ULL);
	KUNIT_EXPECT_EQ(test, stats->bytes, 64ULL);
	KUNIT_EXPECT_EQ(test, netvsc_send_sends(&fx, 0), 7);
	KUNIT_EXPECT_EQ(test, netvsc_send_sends(&fx, 1), 2);

	/* The consumed transaction cannot free or account the skb twice. */
	netvsc_send_completion(fx.ndev, fx.nvdev, fx.channel, &pkt.desc, 0);
	KUNIT_EXPECT_EQ(test, send_slots, 0UL);
	KUNIT_EXPECT_EQ(test, fx.skb_frees, 1U);
	KUNIT_EXPECT_EQ(test, stats->packets, 2ULL);
	KUNIT_EXPECT_EQ(test, stats->bytes, 64ULL);
	KUNIT_EXPECT_EQ(test, netvsc_send_sends(&fx, 1), 2);
out:
	netvsc_completion_fixture_exit(&fx);
}

static void netvsc_tx_complete_skb_success_test(struct kunit *test)
{
	netvsc_tx_complete_skb(test, NVSP_STAT_SUCCESS);
}

static void netvsc_tx_complete_skb_error_test(struct kunit *test)
{
	netvsc_tx_complete_skb(test, NVSP_STAT_FAIL);
}

static struct kunit_case netvsc_completion_test_cases[] = {
	KUNIT_CASE(netvsc_completion_null_address_acct_test),
	KUNIT_CASE(netvsc_completion_rqst_error_sentinel_test),
	KUNIT_CASE(netvsc_completion_no_rqstor_sentinel_test),
	KUNIT_CASE(netvsc_completion_switch_data_path_test),
	KUNIT_CASE(netvsc_completion_unknown_msg_type_test),
	KUNIT_CASE(netvsc_completion_duplicate_completion_test),
	KUNIT_CASE(netvsc_completion_single_decrement_test),
	KUNIT_CASE(netvsc_send_acct_drain_wake_test),
	KUNIT_CASE(netvsc_tx_complete_null_skb_acct_test),
	KUNIT_CASE(netvsc_tx_complete_skb_success_test),
	KUNIT_CASE(netvsc_tx_complete_skb_error_test),
	{}
};

static struct kunit_suite netvsc_completion_test_suite = {
	.name = "hyperv-netvsc-completion",
	.test_cases = netvsc_completion_test_cases,
};

kunit_test_suite(netvsc_completion_test_suite);
