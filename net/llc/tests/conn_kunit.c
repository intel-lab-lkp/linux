// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the LLC type 2 connection state machine.
 *
 * This file is #included by llc_conn.c so that the tests can reach the
 * static helpers of the state machine.
 */
#include <kunit/test.h>
#include <linux/net.h>
#include <net/net_namespace.h>

/*
 * Build the smallest event that reaches the LLC_CONN_STATE_ADM catch-all
 * transition: an I format command PDU with the P bit clear. It matches
 * neither llc_conn_ev_rx_sabme_cmd_pbit_set_x(),
 * llc_conn_ev_rx_disc_cmd_pbit_set_x() nor
 * llc_conn_ev_rx_xxx_cmd_pbit_set_1(), so llc_adm_state_trans_5 wins.
 */
static struct sk_buff *llc_conn_test_rx_pdu(struct kunit *test, struct sock *sk)
{
	struct llc_conn_state_ev *ev;
	struct llc_pdu_sn *pdu;
	struct sk_buff *skb;

	skb = alloc_skb(sizeof(*pdu), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, skb);

	skb_reset_network_header(skb);
	pdu = skb_put(skb, sizeof(*pdu));
	pdu->dsap   = 0x42;
	pdu->ssap   = LLC_PDU_CMD;
	pdu->ctrl_1 = LLC_PDU_TYPE_I;
	pdu->ctrl_2 = 0;

	skb->sk = sk;
	ev = llc_conn_ev(skb);
	memset(ev, 0, sizeof(*ev));
	ev->type = LLC_CONN_EV_TYPE_PDU;

	return skb;
}

static struct socket *llc_conn_test_socket(struct kunit *test)
{
	struct socket *sock;
	int rc;

	rc = sock_create_kern(&init_net, PF_LLC, SOCK_DGRAM, 0, &sock);
	if (rc)
		kunit_skip(test, "cannot create a PF_LLC socket: %d", rc);

	return sock;
}

/*
 * llc_conn_state_table[] and llc_offset_table[] are indexed with "state - 1",
 * which only works while every state is its own 1-based index.
 */
static void llc_conn_state_table_is_one_based(struct kunit *test)
{
	u8 state;

	for (state = LLC_CONN_STATE_ADM; state <= LLC_CONN_STATE_TEMP; state++)
		KUNIT_EXPECT_EQ(test, llc_conn_state_table[state - 1].current_state,
				state);
}

static void llc_conn_state_in_service_bounds(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test, llc_conn_state_in_service(LLC_CONN_OUT_OF_SVC));
	KUNIT_EXPECT_TRUE(test, llc_conn_state_in_service(LLC_CONN_STATE_ADM));
	KUNIT_EXPECT_TRUE(test, llc_conn_state_in_service(LLC_CONN_STATE_TEMP));
	KUNIT_EXPECT_FALSE(test, llc_conn_state_in_service(LLC_CONN_STATE_TEMP + 1));
	KUNIT_EXPECT_FALSE(test, llc_conn_state_in_service(U8_MAX));
}

/*
 * Regression test for the syzbot report below: an unsolicited frame moves a
 * socket sitting in LLC_CONN_STATE_ADM to LLC_CONN_OUT_OF_SVC, and the next
 * frame for the same socket used to index llc_conn_state_table[-1] and
 * llc_offset_table[-1][] before it was dropped.
 *
 * Link: https://lore.kernel.org/all/6a95888b.4d659fcc.734b4.0051.GAE@google.com
 */
static void llc_conn_state_process_out_of_svc(struct kunit *test)
{
	struct sk_buff *first, *second;
	struct socket *sock;
	struct sock *sk;

	sock = llc_conn_test_socket(test);
	sk = sock->sk;

	first = llc_conn_test_rx_pdu(test, sk);
	second = llc_conn_test_rx_pdu(test, sk);

	lock_sock(sk);
	KUNIT_EXPECT_EQ(test, llc_sk(sk)->state, LLC_CONN_STATE_ADM);

	/* The catch-all ADM transition parks the socket out of service. */
	KUNIT_EXPECT_EQ(test, llc_conn_state_process(sk, first), 0);
	KUNIT_EXPECT_EQ(test, llc_sk(sk)->state, LLC_CONN_OUT_OF_SVC);

	/* The next event must be refused rather than indexed with -1. */
	KUNIT_EXPECT_NE(test, llc_conn_state_process(sk, second), 0);
	KUNIT_EXPECT_EQ(test, llc_sk(sk)->state, LLC_CONN_OUT_OF_SVC);
	release_sock(sk);

	sock_release(sock);
}

/* The same refusal has to cover states past the end of the state table. */
static void llc_conn_state_process_bad_state(struct kunit *test)
{
	struct socket *sock;
	struct sk_buff *skb;
	struct sock *sk;

	sock = llc_conn_test_socket(test);
	sk = sock->sk;

	skb = llc_conn_test_rx_pdu(test, sk);

	lock_sock(sk);
	llc_sk(sk)->state = LLC_CONN_STATE_TEMP + 1;
	KUNIT_EXPECT_NE(test, llc_conn_state_process(sk, skb), 0);
	llc_sk(sk)->state = LLC_CONN_STATE_ADM;
	release_sock(sk);

	sock_release(sock);
}

static struct kunit_case llc_conn_test_cases[] = {
	KUNIT_CASE(llc_conn_state_table_is_one_based),
	KUNIT_CASE(llc_conn_state_in_service_bounds),
	KUNIT_CASE(llc_conn_state_process_out_of_svc),
	KUNIT_CASE(llc_conn_state_process_bad_state),
	{}
};

static struct kunit_suite llc_conn_test_suite = {
	.name = "llc2_conn",
	.test_cases = llc_conn_test_cases,
};

kunit_test_suite(llc_conn_test_suite);
