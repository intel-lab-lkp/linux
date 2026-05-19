// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Phylink and PCS support for MaxLinear MxL862xx switch family
 *
 * Copyright (C) 2024 MaxLinear Inc.
 * Copyright (C) 2025 John Crispin <john@phrozen.org>
 * Copyright (C) 2025 Daniel Golle <daniel@makrotopia.org>
 */

#include <linux/phylink.h>
#include <net/dsa.h>

#include "mxl862xx.h"
#include "mxl862xx-api.h"
#include "mxl862xx-cmd.h"
#include "mxl862xx-host.h"
#include "mxl862xx-phylink.h"

void mxl862xx_phylink_get_caps(struct dsa_switch *ds, int port,
			       struct phylink_config *config)
{
	config->mac_capabilities = MAC_ASYM_PAUSE | MAC_SYM_PAUSE | MAC_10 |
				   MAC_100 | MAC_1000 | MAC_2500FD;

	switch (port) {
	case 1 ... 8:
		__set_bit(PHY_INTERFACE_MODE_INTERNAL,
			  config->supported_interfaces);
		break;
	case 9:
	case 13:
		__set_bit(PHY_INTERFACE_MODE_SGMII, config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_1000BASEX, config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_2500BASEX, config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_10GBASER, config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_10GKR, config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_USXGMII, config->supported_interfaces);
		fallthrough;
	case 10 ... 12:
	case 14 ... 16:
		__set_bit(PHY_INTERFACE_MODE_QSGMII, config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_10G_QXGMII, config->supported_interfaces);

		break;
	default:
		break;
	}

	if (port == 9 || port == 13)
		config->mac_capabilities |= MAC_10000FD | MAC_5000FD;
}

static struct mxl862xx_pcs *pcs_to_mxl862xx_pcs(struct phylink_pcs *pcs)
{
	return container_of(pcs, struct mxl862xx_pcs, pcs);
}

static int mxl862xx_xpcs_if_mode(phy_interface_t interface)
{
	switch (interface) {
	case PHY_INTERFACE_MODE_SGMII:
		return MXL862XX_XPCS_IF_SGMII;
	case PHY_INTERFACE_MODE_QSGMII:
		return MXL862XX_XPCS_IF_QSGMII;
	case PHY_INTERFACE_MODE_1000BASEX:
		return MXL862XX_XPCS_IF_1000BASEX;
	case PHY_INTERFACE_MODE_2500BASEX:
		return MXL862XX_XPCS_IF_2500BASEX;
	case PHY_INTERFACE_MODE_USXGMII:
	case PHY_INTERFACE_MODE_10G_QXGMII:
		return MXL862XX_XPCS_IF_USXGMII;
	case PHY_INTERFACE_MODE_10GBASER:
		return MXL862XX_XPCS_IF_10GBASER;
	case PHY_INTERFACE_MODE_10GKR:
		return MXL862XX_XPCS_IF_10GKR;
	default:
		return -EINVAL;
	}
}

static int mxl862xx_xpcs_neg_mode(unsigned int neg_mode)
{
	if (!(neg_mode & PHYLINK_PCS_NEG_INBAND))
		return MXL862XX_XPCS_NEG_NONE;
	if (neg_mode & PHYLINK_PCS_NEG_ENABLED)
		return MXL862XX_XPCS_NEG_INBAND_AN_ON;
	return MXL862XX_XPCS_NEG_INBAND_AN_OFF;
}

static void mxl862xx_pcs_disable(struct phylink_pcs *pcs)
{
	struct mxl862xx_pcs *mpcs = pcs_to_mxl862xx_pcs(pcs);
	struct mxl862xx_priv *priv = mpcs->priv;
	struct mxl862xx_xpcs_pcs_disable dis = {};

	if (mpcs->slot != 0)
		return;

	dis.port_id = mpcs->serdes_id;

	MXL862XX_API_WRITE(priv, MXL862XX_XPCS_PCS_DISABLE, dis);
}

static int mxl862xx_pcs_config(struct phylink_pcs *pcs, unsigned int neg_mode,
			       phy_interface_t interface,
			       const unsigned long *advertising,
			       bool permit_pause_to_mac)
{
	struct mxl862xx_pcs *mpcs = pcs_to_mxl862xx_pcs(pcs);
	struct mxl862xx_priv *priv = mpcs->priv;
	struct mxl862xx_xpcs_pcs_cfg cfg = {};
	int if_mode, ret;
	u16 adv;

	if (mpcs->slot != 0)
		return 0;

	if_mode = mxl862xx_xpcs_if_mode(interface);
	if (if_mode < 0) {
		dev_err(priv->ds->dev, "unsupported interface: %s\n",
			phy_modes(interface));
		return if_mode;
	}

	mpcs->if_mode = if_mode;

	cfg.port_id = mpcs->serdes_id;
	cfg.usx_lane_mode = (interface == PHY_INTERFACE_MODE_10G_QXGMII) ?
			    MXL862XX_XPCS_USX_QUAD : MXL862XX_XPCS_USX_SINGLE;
	cfg.interface = if_mode;
	cfg.neg_mode = mxl862xx_xpcs_neg_mode(neg_mode);
	cfg.role = MXL862XX_XPCS_ROLE_MAC;
	cfg.permit_pause = permit_pause_to_mac ? 1 : 0;

	if (neg_mode & PHYLINK_PCS_NEG_INBAND) {
		switch (interface) {
		case PHY_INTERFACE_MODE_1000BASEX:
		case PHY_INTERFACE_MODE_2500BASEX:
			adv = linkmode_adv_to_mii_adv_x(advertising,
				ETHTOOL_LINK_MODE_1000baseX_Full_BIT);
			cfg.advertising.cl37 = cpu_to_le16(adv);
			break;
		case PHY_INTERFACE_MODE_SGMII:
		case PHY_INTERFACE_MODE_QSGMII:
			cfg.advertising.cl37 = cpu_to_le16(ADVERTISE_SGMII);
			break;
		default:
			break;
		}
	}

	ret = MXL862XX_API_READ(priv, MXL862XX_XPCS_PCS_CONFIG, cfg);
	if (ret)
		return ret;

	return le16_to_cpu(cfg.result) > 0 ? 1 : 0;
}

static void mxl862xx_pcs_get_state(struct phylink_pcs *pcs,
				   unsigned int neg_mode,
				   struct phylink_link_state *state)
{
	struct mxl862xx_pcs *mpcs = pcs_to_mxl862xx_pcs(pcs);
	struct mxl862xx_priv *priv = mpcs->priv;
	struct mxl862xx_xpcs_pcs_state st = {};
	int if_mode, ret;
	u16 bmsr;

	if_mode = mxl862xx_xpcs_if_mode(state->interface);
	if (if_mode < 0)
		return;

	st.port_id = mpcs->serdes_id;
	st.interface = if_mode;
	st.usx_subport = mpcs->slot;

	ret = MXL862XX_API_READ(priv, MXL862XX_XPCS_PCS_GET_STATE, st);
	if (ret)
		return;

	state->link = st.link && !st.pcs_fault;
	state->an_complete = st.an_complete;

	switch (state->interface) {
	case PHY_INTERFACE_MODE_1000BASEX:
	case PHY_INTERFACE_MODE_2500BASEX:
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_QSGMII:
		bmsr = (state->link ? BMSR_LSTATUS : 0) |
		       (state->an_complete ? BMSR_ANEGCOMPLETE : 0);
		phylink_mii_c22_pcs_decode_state(state, neg_mode, bmsr,
						 le16_to_cpu(st.lpa.cl37));
		break;

	case PHY_INTERFACE_MODE_USXGMII:
	case PHY_INTERFACE_MODE_10G_QXGMII:
		if (state->link)
			phylink_decode_usxgmii_word(state,
						    le16_to_cpu(st.lpa.usx));
		break;

	case PHY_INTERFACE_MODE_10GBASER:
	case PHY_INTERFACE_MODE_10GKR:
		if (state->link) {
			state->speed = SPEED_10000;
			state->duplex = DUPLEX_FULL;
		}
		break;

	default:
		state->link = false;
		break;
	}
}

static void mxl862xx_pcs_an_restart(struct phylink_pcs *pcs)
{
	struct mxl862xx_pcs *mpcs = pcs_to_mxl862xx_pcs(pcs);
	struct mxl862xx_priv *priv = mpcs->priv;
	struct mxl862xx_xpcs_an_restart an = {};

	if (mpcs->slot != 0)
		return;

	an.port_id = mpcs->serdes_id;
	an.interface = mpcs->if_mode;

	MXL862XX_API_WRITE(priv, MXL862XX_XPCS_AN_RESTART, an);
}

static void mxl862xx_pcs_link_up(struct phylink_pcs *pcs, unsigned int neg_mode,
				 phy_interface_t interface, int speed,
				 int duplex)
{
	struct mxl862xx_pcs *mpcs = pcs_to_mxl862xx_pcs(pcs);
	struct mxl862xx_priv *priv = mpcs->priv;
	struct mxl862xx_xpcs_pcs_link_up lu = {};
	int if_mode;

	if (mpcs->slot != 0)
		return;

	if_mode = mxl862xx_xpcs_if_mode(interface);
	if (if_mode < 0)
		return;

	lu.port_id = mpcs->serdes_id;
	lu.interface = if_mode;
	lu.duplex = (duplex == DUPLEX_FULL) ? MXL862XX_XPCS_DUPLEX_FULL :
					      MXL862XX_XPCS_DUPLEX_HALF;
	lu.speed = cpu_to_le16(speed);

	MXL862XX_API_WRITE(priv, MXL862XX_XPCS_PCS_LINK_UP, lu);
}

static unsigned int mxl862xx_pcs_inband_caps(struct phylink_pcs *pcs,
					     phy_interface_t interface)
{
	switch (interface) {
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_QSGMII:
	case PHY_INTERFACE_MODE_1000BASEX:
	case PHY_INTERFACE_MODE_2500BASEX:
		return LINK_INBAND_DISABLE | LINK_INBAND_ENABLE;
	case PHY_INTERFACE_MODE_USXGMII:
	case PHY_INTERFACE_MODE_10G_QXGMII:
	case PHY_INTERFACE_MODE_10GKR:
		return LINK_INBAND_ENABLE;
	case PHY_INTERFACE_MODE_10GBASER:
		return LINK_INBAND_DISABLE;
	default:
		return 0;
	}
}

static const struct phylink_pcs_ops mxl862xx_pcs_ops = {
	.pcs_disable = mxl862xx_pcs_disable,
	.pcs_config = mxl862xx_pcs_config,
	.pcs_get_state = mxl862xx_pcs_get_state,
	.pcs_an_restart = mxl862xx_pcs_an_restart,
	.pcs_link_up = mxl862xx_pcs_link_up,
	.pcs_inband_caps = mxl862xx_pcs_inband_caps,
};

void mxl862xx_setup_pcs(struct mxl862xx_priv *priv, struct mxl862xx_pcs *pcs,
			int port)
{
	pcs->priv = priv;
	pcs->serdes_id = MXL862XX_SERDES_PORT_ID(port);
	pcs->slot = MXL862XX_SERDES_SLOT(port);

	pcs->pcs.ops = &mxl862xx_pcs_ops;
	pcs->pcs.poll = true;

	__set_bit(PHY_INTERFACE_MODE_QSGMII, pcs->pcs.supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_10G_QXGMII, pcs->pcs.supported_interfaces);
	if (pcs->slot != 0)
		return;

	__set_bit(PHY_INTERFACE_MODE_SGMII, pcs->pcs.supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_1000BASEX, pcs->pcs.supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_2500BASEX, pcs->pcs.supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_10GBASER, pcs->pcs.supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_10GKR, pcs->pcs.supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_USXGMII, pcs->pcs.supported_interfaces);
}

static struct phylink_pcs *
mxl862xx_phylink_mac_select_pcs(struct phylink_config *config,
				phy_interface_t interface)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	struct mxl862xx_priv *priv = dp->ds->priv;
	int port = dp->index;

	if (!MXL862XX_FW_VER_MIN(priv, 1, 0, 84))
		return NULL;

	switch (port) {
	case 9 ... 16:
		return &priv->serdes_ports[port - 9].pcs;
	default:
		return NULL;
	}
}

static void mxl862xx_phylink_mac_config(struct phylink_config *config,
					unsigned int mode,
					const struct phylink_link_state *state)
{
}

static void mxl862xx_phylink_mac_link_down(struct phylink_config *config,
					   unsigned int mode,
					   phy_interface_t interface)
{
}

static void mxl862xx_phylink_mac_link_up(struct phylink_config *config,
					 struct phy_device *phydev,
					 unsigned int mode,
					 phy_interface_t interface,
					 int speed, int duplex,
					 bool tx_pause, bool rx_pause)
{
}

const struct phylink_mac_ops mxl862xx_phylink_mac_ops = {
	.mac_config = mxl862xx_phylink_mac_config,
	.mac_link_down = mxl862xx_phylink_mac_link_down,
	.mac_link_up = mxl862xx_phylink_mac_link_up,
	.mac_select_pcs = mxl862xx_phylink_mac_select_pcs,
};

/* --- SerDes ethtool statistics --- */

static const char mxl862xx_serdes_stats[][ETH_GSTRING_LEN] = {
	"serdes_tx_main",
	"serdes_tx_pre",
	"serdes_tx_post",
	"serdes_tx_iboost",
	"serdes_tx_vboost",
	"serdes_tx_vboost_en",
	"serdes_rx_att",
	"serdes_rx_vga1",
	"serdes_rx_vga2",
	"serdes_rx_ctle_boost",
	"serdes_rx_ctle_pole",
	"serdes_rx_dfe_tap1",
	"serdes_rx_dfe_bypass",
	"serdes_rx_adapt_mode",
	"serdes_rx_adapt_sel",
	"serdes_rx_signal",
	"serdes_pma_link",
	"serdes_link_fault",
	"serdes_in_reset",
};

static bool mxl862xx_port_has_serdes_stats(struct dsa_switch *ds, int port)
{
	struct mxl862xx_priv *priv = ds->priv;

	return port >= 9 && port <= 16 &&
	       MXL862XX_FW_VER_MIN(priv, 1, 0, 84);
}

int mxl862xx_serdes_stats_count(struct dsa_switch *ds, int port)
{
	if (mxl862xx_port_has_serdes_stats(ds, port))
		return ARRAY_SIZE(mxl862xx_serdes_stats);

	return 0;
}

void mxl862xx_serdes_get_strings(struct dsa_switch *ds, int port, u8 *data)
{
	int i;

	if (!mxl862xx_port_has_serdes_stats(ds, port))
		return;

	for (i = 0; i < ARRAY_SIZE(mxl862xx_serdes_stats); i++)
		ethtool_puts(&data, mxl862xx_serdes_stats[i]);
}

void mxl862xx_serdes_get_stats(struct dsa_switch *ds, int port, u64 *data)
{
	struct mxl862xx_xpcs_eq_get eq = {
		.port_id = MXL862XX_SERDES_PORT_ID(port),
	};
	struct mxl862xx_xpcs_signal_detect sig = {};

	if (!mxl862xx_port_has_serdes_stats(ds, port))
		return;

	sig.port_id = MXL862XX_SERDES_PORT_ID(port);

	if (!MXL862XX_API_READ(ds->priv, MXL862XX_XPCS_EQ_GET, eq)) {
		*data++ = eq.tx.main.value;
		*data++ = eq.tx.pre.value;
		*data++ = eq.tx.post.value;
		*data++ = eq.tx.iboost_lvl.value;
		*data++ = eq.tx.vboost_lvl.value;
		*data++ = eq.tx.vboost_en.value;
		*data++ = eq.rx.att_lvl.value;
		*data++ = eq.rx.vga1_gain.value;
		*data++ = eq.rx.vga2_gain.value;
		*data++ = eq.rx.ctle_boost.value;
		*data++ = eq.rx.ctle_pole.value;
		*data++ = eq.rx.dfe_tap1.value;
		*data++ = eq.rx.dfe_bypass.value;
		*data++ = eq.rx.adapt_mode.value;
		*data++ = eq.rx.adapt_sel.value;
	} else {
		data += 15;
	}

	if (!MXL862XX_API_READ(ds->priv, MXL862XX_XPCS_SIGNAL_DETECT, sig)) {
		*data++ = sig.rx_signal;
		*data++ = sig.pma_link;
		*data++ = sig.link_fault;
		*data++ = sig.in_reset;
	} else {
		data += 4;
	}
}
