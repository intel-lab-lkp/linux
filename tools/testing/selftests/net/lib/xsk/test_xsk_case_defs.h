/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Each XSK_TEST_CASE entry has a name, function, and hardware directions.
 * Only xsk.py reads the directions, the DUT roles in which it runs a case.
 *
 * Keep this list in test ID order. It is shared by xskxceiver and xsk.py.
 */
XSK_TEST_CASE(SEND_RECEIVE, testapp_send_receive, XSK_HW_RX | XSK_HW_TX)
XSK_TEST_CASE(SEND_RECEIVE_2K_FRAME, testapp_send_receive_2k_frame,
	      XSK_HW_RX | XSK_HW_TX)
XSK_TEST_CASE(SEND_RECEIVE_SINGLE_PKT, testapp_single_pkt,
	      XSK_HW_RX | XSK_HW_TX)
XSK_TEST_CASE(POLL_RX, testapp_poll_rx, XSK_HW_RX)
XSK_TEST_CASE(POLL_TX, testapp_poll_tx, XSK_HW_TX)
XSK_TEST_CASE(POLL_RXQ_FULL, testapp_poll_rxq_tmout, XSK_HW_RX)
XSK_TEST_CASE(POLL_TXQ_FULL, testapp_poll_txq_tmout, XSK_HW_TX)
XSK_TEST_CASE(ALIGNED_INV_DESC, testapp_aligned_inv_desc, XSK_HW_TX)
XSK_TEST_CASE(ALIGNED_INV_DESC_2K_FRAME_SIZE,
	      testapp_aligned_inv_desc_2k_frame, XSK_HW_TX)
XSK_TEST_CASE(UMEM_HEADROOM, testapp_headroom, XSK_HW_RX)
XSK_TEST_CASE(BIDIRECTIONAL, testapp_bidirectional, 0)
XSK_TEST_CASE(STAT_RX_DROPPED, testapp_stats_rx_dropped, 0)
XSK_TEST_CASE(STAT_TX_INVALID, testapp_stats_tx_invalid_descs, XSK_HW_TX)
XSK_TEST_CASE(STAT_RX_FULL, testapp_stats_rx_full, 0)
XSK_TEST_CASE(STAT_FILL_EMPTY, testapp_stats_fill_empty, 0)
XSK_TEST_CASE(XDP_PROG_CLEANUP, testapp_xdp_prog_cleanup, XSK_HW_RX)
XSK_TEST_CASE(XDP_DROP_HALF, testapp_xdp_drop, 0)
XSK_TEST_CASE(XDP_SHARED_UMEM, testapp_xdp_shared_umem, 0)
XSK_TEST_CASE(XDP_METADATA_COPY, testapp_xdp_metadata, XSK_HW_RX)
XSK_TEST_CASE(XDP_METADATA_COPY_MULTI_BUFF, testapp_xdp_metadata_mb, 0)
XSK_TEST_CASE(ALIGNED_INV_DESC_MULTI_BUFF, testapp_aligned_inv_desc_mb,
	      XSK_HW_TX)
XSK_TEST_CASE(TOO_MANY_FRAGS, testapp_too_many_frags, XSK_HW_TX)
XSK_TEST_CASE(XDP_ADJUST_TAIL_SHRINK, testapp_adjust_tail_shrink, 0)
XSK_TEST_CASE(TX_QUEUE_CONSUMER, testapp_tx_queue_consumer, 0)
/* Flaky tests */
XSK_TEST_CASE(XDP_ADJUST_TAIL_SHRINK_MULTI_BUFF,
	      testapp_adjust_tail_shrink_mb, 0)
XSK_TEST_CASE(XDP_ADJUST_TAIL_GROW, testapp_adjust_tail_grow, 0)
XSK_TEST_CASE(XDP_ADJUST_TAIL_GROW_MULTI_BUFF, testapp_adjust_tail_grow_mb, 0)
XSK_TEST_CASE(SEND_RECEIVE_9K_PACKETS, testapp_send_receive_mb,
	      XSK_HW_RX | XSK_HW_TX)
/* Tests with huge page dependency */
XSK_TEST_CASE(SEND_RECEIVE_UNALIGNED, testapp_send_receive_unaligned,
	      XSK_HW_RX | XSK_HW_TX)
XSK_TEST_CASE(UNALIGNED_INV_DESC, testapp_unaligned_inv_desc, XSK_HW_TX)
XSK_TEST_CASE(UNALIGNED_INV_DESC_4001_FRAME_SIZE,
	      testapp_unaligned_inv_desc_4001_frame, XSK_HW_TX)
XSK_TEST_CASE(SEND_RECEIVE_UNALIGNED_9K_PACKETS,
	      testapp_send_receive_unaligned_mb, XSK_HW_RX | XSK_HW_TX)
XSK_TEST_CASE(UNALIGNED_INV_DESC_MULTI_BUFF, testapp_unaligned_inv_desc_mb,
	      XSK_HW_TX)
/* Tests with HW ring size dependency */
XSK_TEST_CASE(HW_SW_MIN_RING_SIZE, testapp_hw_sw_min_ring_size, 0)
XSK_TEST_CASE(HW_SW_MAX_RING_SIZE, testapp_hw_sw_max_ring_size, 0)
/* Too long test */
XSK_TEST_CASE(TEARDOWN, testapp_teardown, 0)
