// SPDX-License-Identifier: GPL-2.0
/* VLAN configuration interface for the rtl8365mb switch family
 *
 * Copyright (C) 2022 Alvin Šipraga <alsi@bang-olufsen.dk>
 *
 * VLAN configuration takes place in two separate domains of the switch: the
 * VLAN4k table and the VLAN membership configuration (MC) database. While the
 * VLAN4k table is exhaustive and can be fully populated with 4096 VLAN
 * configurations, the same does not hold for the VLAN membership configuration
 * database, which is limited to 32 entries.
 *
 * The switch will normally only use the VLAN4k table when making forwarding
 * decisions. The VLAN membership configuration database is a vestigial ASIC
 * design and is only used for a few specific features in the rtl8365mb
 * family. This means that the limit of 32 entries should not hinder us in
 * programming a huge number of VLANs into the switch.
 *
 * One necessary use of the VLAN membership configuration database is for the
 * programming of a port-based VLAN ID (PVID). The PVID is programmed on a
 * per-port basis via register field, which refers to a specific VLAN membership
 * configuration via an index 0~31. In order to maintain coherent behaviour on a
 * port with a PVID, it is necessary to keep the VLAN configuration synchronized
 * between the VLAN4k table and the VLAN membership configuration database.
 *
 * Since VLAN membership configs are a scarce resource, it will only be used
 * when strictly needed (i.e. a VLAN with members using PVID). Otherwise, the
 * VLAN4k will be enough.
 *
 * With some exceptions, the entries in both the VLAN4k table and the VLAN
 * membership configuration database offer the same configuration options. The
 * differences are as follows:
 *
 * 1. VLAN4k entries can specify whether to use Independent or Shared VLAN
 *    Learning (IVL or SVL respectively). VLAN membership config entries
 *    cannot. This underscores the fact that VLAN membership configs are not
 *    involved in the learning process of the ASIC.
 *
 * 2. VLAN membership config entries use an "enhanced VLAN ID" (efid), which has
 *    a range 0~8191 compared with the standard 0~4095 range of the VLAN4k
 *    table. This underscores the fact that VLAN membership configs can be used
 *    to group ports on a layer beyond the standard VLAN configuration, which
 *    may be useful for ACL rules which specify alternative forwarding
 *    decisions.
 *
 * VLANMC index 0 is reserved as a neutral PVID, used for standalone ports.
 *
 */

#include "rtl8365mb_vlan.h"
#include "rtl8365mb_table.h"
#include <linux/if_bridge.h>
#include <linux/regmap.h>

/* CVLAN (i.e. VLAN4k) table entry layout, u16[3] */
#define RTL8365MB_CVLAN_ENTRY_SIZE			3 /* 48-bits */
#define RTL8365MB_CVLAN_ENTRY_D0_MBR_MASK		GENMASK(7, 0)
#define   RTL8365MB_CVLAN_MBR_LO_MASK			GENMASK(7, 0)
#define RTL8365MB_CVLAN_ENTRY_D0_UNTAG_MASK		GENMASK(15, 8)
#define   RTL8365MB_CVLAN_UNTAG_LO_MASK			GENMASK(7, 0)
#define RTL8365MB_CVLAN_ENTRY_D1_FID_MASK		GENMASK(3, 0)
#define RTL8365MB_CVLAN_ENTRY_D1_VBPEN_MASK		GENMASK(4, 4)
#define RTL8365MB_CVLAN_ENTRY_D1_VBPRI_MASK		GENMASK(7, 5)
#define RTL8365MB_CVLAN_ENTRY_D1_ENVLANPOL_MASK		GENMASK(8, 8)
#define RTL8365MB_CVLAN_ENTRY_D1_METERIDX_MASK		GENMASK(13, 9)
#define   RTL8365MB_CVLAN_METERIDX_LO_MASK		GENMASK(4, 0)
#define RTL8365MB_CVLAN_ENTRY_D1_IVL_SVL_MASK		GENMASK(14, 14)
/* extends RTL8365MB_CVLAN_ENTRY_D0_MBR_MASK */
#define RTL8365MB_CVLAN_ENTRY_D2_MBR_EXT_MASK		GENMASK(2, 0)
#define   RTL8365MB_CVLAN_MBR_HI_MASK			GENMASK(10, 8)
/* extends RTL8365MB_CVLAN_ENTRY_D0_UNTAG_MASK */
#define RTL8365MB_CVLAN_ENTRY_D2_UNTAG_EXT_MASK		GENMASK(5, 3)
#define   RTL8365MB_CVLAN_UNTAG_HI_MASK			GENMASK(10, 8)
/* extends RTL8365MB_CVLAN_ENTRY_D1_METERIDX_MASK */
#define RTL8365MB_CVLAN_ENTRY_D2_METERIDX_EXT_MASK	GENMASK(6, 6)
#define   RTL8365MB_CVLAN_METERIDX_HI_MASK		GENMASK(5, 5)

/* VLAN member configuration registers 0~31, u16[3] */
#define RTL8365MB_VLAN_MC_BASE				0x0728
#define RTL8365MB_VLAN_MC_ENTRY_SIZE			4 /* 64-bit */
#define RTL8365MB_VLAN_MC_REG(index) \
		(RTL8365MB_VLAN_MC_BASE + \
		 (RTL8365MB_VLAN_MC_ENTRY_SIZE * (index)))
#define   RTL8365MB_VLAN_MC_D0_MBR_MASK			GENMASK(10, 0)
#define   RTL8365MB_VLAN_MC_D1_FID_MASK			GENMASK(3, 0)

#define   RTL8365MB_VLAN_MC_D2_VBPEN_MASK		GENMASK(0, 0)
#define   RTL8365MB_VLAN_MC_D2_VBPRI_MASK		GENMASK(3, 1)
#define   RTL8365MB_VLAN_MC_D2_ENVLANPOL_MASK		GENMASK(4, 4)
#define   RTL8365MB_VLAN_MC_D2_METERIDX_MASK		GENMASK(10, 5)
#define   RTL8365MB_VLAN_MC_D3_EVID_MASK		GENMASK(12, 0)

/* Some limits for VLAN4k/VLAN membership config entries */
#define RTL8365MB_PRIORITYMAX	7
#define RTL8365MB_FIDMAX	15
#define RTL8365MB_METERMAX	63
#define RTL8365MB_VLAN_MCMAX	31

/* RTL8367S supports 4k vlans (vid<=4095) and 32 enhanced vlans
 * for VIDs up to 8191
 */
#define RTL8365MB_MAX_4K_VID	0x0FFF /* 4095 */
#define RTL8365MB_MAX_MC_VID	0x1FFF /* 8191 */

 /* Port-based VID registers 0~5 - each one holds an MC index for two ports */
#define RTL8365MB_VLAN_PVID_CTRL_BASE			0x0700
#define RTL8365MB_VLAN_PVID_CTRL_REG(_p) \
		(RTL8365MB_VLAN_PVID_CTRL_BASE + ((_p) >> 1))
#define   RTL8365MB_VLAN_PVID_CTRL_PORT0_MCIDX_MASK	0x001F
#define   RTL8365MB_VLAN_PVID_CTRL_PORT1_MCIDX_MASK	0x1F00
#define   RTL8365MB_VLAN_PVID_CTRL_PORT_MCIDX_OFFSET(_p) \
		(((_p) & 1) << 3)
#define   RTL8365MB_VLAN_PVID_CTRL_PORT_MCIDX_MASK(_p) \
		(0x1F << RTL8365MB_VLAN_PVID_CTRL_PORT_MCIDX_OFFSET(_p))

/* Frame type filtering registers */
#define RTL8365MB_VLAN_ACCEPT_FRAME_TYPE_BASE	0x07aa
#define RTL8365MB_VLAN_ACCEPT_FRAME_TYPE_REG(port) \
		(RTL8365MB_VLAN_ACCEPT_FRAME_TYPE_BASE + ((port) >> 3))
/* required as FIELD_PREP cannot use non-constant masks */
#define RTL8365MB_VLAN_ACCEPT_FRAME_TYPE_MASK(port) \
		(0x3 << RTL8365MB_VLAN_ACCEPT_FRAME_TYPE_OFFSET(port))
#define RTL8365MB_VLAN_ACCEPT_FRAME_TYPE_OFFSET(port) \
		(((port) & 0x7) << 1)

/**
 * struct rtl8365mb_vlan4k - VLAN4k table entry
 * @vid: VLAN ID (0~4095)
 * @member: port mask of ports in this VLAN
 * @untag: port mask of ports which untag on egress
 * @fid: filter ID - only used with SVL (unused)
 * @priority: priority classification (unused)
 * @priority_en: enable priority (unused)
 * @policing_en: enable policing (unused)
 * @ivl_en: enable IVL instead of default SVL
 * @meteridx: metering index (unused)
 *
 * This structure is used to get/set entries in the VLAN4k table. The
 * VLAN4k table dictates the VLAN configuration for the switch for the
 * vast majority of features.
 */
struct rtl8365mb_vlan4k {
	u16 vid;
	u16 member;
	u16 untag;
	u8 fid : 4;
	u8 priority : 3;
	u8 priority_en : 1;
	u8 policing_en : 1;
	u8 ivl_en : 1;
	u8 meteridx : 6;
};

/**
 * struct rtl8365mb_vlanmc - VLAN membership config
 * @evid: Enhanced VLAN ID (0~8191)
 * @member: port mask of ports in this VLAN
 * @fid: filter ID - only used with SVL (unused)
 * @priority: priority classification (unused)
 * @priority_en: enable priority (unused)
 * @policing_en: enable policing (unused)
 * @meteridx: metering index (unused)
 *
 * This structure is used to get/set entries in the VLAN membership
 * configuration database. This feature is largely vestigial, but
 * still needed for at least the following features:
 *   - PVID configuration
 *   - ACL configuration
 *   - selection of VLAN by the CPU tag when VSEL=1, although the switch
 *     can also select VLAN based on the VLAN tag if VSEL=0
 *
 * This is a low-level structure and it is recommended to interface with
 * the VLAN membership config database via &struct rtl8365mb_vlanmc_entry.
 */
struct rtl8365mb_vlanmc {
	u16 evid;
	u16 member;
	u8 fid : 4;
	u8 priority : 3;
	u8 priority_en : 1;
	u8 policing_en : 1;
	u8 meteridx : 6;
};

enum rtl8365mb_frame_ingress {
	RTL8365MB_FRAME_TYPE_ANY_FRAME = 0,
	RTL8365MB_FRAME_TYPE_TAGGED_ONLY,
	RTL8365MB_FRAME_TYPE_UNTAGGED_ONLY,
};

static int rtl8365mb_vlan_4k_read(struct realtek_priv *priv, u16 vid,
				  struct rtl8365mb_vlan4k *vlan4k)
{
	u16 data[RTL8365MB_CVLAN_ENTRY_SIZE];
	int val;
	int ret;

	ret = rtl8365mb_table_query(priv, RTL8365MB_TABLE_CVLAN,
				    RTL8365MB_TABLE_OP_READ, &vid, 0, 0,
				    data, ARRAY_SIZE(data));
	if (ret)
		return ret;

	/* Unpack table entry */
	memset(vlan4k, 0, sizeof(*vlan4k));
	vlan4k->vid = vid;

	val = FIELD_GET(RTL8365MB_CVLAN_ENTRY_D0_MBR_MASK, data[0]);
	vlan4k->member = FIELD_PREP(RTL8365MB_CVLAN_MBR_LO_MASK, val);
	val = FIELD_GET(RTL8365MB_CVLAN_ENTRY_D2_MBR_EXT_MASK, data[2]);
	vlan4k->member |= FIELD_PREP(RTL8365MB_CVLAN_MBR_HI_MASK, val);

	val = FIELD_GET(RTL8365MB_CVLAN_ENTRY_D0_UNTAG_MASK, data[0]);
	vlan4k->untag = FIELD_PREP(RTL8365MB_CVLAN_UNTAG_LO_MASK, val);
	val = FIELD_GET(RTL8365MB_CVLAN_ENTRY_D2_UNTAG_EXT_MASK, data[2]);
	vlan4k->untag |= FIELD_PREP(RTL8365MB_CVLAN_UNTAG_HI_MASK, val);

	vlan4k->fid = FIELD_GET(RTL8365MB_CVLAN_ENTRY_D1_FID_MASK, data[1]);
	vlan4k->priority_en =
		FIELD_GET(RTL8365MB_CVLAN_ENTRY_D1_VBPEN_MASK, data[1]);
	vlan4k->priority =
		FIELD_GET(RTL8365MB_CVLAN_ENTRY_D1_VBPRI_MASK, data[1]);
	vlan4k->policing_en =
		FIELD_GET(RTL8365MB_CVLAN_ENTRY_D1_ENVLANPOL_MASK, data[1]);

	val = FIELD_GET(RTL8365MB_CVLAN_ENTRY_D1_METERIDX_MASK, data[1]);
	val = FIELD_PREP(RTL8365MB_CVLAN_METERIDX_LO_MASK, val);
	vlan4k->meteridx = val;
	val = FIELD_GET(RTL8365MB_CVLAN_ENTRY_D2_METERIDX_EXT_MASK, data[2]);
	val = FIELD_PREP(RTL8365MB_CVLAN_METERIDX_HI_MASK, val);
	vlan4k->meteridx |= val;

	vlan4k->ivl_en =
		FIELD_GET(RTL8365MB_CVLAN_ENTRY_D1_IVL_SVL_MASK, data[1]);

	return 0;
}

static int rtl8365mb_vlan_4k_write(struct realtek_priv *priv,
				   const struct rtl8365mb_vlan4k *vlan4k)
{
	u16 data[RTL8365MB_CVLAN_ENTRY_SIZE] = { 0 };
	u16 vid;
	int val;

	if (vlan4k->fid > RTL8365MB_FIDMAX ||
	    vlan4k->priority > RTL8365MB_PRIORITYMAX ||
	    vlan4k->meteridx > RTL8365MB_METERMAX)
		return -EINVAL;

	/* Pack table entry value */
	val = FIELD_GET(RTL8365MB_CVLAN_MBR_LO_MASK, vlan4k->member);
	data[0] |= FIELD_PREP(RTL8365MB_CVLAN_ENTRY_D0_MBR_MASK, val);

	val = FIELD_GET(RTL8365MB_CVLAN_UNTAG_LO_MASK, vlan4k->untag);
	data[0] |= FIELD_PREP(RTL8365MB_CVLAN_ENTRY_D0_UNTAG_MASK, val);

	data[1] |= FIELD_PREP(RTL8365MB_CVLAN_ENTRY_D1_FID_MASK, vlan4k->fid);
	data[1] |= FIELD_PREP(RTL8365MB_CVLAN_ENTRY_D1_VBPEN_MASK,
			      vlan4k->priority_en);
	data[1] |= FIELD_PREP(RTL8365MB_CVLAN_ENTRY_D1_VBPRI_MASK,
			      vlan4k->priority);
	data[1] |= FIELD_PREP(RTL8365MB_CVLAN_ENTRY_D1_ENVLANPOL_MASK,
			      vlan4k->policing_en);

	/* FIELD_* does not play nice with struct bitfield. */
	val = vlan4k->meteridx;
	val = FIELD_GET(RTL8365MB_CVLAN_METERIDX_LO_MASK, val);
	data[1] |= FIELD_PREP(RTL8365MB_CVLAN_ENTRY_D1_METERIDX_MASK, val);

	data[1] |= FIELD_PREP(RTL8365MB_CVLAN_ENTRY_D1_IVL_SVL_MASK,
			      vlan4k->ivl_en);

	val = FIELD_GET(RTL8365MB_CVLAN_MBR_HI_MASK, vlan4k->member);
	data[2] |= FIELD_PREP(RTL8365MB_CVLAN_ENTRY_D2_MBR_EXT_MASK, val);

	val = FIELD_GET(RTL8365MB_CVLAN_UNTAG_HI_MASK, vlan4k->untag);
	data[2] |= FIELD_PREP(RTL8365MB_CVLAN_ENTRY_D2_UNTAG_EXT_MASK, val);

	val = vlan4k->meteridx;
	val = FIELD_GET(RTL8365MB_CVLAN_METERIDX_HI_MASK, val);
	data[2] |= FIELD_PREP(RTL8365MB_CVLAN_ENTRY_D2_METERIDX_EXT_MASK, val);

	vid = vlan4k->vid;
	return rtl8365mb_table_query(priv, RTL8365MB_TABLE_CVLAN,
				     RTL8365MB_TABLE_OP_WRITE, &vid, 0, 0,
				     data, ARRAY_SIZE(data));
}

#define RTL_VLAN_ERR(msg)						\
	do {								\
		const char *__msg = (msg);				\
									\
		if (extack)						\
			NL_SET_ERR_MSG_FMT_MOD(extack, "%s", __msg);	\
		dev_err(priv->dev, "%s", __msg);			\
	} while (0)

static int
rtl8365mb_vlan_4k_port_set(struct dsa_switch *ds, int port,
			   const struct switchdev_obj_port_vlan *vlan,
			   struct netlink_ext_ack *extack,
			       bool include)
{
	struct realtek_priv *priv = ds->priv;
	struct rtl8365mb_vlan4k vlan4k = {0};
	int ret;

	dev_dbg(priv->dev, "%s VLAN %d 4K on port %d\n",
		include ? "add" : "del",
		vlan->vid, port);

	if (vlan->vid > RTL8365MB_MAX_4K_VID) {
		RTL_VLAN_ERR("VLAN ID greater than "
			     __stringify(RTL8365MB_MAX_4K_VID));
		return -EINVAL;
	}

	ret = rtl8365mb_vlan_4k_read(priv, vlan->vid, &vlan4k);
	if (ret) {
		RTL_VLAN_ERR("Failed to read VLAN 4k table");
		return ret;
	}

	if (include)
		vlan4k.member |= BIT(port);
	else
		vlan4k.member &= ~BIT(port);

	if (include && (vlan->flags & BRIDGE_VLAN_INFO_UNTAGGED))
		vlan4k.untag |= BIT(port);
	else
		vlan4k.untag &= ~BIT(port);
	vlan4k.ivl_en = true; /* always use Independent VLAN Learning */

	ret = rtl8365mb_vlan_4k_write(priv, &vlan4k);
	if (ret) {
		RTL_VLAN_ERR("Failed to write VLAN 4k table");
		return ret;
	}

	return 0;
}

int rtl8365mb_vlan_4k_port_add(struct dsa_switch *ds, int port,
			       const struct switchdev_obj_port_vlan *vlan,
			       struct netlink_ext_ack *extack)
{
	return rtl8365mb_vlan_4k_port_set(ds, port, vlan, extack, true);
}

int rtl8365mb_vlan_4k_port_del(struct dsa_switch *ds, int port,
			       const struct switchdev_obj_port_vlan *vlan)
{
	return rtl8365mb_vlan_4k_port_set(ds, port, vlan, NULL, false);
}

static int rtl8365mb_vlan_mc_read(struct realtek_priv *priv, u32 index,
				  struct rtl8365mb_vlanmc *vlanmc)
{
	u16 data[RTL8365MB_VLAN_MC_ENTRY_SIZE];
	int ret;

	if (index > RTL8365MB_VLAN_MCMAX)
		return -EINVAL;

	ret = regmap_bulk_read(priv->map, RTL8365MB_VLAN_MC_REG(index), &data,
			       RTL8365MB_VLAN_MC_ENTRY_SIZE);
	if (ret)
		return ret;

	vlanmc->member = FIELD_GET(RTL8365MB_VLAN_MC_D0_MBR_MASK, data[0]);
	vlanmc->fid = FIELD_GET(RTL8365MB_VLAN_MC_D1_FID_MASK, data[1]);
	vlanmc->priority = FIELD_GET(RTL8365MB_VLAN_MC_D2_VBPRI_MASK, data[2]);
	vlanmc->evid = FIELD_GET(RTL8365MB_VLAN_MC_D3_EVID_MASK, data[3]);

	return 0;
}

static int rtl8365mb_vlan_mc_write(struct realtek_priv *priv, u32 index,
				   const struct rtl8365mb_vlanmc *vlanmc)
{
	u16 data[4] = { 0 };
	int ret;

	if (index > RTL8365MB_VLAN_MCMAX ||
	    vlanmc->fid > RTL8365MB_FIDMAX ||
	    vlanmc->priority > RTL8365MB_PRIORITYMAX ||
	    vlanmc->meteridx > RTL8365MB_METERMAX)
		return -EINVAL;

	data[0] |= FIELD_PREP(RTL8365MB_VLAN_MC_D0_MBR_MASK, vlanmc->member);
	data[1] |= FIELD_PREP(RTL8365MB_VLAN_MC_D1_FID_MASK, vlanmc->fid);
	data[2] |= FIELD_PREP(RTL8365MB_VLAN_MC_D2_METERIDX_MASK,
			      vlanmc->meteridx);
	data[2] |= FIELD_PREP(RTL8365MB_VLAN_MC_D2_ENVLANPOL_MASK,
			      vlanmc->policing_en);
	data[2] |=
		FIELD_PREP(RTL8365MB_VLAN_MC_D2_VBPRI_MASK, vlanmc->priority);
	data[2] |= FIELD_PREP(RTL8365MB_VLAN_MC_D2_VBPEN_MASK,
			      vlanmc->priority_en);
	data[3] |= FIELD_PREP(RTL8365MB_VLAN_MC_D3_EVID_MASK, vlanmc->evid);

	ret = regmap_bulk_write(priv->map, RTL8365MB_VLAN_MC_REG(index), &data,
				RTL8365MB_VLAN_MC_ENTRY_SIZE);

	return ret;
}

static int rtl8365mb_vlan_mc_erase(struct realtek_priv *priv, u32 index)
{
	u16 data[4] = { 0 };
	int ret;

	if (index > RTL8365MB_VLAN_MCMAX)
		return -EINVAL;

	ret = regmap_bulk_write(priv->map, RTL8365MB_VLAN_MC_REG(index), &data,
				RTL8365MB_VLAN_MC_ENTRY_SIZE);

	return ret;
}

/**
 * rtl8365mb_vlan_mc_find() - find VLANMC index by VID or the first free index
 *
 * @priv: realtek_priv pointer
 * @vid: VLAN ID
 * @index: found index
 * @first_free: found free index
 *
 * If a VLAN MC entry using @vid was found, @index will return the matched index
 * and @first_free is undefined. If not found, @index will return 0 and
 * @first_free will return the first found free index in VLAN MC or 0 if the
 * table is full.
 *
 * Although 0 is a valid VLAN MC index, it is reserved for ports without PVID,
 * including standalone, non-member ports.
 *
 * Both @index and @first_free will be in the * 1..@RTL8365MB_VLAN_MCMAX range.
 *
 * Return: Returns 0 on success, a negative error on failure.
 *
 */
static int rtl8365mb_vlan_mc_find(struct realtek_priv *priv, u16 vid,
				  u8 *index, u8 *first_free)
{
	u32 vlan_entry_d3;
	u8 vlanmc_idx;
	u16 evid;
	int ret;

	if (!index)
		return -EINVAL;
	if (!first_free)
		return -EINVAL;
	if (!vid)
		return -EINVAL;

	*index = 0;
	*first_free = 0;

	/* look for existing entry or an empty one */
	/* vlanmc index 0 is reserved as a neutral PVID value for standalone
	 * ports. Traffic reaches the CPU via VLAN transparent mode.
	 **/
	for (vlanmc_idx = 1; vlanmc_idx <= RTL8365MB_VLAN_MCMAX; vlanmc_idx++) {
		/* just read the 4th word, where the evid is */
		ret = regmap_read(priv->map,
				  RTL8365MB_VLAN_MC_REG(vlanmc_idx) + 3,
				  &vlan_entry_d3);
		if (ret)
			return ret;

		evid = FIELD_GET(RTL8365MB_VLAN_MC_D3_EVID_MASK, vlan_entry_d3);

		if (evid == vid) {
			*index = vlanmc_idx;
			return 0;
		}

		if (evid == 0x0 && *first_free < 1)
			*first_free = vlanmc_idx;
	}
	return 0;
}

static int rtl8365mb_vlan_port_get_pvid(struct realtek_priv *priv,
					int port, u8 *vlanmc_idx)
{
	u32 data;
	int ret;

	ret = regmap_read(priv->map, RTL8365MB_VLAN_PVID_CTRL_REG(port), &data);
	if (ret)
		return ret;

	*vlanmc_idx = (data & RTL8365MB_VLAN_PVID_CTRL_PORT_MCIDX_MASK(port))
		      >> RTL8365MB_VLAN_PVID_CTRL_PORT_MCIDX_OFFSET(port);

	return 0;
}

/**
 * rtl8365mb_vlan_mc_pvid_members() - Get a bitmap of vlan PVID members
 *
 * @ds: DSA switch
 * @vlanmc_idx: the index of a VLAN in VLAN MC table
 * @members: the returned bitmap of members that have PVID status
 *
 * This function iterates over DSA ports and creates a bitmap representation of
 * those ports that have PVID pointing to this VLAN (identified by its table
 * index and not VID). If you need to get the table index from VID, see
 * rtl8365mb_vlan_mc_find()
 *
 * Return: Returns 0 on success, a negative error on failure.
 **/
static int rtl8365mb_vlan_mc_pvid_members(struct dsa_switch *ds,
					  u8 vlanmc_idx, u16 *members)
{
	struct realtek_priv *priv = ds->priv;
	struct dsa_port *dp;
	u8 _vlanmc_idx;
	int ret;

	if (!members)
		return -EINVAL;

	*members = 0;

	dsa_switch_for_each_available_port(dp, ds) {
		ret = rtl8365mb_vlan_port_get_pvid(priv, dp->index,
						   &_vlanmc_idx);
		if (ret)
			return ret;

		if (_vlanmc_idx == vlanmc_idx)
			*members |= BIT(dp->index);
	}

	return 0;
}

/** rtl8365mb_vlanmc_port_set() - include or exclude a port from vlanMC
 * @ds: dsa switch
 * @port: the port number
 * @vlan: the vlan to include/exclude @port
 * @extack: optional extack to return errors
 * @include: whether to include or exclude @port
 *
 * This function is used to include/exclude ports to the vlanMC table.
 *
 * VlanMC stands for VLAN membership config and it is used exclusively for
 * PVID. If @vlan members are not using PVID, this function will either
 * remove or not create a new vlanMC entry.
 *
 * vlanMC members are kept in sync with vlan4k, although the switch only
 * checks membership in vlan4k table.
 *
 * Port PVID and accepted frame type are updated as well.
 *
 * Return: Returns 0 on success, a negative error on failure.
 *
 */
static
int rtl8365mb_vlan_mc_port_set(struct dsa_switch *ds, int port,
			       const struct switchdev_obj_port_vlan *vlan,
			       struct netlink_ext_ack *extack,
			       bool include)
{
	bool pvid = !!(vlan->flags & BRIDGE_VLAN_INFO_PVID);
	struct realtek_priv *priv = ds->priv;
	struct rtl8365mb_vlan4k vlan4k = {0};
	struct rtl8365mb_vlanmc vlanmc = {0};
	u16 pvid_members = 0;
	u8 first_unused = 0;
	u8 vlanmc_idx = 0;
	int ret;

	dev_dbg(priv->dev, "%s VLAN %d MC on port %d\n",
		include ? "add" : "del",
		vlan->vid, port);

	if (vlan->vid > RTL8365MB_MAX_MC_VID) {
		RTL_VLAN_ERR("VLAN ID greater than "
			     __stringify(RTL8365MB_MAX_MC_VID));
		return -EINVAL;
	}

	/* look for existing entry or an empty slot */
	ret = rtl8365mb_vlan_mc_find(priv, vlan->vid, &vlanmc_idx,
				     &first_unused);
	if (ret) {
		RTL_VLAN_ERR("Failed to find a VLAN MC table index");
		return ret;
	}

	if (vlanmc_idx) {
		ret = rtl8365mb_vlan_mc_read(priv, vlanmc_idx, &vlanmc);
		if (ret) {
			RTL_VLAN_ERR("Failed to read VLAN MC table");
			return ret;
		}
	} else if (include) {
		/* for now, vlan_mc is only required for PVID. Defer allocation
		 * until at least one port uses PVID.
		 */
		if (!pvid) {
			dev_dbg(priv->dev,
				"Not creating VlanMC for vlan %d until a port uses PVID (%d does not)\n",
				vlan->vid, port);
			return 0;
		}

		if (!first_unused) {
			RTL_VLAN_ERR("All VLAN MC entries ("
				     __stringify(RTL8365MB_VLAN_MCMAX + 1)
				     ") are in use.");
			return -E2BIG;
		}

		/* Retrieve vlan4k members as we might have deferred VlanMC
		 * before.
		 */
		if (vlan->vid <= RTL8365MB_MAX_4K_VID) {
			ret = rtl8365mb_vlan_4k_read(priv, vlan->vid, &vlan4k);
			if (ret) {
				RTL_VLAN_ERR("Failed to read VLAN 4k table");
				return ret;
			}
		}

		vlanmc_idx = first_unused;
		vlanmc.evid = vlan->vid;

		/* for new vlan_mc, sync current vlan4k members,
		 * although only vlan4k members matter.
		 */
		vlanmc.member |= vlan4k.member;
	} else /* excluding and VLANMC not found */ {
		return 0;
	}

	ret = rtl8365mb_vlan_mc_pvid_members(ds, vlanmc_idx,
					     &pvid_members);
	if (ret) {
		RTL_VLAN_ERR("Failed to read VLANMC PVID members");
		return ret;
	}
	dev_dbg(priv->dev,
		"VLAN %d (idx: %d) PVID curr members: %08x\n",
		vlan->vid, vlanmc_idx, pvid_members);

	/* here we either have an existing VLANMC (with PVID members) or the
	 * added port is using this VLAN as PVID
	 */
	if (include) {
		vlanmc.member |= BIT(port);
		if (pvid)
			pvid_members |= BIT(port);
	} else {
		vlanmc.member &= ~BIT(port);
		pvid_members &= ~BIT(port);
	}

	/* just like we don't need to create a VLAN_MC when there is no port
	 * using it as PVID, we can erase it when there is no more port using
	 * it as PVID.
	 */
	if (!pvid_members) {
		dev_dbg(priv->dev,
			"Clearing VlanMC index %d previously used by VID %d\n",
			vlanmc_idx, vlan->vid);
		ret = rtl8365mb_vlan_mc_erase(priv, vlanmc_idx);
	} else {
		dev_dbg(priv->dev,
			"Saving VlanMC index %d with VID %d\n",
			vlanmc_idx, vlan->vid);
		ret = rtl8365mb_vlan_mc_write(priv, vlanmc_idx, &vlanmc);
	}
	if (ret) {
		RTL_VLAN_ERR("Failed to write vlan MC entry");
		return ret;
	}

	return 0;
}

static int rtl8365mb_vlan_port_set_pvid(struct realtek_priv *priv,
					int port, u16 vlanmc_idx)
{
	int ret;
	u32 val;

	val = vlanmc_idx << RTL8365MB_VLAN_PVID_CTRL_PORT_MCIDX_OFFSET(port);
	ret = regmap_update_bits(priv->map,
				 RTL8365MB_VLAN_PVID_CTRL_REG(port),
				 RTL8365MB_VLAN_PVID_CTRL_PORT_MCIDX_MASK(port),
				 val);
	if (ret)
		return ret;

	return 0;
}

static int
rtl8365mb_vlan_port_set_framefilter(struct realtek_priv *priv,
				    int port,
				    enum rtl8365mb_frame_ingress accepted_frame)
{
	/* Even if ACCEPT_FRAME_TYPE_ANY, the switch will still check if the
	 * port is a member of vlan PVID
	 */
	accepted_frame = accepted_frame
			 << RTL8365MB_VLAN_ACCEPT_FRAME_TYPE_OFFSET(port);

	return regmap_update_bits(priv->map,
				  RTL8365MB_VLAN_ACCEPT_FRAME_TYPE_REG(port),
				  RTL8365MB_VLAN_ACCEPT_FRAME_TYPE_MASK(port),
				  accepted_frame);
}

int rtl8365mb_vlan_pvid_port_add(struct dsa_switch *ds, int port,
				 const struct switchdev_obj_port_vlan *vlan,
				 struct netlink_ext_ack *extack)
{
	bool pvid = !!(vlan->flags & BRIDGE_VLAN_INFO_PVID);
	enum rtl8365mb_frame_ingress accepted_frame;
	struct realtek_priv *priv = ds->priv;
	u8 _unused_first_free_idx;
	u8 vlanmc_idx;
	int ret;

	if (!pvid)
		return 0;

	/* Find or allocate a new vlan MC and add port to members,
	 * although members are not checked by the HW in vlan MC.
	 */
	ret = rtl8365mb_vlan_mc_port_set(ds, port, vlan, extack, true);
	if (ret)
		return ret;

	/* look for existing entry */
	ret = rtl8365mb_vlan_mc_find(priv, vlan->vid, &vlanmc_idx,
				     &_unused_first_free_idx);
	if (ret) {
		RTL_VLAN_ERR("Failed to find a VLAN MC table index");
		goto undo_vlan_mc_port_set;
	}

	if (!vlanmc_idx) {
		RTL_VLAN_ERR("VLAN should already exist in VLAN MC");
		goto undo_vlan_mc_port_set;
	}

	ret = rtl8365mb_vlan_port_set_pvid(priv, port, vlanmc_idx);
	if (ret) {
		RTL_VLAN_ERR("Failed to set port PVID");
		goto undo_vlan_mc_port_set;
	}

	/* Changing accept frame is what enables PVID (if not enabled before) */
	accepted_frame = RTL8365MB_FRAME_TYPE_ANY_FRAME;
	ret = rtl8365mb_vlan_port_set_framefilter(priv, port, accepted_frame);
	if (ret) {
		RTL_VLAN_ERR("Failed to set port frame filter");
		goto undo_vlan_pvid_port_set;
	}

	return 0;

undo_vlan_pvid_port_set:
	(void)rtl8365mb_vlan_port_set_pvid(priv, port, 0);

undo_vlan_mc_port_set:
	(void)rtl8365mb_vlan_mc_port_set(ds, port, vlan, NULL, false);
	return ret;
}

int rtl8365mb_vlan_pvid_port_del(struct dsa_switch *ds, int port,
				 const struct switchdev_obj_port_vlan *vlan)
{
	enum rtl8365mb_frame_ingress accepted_frame;
	struct netlink_ext_ack *extack = NULL;
	struct realtek_priv *priv = ds->priv;
	struct rtl8365mb_vlanmc vlanmc = {0};
	u8 vlanmc_idx;
	int ret;

	ret = rtl8365mb_vlan_port_get_pvid(priv, port, &vlanmc_idx);
	if (ret)
		return ret;

	/* Port is not using PVID. Nothing to remove. */
	if (!vlanmc_idx)
		return 0;

	ret = rtl8365mb_vlan_mc_read(priv, vlanmc_idx, &vlanmc);
	if (ret) {
		RTL_VLAN_ERR("Failed to read VLAN MC table");
		return ret;
	}

	/* We are leaving a non PVID vlan, Nothing to remove. */
	if (vlanmc.evid != vlan->vid)
		return 0;

	/* Changing accept frame is what really removes PVID */
	accepted_frame = RTL8365MB_FRAME_TYPE_TAGGED_ONLY;
	ret = rtl8365mb_vlan_port_set_framefilter(priv, port, accepted_frame);
	if (ret) {
		RTL_VLAN_ERR("Failed to set port frame filter");
		return ret;
	}

	ret = rtl8365mb_vlan_port_set_pvid(priv, port, 0);
	if (ret) {
		RTL_VLAN_ERR("Failed to set port PVID to 0");
		return ret;
	}

	/* Clears the VLAN MC membership and maybe VLAN MC entry if empty */
	return rtl8365mb_vlan_mc_port_set(ds, port, vlan, NULL, false);
}
