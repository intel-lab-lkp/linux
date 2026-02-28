// SPDX-License-Identifier: GPL-2.0
/*
 * Mediatek MT7628 Embedded Switch (ESW) DSA driver
 * Copyright (C) 2026 Joris Vaisvila <joey@tinyisr.com>
 *
 * Portions derived from OpenWRT esw_rt3050 driver:
 * Copyright (C) 2009-2015 John Crispin <blogic@openwrt.org>
 * Copyright (C) 2009-2015 Felix Fietkau <nbd@nbd.name>
 * Copyright (C) 2013-2015 Michael Lee <igvtee@gmail.com>
 * Copyright (C) 2016 Vittorio Gambaletta <openwrt@vittgam.net>
 */

#include <linux/platform_device.h>
#include <linux/etherdevice.h>
#include <linux/netdevice.h>
#include <linux/dsa/8021q.h>
#include <linux/if_bridge.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <net/dsa.h>

#define MT7628_ESW_REG_IMR 0x04
#define MT7628_ESW_REG_FCT0 0x08
#define MT7628_ESW_REG_PFC1 0x14
#define MT7628_ESW_REG_PVIDC(_n) (0x40 + 4 * (_n))
#define MT7628_ESW_REG_VLANI(_n) (0x50 + 4 * (_n))
#define MT7628_ESW_REG_VMSC(_n) (0x70 + 4 * (_n))
#define MT7628_ESW_REG_VUB(_n) (0x100 + 4 * (_n))
#define MT7628_ESW_REG_SOCPC 0x8c
#define MT7628_ESW_REG_POC0 0x90
#define MT7628_ESW_REG_POC2 0x98
#define MT7628_ESW_REG_SGC 0x9c
#define MT7628_ESW_REG_PCR0 0xc0
#define MT7628_ESW_REG_PCR1 0xc4
#define MT7628_ESW_REG_FPA2 0xc8
#define MT7628_ESW_REG_FCT2 0xcc
#define MT7628_ESW_REG_SGC2 0xe4

#define MT7628_ESW_PFC1_EN_VLAN GENMASK(22, 16)

#define MT7628_ESW_PVIDC_PVID_M 0xfff
#define MT7628_ESW_PVIDC_PVID_S 12
#define MT7628_ESW_VLANI_VID_M 0xfff
#define MT7628_ESW_VLANI_VID_S 12
#define MT7628_ESW_VMSC_MSC_M 0xff
#define MT7628_ESW_VMSC_MSC_S 8
#define MT7628_ESW_VUB_S 7
#define MT7628_ESW_VUB_M 0x7f

#define MT7628_ESW_SOCPC_CRC_PADDING BIT(25)
#define MT7628_ESW_SOCPC_DISBC2CPU GENMASK(22, 16)
#define MT7628_ESW_SOCPC_DISMC2CPU GENMASK(14, 8)
#define MT7628_ESW_SOCPC_DISUN2CPU GENMASK(6, 0)

#define MT7628_ESW_POC0_PORT_DISABLE GENMASK(29, 23)

#define MT7628_ESW_POC2_PER_VLAN_UNTAG_EN BIT(15)

#define MT7628_ESW_SGC_AGING_INTERVAL GENMASK(3, 0)
#define MT7628_ESW_BC_STORM_PROT GENMASK(5, 4)
#define MT7628_ESW_PKT_MAX_LEN GENMASK(7, 6)
#define MT7628_ESW_DIS_PKT_ABORT BIT(8)
#define MT7628_ESW_ADDRESS_HASH_ALG GENMASK(10, 9)
#define MT7628_ESW_DISABLE_TX_BACKOFF BIT(11)
#define MT7628_ESW_BP_JAM_CNT GENMASK(15, 12)
#define MT7628_ESW_DISMIIPORT_WASTX GENMASK(17, 16)
#define MT7628_ESW_BP_MODE GENMASK(19, 18)
#define MT7628_ESW_BISH_DIS BIT(20)
#define MT7628_ESW_BISH_TH GENMASK(22, 21)
#define MT7628_ESW_LED_FLASH_TIME GENMASK(24, 23)
#define MT7628_ESW_RMC_RULE GENMASK(26, 25)
#define MT7628_ESW_IP_MULT_RULE GENMASK(28, 27)
#define MT7628_ESW_LEN_ERR_CHK BIT(29)
#define MT7628_ESW_BKOFF_ALG BIT(30)

#define MT7628_ESW_PCR0_WT_NWAY_DATA GENMASK(31, 16)
#define MT7628_ESW_PCR0_RD_PHY_CMD BIT(14)
#define MT7628_ESW_PCR0_WT_PHY_CMD BIT(13)
#define MT7628_ESW_PCR0_CPU_PHY_REG GENMASK(12, 8)
#define MT7628_ESW_PCR0_CPU_PHY_ADDR GENMASK(4, 0)

#define MT7628_ESW_PCR1_RD_DATA GENMASK(31, 16)
#define MT7628_ESW_PCR1_RD_DONE BIT(1)
#define MT7628_ESW_PCR1_WT_DONE BIT(0)

#define MT7628_ESW_FPA2_AP_EN BIT(29)
#define MT7628_ESW_FPA2_EXT_PHY_ADDR_BASE GENMASK(28, 24)
#define MT7628_ESW_FPA2_FORCE_RGMII_LINK1 BIT(13)
#define MT7628_ESW_FPA2_FORCE_RGMII_EN1 BIT(11)

#define MT7628_ESW_FCT2_MUST_DROP_RLS_TH GENMASK(17, 13)
#define MT7628_ESW_FCT2_MUST_DROP_SET_TH GENMASK(12, 8)
#define MT7628_ESW_FCT2_MC_PER_PORT_TH GENMASK(5, 0)

#define MT7628_ESW_SGC2_SPECIAL_TAG_EN BIT(23)
#define MT7628_ESW_SGC2_TX_CPU_TPID_BIT_MAP GENMASK(22, 16)
#define MT7628_ESW_SGC2_DOUBLE_TAG_EN GENMASK(6, 0)

#define MT7628_ESW_PORTS_NOCPU GENMASK(5, 0)
#define MT7628_ESW_PORTS_CPU BIT(6)
#define MT7628_ESW_PORTS_ALL GENMASK(6, 0)

#define MT7628_ESW_NUM_PORTS 7
#define MT7628_NUM_VLANS 16

static const struct regmap_config mt7628_esw_regmap_cfg = {
	.name = "mt7628-esw",
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.fast_io = true,
	.reg_format_endian = REGMAP_ENDIAN_LITTLE,
	.val_format_endian = REGMAP_ENDIAN_LITTLE,
};

struct mt7628_vlan {
	bool active;
	u8 members;
	u8 untag;
	u16 vid;
};

struct mt7628_esw {
	void __iomem *base;
	struct reset_control *rst_ephy;
	struct reset_control *rst_esw;
	struct regmap *regmap;
	struct dsa_switch *ds;
	u16 tag_8021q_pvid[MT7628_ESW_NUM_PORTS];
	struct mt7628_vlan vlans[MT7628_NUM_VLANS];
};

static int mt7628_phy_read(struct dsa_switch *ds, int port, int regnum)
{
	int ret = 0;
	u32 val = 0;
	struct mt7628_esw *esw = ds->priv;

	ret = regmap_read_poll_timeout(esw->regmap, MT7628_ESW_REG_PCR1, val,
				       !(val & MT7628_ESW_PCR1_RD_DONE), 10,
				       5000);
	if (ret)
		goto out;

	ret = regmap_write(esw->regmap, MT7628_ESW_REG_PCR0,
			   FIELD_PREP(MT7628_ESW_PCR0_CPU_PHY_REG, regnum) |
				   FIELD_PREP(MT7628_ESW_PCR0_CPU_PHY_ADDR,
					      port) |
				   MT7628_ESW_PCR0_RD_PHY_CMD);
	if (ret)
		goto out;

	ret = regmap_read_poll_timeout(esw->regmap, MT7628_ESW_REG_PCR1, val,
				       (val & MT7628_ESW_PCR1_RD_DONE), 10,
				       5000);
out:
	if (ret) {
		dev_err(ds->dev, "read failed. MDIO timeout?\n");
		return -ETIMEDOUT;
	}
	return FIELD_GET(MT7628_ESW_PCR1_RD_DATA, val);
}

static int mt7628_phy_write(struct dsa_switch *ds, int port, int regnum,
			    u16 dat)
{
	u32 val;
	int ret = 0;
	struct mt7628_esw *esw = ds->priv;

	ret = regmap_read_poll_timeout(esw->regmap, MT7628_ESW_REG_PCR1, val,
				       !(val & MT7628_ESW_PCR1_WT_DONE), 10,
				       5000);
	if (ret)
		goto out;

	ret = regmap_write(
		esw->regmap, MT7628_ESW_REG_PCR0,
		FIELD_PREP(MT7628_ESW_PCR0_WT_NWAY_DATA, dat) |
			FIELD_PREP(MT7628_ESW_PCR0_CPU_PHY_REG, regnum) |
			FIELD_PREP(MT7628_ESW_PCR0_CPU_PHY_ADDR, port) |
			MT7628_ESW_PCR0_WT_PHY_CMD);
	if (ret)
		goto out;

	ret = regmap_read_poll_timeout(esw->regmap, MT7628_ESW_REG_PCR1, val,
				       (val & MT7628_ESW_PCR1_WT_DONE), 10,
				       5000);
out:
	if (ret) {
		dev_err(ds->dev, "write failed. MDIO timeout?\n");
		return -ETIMEDOUT;
	}
	return ret;
}

static void mt7628_vendor_phys_init(struct dsa_switch *ds)
{
	/* vendor specific init sequence from openwrt/uboot */
	mt7628_phy_write(ds, 0, 31, 0x2000); /* change G2 page */
	mt7628_phy_write(ds, 0, 26, 0x0020);

	for (int i = 0; i < 5; i++) {
		mt7628_phy_write(ds, i, 31, 0x8000); /* change L0 page */
		mt7628_phy_write(ds, i, 0, 0x3100);

		/* EEE disable */
		mt7628_phy_write(ds, i, 30, 0xa000);
		mt7628_phy_write(ds, i, 31, 0xa000); /* change L2 page */
		mt7628_phy_write(ds, i, 16, 0x0606);
		mt7628_phy_write(ds, i, 23, 0x0f0e);
		mt7628_phy_write(ds, i, 24, 0x1610);
		mt7628_phy_write(ds, i, 30, 0x1f15);
		mt7628_phy_write(ds, i, 28, 0x6111);
		mt7628_phy_write(ds, i, 31, 0x2000);
		mt7628_phy_write(ds, i, 26, 0x0000);
	}

	/* 100Base AOI setting */
	mt7628_phy_write(ds, 0, 31, 0x5000); /* change G5 page */
	mt7628_phy_write(ds, 0, 19, 0x004a);
	mt7628_phy_write(ds, 0, 20, 0x015a);
	mt7628_phy_write(ds, 0, 21, 0x00ee);
	mt7628_phy_write(ds, 0, 22, 0x0033);
	mt7628_phy_write(ds, 0, 23, 0x020a);
	mt7628_phy_write(ds, 0, 24, 0x0000);
	mt7628_phy_write(ds, 0, 25, 0x024a);
	mt7628_phy_write(ds, 0, 26, 0x035a);
	mt7628_phy_write(ds, 0, 27, 0x02ee);
	mt7628_phy_write(ds, 0, 28, 0x0233);
	mt7628_phy_write(ds, 0, 29, 0x000a);
	mt7628_phy_write(ds, 0, 30, 0x0000);
}

static void mt7628_switch_init(struct dsa_switch *ds)
{
	struct mt7628_esw *esw = ds->priv;
	/* undocumented init sequence from openwrt/uboot */
	regmap_write(esw->regmap, MT7628_ESW_REG_FCT0, 0xC8A07850);
	regmap_write(esw->regmap, MT7628_ESW_REG_SGC2, 0x00000000);

	regmap_write(
		esw->regmap, MT7628_ESW_REG_FCT2,
		FIELD_PREP(MT7628_ESW_FCT2_MC_PER_PORT_TH, 0xc) |
			FIELD_PREP(MT7628_ESW_FCT2_MUST_DROP_SET_TH, 0x10) |
			FIELD_PREP(MT7628_ESW_FCT2_MUST_DROP_RLS_TH, 0x12));

	/*
	 * general switch configuration:
	 * 300s aging interval
	 * broadcast storm prevention disabled
	 * max packet length 1536 bytes
	 * disable collision 16 packet abort and late collision abort
	 * use xor48 for address hashing
	 * disable tx backoff
	 * 10 packet back pressure jam
	 * disable was_transmit
	 * jam until BP condition released
	 * 30ms LED flash
	 * rmc tb fault to all ports
	 * unmatched IGMP as broadcast
	 */
	regmap_write(esw->regmap, MT7628_ESW_REG_SGC,
		     FIELD_PREP(MT7628_ESW_SGC_AGING_INTERVAL, 1) |
			     FIELD_PREP(MT7628_ESW_BC_STORM_PROT, 0) |
			     FIELD_PREP(MT7628_ESW_PKT_MAX_LEN, 0) |
			     MT7628_ESW_DIS_PKT_ABORT |
			     FIELD_PREP(MT7628_ESW_ADDRESS_HASH_ALG, 1) |
			     MT7628_ESW_DISABLE_TX_BACKOFF |
			     FIELD_PREP(MT7628_ESW_BP_JAM_CNT, 10) |
			     FIELD_PREP(MT7628_ESW_DISMIIPORT_WASTX, 0) |
			     FIELD_PREP(MT7628_ESW_BP_MODE, 0b10) |
			     FIELD_PREP(MT7628_ESW_LED_FLASH_TIME, 0) |
			     FIELD_PREP(MT7628_ESW_RMC_RULE, 0) |
			     FIELD_PREP(MT7628_ESW_IP_MULT_RULE, 0));

	regmap_write(esw->regmap, MT7628_ESW_REG_SOCPC,
		     MT7628_ESW_SOCPC_CRC_PADDING |
			     FIELD_PREP(MT7628_ESW_SOCPC_DISUN2CPU,
					MT7628_ESW_PORTS_CPU) |
			     FIELD_PREP(MT7628_ESW_SOCPC_DISMC2CPU,
					MT7628_ESW_PORTS_CPU) |
			     FIELD_PREP(MT7628_ESW_SOCPC_DISBC2CPU,
					MT7628_ESW_PORTS_CPU));

	regmap_set_bits(esw->regmap, MT7628_ESW_REG_FPA2,
			MT7628_ESW_FPA2_FORCE_RGMII_EN1 |
				MT7628_ESW_FPA2_FORCE_RGMII_LINK1 |
				MT7628_ESW_FPA2_AP_EN);

	regmap_update_bits(esw->regmap, MT7628_ESW_REG_FPA2,
			   MT7628_ESW_FPA2_EXT_PHY_ADDR_BASE,
			   FIELD_PREP(MT7628_ESW_FPA2_EXT_PHY_ADDR_BASE, 31));

	/* disable all interrupts */
	regmap_write(esw->regmap, MT7628_ESW_REG_IMR, 0);

	/* enable special tag on CPU port */
	regmap_write(esw->regmap, MT7628_ESW_REG_SGC2,
		     MT7628_ESW_SGC2_SPECIAL_TAG_EN |
			     FIELD_PREP(MT7628_ESW_SGC2_TX_CPU_TPID_BIT_MAP,
					MT7628_ESW_PORTS_CPU));

	/* set up switch for double tagging to simulate vlan unawareness */
	regmap_set_bits(esw->regmap, MT7628_ESW_REG_POC2,
			MT7628_ESW_POC2_PER_VLAN_UNTAG_EN);
	regmap_update_bits(
		esw->regmap, MT7628_ESW_REG_PFC1, MT7628_ESW_PFC1_EN_VLAN,
		FIELD_PREP(MT7628_ESW_PFC1_EN_VLAN, MT7628_ESW_PORTS_ALL));
}

static void esw_set_vlan_id(struct mt7628_esw *esw, unsigned vlan, unsigned vid)
{
	unsigned s = MT7628_ESW_VLANI_VID_S * (vlan % 2);
	regmap_update_bits(esw->regmap, MT7628_ESW_REG_VLANI(vlan / 2),
			   MT7628_ESW_VLANI_VID_M << s,
			   (vid & MT7628_ESW_VLANI_VID_M) << s);
}

static void esw_set_pvid(struct mt7628_esw *esw, unsigned port, unsigned pvid)
{
	unsigned s = MT7628_ESW_PVIDC_PVID_S * (port % 2);
	regmap_update_bits(esw->regmap, MT7628_ESW_REG_PVIDC(port / 2),
			   MT7628_ESW_PVIDC_PVID_M << s,
			   (pvid & MT7628_ESW_PVIDC_PVID_M) << s);
}

static void esw_set_vmsc(struct mt7628_esw *esw, unsigned vlan, unsigned msc)
{
	unsigned s = MT7628_ESW_VMSC_MSC_S * (vlan % 4);
	regmap_update_bits(esw->regmap, MT7628_ESW_REG_VMSC(vlan / 4),
			   MT7628_ESW_VMSC_MSC_M << s,
			   (msc & MT7628_ESW_VMSC_MSC_M) << s);
}

static void esw_set_vub(struct mt7628_esw *esw, unsigned vlan, unsigned msc)
{
	unsigned s = MT7628_ESW_VUB_S * (vlan % 4);
	regmap_update_bits(esw->regmap, MT7628_ESW_REG_VUB(vlan / 4),
			   MT7628_ESW_VUB_M << s,
			   (msc & MT7628_ESW_VUB_M) << s);
}

static void mt7628_vlan_sync(struct dsa_switch *ds)
{
	struct mt7628_esw *esw = ds->priv;
	int i;
	for (i = 0; i < MT7628_NUM_VLANS; i++) {
		struct mt7628_vlan *vlan = &esw->vlans[i];
		esw_set_vmsc(esw, i, vlan->members);
		esw_set_vlan_id(esw, i, vlan->vid);
		esw_set_vub(esw, i, vlan->untag);
	}

	for (i = 0; i < ds->num_ports; i++)
		esw_set_pvid(esw, i, esw->tag_8021q_pvid[i]);
}

static int mt7628_setup(struct dsa_switch *ds)
{
	struct mt7628_esw *esw = ds->priv;
	reset_control_reset(esw->rst_esw);
	usleep_range(1000, 2000);
	reset_control_reset(esw->rst_ephy);
	usleep_range(1000, 2000);
	/*
	 * all MMIO reads hang if esw is not out of reset
	 * ephy needs extra time to get out of reset or it ends up misconfigured
	 */
	mt7628_vendor_phys_init(ds);
	mt7628_switch_init(ds);
	rtnl_lock();
	dsa_tag_8021q_register(ds, htons(ETH_P_8021Q));
	rtnl_unlock();
	return 0;
}

static int mt7628_port_enable(struct dsa_switch *ds, int port,
			      struct phy_device *phy)
{
	struct mt7628_esw *esw = ds->priv;
	regmap_clear_bits(esw->regmap, MT7628_ESW_REG_POC0,
			  FIELD_PREP(MT7628_ESW_POC0_PORT_DISABLE, BIT(port)));
	return 0;
}

static void mt7628_port_disable(struct dsa_switch *ds, int port)
{
	struct mt7628_esw *esw = ds->priv;
	regmap_set_bits(esw->regmap, MT7628_ESW_REG_POC0,
			FIELD_PREP(MT7628_ESW_POC0_PORT_DISABLE, BIT(port)));
}

static enum dsa_tag_protocol
mt7628_get_tag_proto(struct dsa_switch *ds, int port, enum dsa_tag_protocol mp)
{
	return DSA_TAG_PROTO_MT7628;
}

static void mt7628_phylink_get_caps(struct dsa_switch *ds, int port,
				    struct phylink_config *config)
{
	config->mac_capabilities = MAC_100 | MAC_10;
	__set_bit(PHY_INTERFACE_MODE_MII, config->supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_GMII, config->supported_interfaces);
}

static int mt7628_dsa_8021q_vlan_add(struct dsa_switch *ds, int port, u16 vid,
				     u16 flags)
{
	struct mt7628_esw *esw = ds->priv;
	struct mt7628_vlan *vlan = NULL;

	for (int i = 0; i < MT7628_NUM_VLANS; i++) {
		struct mt7628_vlan *check_vlan = &esw->vlans[i];
		if (!check_vlan->active) {
			vlan = check_vlan;
		} else if (check_vlan->vid == vid) {
			vlan = check_vlan;
			break;
		}
	}

	if (!vlan)
		return -ENOSPC;

	vlan->vid = vid;
	vlan->active = true;
	vlan->members |= BIT(port);

	if (flags & BRIDGE_VLAN_INFO_PVID)
		esw->tag_8021q_pvid[port] = vid;

	if (flags & BRIDGE_VLAN_INFO_UNTAGGED)
		vlan->untag |= BIT(port);

	mt7628_vlan_sync(ds);
	return 0;
}

static int mt7628_dsa_8021q_vlan_del(struct dsa_switch *ds, int port, u16 vid)
{
	struct mt7628_esw *esw = ds->priv;
	struct mt7628_vlan *vlan = NULL;

	for (int i = 0; i < MT7628_NUM_VLANS; i++) {
		struct mt7628_vlan *check_vlan = &esw->vlans[i];
		if (check_vlan->active || check_vlan->vid != vid)
			continue;
		vlan = check_vlan;
		break;
	}
	if (!vlan)
		return -ENOENT;

	vlan->members &= ~BIT(port);
	vlan->untag &= ~BIT(port);

	if (!vlan->members)
		vlan->active = false;

	mt7628_vlan_sync(ds);
	return 0;
}

static struct dsa_switch_ops mt7628_switch_ops = {
	.get_tag_protocol = mt7628_get_tag_proto,
	.setup = mt7628_setup,
	.port_enable = mt7628_port_enable,
	.port_disable = mt7628_port_disable,
	.phy_read = mt7628_phy_read,
	.phy_write = mt7628_phy_write,
	.phylink_get_caps = mt7628_phylink_get_caps,
	.tag_8021q_vlan_add = mt7628_dsa_8021q_vlan_add,
	.tag_8021q_vlan_del = mt7628_dsa_8021q_vlan_del,
};

static int mt7628_probe(struct platform_device *pdev)
{
	struct dsa_switch *ds = NULL;
	struct mt7628_esw *esw = NULL;
	struct device *dev = &pdev->dev;

	ds = devm_kzalloc(&pdev->dev, sizeof(*ds), GFP_KERNEL);
	if (!ds)
		return -ENOMEM;

	esw = devm_kzalloc(&pdev->dev, sizeof(*esw), GFP_KERNEL);
	if (!esw)
		return -ENOMEM;

	esw->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(esw->base))
		return PTR_ERR(esw->base);

	esw->regmap = devm_regmap_init_mmio(&pdev->dev, esw->base,
					    &mt7628_esw_regmap_cfg);
	if (IS_ERR(esw->regmap))
		return PTR_ERR(esw->regmap);

	esw->rst_ephy = devm_reset_control_get_exclusive(&pdev->dev, "ephy");
	if (IS_ERR(esw->rst_ephy)) {
		dev_err(dev, "failed to get EPHY reset: %pe\n", esw->rst_ephy);
		esw->rst_ephy = NULL;
	}

	esw->rst_esw = devm_reset_control_get_exclusive(&pdev->dev, "esw");
	if (IS_ERR(esw->rst_esw)) {
		dev_err(dev, "failed to get ESW reset: %pe\n", esw->rst_esw);
		esw->rst_esw = NULL;
	}

	ds->dev = dev;
	ds->num_ports = MT7628_ESW_NUM_PORTS;
	ds->ops = &mt7628_switch_ops;
	ds->priv = esw;
	esw->ds = ds;
	dev_set_drvdata(dev, esw);

	return dsa_register_switch(ds);
}

static void mt7628_remove(struct platform_device *pdev)
{
	struct mt7628_esw *esw = platform_get_drvdata(pdev);
	if (!esw)
		return;

	dsa_unregister_switch(esw->ds);
}

static void mt7628_shutdown(struct platform_device *pdev)
{
	struct mt7628_esw *esw = platform_get_drvdata(pdev);
	if (!esw)
		return;

	dsa_switch_shutdown(esw->ds);
	dev_set_drvdata(&pdev->dev, NULL);
}

static const struct of_device_id mt7628_of_match[] = {
	{
		.compatible = "mediatek,mt7628-esw",
	},
	{},
};

MODULE_DEVICE_TABLE(of, mt7628_of_match);

static struct platform_driver mt7628_driver = {
    .driver = {
        .name = "mt7628-esw",
        .of_match_table = mt7628_of_match,
    },
    .probe = mt7628_probe,
    .remove = mt7628_remove,
    .shutdown = mt7628_shutdown,
};

module_platform_driver(mt7628_driver);

MODULE_AUTHOR("Joris Vaisvila <joey@tinyisr.com>");
MODULE_DESCRIPTION("Driver for Mediatek MT7628 embedded switch");
MODULE_LICENSE("GPL v2");
