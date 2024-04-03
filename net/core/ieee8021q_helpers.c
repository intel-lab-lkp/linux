// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2024 Pengutronix, Oleksij Rempel <kernel@pengutronix.de>

#include <linux/printk.h>
#include <linux/types.h>
#include <net/dscp.h>
#include <net/ieee8021q.h>

/* Following arrays map Traffic Types (TT) to traffic classes (TC) for different
 * number of queues as shown in the example provided by  IEEE 802.1Q-2022 in
 * Annex I "I.3 Traffic type to traffic class mapping" and Table I-1 "Traffic
 * type to traffic class mapping".
 */
static const u8 ieee8021q_8queue_tt_tc_map[] = {
	[IEEE8021Q_TT_BK] = 0,
	[IEEE8021Q_TT_BE] = 1,
	[IEEE8021Q_TT_EE] = 2,
	[IEEE8021Q_TT_CA] = 3,
	[IEEE8021Q_TT_VI] = 4,
	[IEEE8021Q_TT_VO] = 5,
	[IEEE8021Q_TT_IC] = 6,
	[IEEE8021Q_TT_NC] = 7,
};

static const u8 ieee8021q_7queue_tt_tc_map[] = {
	[IEEE8021Q_TT_BK] = 0,
	[IEEE8021Q_TT_BE] = 1,
	[IEEE8021Q_TT_EE] = 2,
	[IEEE8021Q_TT_CA] = 3,
	[IEEE8021Q_TT_VI] = 4,	[IEEE8021Q_TT_VO] = 4,
	[IEEE8021Q_TT_IC] = 5,
	[IEEE8021Q_TT_NC] = 6,
};

static const u8 ieee8021q_6queue_tt_tc_map[] = {
	[IEEE8021Q_TT_BK] = 0,
	[IEEE8021Q_TT_BE] = 1,
	[IEEE8021Q_TT_EE] = 2,	[IEEE8021Q_TT_CA] = 2,
	[IEEE8021Q_TT_VI] = 3,	[IEEE8021Q_TT_VO] = 3,
	[IEEE8021Q_TT_IC] = 4,
	[IEEE8021Q_TT_NC] = 5,
};

static const u8 ieee8021q_5queue_tt_tc_map[] = {
	[IEEE8021Q_TT_BK] = 0, [IEEE8021Q_TT_BE] = 0,
	[IEEE8021Q_TT_EE] = 1, [IEEE8021Q_TT_CA] = 1,
	[IEEE8021Q_TT_VI] = 2, [IEEE8021Q_TT_VO] = 2,
	[IEEE8021Q_TT_IC] = 3,
	[IEEE8021Q_TT_NC] = 4,
};

static const u8 ieee8021q_4queue_tt_tc_map[] = {
	[IEEE8021Q_TT_BK] = 0, [IEEE8021Q_TT_BE] = 0,
	[IEEE8021Q_TT_EE] = 1, [IEEE8021Q_TT_CA] = 1,
	[IEEE8021Q_TT_VI] = 2, [IEEE8021Q_TT_VO] = 2,
	[IEEE8021Q_TT_IC] = 3, [IEEE8021Q_TT_NC] = 3,
};

static const u8 ieee8021q_3queue_tt_tc_map[] = {
	[IEEE8021Q_TT_BK] = 0, [IEEE8021Q_TT_BE] = 0,
	[IEEE8021Q_TT_EE] = 0, [IEEE8021Q_TT_CA] = 0,
	[IEEE8021Q_TT_VI] = 1, [IEEE8021Q_TT_VO] = 1,
	[IEEE8021Q_TT_IC] = 2, [IEEE8021Q_TT_NC] = 2,
};

static const u8 ieee8021q_2queue_tt_tc_map[] = {
	[IEEE8021Q_TT_BK] = 0, [IEEE8021Q_TT_BE] = 0,
	[IEEE8021Q_TT_EE] = 0, [IEEE8021Q_TT_CA] = 0,
	[IEEE8021Q_TT_VI] = 1, [IEEE8021Q_TT_VO] = 1,
	[IEEE8021Q_TT_IC] = 1, [IEEE8021Q_TT_NC] = 1,
};

static const u8 ieee8021q_1queue_tt_tc_map[] = {
	[IEEE8021Q_TT_BK] = 0, [IEEE8021Q_TT_BE] = 0,
	[IEEE8021Q_TT_EE] = 0, [IEEE8021Q_TT_CA] = 0,
	[IEEE8021Q_TT_VI] = 0, [IEEE8021Q_TT_VO] = 0,
	[IEEE8021Q_TT_IC] = 0, [IEEE8021Q_TT_NC] = 0,
};

/**
 * ieee8021q_tt_to_tc - Map IEEE 802.1Q Traffic Type to Traffic Class
 * @tt: IEEE 802.1Q Traffic Type
 * @num_queues: Number of queues
 *
 * This function maps an IEEE 802.1Q Traffic Type to a Traffic Class (TC) based
 * on the number of queues configured on the switch. The mapping is based on the
 * example provided by IEEE 802.1Q-2022 in Annex I "I.3 Traffic type to traffic
 * class mapping" and Table I-1 "Traffic type to traffic class mapping".
 *
 * Return: Traffic Class corresponding to the given Traffic Type.
 */
int ieee8021q_tt_to_tc(int tt, int num_queues)
{
	switch (num_queues) {
	case 8:
		return ieee8021q_8queue_tt_tc_map[tt];
	case 7:
		return ieee8021q_7queue_tt_tc_map[tt];
	case 6:
		return ieee8021q_6queue_tt_tc_map[tt];
	case 5:
		return ieee8021q_5queue_tt_tc_map[tt];
	case 4:
		return ieee8021q_4queue_tt_tc_map[tt];
	case 3:
		return ieee8021q_3queue_tt_tc_map[tt];
	case 2:
		return ieee8021q_2queue_tt_tc_map[tt];
	case 1:
		return ieee8021q_1queue_tt_tc_map[tt];
	}

	pr_warn("Invalid number of queues %d\n", num_queues);
	return 0;
}
EXPORT_SYMBOL_GPL(ieee8021q_tt_to_tc);

/**
 * ietf_dscp_to_ieee8021q_tt - Map IETF DSCP to IEEE 802.1Q Traffic Type
 * @dscp: IETF DSCP value
 *
 * This function maps an IETF DSCP value to an IEEE 802.1Q Traffic Type (TT).
 * Since there is no corresponding mapping between DSCP and IEEE 802.1Q Traffic
 * Type, this function is inspired by the RFC8325 documentation which describe
 * the mapping between DSCP and 802.11 User Priority (UP) values.
 *
 * Return: IEEE 802.1Q Traffic Type corresponding to the given DSCP value
 */

int ietf_dscp_to_ieee8021q_tt(int dscp)
{
	switch (dscp) {
	case DSCP_CS0:
	case DSCP_AF11:
	case DSCP_AF12:
	case DSCP_AF13:
		return IEEE8021Q_TT_BE;
	case DSCP_CS1:
		return IEEE8021Q_TT_BK;
	case DSCP_CS2:
	case DSCP_AF21:
	case DSCP_AF22:
	case DSCP_AF23:
		return IEEE8021Q_TT_EE;
	case DSCP_CS3:
	case DSCP_AF31:
	case DSCP_AF32:
	case DSCP_AF33:
		return IEEE8021Q_TT_CA;
	case DSCP_CS4:
	case DSCP_AF41:
	case DSCP_AF42:
	case DSCP_AF43:
		return IEEE8021Q_TT_VI;
	case DSCP_CS5:
	case DSCP_EF:
	case DSCP_VOICE_ADMIT:
		return IEEE8021Q_TT_VO;
	case DSCP_CS6:
		return IEEE8021Q_TT_IC;
	case DSCP_CS7:
		return IEEE8021Q_TT_NC;
	}

	return (dscp >> 3) & 0x7;
}
EXPORT_SYMBOL_GPL(ietf_dscp_to_ieee8021q_tt);
