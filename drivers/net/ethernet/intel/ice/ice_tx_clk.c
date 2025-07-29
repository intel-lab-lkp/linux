// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (C) 2025, Intel Corporation. */

#include <linux/netdev_tx_clk.h>
#include "ice_tx_clk.h"

enum ice_clk_type {
	ICE_TX_CLK_OCXO = 0,
	ICE_TX_CLK_SYNCE_REF,
	ICE_TX_CLK_EXT_REF,

	ICE_TX_CLK_COUNT /* always last */
};

static const char *ice_clk_names[ICE_TX_CLK_COUNT] = {
	"ocxo",
	"SyncE_ref",
	"ext_ref"
};

struct ice_tx_clk_data {
	struct ice_pf *pf;
	u8 clk_id;
};

static const struct netdev_tx_clk_ops ice_tx_clk_ops;

static int ice_tx_clk_enable(void *priv_data)
{
	struct ice_tx_clk_data *clk_data = priv_data;
	struct ice_pf *pf = clk_data->pf;
	u8 clk_id = clk_data->clk_id;

	if (clk_id >= ICE_TX_CLK_COUNT) {
		dev_err(ice_pf_to_dev(pf), "Invalid clock ID: %d\n", clk_id);
		return -EINVAL;
	}

	if (pf->tx_clk_active != clk_id) {
		dev_dbg(ice_pf_to_dev(pf), "PF%d switching from %s to %s clock\n",
			pf->hw.pf_id, ice_clk_names[pf->tx_clk_active],
			ice_clk_names[clk_id]);

		pf->tx_clk_active = clk_id;
		/* TODO: add TX clock switching logic */
	}

	return 0;
}

static int ice_tx_clk_is_enabled(void *priv_data)
{
	struct ice_tx_clk_data *clk_data = priv_data;
	struct ice_pf *pf = clk_data->pf;
	u8 clk_id = clk_data->clk_id;

	return (pf->tx_clk_active == clk_id) ? 1 : 0;
}

static const struct netdev_tx_clk_ops ice_tx_clk_ops = {
	.enable = ice_tx_clk_enable,
	.is_enabled = ice_tx_clk_is_enabled,
};

void ice_tx_clk_init(struct ice_pf *pf)
{
	struct ice_vsi *vsi = ice_get_main_vsi(pf);
	struct ice_tx_clk_data *clk_data[ICE_TX_CLK_COUNT];
	int i, ret;

	if (!vsi || !vsi->netdev)
		return;

	for (i = 0; i < ICE_TX_CLK_COUNT; i++) {
		clk_data[i] = kzalloc(sizeof(*clk_data[i]), GFP_KERNEL);
		if (!clk_data[i]) {
			while (--i >= 0)
				kfree(clk_data[i]);
			return;
		}

		clk_data[i]->pf = pf;
		clk_data[i]->clk_id = i;
	}

	pf->tx_clk_active = ICE_TX_CLK_OCXO;

	for (i = 0; i < ICE_TX_CLK_COUNT; i++) {
		ret = netdev_tx_clk_register(vsi->netdev, ice_clk_names[i],
					     &ice_tx_clk_ops, clk_data[i]);
		if (ret) {
			dev_err(ice_pf_to_dev(pf),
				"Failed to register %s clock: %d\n",
				ice_clk_names[i], ret);
		}
	}

	dev_dbg(ice_pf_to_dev(pf), "ICE TX clocks initialized for PF%d (default: %s)\n",
		pf->hw.pf_id, ice_clk_names[ICE_TX_CLK_OCXO]);
}

void ice_tx_clk_deinit(struct ice_pf *pf)
{
	struct ice_vsi *vsi = ice_get_main_vsi(pf);

	if (!vsi || !vsi->netdev)
		return;

	netdev_tx_clk_cleanup(vsi->netdev);

	dev_dbg(ice_pf_to_dev(pf), "ICE TX clocks deinitialized for PF%d\n",
		pf->hw.pf_id);
}
