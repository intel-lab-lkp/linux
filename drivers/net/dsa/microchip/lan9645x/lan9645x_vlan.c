// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2026 Microchip Technology Inc.
 */

#include "lan9645x_main.h"

#define VLANACCESS_CMD_IDLE		0
#define VLANACCESS_CMD_READ		1
#define VLANACCESS_CMD_WRITE		2
#define VLANACCESS_CMD_INIT		3

int lan9645x_port_vlan_prepare(struct lan9645x_port *p, u16 vid, bool pvid,
			       bool untagged, struct netlink_ext_ack *extack)
{
	struct lan9645x *lan9645x = p->lan9645x;

	if (p->chip_port == lan9645x->npi)
		return 0;

	if (untagged && p->untagged_vid && p->untagged_vid != vid) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Only one egress-untagged VLAN allowed.");
		return -EBUSY;
	}

	if (vid > VLAN_MAX) {
		NL_SET_ERR_MSG_MOD(extack, "VLAN range 4094-4095 reserved.");
		return -EBUSY;
	}

	return 0;
}

static int lan9645x_vlan_wait_for_completion(struct lan9645x *lan9645x)
{
	u32 val;

	return lan9645x_rd_poll_timeout(lan9645x, ANA_VLANACCESS, val,
					ANA_VLANACCESS_VLAN_TBL_CMD_GET(val) ==
					VLANACCESS_CMD_IDLE);
}

static void lan9645x_vlan_set_mask(struct lan9645x *lan9645x, u16 vid)
{
	u16 mask = lan9645x->vlan_mask[vid];
	u8 f = lan9645x->vlan_flags[vid];
	bool cpu_dis;
	u32 val;

	cpu_dis = !(mask & BIT(CPU_PORT));

	/* Set flags and the VID to configure */
	val = ANA_VLANTIDX_VLAN_PGID_CPU_DIS_SET(cpu_dis) |
	      ANA_VLANTIDX_V_INDEX_SET(vid) |
	      ANA_VLANTIDX_VLAN_SEC_FWD_ENA_SET(!!(f & VLAN_S_FWD_ENA)) |
	      ANA_VLANTIDX_VLAN_FLOOD_DIS_SET(!!(f & VLAN_FLD_DIS)) |
	      ANA_VLANTIDX_VLAN_PRIV_VLAN_SET(!!(f & VLAN_PRV_VLAN)) |
	      ANA_VLANTIDX_VLAN_LEARN_DISABLED_SET(!!(f & VLAN_LRN_DIS)) |
	      ANA_VLANTIDX_VLAN_MIRROR_SET(!!(f & VLAN_MIR)) |
	      ANA_VLANTIDX_VLAN_SRC_CHK_SET(!!(f & VLAN_SRC_CHK));

	lan_wr(val, lan9645x, ANA_VLANTIDX);

	/* Set the vlan port members mask, which enables ingress filtering */
	lan_wr(mask, lan9645x, ANA_VLAN_PORT_MASK);

	/* Issue a write command */
	lan_wr(VLANACCESS_CMD_WRITE, lan9645x, ANA_VLANACCESS);

	if (lan9645x_vlan_wait_for_completion(lan9645x))
		dev_err(lan9645x->dev, "Vlan set mask failed\n");
}

static void lan9645x_vlan_port_add_vlan_mask(struct lan9645x_port *p, u16 vid)
{
	struct lan9645x *lan9645x = p->lan9645x;

	lan9645x->vlan_mask[vid] |= BIT(p->chip_port);
	lan9645x_vlan_set_mask(lan9645x, vid);
}

static void lan9645x_vlan_port_del_vlan_mask(struct lan9645x_port *p, u16 vid)
{
	struct lan9645x *lan9645x = p->lan9645x;

	lan9645x->vlan_mask[vid] &= ~BIT(p->chip_port);
	lan9645x_vlan_set_mask(lan9645x, vid);
}

static void lan9645x_vlan_cpu_add_vlan_mask(struct lan9645x *lan9645x, u16 vid)
{
	lan9645x->vlan_mask[vid] |= BIT(CPU_PORT);
	lan9645x_vlan_set_mask(lan9645x, vid);
}

static void lan9645x_vlan_cpu_del_vlan_mask(struct lan9645x *lan9645x, u16 vid)
{
	lan9645x->vlan_mask[vid] &= ~BIT(CPU_PORT);
	lan9645x_vlan_set_mask(lan9645x, vid);
}

static bool lan9645x_vlan_port_any_vlan_mask(struct lan9645x *lan9645x, u16 vid)
{
	return !!(lan9645x->vlan_mask[vid] & ~BIT(CPU_PORT));
}

void lan9645x_vlan_cpu_set_vlan(struct lan9645x *lan9645x, u16 vid)
{
	__set_bit(vid, lan9645x->cpu_vlan_mask);
}

void lan9645x_vlan_cpu_clear_vlan(struct lan9645x *lan9645x, u16 vid)
{
	__clear_bit(vid, lan9645x->cpu_vlan_mask);
}

static bool lan9645x_vlan_cpu_member_cpu_vlan_mask(struct lan9645x *lan9645x,
						   u16 vid)
{
	return test_bit(vid, lan9645x->cpu_vlan_mask);
}

u16 lan9645x_vlan_unaware_pvid(bool is_bridged)
{
	return is_bridged ? UNAWARE_PVID : HOST_PVID;
}

static u16 lan9645x_vlan_port_get_pvid(struct lan9645x_port *port)
{
	bool is_bridged = lan9645x_port_is_bridged(port);

	if (is_bridged && port->vlan_aware)
		return port->pvid;
	else
		return lan9645x_vlan_unaware_pvid(is_bridged);
}

static void lan9645x_vlan_port_set_vid(struct lan9645x_port *p, u16 vid,
				       bool pvid, bool untagged)
{
	/* Egress vlan classification */
	if (untagged)
		p->untagged_vid = vid;

	/* Default ingress vlan classification */
	if (pvid)
		p->pvid = vid;
}

static void lan9645x_vlan_port_remove_vid(struct lan9645x_port *p, u16 vid)
{
	if (p->pvid == vid)
		p->pvid = 0;

	if (p->untagged_vid == vid)
		p->untagged_vid = 0;
}

void lan9645x_vlan_port_set_vlan_aware(struct lan9645x_port *p,
				       bool vlan_aware)
{
	p->vlan_aware = vlan_aware;
}

void lan9645x_vlan_port_apply(struct lan9645x_port *p)
{
	struct lan9645x *lan9645x = p->lan9645x;
	u32 val, tag_cfg;
	u16 pvid;

	pvid = lan9645x_vlan_port_get_pvid(p);

	/* Ingress classification (ANA_PORT_VLAN_CFG) */
	/* Default vlan to classify for untagged frames (may be zero) */
	val = ANA_VLAN_CFG_VLAN_VID_SET(pvid);
	if (p->vlan_aware)
		val |= ANA_VLAN_CFG_VLAN_AWARE_ENA_SET(1) |
			ANA_VLAN_CFG_VLAN_POP_CNT_SET(1);

	lan_rmw(val,
		ANA_VLAN_CFG_VLAN_VID |
		ANA_VLAN_CFG_VLAN_AWARE_ENA |
		ANA_VLAN_CFG_VLAN_POP_CNT,
		lan9645x, ANA_VLAN_CFG(p->chip_port));

	lan_rmw(DEV_MAC_TAGS_CFG_VLAN_AWR_ENA_SET(p->vlan_aware) |
		DEV_MAC_TAGS_CFG_PB_ENA_SET(p->vlan_aware),
		DEV_MAC_TAGS_CFG_VLAN_AWR_ENA |
		DEV_MAC_TAGS_CFG_PB_ENA,
		lan9645x, DEV_MAC_TAGS_CFG(p->chip_port));

	/* Drop frames with multicast source address */
	val = ANA_DROP_CFG_DROP_MC_SMAC_ENA_SET(1);
	if (p->vlan_aware && !pvid)
		/* If port is vlan-aware and tagged, drop untagged and priority
		 * tagged frames.
		 */
		val |= ANA_DROP_CFG_DROP_UNTAGGED_ENA_SET(1) |
			ANA_DROP_CFG_DROP_PRIO_S_TAGGED_ENA_SET(1) |
			ANA_DROP_CFG_DROP_PRIO_C_TAGGED_ENA_SET(1);

	lan_wr(val, lan9645x, ANA_DROP_CFG(p->chip_port));

	/* TAG_TPID_CFG encoding:
	 *
	 * 0: Use 0x8100.
	 * 1: Use 0x88A8.
	 * 2: Use custom value from PORT_VLAN_CFG.PORT_TPID.
	 * 3: Use PORT_VLAN_CFG.PORT_TPID, unless ingress tag was a C-tag
	 *    (EtherType = 0x8100)
	 *
	 * Use 3 and PORT_VLAN_CFG.PORT_TPID=0x88a8 to ensure stags are not
	 * rewritten to ctags on egress.
	 */
	val = REW_TAG_CFG_TAG_TPID_CFG_SET(3);
	if (p->vlan_aware) {
		if (p->untagged_vid)
			tag_cfg = LAN9645X_TAG_NO_PVID_NO_UNAWARE;
		else
			tag_cfg = LAN9645X_TAG_ALL;

		val |= REW_TAG_CFG_TAG_CFG_SET(tag_cfg);
	}

	lan_rmw(val,
		REW_TAG_CFG_TAG_TPID_CFG |
		REW_TAG_CFG_TAG_CFG,
		lan9645x, REW_TAG_CFG(p->chip_port));

	/* Set default VLAN and tag type to 8021Q */

	lan_rmw(REW_PORT_VLAN_CFG_PORT_TPID_SET(ETH_P_8021AD) |
		REW_PORT_VLAN_CFG_PORT_VID_SET(p->untagged_vid),
		REW_PORT_VLAN_CFG_PORT_TPID |
		REW_PORT_VLAN_CFG_PORT_VID,
		lan9645x, REW_PORT_VLAN_CFG(p->chip_port));
}

void lan9645x_vlan_port_add_vlan(struct lan9645x_port *p, u16 vid, bool pvid,
				 bool untagged)
{
	struct lan9645x *lan9645x = p->lan9645x;

	if (lan9645x_vlan_cpu_member_cpu_vlan_mask(lan9645x, vid))
		lan9645x_vlan_cpu_add_vlan_mask(lan9645x, vid);

	lan9645x_vlan_port_set_vid(p, vid, pvid, untagged);
	lan9645x_vlan_port_add_vlan_mask(p, vid);
	lan9645x_vlan_port_apply(p);
}

void lan9645x_vlan_port_del_vlan(struct lan9645x_port *port, u16 vid)
{
	struct lan9645x *lan9645x = port->lan9645x;

	lan9645x_vlan_port_remove_vid(port, vid);
	lan9645x_vlan_port_del_vlan_mask(port, vid);
	lan9645x_vlan_port_apply(port);

	if (!lan9645x_vlan_port_any_vlan_mask(lan9645x, vid))
		lan9645x_vlan_cpu_del_vlan_mask(lan9645x, vid);
}

static void lan9645x_vlan_port_rew_host(struct lan9645x_port *port)
{
	struct lan9645x *lan9645x = port->lan9645x;

	lan_rmw(REW_TAG_CFG_TAG_CFG_SET(LAN9645X_TAG_NO_PVID_NO_UNAWARE) |
		REW_TAG_CFG_TAG_TPID_CFG_SET(3),
		REW_TAG_CFG_TAG_CFG |
		REW_TAG_CFG_TAG_TPID_CFG,
		lan9645x, REW_TAG_CFG(port->chip_port));

	/* Standalone ports must have the reserved VID set in the rewriter,
	 * because the TAG_CFG used above acts on this PORT_VID value, otherwise
	 * untagged frames are tagged with HOST_PVID.
	 *
	 * Usually frames leaving the switch from standalone ports go to the
	 * CPU, where tagging is disabled. But if we mirror a standalone port,
	 * the problem becomes apparent.
	 */
	lan_rmw(REW_PORT_VLAN_CFG_PORT_TPID_SET(ETH_P_8021AD) |
		REW_PORT_VLAN_CFG_PORT_VID_SET(HOST_PVID),
		REW_PORT_VLAN_CFG_PORT_TPID |
		REW_PORT_VLAN_CFG_PORT_VID,
		lan9645x, REW_PORT_VLAN_CFG(port->chip_port));
}

void lan9645x_vlan_set_hostmode(struct lan9645x_port *p)
{
	lan9645x_vlan_port_set_vlan_aware(p, false);
	lan9645x_vlan_port_set_vid(p, HOST_PVID, false, false);
	lan9645x_vlan_port_apply(p);
	lan9645x_vlan_port_rew_host(p);
}

void lan9645x_vlan_init(struct lan9645x *lan9645x)
{
	u32 all_phys_ports, all_ports;
	u16 port, vid;

	all_phys_ports = GENMASK(lan9645x->num_phys_ports - 1, 0);
	all_ports = all_phys_ports | BIT(CPU_PORT);

	/* Clear VLAN table, by default all ports are members of all VLANS */
	lan_wr(ANA_VLANACCESS_VLAN_TBL_CMD_SET(VLANACCESS_CMD_INIT),
	       lan9645x, ANA_VLANACCESS);

	if (lan9645x_vlan_wait_for_completion(lan9645x))
		dev_err(lan9645x->dev, "Vlan clear table failed\n");

	for (vid = 1; vid < VLAN_N_VID; vid++) {
		lan9645x->vlan_mask[vid] = 0;
		lan9645x_vlan_set_mask(lan9645x, vid);
	}

	/* Set all the ports + cpu to be part of HOST_PVID and UNAWARE_PVID */
	lan9645x->vlan_mask[HOST_PVID] = all_ports;
	lan9645x_vlan_set_mask(lan9645x, HOST_PVID);

	lan9645x->vlan_mask[UNAWARE_PVID] = all_ports;
	lan9645x_vlan_set_mask(lan9645x, UNAWARE_PVID);

	lan9645x_vlan_cpu_set_vlan(lan9645x, UNAWARE_PVID);

	/* Configure the CPU port to be vlan aware */
	lan_wr(ANA_VLAN_CFG_VLAN_VID_SET(UNAWARE_PVID) |
	       ANA_VLAN_CFG_VLAN_AWARE_ENA_SET(1) |
	       ANA_VLAN_CFG_VLAN_POP_CNT_SET(1),
	       lan9645x, ANA_VLAN_CFG(CPU_PORT));

	/* Set vlan ingress filter mask to all ports */
	lan_wr(all_ports, lan9645x, ANA_VLANMASK);

	for (port = 0; port < lan9645x->num_phys_ports; port++) {
		lan_wr(0, lan9645x, REW_PORT_VLAN_CFG(port));
		lan_wr(0, lan9645x, REW_TAG_CFG(port));
	}
}
