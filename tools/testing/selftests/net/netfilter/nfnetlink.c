#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <arpa/inet.h>
#include <libmnl/libmnl.h>
#include <linux/netfilter/nfnetlink.h>
#include <linux/netfilter/nf_tables.h>
#include <linux/netfilter/nf_conntrack_common.h>
#include <linux/netfilter.h>
#include <sys/types.h>

#include "../../kselftest_harness.h"

#define BATCH_PAGE_SIZE 8192

static bool batch_begin(struct mnl_nlmsg_batch *batch, uint32_t seq)
{
	struct nlmsghdr *nlh;
	struct nfgenmsg *nfh;

	nlh = mnl_nlmsg_put_header(mnl_nlmsg_batch_current(batch));
	nlh->nlmsg_type = NFNL_MSG_BATCH_BEGIN;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	nlh->nlmsg_seq = seq;

	nfh = mnl_nlmsg_put_extra_header(nlh, sizeof(struct nfgenmsg));
	nfh->nfgen_family = NFPROTO_UNSPEC;
	nfh->version = NFNETLINK_V0;
	nfh->res_id = NFNL_SUBSYS_NFTABLES;

	return mnl_nlmsg_batch_next(batch);
}

static bool batch_end(struct mnl_nlmsg_batch *batch, uint32_t seq)
{
	struct nlmsghdr *nlh;
	struct nfgenmsg *nfh;

	nlh = mnl_nlmsg_put_header(mnl_nlmsg_batch_current(batch));
	nlh->nlmsg_type = NFNL_MSG_BATCH_END;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	nlh->nlmsg_seq = seq;

	nfh = mnl_nlmsg_put_extra_header(nlh, sizeof(struct nfgenmsg));
	nfh->nfgen_family = NFPROTO_UNSPEC;
	nfh->version = NFNETLINK_V0;
	nfh->res_id = NFNL_SUBSYS_NFTABLES;

	return mnl_nlmsg_batch_next(batch);
}

static bool
create_table(struct mnl_nlmsg_batch *batch, const char *name, uint32_t seq)
{
	struct nlmsghdr *nlh;
	struct nfgenmsg *nfh;

	nlh = mnl_nlmsg_put_header(mnl_nlmsg_batch_current(batch));
	nlh->nlmsg_type = (NFNL_SUBSYS_NFTABLES << 8) | NFT_MSG_NEWTABLE;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_ACK;
	nlh->nlmsg_seq = seq;

	nfh = mnl_nlmsg_put_extra_header(nlh, sizeof(struct nfgenmsg));
	nfh->nfgen_family = NFPROTO_INET;
	nfh->version = NFNETLINK_V0;
	nfh->res_id = 0;

	mnl_attr_put_strz(nlh, NFTA_TABLE_NAME, name);

	return mnl_nlmsg_batch_next(batch);
}

static bool
create_chain(struct mnl_nlmsg_batch *batch, const char *table,
	     const char *chain, uint32_t seq)
{
	struct nlmsghdr *nlh;
	struct nfgenmsg *nfh;

	nlh = mnl_nlmsg_put_header(mnl_nlmsg_batch_current(batch));
	nlh->nlmsg_type = (NFNL_SUBSYS_NFTABLES << 8) | NFT_MSG_NEWCHAIN;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_ACK;
	nlh->nlmsg_seq = seq;

	nfh = mnl_nlmsg_put_extra_header(nlh, sizeof(struct nfgenmsg));
	nfh->nfgen_family = NFPROTO_INET;
	nfh->version = NFNETLINK_V0;
	nfh->res_id = 0;

	mnl_attr_put_strz(nlh, NFTA_CHAIN_TABLE, table);
	mnl_attr_put_strz(nlh, NFTA_CHAIN_NAME, chain);

	return mnl_nlmsg_batch_next(batch);
}

static bool
create_rule_with_ct(struct mnl_nlmsg_batch *batch, const char *table,
		    const char *chain, uint32_t seq)
{
	struct nlmsghdr *nlh;
	struct nfgenmsg *nfh;
	struct nlattr *nest1, *nest2, *nest3, *nest4;

	nlh = mnl_nlmsg_put_header(mnl_nlmsg_batch_current(batch));
	nlh->nlmsg_type = (NFNL_SUBSYS_NFTABLES << 8) | NFT_MSG_NEWRULE;
	nlh->nlmsg_flags =
	    NLM_F_REQUEST | NLM_F_CREATE | NLM_F_APPEND | NLM_F_ACK;
	nlh->nlmsg_seq = seq;

	nfh = mnl_nlmsg_put_extra_header(nlh, sizeof(struct nfgenmsg));
	nfh->nfgen_family = NFPROTO_INET;
	nfh->version = NFNETLINK_V0;
	nfh->res_id = 0;

	mnl_attr_put_strz(nlh, NFTA_RULE_TABLE, table);
	mnl_attr_put_strz(nlh, NFTA_RULE_CHAIN, chain);

	/* Create expressions list */
	nest1 = mnl_attr_nest_start(nlh, NFTA_RULE_EXPRESSIONS);

	/* CT expression to load ct state */
	nest2 = mnl_attr_nest_start(nlh, NFTA_LIST_ELEM);
	mnl_attr_put_strz(nlh, NFTA_EXPR_NAME, "ct");

	nest3 = mnl_attr_nest_start(nlh, NFTA_EXPR_DATA);
	mnl_attr_put_u32(nlh, NFTA_CT_KEY, htonl(NFT_CT_STATE));
	mnl_attr_put_u32(nlh, NFTA_CT_DREG, htonl(NFT_REG_1));
	mnl_attr_nest_end(nlh, nest3);

	mnl_attr_nest_end(nlh, nest2);

	/* Bitwise expression */
	nest2 = mnl_attr_nest_start(nlh, NFTA_LIST_ELEM);
	mnl_attr_put_strz(nlh, NFTA_EXPR_NAME, "bitwise");

	nest3 = mnl_attr_nest_start(nlh, NFTA_EXPR_DATA);
	mnl_attr_put_u32(nlh, NFTA_BITWISE_SREG, htonl(NFT_REG_1));
	mnl_attr_put_u32(nlh, NFTA_BITWISE_DREG, htonl(NFT_REG_1));
	mnl_attr_put_u32(nlh, NFTA_BITWISE_LEN, htonl(4));

	uint32_t mask =
	    NF_CT_STATE_BIT(IP_CT_ESTABLISHED) | NF_CT_STATE_BIT(IP_CT_RELATED);
	nest4 = mnl_attr_nest_start(nlh, NFTA_BITWISE_MASK);
	mnl_attr_put(nlh, NFTA_DATA_VALUE, sizeof(mask), &mask);
	mnl_attr_nest_end(nlh, nest4);

	uint32_t val = 0x00000000;
	nest4 = mnl_attr_nest_start(nlh, NFTA_BITWISE_XOR);
	mnl_attr_put(nlh, NFTA_DATA_VALUE, sizeof(val), &val);
	mnl_attr_nest_end(nlh, nest4);

	mnl_attr_nest_end(nlh, nest3);
	mnl_attr_nest_end(nlh, nest2);

	/* Cmp expression */
	nest2 = mnl_attr_nest_start(nlh, NFTA_LIST_ELEM);
	mnl_attr_put_strz(nlh, NFTA_EXPR_NAME, "cmp");

	nest3 = mnl_attr_nest_start(nlh, NFTA_EXPR_DATA);
	mnl_attr_put_u32(nlh, NFTA_CMP_SREG, htonl(NFT_REG_1));
	mnl_attr_put_u32(nlh, NFTA_CMP_OP, htonl(NFT_CMP_NEQ));

	uint32_t cmp_data = 0x00000000;
	nest4 = mnl_attr_nest_start(nlh, NFTA_CMP_DATA);
	mnl_attr_put(nlh, NFTA_DATA_VALUE, sizeof(cmp_data), &cmp_data);
	mnl_attr_nest_end(nlh, nest4);

	mnl_attr_nest_end(nlh, nest3);
	mnl_attr_nest_end(nlh, nest2);

	/* Counter expression */
	nest2 = mnl_attr_nest_start(nlh, NFTA_LIST_ELEM);
	mnl_attr_put_strz(nlh, NFTA_EXPR_NAME, "counter");

	mnl_attr_nest_end(nlh, nest2);

	mnl_attr_nest_end(nlh, nest1);

	return mnl_nlmsg_batch_next(batch);
}

static bool create_invalid_table(struct mnl_nlmsg_batch *batch, uint32_t seq)
{
	struct nlmsghdr *nlh;
	struct nfgenmsg *nfh;

	nlh = mnl_nlmsg_put_header(mnl_nlmsg_batch_current(batch));
	nlh->nlmsg_type = (NFNL_SUBSYS_NFTABLES << 8) | NFT_MSG_NEWTABLE;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_ACK;
	nlh->nlmsg_seq = seq;

	nfh = mnl_nlmsg_put_extra_header(nlh, sizeof(struct nfgenmsg));
	nfh->nfgen_family = NFPROTO_INET;
	nfh->version = NFNETLINK_V0;
	nfh->res_id = 0;

	/* Intentionally omit table name attribute to cause error */

	return mnl_nlmsg_batch_next(batch);
}

static void validate_res(struct mnl_socket *nl, uint32_t first_seq,
			 uint32_t expected_acks,
			 struct __test_metadata *_metadata)
{
	char buf[BATCH_PAGE_SIZE];
	int ret, acks_received = 0;
	uint32_t last_seq = 0;
	bool out_of_order = false;

	while (1) {
		ret = mnl_socket_recvfrom(nl, buf, sizeof(buf));
		if (ret == -1) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				break;
			}

			TH_LOG("Unexpected error on recv: %s", strerror(errno));
			ASSERT_TRUE(false);
			return;
		}

		struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
		while (mnl_nlmsg_ok(nlh, ret)) {
			if (nlh->nlmsg_type == NLMSG_ERROR) {
				struct nlmsgerr *err =
				    mnl_nlmsg_get_payload(nlh);
				acks_received++;

				if (err->error != 0) {
					TH_LOG("[seq=%u] Error: %s",
					       nlh->nlmsg_seq,
					       strerror(-err->error));
				} else {
					TH_LOG("[seq=%u] ACK", nlh->nlmsg_seq);
				}

				if (nlh->nlmsg_seq <= last_seq) {
					out_of_order = true;
					TH_LOG
					    ("Out of order ack: seq %u after %u",
					     nlh->nlmsg_seq, last_seq);
				}
				last_seq = nlh->nlmsg_seq;
			}
			nlh = mnl_nlmsg_next(nlh, &ret);
		}
	}

	ASSERT_FALSE(out_of_order);

	ASSERT_EQ(acks_received, expected_acks);
}

FIXTURE(nfnetlink_batch)
{
	struct mnl_socket *nl;
};

FIXTURE_SETUP(nfnetlink_batch)
{
	struct timeval tv = {.tv_sec = 1,.tv_usec = 0 };

	self->nl = mnl_socket_open(NETLINK_NETFILTER);
	ASSERT_NE(self->nl, NULL);

	ASSERT_EQ(mnl_socket_bind(self->nl, 0, MNL_SOCKET_AUTOPID), 0);

	ASSERT_EQ(setsockopt
		  (mnl_socket_get_fd(self->nl), SOL_SOCKET, SO_RCVTIMEO, &tv,
		   sizeof(tv)), 0);
}

FIXTURE_TEARDOWN(nfnetlink_batch)
{
	if (self->nl) {
		mnl_socket_close(self->nl);
	}
}

TEST_F(nfnetlink_batch, simple_batch)
{
	struct mnl_nlmsg_batch *batch;
	char buf[BATCH_PAGE_SIZE];
	uint32_t seq = time(NULL);
	int ret;

	batch = mnl_nlmsg_batch_start(buf, sizeof(buf));
	ASSERT_NE(batch, NULL);

	ASSERT_TRUE(batch_begin(batch, seq++));
	create_table(batch, "test_table1", seq++);
	batch_end(batch, seq++);

	ret = mnl_socket_sendto(self->nl, mnl_nlmsg_batch_head(batch),
				mnl_nlmsg_batch_size(batch));
	ASSERT_GT(ret, 0);

	// Expect 3 acks: batch_begin, create_table, batch_end
	validate_res(self->nl, seq - 3, 3, _metadata);

	mnl_nlmsg_batch_stop(batch);
}

TEST_F(nfnetlink_batch, module_load)
{
	struct mnl_nlmsg_batch *batch;
	char buf[BATCH_PAGE_SIZE];
	uint32_t seq = time(NULL) + 1000;
	int ret;

	batch = mnl_nlmsg_batch_start(buf, sizeof(buf));
	ASSERT_NE(batch, NULL);

	batch_begin(batch, seq++);
	create_table(batch, "test_table2", seq++);
	create_chain(batch, "test_table2", "test_chain", seq++);
	create_rule_with_ct(batch, "test_table2", "test_chain", seq++);
	batch_end(batch, seq++);

	ret = mnl_socket_sendto(self->nl, mnl_nlmsg_batch_head(batch),
				mnl_nlmsg_batch_size(batch));
	ASSERT_GT(ret, 0);

	// Expect 5 acks: batch_begin, table, chain, rule, batch_end
	validate_res(self->nl, seq - 5, 5, _metadata);

	mnl_nlmsg_batch_stop(batch);
}

// Repeat the CT test to verify module loading behavior
TEST_F(nfnetlink_batch, post_module_load)
{
	struct mnl_nlmsg_batch *batch;
	char buf[BATCH_PAGE_SIZE];
	uint32_t seq = time(NULL) + 2000;
	int ret;

	batch = mnl_nlmsg_batch_start(buf, sizeof(buf));
	ASSERT_NE(batch, NULL);

	batch_begin(batch, seq++);
	create_table(batch, "test_table6", seq++);
	create_chain(batch, "test_table6", "test_chain", seq++);
	create_rule_with_ct(batch, "test_table6", "test_chain", seq++);
	batch_end(batch, seq++);

	ret = mnl_socket_sendto(self->nl, mnl_nlmsg_batch_head(batch),
				mnl_nlmsg_batch_size(batch));
	ASSERT_GT(ret, 0);

	validate_res(self->nl, seq - 5, 5, _metadata);

	mnl_nlmsg_batch_stop(batch);
}

TEST_F(nfnetlink_batch, invalid_batch)
{
	struct mnl_nlmsg_batch *batch;
	char buf[BATCH_PAGE_SIZE];
	uint32_t seq = time(NULL) + 3000;
	int ret;

	batch = mnl_nlmsg_batch_start(buf, sizeof(buf));
	ASSERT_NE(batch, NULL);

	batch_begin(batch, seq++);
	create_table(batch, "test_table3", seq++);
	create_invalid_table(batch, seq++);
	create_chain(batch, "test_table3", "test_chain", seq++);
	batch_end(batch, seq++);

	ret = mnl_socket_sendto(self->nl, mnl_nlmsg_batch_head(batch),
				mnl_nlmsg_batch_size(batch));
	ASSERT_GT(ret, 0);

	// Expect 5 acks: batch_begin, table, invalid_table(error), chain, batch_end
	validate_res(self->nl, seq - 5, 5, _metadata);

	mnl_nlmsg_batch_stop(batch);
}

TEST_F(nfnetlink_batch, batch_with_fatal_error)
{
	struct mnl_nlmsg_batch *batch;
	char buf[BATCH_PAGE_SIZE];
	uint32_t seq = time(NULL) + 4000;
	uid_t uid;
	int ret;

	batch = mnl_nlmsg_batch_start(buf, sizeof(buf));
	ASSERT_NE(batch, NULL);

	batch_begin(batch, seq++);
	create_table(batch, "test_table4", seq++);
	create_table(batch, "test_table5", seq++);
	batch_end(batch, seq++);

	// Drop privileges to trigger EPERM
	uid = geteuid();
	if (uid == 0) {
		ASSERT_EQ(seteuid(65534), 0);
	}

	ret = mnl_socket_sendto(self->nl, mnl_nlmsg_batch_head(batch),
				mnl_nlmsg_batch_size(batch));

	// Restore privileges
	if (uid == 0) {
		seteuid(uid);
	}

	ASSERT_GT(ret, 0);

	// EPERM is fatal and should abort batch, expect only 1 ACK
	validate_res(self->nl, seq - 4, 1, _metadata);

	mnl_nlmsg_batch_stop(batch);
}

TEST_HARNESS_MAIN
