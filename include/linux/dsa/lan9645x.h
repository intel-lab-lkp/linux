/* SPDX-License-Identifier: GPL-2.0
 * Copyright (C) 2026 Microchip Technology Inc.
 */

#ifndef _NET_DSA_TAG_LAN9645X_H_
#define _NET_DSA_TAG_LAN9645X_H_

#include <linux/if_bridge.h>
#include <linux/if_vlan.h>
#include <net/dsa.h>

/* LAN9645x supports 3 different formats on an NPI port, long prefix, short
 * prefix and no prefix. The format can be configured asymmetrically on RX and
 * TX. We use long prefix on extraction (RX), and no prefix on injection.
 * The long prefix on extraction helps get through the conduit port on host
 * side, since it will see a broadcast MAC.
 *
 * The internal frame header (IFH) is 28 bytes, and the fields are documented
 * below.
 *
 * Long prefix, 16 bytes + IFH:
 * - DMAC    = 0xFFFFFFFFFFFF on extraction.
 * - SMAC    = 0xFEFFFFFFFFFF on extraction.
 * - ETYPE   = 0x8880
 * - payload = 0x0011
 * - IFH
 *
 * Short prefix, 4 bytes + IFH:
 * - 0x8880
 * - 0x0011
 * - IFH
 *
 * No prefix:
 * - IFH
 *
 */
#define LAN9645X_IFH_TAG_TYPE_C	0
#define LAN9645X_IFH_TAG_TYPE_S	1
#define LAN9645X_IFH_LEN_U32		7
#define LAN9645X_IFH_LEN		(LAN9645X_IFH_LEN_U32 * sizeof(u32))
#define LAN9645X_IFH_BITS		(LAN9645X_IFH_LEN * BITS_PER_BYTE)
#define LAN9645X_SHORT_PREFIX_LEN	4
#define LAN9645X_LONG_PREFIX_LEN	16
#define LAN9645X_TOTAL_TAG_LEN (LAN9645X_LONG_PREFIX_LEN + LAN9645X_IFH_LEN)

#define IFH_INJ_TIMESTAMP		192
#define IFH_BYPASS			191
#define IFH_MASQ			190
#define IFH_TIMESTAMP			186
#define IFH_TIMESTAMP_NS		194
#define IFH_TIMESTAMP_SUBNS		186
#define IFH_MASQ_PORT			186
#define IFH_RCT_INJ			185
#define IFH_LEN				171
#define IFH_WRDMODE			169
#define IFH_RTAGD			167
#define IFH_CUTTHRU			166
#define IFH_REW_CMD			156
#define IFH_REW_OAM			155
#define IFH_PDU_TYPE			151
#define IFH_FCS_UPD			150
#define IFH_DP				149
#define IFH_RTE_INB_UPDATE		148
#define IFH_POP_CNT			146
#define IFH_ETYPE_OFS			144
#define IFH_SRCPORT			140
#define IFH_SEQ_NUM			120
#define IFH_TAG_TYPE			119
#define IFH_TCI				103
#define IFH_DSCP			97
#define IFH_QOS_CLASS			94
#define IFH_CPUQ			86
#define IFH_LEARN_FLAGS			84
#define IFH_SFLOW_ID			80
#define IFH_ACL_HIT			79
#define IFH_ACL_IDX			73
#define IFH_ISDX			65
#define IFH_DSTS			55
#define IFH_FLOOD			53
#define IFH_SEQ_OP			51
#define IFH_IPV				48
#define IFH_AFI				47
#define IFH_RTP_ID			37
#define IFH_RTP_SUBID			36
#define IFH_PN_DATA_STATUS		28
#define IFH_PN_TRANSF_STATUS_ZERO	27
#define IFH_PN_CC			11
#define IFH_DUPL_DISC_ENA		10
#define IFH_RCT_AVAIL			9

#define IFH_INJ_TIMESTAMP_SZ		32
#define IFH_BYPASS_SZ			1
#define IFH_MASQ_SZ			1
#define IFH_TIMESTAMP_SZ		38
#define IFH_TIMESTAMP_NS_SZ		30
#define IFH_TIMESTAMP_SUBNS_SZ		8
#define IFH_MASQ_PORT_SZ		4
#define IFH_RCT_INJ_SZ			1
#define IFH_LEN_SZ			14
#define IFH_WRDMODE_SZ			2
#define IFH_RTAGD_SZ			2
#define IFH_CUTTHRU_SZ			1
#define IFH_REW_CMD_SZ			10
#define IFH_REW_OAM_SZ			1
#define IFH_PDU_TYPE_SZ			4
#define IFH_FCS_UPD_SZ			1
#define IFH_DP_SZ			1
#define IFH_RTE_INB_UPDATE_SZ		1
#define IFH_POP_CNT_SZ			2
#define IFH_ETYPE_OFS_SZ		2
#define IFH_SRCPORT_SZ			4
#define IFH_SEQ_NUM_SZ			16
#define IFH_TAG_TYPE_SZ			1
#define IFH_TCI_SZ			16
#define IFH_DSCP_SZ			6
#define IFH_QOS_CLASS_SZ		3
#define IFH_CPUQ_SZ			8
#define IFH_LEARN_FLAGS_SZ		2
#define IFH_SFLOW_ID_SZ			4
#define IFH_ACL_HIT_SZ			1
#define IFH_ACL_IDX_SZ			6
#define IFH_ISDX_SZ			8
#define IFH_DSTS_SZ			10
#define IFH_FLOOD_SZ			2
#define IFH_SEQ_OP_SZ			2
#define IFH_IPV_SZ			3
#define IFH_AFI_SZ			1
#define IFH_RTP_ID_SZ			10
#define IFH_RTP_SUBID_SZ		1
#define IFH_PN_DATA_STATUS_SZ		8
#define IFH_PN_TRANSF_STATUS_ZERO_SZ	1
#define IFH_PN_CC_SZ			16
#define IFH_DUPL_DISC_ENA_SZ		1
#define IFH_RCT_AVAIL_SZ		1

#define LAN9645X_VALIDATE_FIELD(_fld, _fld_sz)				\
do {									\
	BUILD_BUG_ON_MSG((_fld_sz) > 32, "IFH field size wider than 32.");\
	BUILD_BUG_ON_MSG((_fld_sz) == 0, "IFH field size of 0.");	\
	BUILD_BUG_ON_MSG((_fld) + (_fld_sz) > LAN9645X_IFH_BITS,	\
			 "IFH field overflows IFH");			\
} while (0)

#define LAN9645X_IFH_GET(_ifh, _fld) \
({ \
	LAN9645X_VALIDATE_FIELD(_fld, _fld##_SZ);\
	lan9645x_ifh_get((_ifh), (_fld), _fld##_SZ); \
})

#define LAN9645X_IFH_SET(_ifh, _fld, _val) \
({ \
	LAN9645X_VALIDATE_FIELD(_fld, _fld##_SZ);\
	lan9645x_ifh_set((_ifh), (_val), (_fld), _fld##_SZ); \
})

#define BTM_MSK(n)	((u8)GENMASK(n, 0))
#define TOP_MSK(n)	((u8)GENMASK(7, n))

static inline void set_merge_mask(u8 *on_zero, u8 on_one, u8 mask)
{
	*on_zero =  *on_zero ^ ((*on_zero ^ on_one) & mask);
}

/* The internal frame header (IFH) is a big-endian 28 byte unpadded bit array.
 * Frames can be prepended with an IFH on injection and extraction. There
 * are two field layouts, one for extraction and one for injection.
 *
 *    IFH bits go from high to low, for instance
 *    ifh[0]  = [223:215]
 *    ifh[27] = [7:0]
 *
 * Here is an example of setting a value starting at bit 13 of bit length 17.
 *
 * val    = 0x1ff
 * pos    = 13
 * length = 17
 *
 *
 * IFH[]   0                         23       24       25        26      27
 *
 *                                           end_u8           start_u8
 *      +--------+----------------+--------+--------+--------+--------+--------+
 *      |        |                |        |        |        |        |        |
 * IFH  |        | ....           |        |  vvvvvvvvvvvvvvvvvvv     |        |
 *      |        |                |        |  |     |        |  |     |        |
 *      +--------+----------------+--------+--+-----+--------+--+-----+--------+
 * Bits  223                       39    32 31|   24 23    16 15|    8 7      0
 *                                            |                 |
 *                                            |                 |
 *                                            |                 |
 *                                            v                 v
 *                                        end       = 29       pos        = 13
 *                                        end_rem   = 5        pos_rem    = 5
 *                                        end_u8    = 3        start_u8   = 1
 *                                        BTM_MSK(5)= 0x3f     TOP_MSK(5) = 0xe0
 *
 *
 * In end_u8 and start_u8 we must merge the existing IFH byte with the new
 * value. In the 'middle' bytes of the value we can overwrite the corresponding
 * IFH byte.
 */
static inline void lan9645x_ifh_set(u8 *ifh, u32 val, size_t pos, size_t length)
{
	size_t end = (pos + length) - 1;
	size_t start_u8 = pos >> 3;
	size_t end_u8 = end >> 3;
	size_t end_rem = end & 0x7;
	size_t pos_rem = pos & 0x7;
	u8 end_mask, start_mask;
	size_t vshift;
	u8 *ptr;

	end_mask = BTM_MSK(end_rem);
	start_mask = TOP_MSK(pos_rem);

	ptr = &ifh[LAN9645X_IFH_LEN - 1 - end_u8];

	if (end_u8 == start_u8)
		return set_merge_mask(ptr, val << pos_rem,
				      end_mask & start_mask);

	vshift = length - end_rem - 1;
	set_merge_mask(ptr++, val >> vshift, end_mask);

	for (size_t j = 1; j < end_u8 - start_u8; j++) {
		vshift -= 8;
		*ptr++ = val >> vshift;
	}

	set_merge_mask(ptr, val << pos_rem, start_mask);
}

static inline u32 lan9645x_ifh_get(const u8 *ifh, size_t pos, size_t length)
{
	size_t end = (pos + length) - 1;
	size_t start_u8 = pos >> 3;
	size_t end_u8 = end >> 3;
	size_t end_rem = end & 0x7;
	size_t pos_rem = pos & 0x7;
	u8 end_mask, start_mask;
	const u8 *ptr;
	u32 val;

	end_mask = BTM_MSK(end_rem);
	start_mask = TOP_MSK(pos_rem);

	ptr = &ifh[LAN9645X_IFH_LEN - 1 - end_u8];

	if (end_u8 == start_u8)
		return (*ptr & end_mask & start_mask) >> pos_rem;

	val = *ptr++ & end_mask;

	for (size_t j = 1; j < end_u8 - start_u8; j++)
		val = val << 8 | *ptr++;

	return val << (8 - pos_rem) | (*ptr & start_mask) >> pos_rem;
}

static inline void lan9645x_xmit_get_vlan_info(struct sk_buff *skb,
					       struct net_device *br,
					       u32 *vlan_tci, u32 *tag_type)
{
	struct vlan_ethhdr *hdr;
	u16 proto, tci;

	if (!br || !br_vlan_enabled(br)) {
		*vlan_tci = 0;
		*tag_type = LAN9645X_IFH_TAG_TYPE_C;
		return;
	}

	hdr = (struct vlan_ethhdr *)skb_mac_header(skb);
	br_vlan_get_proto(br, &proto);

	if (ntohs(hdr->h_vlan_proto) == proto) {
		vlan_remove_tag(skb, &tci);
		*vlan_tci = tci;
	} else {
		rcu_read_lock();
		br_vlan_get_pvid_rcu(br, &tci);
		rcu_read_unlock();
		*vlan_tci = tci;
	}

	*tag_type = (proto != ETH_P_8021Q) ? LAN9645X_IFH_TAG_TYPE_S :
					     LAN9645X_IFH_TAG_TYPE_C;
}

#endif /* _NET_DSA_TAG_LAN9645X_H_ */
