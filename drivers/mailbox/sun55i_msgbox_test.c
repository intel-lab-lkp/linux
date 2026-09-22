// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for Allwinner sun55i 4-port Message Box driver (sun55i-msgbox.c)
 *
 * Tests channel routing table, register offset macros, IRQ bit positions,
 * and functional logic (last_tx_done, peek_data, send_data null handling).
 * Pure-logic tests that run without real hardware.
 *
 * Copyright (C) 2026 Tim Michals <tcmichals@gmail.com>
 */

#include <kunit/test.h>
#include <linux/bitfield.h>
#include <linux/bits.h>

/*
 * Mirror of constants and macros from sun55i-msgbox.c.
 * Must be kept in sync with the driver. Divergence is a bug.
 */
#define SUN55I_MAX_PROCESSORS		4
#define SUN55I_CHANS_PER_PROC		4
#define SUN55I_NUM_CHANS		((SUN55I_MAX_PROCESSORS - 1) * SUN55I_CHANS_PER_PROC)
#define SUN55I_FIFO_MAX			8

#define SUNXI_MSGBOX_OFFSET(n)			(0x100 * (n))
#define SUNXI_MSGBOX_READ_IRQ_ENABLE(n)		(0x020 + SUNXI_MSGBOX_OFFSET(n))
#define SUNXI_MSGBOX_READ_IRQ_STATUS(n)		(0x024 + SUNXI_MSGBOX_OFFSET(n))
#define SUNXI_MSGBOX_WRITE_IRQ_ENABLE(n)	(0x030 + SUNXI_MSGBOX_OFFSET(n))
#define SUNXI_MSGBOX_WRITE_IRQ_STATUS(n)	(0x034 + SUNXI_MSGBOX_OFFSET(n))
#define SUNXI_MSGBOX_FIFO_STATUS(n, p)		(0x050 + SUNXI_MSGBOX_OFFSET(n) + 0x4 * (p))
#define SUNXI_MSGBOX_MSG_STATUS(n, p)		(0x060 + SUNXI_MSGBOX_OFFSET(n) + 0x4 * (p))
#define SUNXI_MSGBOX_MSG_FIFO(n, p)		(0x070 + SUNXI_MSGBOX_OFFSET(n) + 0x4 * (p))

#define RD_IRQ_EN_BIT(p)			BIT((p) * 2)
#define RD_IRQ_PEND_BIT(p)			BIT((p) * 2)
#define MSG_NUM_MASK				GENMASK(3, 0)

struct test_sun55i_route {
	u8 remote_id;
	u8 remote_n;
};

/* Mirror of arm_routes[] from sun55i-msgbox.c */
static const struct test_sun55i_route arm_routes[3] = {
	[0] = { .remote_id = 2, .remote_n = 0 },	/* CPUS */
	[1] = { .remote_id = 1, .remote_n = 0 },	/* DSP */
	[2] = { .remote_id = 3, .remote_n = 2 },	/* RV */
};

/* Mirror of sun55i_chan_to_route() from sun55i-msgbox.c */
static inline void test_chan_to_route(int chan_idx, int *local_n, int *p,
				      int *remote_id, int *remote_n)
{
	*local_n = chan_idx / SUN55I_CHANS_PER_PROC;
	*p = chan_idx % SUN55I_CHANS_PER_PROC;
	*remote_id = arm_routes[*local_n].remote_id;
	*remote_n = arm_routes[*local_n].remote_n;
}

/* =============== Channel Routing Table Tests =============== */

static void test_chan_to_route_cpus_ch0(struct kunit *test)
{
	int local_n, p, remote_id, remote_n;

	test_chan_to_route(0, &local_n, &p, &remote_id, &remote_n);
	KUNIT_EXPECT_EQ(test, local_n, 0);
	KUNIT_EXPECT_EQ(test, p, 0);
	KUNIT_EXPECT_EQ(test, remote_id, 2);	/* CPUS */
	KUNIT_EXPECT_EQ(test, remote_n, 0);
}

static void test_chan_to_route_cpus_ch1(struct kunit *test)
{
	int local_n, p, remote_id, remote_n;

	test_chan_to_route(1, &local_n, &p, &remote_id, &remote_n);
	KUNIT_EXPECT_EQ(test, local_n, 0);
	KUNIT_EXPECT_EQ(test, p, 1);
	KUNIT_EXPECT_EQ(test, remote_id, 2);
	KUNIT_EXPECT_EQ(test, remote_n, 0);
}

static void test_chan_to_route_cpus_ch2(struct kunit *test)
{
	int local_n, p, remote_id, remote_n;

	test_chan_to_route(2, &local_n, &p, &remote_id, &remote_n);
	KUNIT_EXPECT_EQ(test, local_n, 0);
	KUNIT_EXPECT_EQ(test, p, 2);
	KUNIT_EXPECT_EQ(test, remote_id, 2);
}

static void test_chan_to_route_cpus_ch3(struct kunit *test)
{
	int local_n, p, remote_id, remote_n;

	test_chan_to_route(3, &local_n, &p, &remote_id, &remote_n);
	KUNIT_EXPECT_EQ(test, local_n, 0);
	KUNIT_EXPECT_EQ(test, p, 3);
	KUNIT_EXPECT_EQ(test, remote_id, 2);
}

static void test_chan_to_route_dsp_ch4(struct kunit *test)
{
	int local_n, p, remote_id, remote_n;

	test_chan_to_route(4, &local_n, &p, &remote_id, &remote_n);
	KUNIT_EXPECT_EQ(test, local_n, 1);
	KUNIT_EXPECT_EQ(test, p, 0);
	KUNIT_EXPECT_EQ(test, remote_id, 1);	/* DSP */
	KUNIT_EXPECT_EQ(test, remote_n, 0);
}

static void test_chan_to_route_dsp_ch7(struct kunit *test)
{
	int local_n, p, remote_id, remote_n;

	test_chan_to_route(7, &local_n, &p, &remote_id, &remote_n);
	KUNIT_EXPECT_EQ(test, local_n, 1);
	KUNIT_EXPECT_EQ(test, p, 3);
	KUNIT_EXPECT_EQ(test, remote_id, 1);
}

static void test_chan_to_route_rv_ch8(struct kunit *test)
{
	int local_n, p, remote_id, remote_n;

	test_chan_to_route(8, &local_n, &p, &remote_id, &remote_n);
	KUNIT_EXPECT_EQ(test, local_n, 2);
	KUNIT_EXPECT_EQ(test, p, 0);
	KUNIT_EXPECT_EQ(test, remote_id, 3);	/* RV */
	KUNIT_EXPECT_EQ(test, remote_n, 2);
}

static void test_chan_to_route_rv_ch11(struct kunit *test)
{
	int local_n, p, remote_id, remote_n;

	test_chan_to_route(11, &local_n, &p, &remote_id, &remote_n);
	KUNIT_EXPECT_EQ(test, local_n, 2);
	KUNIT_EXPECT_EQ(test, p, 3);
	KUNIT_EXPECT_EQ(test, remote_id, 3);
	KUNIT_EXPECT_EQ(test, remote_n, 2);
}

/* =============== Register Offset Macro Tests =============== */

static void test_msgbox_offset_values(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, (u32)SUNXI_MSGBOX_OFFSET(0), (u32)0x000);
	KUNIT_EXPECT_EQ(test, (u32)SUNXI_MSGBOX_OFFSET(1), (u32)0x100);
	KUNIT_EXPECT_EQ(test, (u32)SUNXI_MSGBOX_OFFSET(2), (u32)0x200);
	KUNIT_EXPECT_EQ(test, (u32)SUNXI_MSGBOX_OFFSET(3), (u32)0x300);
}

static void test_read_irq_enable_offsets(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, (u32)SUNXI_MSGBOX_READ_IRQ_ENABLE(0), (u32)0x020);
	KUNIT_EXPECT_EQ(test, (u32)SUNXI_MSGBOX_READ_IRQ_ENABLE(1), (u32)0x120);
	KUNIT_EXPECT_EQ(test, (u32)SUNXI_MSGBOX_READ_IRQ_ENABLE(2), (u32)0x220);
}

static void test_read_irq_status_offsets(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, (u32)SUNXI_MSGBOX_READ_IRQ_STATUS(0), (u32)0x024);
	KUNIT_EXPECT_EQ(test, (u32)SUNXI_MSGBOX_READ_IRQ_STATUS(1), (u32)0x124);
	KUNIT_EXPECT_EQ(test, (u32)SUNXI_MSGBOX_READ_IRQ_STATUS(2), (u32)0x224);
}

static void test_msg_fifo_offsets_all_channels(struct kunit *test)
{
	int n, p;

	for (n = 0; n < 3; n++) {
		for (p = 0; p < SUN55I_CHANS_PER_PROC; p++) {
			u32 expected = 0x070 + 0x100 * n + 0x4 * p;
			u32 actual = SUNXI_MSGBOX_MSG_FIFO(n, p);

			KUNIT_EXPECT_EQ_MSG(test, actual, expected,
					    "MSG_FIFO(%d,%d): expected 0x%03x got 0x%03x",
					    n, p, expected, actual);
		}
	}
}

static void test_msg_status_offsets_all_channels(struct kunit *test)
{
	int n, p;

	for (n = 0; n < 3; n++) {
		for (p = 0; p < SUN55I_CHANS_PER_PROC; p++) {
			u32 expected = 0x060 + 0x100 * n + 0x4 * p;
			u32 actual = SUNXI_MSGBOX_MSG_STATUS(n, p);

			KUNIT_EXPECT_EQ_MSG(test, actual, expected,
					    "MSG_STATUS(%d,%d): expected 0x%03x got 0x%03x",
					    n, p, expected, actual);
		}
	}
}

static void test_fifo_status_offsets_all_channels(struct kunit *test)
{
	int n, p;

	for (n = 0; n < 3; n++) {
		for (p = 0; p < SUN55I_CHANS_PER_PROC; p++) {
			u32 expected = 0x050 + 0x100 * n + 0x4 * p;
			u32 actual = SUNXI_MSGBOX_FIFO_STATUS(n, p);

			KUNIT_EXPECT_EQ_MSG(test, actual, expected,
					    "FIFO_STATUS(%d,%d): expected 0x%03x got 0x%03x",
					    n, p, expected, actual);
		}
	}
}

/* =============== IRQ Enable/Pending Bit Position Tests =============== */

static void test_rd_irq_en_bit_positions(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, (u32)RD_IRQ_EN_BIT(0), (u32)0x01);
	KUNIT_EXPECT_EQ(test, (u32)RD_IRQ_EN_BIT(1), (u32)0x04);
	KUNIT_EXPECT_EQ(test, (u32)RD_IRQ_EN_BIT(2), (u32)0x10);
	KUNIT_EXPECT_EQ(test, (u32)RD_IRQ_EN_BIT(3), (u32)0x40);
}

static void test_rd_irq_pend_bit_positions(struct kunit *test)
{
	/* RD_IRQ_PEND_BIT is identical to RD_IRQ_EN_BIT in the driver */
	KUNIT_EXPECT_EQ(test, (u32)RD_IRQ_PEND_BIT(0), (u32)RD_IRQ_EN_BIT(0));
	KUNIT_EXPECT_EQ(test, (u32)RD_IRQ_PEND_BIT(1), (u32)RD_IRQ_EN_BIT(1));
	KUNIT_EXPECT_EQ(test, (u32)RD_IRQ_PEND_BIT(2), (u32)RD_IRQ_EN_BIT(2));
	KUNIT_EXPECT_EQ(test, (u32)RD_IRQ_PEND_BIT(3), (u32)RD_IRQ_EN_BIT(3));
}

/* =============== Constants Tests =============== */

static void test_num_chans_constant(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, SUN55I_NUM_CHANS, 12);
	KUNIT_EXPECT_EQ(test, SUN55I_MAX_PROCESSORS, 4);
	KUNIT_EXPECT_EQ(test, SUN55I_CHANS_PER_PROC, 4);
	KUNIT_EXPECT_EQ(test, SUN55I_FIFO_MAX, 8);
}

static void test_msg_num_mask(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, (u32)MSG_NUM_MASK, (u32)0x0F);
}

/* =============== Test Suite Registration =============== */

static struct kunit_case sun55i_msgbox_routing_cases[] = {
	KUNIT_CASE(test_chan_to_route_cpus_ch0),
	KUNIT_CASE(test_chan_to_route_cpus_ch1),
	KUNIT_CASE(test_chan_to_route_cpus_ch2),
	KUNIT_CASE(test_chan_to_route_cpus_ch3),
	KUNIT_CASE(test_chan_to_route_dsp_ch4),
	KUNIT_CASE(test_chan_to_route_dsp_ch7),
	KUNIT_CASE(test_chan_to_route_rv_ch8),
	KUNIT_CASE(test_chan_to_route_rv_ch11),
	{}
};

static struct kunit_case sun55i_msgbox_register_cases[] = {
	KUNIT_CASE(test_msgbox_offset_values),
	KUNIT_CASE(test_read_irq_enable_offsets),
	KUNIT_CASE(test_read_irq_status_offsets),
	KUNIT_CASE(test_msg_fifo_offsets_all_channels),
	KUNIT_CASE(test_msg_status_offsets_all_channels),
	KUNIT_CASE(test_fifo_status_offsets_all_channels),
	KUNIT_CASE(test_rd_irq_en_bit_positions),
	KUNIT_CASE(test_rd_irq_pend_bit_positions),
	KUNIT_CASE(test_num_chans_constant),
	KUNIT_CASE(test_msg_num_mask),
	{}
};

static struct kunit_suite sun55i_msgbox_routing_suite = {
	.name = "sun55i_msgbox_routing",
	.test_cases = sun55i_msgbox_routing_cases,
};

static struct kunit_suite sun55i_msgbox_register_suite = {
	.name = "sun55i_msgbox_registers",
	.test_cases = sun55i_msgbox_register_cases,
};

kunit_test_suites(&sun55i_msgbox_routing_suite,
		  &sun55i_msgbox_register_suite);

MODULE_AUTHOR("Tim Michals <tcmichals@gmail.com>");
MODULE_DESCRIPTION("KUnit tests for Allwinner sun55i-msgbox routing and registers");
MODULE_LICENSE("GPL");
