/* SPDX-License-Identifier: GPL-2.0 */
/*
 * NXP SerDes XPCS shared core
 * Copyright 2026 NXP
 */

#ifndef __LINUX_PCS_NXP_SERDES_XPCS_H
#define __LINUX_PCS_NXP_SERDES_XPCS_H

#include <linux/phy.h>
#include <linux/phylink.h>
#include <linux/regmap.h>

/* AN mode values as defined by the DesignWare XPCS */
enum nxp_serdes_an_mode {
	NXP_SERDES_AN_C37_SGMII = 2,
	NXP_SERDES_2500BASEX = 3,
	NXP_SERDES_10GBASER = 5,
	NXP_SERDES_AN_C37_USXGMII = 6,
};

/* The revision runs its own reset sequence, skip the soft reset */
#define NXP_SERDES_QUIRK_NO_SOFT_RESET	BIT(0)
/* The revision reports the link state to the MAC, do not poll the PCS */
#define NXP_SERDES_QUIRK_NO_POLL	BIT(1)
/* The revision does not implement the VR_MII_EEE_MCTRL0/1 registers */
#define NXP_SERDES_QUIRK_NO_EEE		BIT(2)

struct nxp_serdes_xpcs;

struct nxp_serdes_xpcs_compat {
	phy_interface_t interface;
	const int *supported;
	enum nxp_serdes_an_mode an_mode;
	int (*pma_config)(struct nxp_serdes_xpcs *xpcs);
	int (*pma_link_up)(struct nxp_serdes_xpcs *xpcs, int speed,
			   int duplex);
};

/* Description of one SerDes revision, provided by the platform glue */
struct nxp_serdes_xpcs_desc {
	const char *name;
	u32 pma_id;
	const struct nxp_serdes_xpcs_compat *compat;
	unsigned int quirks;
	void (*reset)(struct nxp_serdes_xpcs *xpcs);
};

struct nxp_serdes_xpcs {
	struct device *dev;
	struct regmap *xpcs_regmap;
	struct regmap *phy_regmap;
	const struct nxp_serdes_xpcs_desc *desc;
	struct phylink_pcs pcs;
	phy_interface_t interface;
	u32 pma_id;
	u8 portid;
	bool need_reset;
	bool sgmii_internal_ref_clk;
	void *priv;
};

#define phylink_pcs_to_nxp_serdes_xpcs(pl_pcs) \
	container_of((pl_pcs), struct nxp_serdes_xpcs, pcs)

static inline void nxp_serdes_xpcs_set_drvdata(struct nxp_serdes_xpcs *xpcs,
					       void *priv)
{
	xpcs->priv = priv;
}

static inline void *nxp_serdes_xpcs_get_drvdata(struct nxp_serdes_xpcs *xpcs)
{
	return xpcs->priv;
}

/* Feature sets for struct nxp_serdes_xpcs_compat::supported */
extern const int nxp_serdes_xpcs_usxgmii_features[];
extern const int nxp_serdes_xpcs_2500basex_features[];
extern const int nxp_serdes_xpcs_sgmii_features[];

struct phylink_pcs *nxp_serdes_xpcs_create(struct device *dev,
					   struct regmap *xpcs_regmap,
					   struct regmap *phy_regmap,
					   const struct nxp_serdes_xpcs_desc *desc,
					   u8 portid,
					   phy_interface_t interface);
void nxp_serdes_xpcs_destroy(struct phylink_pcs *pcs);

/* Register accesses are encoded in the regmap as "(devad << 16) | reg", with
 * "devad" the Clause 45 device address and "reg" the 16-bit register address.
 * Decoding both fields is up to the regmap of the platform glue.
 */
int nxp_serdes_xpcs_read(struct nxp_serdes_xpcs *xpcs, u8 devad, u32 reg);
int nxp_serdes_xpcs_write(struct nxp_serdes_xpcs *xpcs, u8 devad, u32 reg,
			  u16 val);
int nxp_serdes_xpcs_modify(struct nxp_serdes_xpcs *xpcs, u8 devad, u32 reg,
			   u16 mask, u16 set);

#endif /* __LINUX_PCS_NXP_SERDES_XPCS_H */
