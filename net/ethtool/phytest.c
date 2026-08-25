// SPDX-License-Identifier: GPL-2.0-only

#include "netlink.h"
#include "common.h"

struct phy_test_req_info {
	struct ethnl_req_info base;
};

struct phy_test_reply_data {
	struct ethnl_reply_data base;
	struct ethtool_phy_test test;
};

#define PHY_TEST_REPDATA(__reply_base) \
	container_of(__reply_base, struct phy_test_reply_data, base)

/* PHY_TEST_GET */

const struct nla_policy ethnl_phy_test_get_policy[ETHTOOL_A_PHY_TEST_LANE + 1] = {
	[ETHTOOL_A_PHY_TEST_HEADER]	= NLA_POLICY_NESTED(ethnl_header_policy),
	[ETHTOOL_A_PHY_TEST_BLOCK_ID]	= { .type = NLA_U32 },
	[ETHTOOL_A_PHY_TEST_LANE]	= { .type = NLA_U32 },
};

static int phy_test_reply_size(const struct ethnl_req_info *req_base,
			       const struct ethnl_reply_data *reply_base)
{
	return nla_total_size(sizeof(u32)) +	/* block_id */
	       nla_total_size(sizeof(u32)) +	/* lane */
	       nla_total_size(sizeof(u32)) +	/* tx_pattern */
	       nla_total_size(sizeof(u32)) +	/* rx_pattern */
	       nla_total_size(sizeof(u32)) +	/* active_tests */
	       nla_total_size(sizeof(u8))  +	/* checker_lock */
	       nla_total_size(sizeof(u64)) +	/* error_count */
	       nla_total_size(sizeof(u64));	/* total_bits_sent */
}

static int phy_test_prepare_data(const struct ethnl_req_info *req_base,
				 struct ethnl_reply_data *reply_base,
				 const struct genl_info *info)
{
	struct phy_test_reply_data *data = PHY_TEST_REPDATA(reply_base);
	struct net_device *dev = reply_base->dev;
	struct nlattr **tb = info->attrs;

	if (!dev->ethtool_ops->get_phy_test)
		return -EOPNOTSUPP;

	memset(&data->test, 0, sizeof(data->test));

	if (tb[ETHTOOL_A_PHY_TEST_BLOCK_ID]) {
		data->test.block_id = nla_get_u32(tb[ETHTOOL_A_PHY_TEST_BLOCK_ID]);
		data->test.cmd |= PHY_TEST_CMD_BLOCK_ID;
	}
	if (tb[ETHTOOL_A_PHY_TEST_LANE]) {
		data->test.lane = nla_get_u32(tb[ETHTOOL_A_PHY_TEST_LANE]);
		data->test.cmd |= PHY_TEST_CMD_LANE;
	}

	return dev->ethtool_ops->get_phy_test(dev, &data->test);
}

static int phy_test_fill_reply(struct sk_buff *skb,
			       const struct ethnl_req_info *req_base,
			       const struct ethnl_reply_data *reply_base)
{
	const struct phy_test_reply_data *data = PHY_TEST_REPDATA(reply_base);
	const struct ethtool_phy_test *t = &data->test;

	if (nla_put_u32(skb, ETHTOOL_A_PHY_TEST_BLOCK_ID, t->block_id) ||
	    nla_put_u32(skb, ETHTOOL_A_PHY_TEST_LANE, t->lane) ||
	    nla_put_u32(skb, ETHTOOL_A_PHY_TEST_TX_PATTERN, t->tx_pattern) ||
	    nla_put_u32(skb, ETHTOOL_A_PHY_TEST_RX_PATTERN, t->rx_pattern) ||
	    nla_put_u32(skb, ETHTOOL_A_PHY_TEST_ACTIVE_TESTS,
			t->active_tests) ||
	    nla_put_u8(skb, ETHTOOL_A_PHY_TEST_CHECKER_LOCK, t->checker_lock) ||
	    nla_put_u64_64bit(skb, ETHTOOL_A_PHY_TEST_ERROR_COUNT,
			      t->error_count, ETHTOOL_A_PHY_TEST_UNSPEC) ||
	    nla_put_u64_64bit(skb, ETHTOOL_A_PHY_TEST_TOTAL_BITS_SENT,
			      t->total_bits_sent, ETHTOOL_A_PHY_TEST_UNSPEC))
		return -EMSGSIZE;

	return 0;
}

const struct ethnl_request_ops ethnl_phy_test_request_ops = {
	.request_cmd	= ETHTOOL_MSG_PHY_TEST_GET,
	.reply_cmd	= ETHTOOL_MSG_PHY_TEST_GET_REPLY,
	.hdr_attr	= ETHTOOL_A_PHY_TEST_HEADER,
	.req_info_size	= sizeof(struct phy_test_req_info),
	.reply_data_size = sizeof(struct phy_test_reply_data),
	.prepare_data	= phy_test_prepare_data,
	.reply_size	= phy_test_reply_size,
	.fill_reply	= phy_test_fill_reply,
};

/* PHY_TEST_ACT */

const struct nla_policy ethnl_phy_test_act_policy[ETHTOOL_A_PHY_TEST_MAX + 1] = {
	[ETHTOOL_A_PHY_TEST_HEADER]		= NLA_POLICY_NESTED(ethnl_header_policy),
	[ETHTOOL_A_PHY_TEST_BLOCK_ID]		= { .type = NLA_U32 },
	[ETHTOOL_A_PHY_TEST_LANE]		= { .type = NLA_U32 },
	[ETHTOOL_A_PHY_TEST_TX_PATTERN]		= { .type = NLA_U32 },
	[ETHTOOL_A_PHY_TEST_RX_PATTERN]		= { .type = NLA_U32 },
	[ETHTOOL_A_PHY_TEST_BERT_ACTION]	= { .type = NLA_U32 },
	[ETHTOOL_A_PHY_TEST_INJECT_ERROR_COUNT]	= { .type = NLA_U32 },
};

int ethnl_act_phy_test(struct sk_buff *skb, struct genl_info *info)
{
	struct ethnl_req_info req_info = {};
	struct nlattr **tb = info->attrs;
	struct ethtool_phy_test test = {};
	struct net_device *dev;
	int ret;

	ret = ethnl_parse_header_dev_get(&req_info,
					 tb[ETHTOOL_A_PHY_TEST_HEADER],
					 genl_info_net(info), info->extack,
					 true);
	if (ret < 0)
		return ret;

	dev = req_info.dev;

	if (!dev->ethtool_ops->set_phy_test) {
		ret = -EOPNOTSUPP;
		goto out_dev;
	}

	if (tb[ETHTOOL_A_PHY_TEST_BLOCK_ID]) {
		test.block_id = nla_get_u32(tb[ETHTOOL_A_PHY_TEST_BLOCK_ID]);
		test.cmd |= PHY_TEST_CMD_BLOCK_ID;
	}
	if (tb[ETHTOOL_A_PHY_TEST_LANE]) {
		test.lane = nla_get_u32(tb[ETHTOOL_A_PHY_TEST_LANE]);
		test.cmd |= PHY_TEST_CMD_LANE;
	}
	if (tb[ETHTOOL_A_PHY_TEST_TX_PATTERN]) {
		test.tx_pattern = nla_get_u32(tb[ETHTOOL_A_PHY_TEST_TX_PATTERN]);
		test.cmd |= PHY_TEST_CMD_TX_PATTERN;
	}
	if (tb[ETHTOOL_A_PHY_TEST_RX_PATTERN]) {
		test.rx_pattern = nla_get_u32(tb[ETHTOOL_A_PHY_TEST_RX_PATTERN]);
		test.cmd |= PHY_TEST_CMD_RX_PATTERN;
	}
	if (tb[ETHTOOL_A_PHY_TEST_BERT_ACTION]) {
		test.bert_action = nla_get_u32(tb[ETHTOOL_A_PHY_TEST_BERT_ACTION]);
		test.cmd |= PHY_TEST_CMD_BERT_ACTION;
	}
	if (tb[ETHTOOL_A_PHY_TEST_INJECT_ERROR_COUNT]) {
		test.inject_error_count =
			nla_get_u32(tb[ETHTOOL_A_PHY_TEST_INJECT_ERROR_COUNT]);
		test.cmd |= PHY_TEST_CMD_INJECT_COUNT;
	}

	rtnl_lock();
	ret = ethnl_ops_begin(dev);
	if (ret < 0)
		goto out_rtnl;

	ret = dev->ethtool_ops->set_phy_test(dev, &test);
	ethnl_ops_complete(dev);

out_rtnl:
	rtnl_unlock();
out_dev:
	ethnl_parse_header_dev_put(&req_info);
	return ret;
}
