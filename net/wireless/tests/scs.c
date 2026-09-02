// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for the SCS and MSCS classifier
 *
 * Each case is one that fails with a wrong priority rather than a crash.
 *
 * Copyright (C) 2026 Felix Fietkau <nbd@nbd.name>
 */
#include <kunit/skbuff.h>
#include <kunit/test.h>
#include <linux/ieee80211.h>
#include <linux/ip.h>
#include <net/cfg80211.h>
#include "../core.h"

static const u8 test_da[ETH_ALEN] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x02 };

/* An Ethernet header, an IPv4 header and four octets of UDP ports */
static const u8 udp_frame[] = {
	0x02, 0x00, 0x00, 0x00, 0x00, 0x02,	/* destination address */
	0x02, 0x00, 0x00, 0x00, 0x00, 0x01,	/* source address */
	0x08, 0x00,
	0x45, 0x00, 0x00, 0x20, 0xab, 0xcd, 0x00, 0x00,
	0x40, 0x11, 0x00, 0x00,
	192, 168, 1, 1,
	192, 168, 1, 2,
	0x13, 0x88, 0x27, 0x10,
	0xde, 0xad, 0xbe, 0xef,
};

/* The same datagram, as a fragment with a nonzero offset */
static const u8 udp_fragment[] = {
	0x02, 0x00, 0x00, 0x00, 0x00, 0x02,	/* destination address */
	0x02, 0x00, 0x00, 0x00, 0x00, 0x01,	/* source address */
	0x08, 0x00,
	0x45, 0x00, 0x00, 0x20, 0xab, 0xcd, 0x00, 0x02,
	0x40, 0x11, 0x00, 0x00,
	192, 168, 1, 1,
	192, 168, 1, 2,
	0x13, 0x88, 0x27, 0x10,
	0xde, 0xad, 0xbe, 0xef,
};

static struct sk_buff *test_skb(struct kunit *test, const u8 *data, size_t len)
{
	struct sk_buff *skb = kunit_zalloc_skb(test, 256, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, skb);
	skb_put_data(skb, data, len);

	return skb;
}

#define FLOW_F(name)	BIT(CFG80211_FLOW_F_##name)

/* Reading the layer 4 header of a later fragment would match payload octets */
static void flow_parse_fragment(struct kunit *test)
{
	struct cfg80211_flow_info info;

	KUNIT_ASSERT_TRUE(test, cfg80211_flow_parse(test_skb(test, udp_frame,
							     sizeof(udp_frame)),
						    &info));
	KUNIT_EXPECT_TRUE(test, info.present & FLOW_F(SRC_PORT));
	KUNIT_EXPECT_TRUE(test, info.present & FLOW_F(DST_PORT));
	KUNIT_EXPECT_EQ(test, be16_to_cpu(info.key.src_port), 5000);
	KUNIT_EXPECT_EQ(test, be16_to_cpu(info.key.dst_port), 10000);
	KUNIT_EXPECT_EQ(test, info.key.proto, IPPROTO_UDP);

	KUNIT_ASSERT_TRUE(test, cfg80211_flow_parse(test_skb(test, udp_fragment,
							     sizeof(udp_fragment)),
						    &info));
	KUNIT_EXPECT_FALSE(test, info.present & FLOW_F(SRC_PORT));
	KUNIT_EXPECT_FALSE(test, info.present & FLOW_F(DST_PORT));
	KUNIT_EXPECT_EQ(test, info.key.src_port, 0);
	KUNIT_EXPECT_EQ(test, info.key.dst_port, 0);

	/* The addresses are still classifier parameters of a later fragment */
	KUNIT_EXPECT_TRUE(test, info.present & FLOW_F(IP_SRC));
	KUNIT_EXPECT_TRUE(test, info.present & FLOW_F(IP_DST));
}

static struct cfg80211_scs_desc *
test_scs_desc(struct kunit *test, u8 id, u8 up, const u8 *elems, size_t len)
{
	struct cfg80211_scs_desc *desc;
	int n_tclas;

	n_tclas = cfg80211_tclas_count(elems, len);
	KUNIT_ASSERT_GE(test, n_tclas, 0);

	desc = kunit_kzalloc(test, struct_size(desc, tclas, n_tclas),
			     GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, desc);

	desc->id = id;
	desc->up = up;
	desc->n_tclas = n_tclas;
	KUNIT_ASSERT_EQ(test, 0,
			cfg80211_parse_tclas(elems, len, desc->tclas, n_tclas,
					     &desc->tclas_processing));

	return desc;
}

/* When two descriptors match, the more granular one wins */
static void scs_granularity(struct kunit *test)
{
	/* Classifier type 4 over IPv4, source and destination address */
	static const u8 two_params[] = {
		WLAN_EID_TCLAS, 19,
		255,
		4, 0x06,		/* type, mask: src and dst address */
		4,			/* version */
		192, 168, 1, 1,
		192, 168, 1, 2,
		0, 0, 0, 0,		/* ports, not selected */
		0, 0, 0,		/* dscp, protocol, reserved */
	};
	/* The same, and the destination port as well */
	static const u8 three_params[] = {
		WLAN_EID_TCLAS, 19,
		255,
		4, 0x16,		/* src, dst, dst port */
		4,
		192, 168, 1, 1,
		192, 168, 1, 2,
		0, 0, 0x27, 0x10,
		0, 0, 0,
	};
	/* A source port that the frame does not carry a matching value for */
	static const u8 wrong_port[] = {
		WLAN_EID_TCLAS, 19,
		255,
		4, 0x08,		/* src port only */
		4,
		0, 0, 0, 0,
		0, 0, 0, 0,
		0x00, 0x35, 0, 0,
		0, 0, 0,
	};
	struct cfg80211_scs_desc *list[3];
	struct cfg80211_scs_verdict verdict;
	struct cfg80211_flow_info info;

	list[0] = test_scs_desc(test, 1, 3, two_params, sizeof(two_params));
	list[1] = test_scs_desc(test, 2, 6, three_params, sizeof(three_params));
	list[2] = test_scs_desc(test, 3, 7, wrong_port, sizeof(wrong_port));

	KUNIT_ASSERT_TRUE(test, cfg80211_flow_parse(test_skb(test, udp_frame,
							     sizeof(udp_frame)),
						    &info));

	/*
	 * Neither mask sets the version bit, but a type 4 classifier compares
	 * the version anyway. It still does not count, so the counts are 2 and
	 * 3 and the descriptor order below is what the rule decides.
	 */
	KUNIT_EXPECT_EQ(test, list[0]->tclas[0].fields,
			FLOW_F(IP_VERSION) | FLOW_F(IP_SRC) | FLOW_F(IP_DST));
	KUNIT_EXPECT_EQ(test, list[1]->tclas[0].fields,
			FLOW_F(IP_VERSION) | FLOW_F(IP_SRC) | FLOW_F(IP_DST) |
			FLOW_F(DST_PORT));

	/* Only the two address descriptors match, and the granular one wins */
	cfg80211_scs_evaluate(list, 3, &info, &verdict);
	KUNIT_EXPECT_TRUE(test, verdict.match);
	KUNIT_EXPECT_EQ(test, verdict.scsid, 2);
	KUNIT_EXPECT_EQ(test, verdict.up, 6);

	/* The order of the list must not decide it */
	swap(list[0], list[1]);
	cfg80211_scs_evaluate(list, 3, &info, &verdict);
	KUNIT_EXPECT_TRUE(test, verdict.match);
	KUNIT_EXPECT_EQ(test, verdict.scsid, 2);

	/* A frame without ports leaves only the address descriptor */
	KUNIT_ASSERT_TRUE(test, cfg80211_flow_parse(test_skb(test, udp_fragment,
							     sizeof(udp_fragment)),
						    &info));
	cfg80211_scs_evaluate(list, 3, &info, &verdict);
	KUNIT_EXPECT_TRUE(test, verdict.match);
	KUNIT_EXPECT_EQ(test, verdict.scsid, 1);
	KUNIT_EXPECT_EQ(test, verdict.up, 3);
	KUNIT_EXPECT_TRUE(test, verdict.undecided);
}

/* A key learned from an uplink frame must describe the downlink direction */
static void flow_key_mirror(struct kunit *test)
{
	/* Classifier type 4 over IPv4, both addresses and both ports */
	static const u8 mask_elem[] = {
		WLAN_EID_EXTENSION, 19,
		WLAN_EID_EXT_TCLAS_MASK,
		4, 0x1e,
		4,
		0, 0, 0, 0,
		0, 0, 0, 0,
		0, 0, 0, 0,
		0, 0, 0,
	};
	struct cfg80211_flow_key as_is, mirrored;
	struct cfg80211_flow_info info;
	u32 fields;

	KUNIT_ASSERT_EQ(test, 0,
			cfg80211_parse_tclas_mask(mask_elem, sizeof(mask_elem),
						  &fields));
	KUNIT_EXPECT_EQ(test, fields,
			FLOW_F(IP_VERSION) | FLOW_F(IP_SRC) | FLOW_F(IP_DST) |
			FLOW_F(SRC_PORT) | FLOW_F(DST_PORT));

	KUNIT_ASSERT_TRUE(test, cfg80211_flow_parse(test_skb(test, udp_frame,
							     sizeof(udp_frame)),
						    &info));

	KUNIT_ASSERT_TRUE(test, cfg80211_flow_key_build(&info, fields,
							CFG80211_FLOW_AS_IS,
							&as_is));
	KUNIT_ASSERT_TRUE(test, cfg80211_flow_key_build(&info, fields,
							CFG80211_FLOW_MIRRORED,
							&mirrored));

	KUNIT_EXPECT_MEMEQ(test, &as_is.src, &mirrored.dst, sizeof(as_is.src));
	KUNIT_EXPECT_MEMEQ(test, &as_is.dst, &mirrored.src, sizeof(as_is.dst));
	KUNIT_EXPECT_EQ(test, as_is.src_port, mirrored.dst_port);
	KUNIT_EXPECT_EQ(test, as_is.dst_port, mirrored.src_port);

	/* Nothing the layout does not select may reach the key */
	KUNIT_EXPECT_MEMEQ(test, as_is.sa, "\x00\x00\x00\x00\x00\x00",
			   ETH_ALEN);
	KUNIT_EXPECT_EQ(test, as_is.eth_type, 0);
	KUNIT_EXPECT_EQ(test, as_is.proto, 0);

	/* The version is one the layout does select, so it is in both keys */
	KUNIT_EXPECT_EQ(test, as_is.ip_version, 4);
	KUNIT_EXPECT_EQ(test, mirrored.ip_version, 4);
}

/* The Ethernet addresses mirror as a pair */
static void flow_key_mirror_eth(struct kunit *test)
{
	static const u8 mask_elem[] = {
		WLAN_EID_EXTENSION, 17,
		WLAN_EID_EXT_TCLAS_MASK,
		0, 0x01,		/* classifier type 0, source address */
		0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0,
		0, 0,
	};
	struct cfg80211_flow_key key;
	struct cfg80211_flow_info info;
	u32 fields;

	KUNIT_ASSERT_EQ(test, 0,
			cfg80211_parse_tclas_mask(mask_elem, sizeof(mask_elem),
						  &fields));
	KUNIT_EXPECT_EQ(test, fields, FLOW_F(ETH_SA));

	KUNIT_ASSERT_TRUE(test, cfg80211_flow_parse(test_skb(test, udp_frame,
							     sizeof(udp_frame)),
						    &info));

	KUNIT_ASSERT_TRUE(test, cfg80211_flow_key_build(&info, fields,
							CFG80211_FLOW_MIRRORED,
							&key));

	/* The mirror parameter of the source address is the destination */
	KUNIT_EXPECT_MEMEQ(test, key.sa, test_da, ETH_ALEN);
	KUNIT_EXPECT_MEMEQ(test, key.da, "\x00\x00\x00\x00\x00\x00", ETH_ALEN);
}

/* Classifier types that classify an MPDU, and type 3 in a mask, are refused */
static void tclas_refused(struct kunit *test)
{
	static const u8 mac_header[] = {
		WLAN_EID_TCLAS, 5,
		255,
		6, 0x00, 0x00, 0x00,	/* type 6, three octet classifier mask */
	};
	static const u8 up_not_255[] = {
		WLAN_EID_TCLAS, 17,
		0,			/* a UP that takes part in the match */
		0, 0x01,
		0x02, 0x00, 0x00, 0x00, 0x00, 0x01,
		0x02, 0x00, 0x00, 0x00, 0x00, 0x02,
		0x08, 0x00,
	};
	static const u8 filter_in_mask[] = {
		WLAN_EID_EXTENSION, 9,
		WLAN_EID_EXT_TCLAS_MASK,
		3, 0,
		0, 0,
		0x00, 0x00,
		0xff, 0xff,
	};
	/* Classifier type 1, superseded by type 4 and deprecated for IPv6 */
	static const u8 legacy_ip[] = {
		WLAN_EID_TCLAS, 19,
		255,
		1, 0x06,
		4,
		192, 168, 1, 1,
		192, 168, 1, 2,
		0, 0, 0, 0,
		0, 0, 0,
	};
	/* Classifier type 2, deprecated in favour of type 5 */
	static const u8 legacy_vlan[] = {
		WLAN_EID_TCLAS, 5,
		255,
		2, 0x01,
		0x00, 0x64,
	};
	enum cfg80211_tclas_processing processing;
	struct cfg80211_tclas tclas;
	u32 fields;

	KUNIT_EXPECT_EQ(test, -EOPNOTSUPP,
			cfg80211_parse_tclas(mac_header, sizeof(mac_header),
					     &tclas, 1, &processing));
	KUNIT_EXPECT_EQ(test, -EOPNOTSUPP,
			cfg80211_parse_tclas(up_not_255, sizeof(up_not_255),
					     &tclas, 1, &processing));
	KUNIT_EXPECT_EQ(test, -EOPNOTSUPP,
			cfg80211_parse_tclas(legacy_ip, sizeof(legacy_ip),
					     &tclas, 1, &processing));
	KUNIT_EXPECT_EQ(test, -EOPNOTSUPP,
			cfg80211_parse_tclas(legacy_vlan, sizeof(legacy_vlan),
					     &tclas, 1, &processing));
	KUNIT_EXPECT_EQ(test, -EOPNOTSUPP,
			cfg80211_parse_tclas_mask(filter_in_mask,
						  sizeof(filter_in_mask),
						  &fields));
}

/* A descriptor that matched is decided, whichever element was skipped */
static void scs_undecided(struct kunit *test)
{
	/* A source address element and a source port element, match either */
	static const u8 addr_or_port[] = {
		WLAN_EID_TCLAS, 19,
		255,
		4, 0x02,		/* source address */
		4,
		192, 168, 1, 1,
		0, 0, 0, 0,
		0, 0, 0, 0,
		0, 0, 0,

		WLAN_EID_TCLAS, 19,
		255,
		4, 0x08,		/* source port */
		4,
		0, 0, 0, 0,
		0, 0, 0, 0,
		0x13, 0x88, 0, 0,
		0, 0, 0,

		WLAN_EID_TCLAS_PROCESSING, 1, 1,
	};
	/* The source port alone */
	static const u8 port_only[] = {
		WLAN_EID_TCLAS, 19,
		255,
		4, 0x08,
		4,
		0, 0, 0, 0,
		0, 0, 0, 0,
		0x13, 0x88, 0, 0,
		0, 0, 0,
	};
	struct cfg80211_scs_verdict verdict;
	struct cfg80211_flow_info info;
	struct cfg80211_scs_desc *list[1];

	list[0] = test_scs_desc(test, 1, 4, addr_or_port, sizeof(addr_or_port));
	KUNIT_ASSERT_EQ(test, list[0]->n_tclas, 2);
	KUNIT_ASSERT_EQ(test, list[0]->tclas_processing, 1);

	/* Both elements match a whole datagram */
	KUNIT_ASSERT_TRUE(test, cfg80211_flow_parse(test_skb(test, udp_frame,
							     sizeof(udp_frame)),
						    &info));
	cfg80211_scs_evaluate(list, 1, &info, &verdict);
	KUNIT_EXPECT_TRUE(test, verdict.match);
	KUNIT_EXPECT_FALSE(test, verdict.undecided);

	/*
	 * A later fragment has no ports, so the port element is skipped. The
	 * address element still matches, so the descriptor is decided.
	 */
	KUNIT_ASSERT_TRUE(test, cfg80211_flow_parse(test_skb(test, udp_fragment,
							     sizeof(udp_fragment)),
						    &info));
	cfg80211_scs_evaluate(list, 1, &info, &verdict);
	KUNIT_EXPECT_TRUE(test, verdict.match);
	KUNIT_EXPECT_FALSE(test, verdict.undecided);

	/* With nothing else to match on, the same fragment leaves it open */
	list[0] = test_scs_desc(test, 2, 4, port_only, sizeof(port_only));
	cfg80211_scs_evaluate(list, 1, &info, &verdict);
	KUNIT_EXPECT_FALSE(test, verdict.match);
	KUNIT_EXPECT_TRUE(test, verdict.undecided);
}

/* The presence bitmap decides where every optional field starts */
static void qos_characteristics(struct kunit *test)
{
	/* Downlink, TID 5, UP 5, and a Service Start Time with no LinkID */
	static const u8 no_link_id[] = {
		0xff, 0x17, 0x71,	/* element header, 113 */
		0x55, 0x05, 0x00, 0x00,	/* control info, presence bit 1 */
		0xe8, 0x03, 0x00, 0x00,	/* minimum service interval */
		0xd0, 0x07, 0x00, 0x00,	/* maximum service interval */
		0x88, 0x13, 0x00,	/* minimum data rate */
		0x20, 0x4e, 0x00,	/* delay bound */
		0x78, 0x56, 0x34, 0x12,	/* service start time */
	};
	static const u8 with_link_id[] = {
		0xff, 0x18, 0x71,
		0x55, 0x0d, 0x00, 0x00,	/* presence bits 1 and 2 */
		0xe8, 0x03, 0x00, 0x00,
		0xd0, 0x07, 0x00, 0x00,
		0x88, 0x13, 0x00,
		0x20, 0x4e, 0x00,
		0x78, 0x56, 0x34, 0x12,
		0x03,			/* service start time LinkID */
	};
	/* A LinkID with no Service Start Time refers to a field that is absent */
	static const u8 orphan_link_id[] = {
		0xff, 0x14, 0x71,
		0x55, 0x09, 0x00, 0x00,	/* presence bit 2 alone */
		0xe8, 0x03, 0x00, 0x00,
		0xd0, 0x07, 0x00, 0x00,
		0x88, 0x13, 0x00,
		0x20, 0x4e, 0x00,
		0x03,
	};
	/* The header names the element, so a wrong extension is not this one */
	static const u8 bad_ext_id[] = {
		0xff, 0x17, 0x70,
		0x55, 0x05, 0x00, 0x00,
		0xe8, 0x03, 0x00, 0x00,
		0xd0, 0x07, 0x00, 0x00,
		0x88, 0x13, 0x00,
		0x20, 0x4e, 0x00,
		0x78, 0x56, 0x34, 0x12,
	};
	/* The Length field must agree with the octets behind it */
	static const u8 bad_length[] = {
		0xff, 0x18, 0x71,
		0x55, 0x05, 0x00, 0x00,
		0xe8, 0x03, 0x00, 0x00,
		0xd0, 0x07, 0x00, 0x00,
		0x88, 0x13, 0x00,
		0x20, 0x4e, 0x00,
		0x78, 0x56, 0x34, 0x12,
	};
	/* The length must match the parameters the bitmap says are present */
	static const u8 short_elem[] = {
		0xff, 0x13, 0x71,
		0x55, 0x05, 0x00, 0x00,
		0xe8, 0x03, 0x00, 0x00,
		0xd0, 0x07, 0x00, 0x00,
		0x88, 0x13, 0x00,
		0x20, 0x4e, 0x00,
	};

	KUNIT_EXPECT_TRUE(test, ieee80211_qos_char_size_ok(no_link_id,
							   sizeof(no_link_id)));
	KUNIT_EXPECT_EQ(test, ieee80211_qos_char_direction((const void *)no_link_id),
			IEEE80211_QOS_CHAR_DIR_DOWNLINK);
	KUNIT_EXPECT_EQ(test, ieee80211_qos_char_presence((const void *)no_link_id),
			IEEE80211_QOS_CHAR_PRES_SERVICE_START_TIME);

	KUNIT_EXPECT_TRUE(test, ieee80211_qos_char_size_ok(with_link_id,
							   sizeof(with_link_id)));
	KUNIT_EXPECT_EQ(test, ieee80211_qos_char_presence((const void *)with_link_id),
			IEEE80211_QOS_CHAR_PRES_SERVICE_START_TIME |
			IEEE80211_QOS_CHAR_PRES_SERVICE_START_LINK_ID);

	KUNIT_EXPECT_FALSE(test, ieee80211_qos_char_size_ok(orphan_link_id,
							    sizeof(orphan_link_id)));
	KUNIT_EXPECT_FALSE(test, ieee80211_qos_char_size_ok(short_elem,
							    sizeof(short_elem)));
	KUNIT_EXPECT_FALSE(test, ieee80211_qos_char_size_ok(bad_ext_id,
							    sizeof(bad_ext_id)));
	KUNIT_EXPECT_FALSE(test, ieee80211_qos_char_size_ok(bad_length,
							    sizeof(bad_length)));
}

/*
 * An uplink or direct link descriptor carries no classifier and no user
 * priority, per IEEE Std 802.11be-2024, 35.17. Refusing one would leave the
 * traffic description that a driver schedules against unreachable.
 */
static void scs_traffic_description(struct kunit *test)
{
	/* Uplink, and a Service Start Time with no LinkID */
	static const u8 uplink[] = {
		0xff, 0x17, 0x71,
		0x54, 0x05, 0x00, 0x00,
		0xe8, 0x03, 0x00, 0x00,
		0xd0, 0x07, 0x00, 0x00,
		0x88, 0x13, 0x00,
		0x20, 0x4e, 0x00,
		0x78, 0x56, 0x34, 0x12,
	};
	struct cfg80211_scs_desc desc = {
		.req_type = NL80211_SCS_REQ_ADD,
		.tclas_processing = CFG80211_TCLAS_PROCESSING_ABSENT,
		.qos_char = (const void *)uplink,
		.qos_char_len = sizeof(uplink),
		.id = 1,
	};

	KUNIT_ASSERT_TRUE(test, ieee80211_qos_char_size_ok(uplink,
							   sizeof(uplink)));
	KUNIT_ASSERT_EQ(test, ieee80211_qos_char_direction((const void *)uplink),
			IEEE80211_QOS_CHAR_DIR_UPLINK);

	KUNIT_EXPECT_TRUE(test, cfg80211_scs_desc_valid(&desc));

	/* 35.17 bans the TCLAS Processing element from such a descriptor */
	desc.tclas_processing = CFG80211_TCLAS_PROCESSING_DEFAULT;
	KUNIT_EXPECT_FALSE(test, cfg80211_scs_desc_valid(&desc));
}

static struct kunit_case scs_cases[] = {
	KUNIT_CASE(flow_parse_fragment),
	KUNIT_CASE(scs_granularity),
	KUNIT_CASE(scs_undecided),
	KUNIT_CASE(flow_key_mirror),
	KUNIT_CASE(flow_key_mirror_eth),
	KUNIT_CASE(tclas_refused),
	KUNIT_CASE(qos_characteristics),
	KUNIT_CASE(scs_traffic_description),
	{}
};

static struct kunit_suite scs = {
	.name = "cfg80211-scs",
	.test_cases = scs_cases,
};

kunit_test_suite(scs);
