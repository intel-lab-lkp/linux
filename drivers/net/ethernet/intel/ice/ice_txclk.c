// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2026 Intel Corporation */

#include "ice.h"
#include "ice_cpi.h"
#include "ice_txclk.h"

#define ICE_PHY0	0
#define ICE_PHY1	1

/**
 * ice_txclk_enable_peer - Enable required TX reference clock on peer PHY
 * @pf: pointer to the PF structure
 * @clk: TX reference clock that must be enabled
 *
 * Some TX reference clocks on E825-class devices (SyncE and EREF0) must
 * be enabled on both PHY complexes to allow proper routing:
 *
 *   - SyncE must be enabled on both PHYs when used by PHY0
 *   - EREF0 must be enabled on both PHYs when used by PHY1
 *
 * If the requested clock is not yet enabled on the peer PHY, enable it.
 * ENET does not require duplication and is ignored.
 *
 * Return: 0 on success or negative error code on failure.
 */
static int ice_txclk_enable_peer(struct ice_pf *pf, enum ice_e825c_ref_clk clk)
{
	struct ice_pf *ctrl_pf = ice_get_ctrl_pf(pf);
	u8 port_num, phy;
	int err;

	if (clk == ICE_REF_CLK_ENET)
		return 0;

	if (IS_ERR_OR_NULL(ctrl_pf)) {
		dev_err(ice_pf_to_dev(pf),
			"Can't enable tx-clk on peer: no controlling PF\n");
		return -EINVAL;
	}

	port_num = pf->ptp.port.port_num;
	phy = port_num / pf->hw.ptp.ports_per_phy;

	if ((clk == ICE_REF_CLK_SYNCE && phy == ICE_PHY0 &&
	     !ice_txclk_any_port_uses(ctrl_pf, ICE_PHY1, clk)) ||
	    (clk == ICE_REF_CLK_EREF0 && phy == ICE_PHY1 &&
	     !ice_txclk_any_port_uses(ctrl_pf, ICE_PHY0, clk))) {
		u8 peer_phy = phy ? ICE_PHY0 : ICE_PHY1;

		err = ice_cpi_ena_dis_clk_ref(&pf->hw, peer_phy, clk, true);
		if (err) {
			dev_err(ice_hw_to_dev(&pf->hw),
				"Failed to enable the %u TX clock for the %u PHY\n",
				clk, peer_phy);
			return err;
		}
	}

	return 0;
}

/**
 * ice_txclk_disable_unused - Disable TX reference clock no longer in use
 * @pf: pointer to the PF structure
 * @ref_clk: TX reference clock source to evaluate for disabling
 *
 * Disable (from power-saving reasons) a TX reference clock after a clock
 * switch, provided that:
 *   - no port on the local PHY uses this clock, and
 *   - for SyncE: no port on PHY0 or PHY1 requires the clock, depending on
 *     where it must remain enabled for routing.
 *   - for EREF0: same logic as above but inverted PHY roles.
 *
 * Some reference clocks must be enabled on both PHY complexes when used
 * (SyncE for PHY0, EREF0 for PHY1). The function therefore also attempts
 * to implicitly disable the peer PHY copy when no port requires it.
 *
 * Return: 0 on success or negative error code if disabling fails.
 */
static int
ice_txclk_disable_unused(struct ice_pf *pf, enum ice_e825c_ref_clk ref_clk)
{
	struct ice_pf *ctrl_pf = ice_get_ctrl_pf(pf);
	struct ice_hw *hw = &pf->hw;
	int err = 0;
	u8 cur_phy;

	if (IS_ERR_OR_NULL(ctrl_pf)) {
		dev_err(ice_pf_to_dev(pf),
			"Can't disable unused tx-clk: no controlling PF\n");
		return -EINVAL;
	}

	cur_phy = pf->ptp.port.port_num / hw->ptp.ports_per_phy;

	if (ref_clk == ICE_REF_CLK_SYNCE) {
		/* Don't disable SyncE clock if it's still in use on PHY 0 */
		if (ice_txclk_any_port_uses(ctrl_pf, ICE_PHY0, ref_clk))
			return 0;
		if (cur_phy == ICE_PHY0 &&
		    !ice_txclk_any_port_uses(ctrl_pf, ICE_PHY1, ref_clk)) {
			err = ice_cpi_ena_dis_clk_ref(hw, ICE_PHY1, ref_clk,
						      false);
			if (err) {
				cur_phy = ICE_PHY1;
				goto err;
			}
		}
	} else if (ref_clk == ICE_REF_CLK_EREF0) {
		/* Don't disable EREF0 clock if it's still in use on PHY 1 */
		if (ice_txclk_any_port_uses(ctrl_pf, ICE_PHY1, ref_clk))
			return 0;
		if (cur_phy == ICE_PHY1 &&
		    !ice_txclk_any_port_uses(ctrl_pf, ICE_PHY0, ref_clk)) {
			err = ice_cpi_ena_dis_clk_ref(hw, ICE_PHY0, ref_clk,
						      false);
			if (err) {
				cur_phy = ICE_PHY0;
				goto err;
			}
		}
	}

	if (!ice_txclk_any_port_uses(ctrl_pf, cur_phy, ref_clk))
		err = ice_cpi_ena_dis_clk_ref(hw, cur_phy, ref_clk, false);
err:
	if (err)
		dev_warn(ice_pf_to_dev(pf), "Failed to disable the %u TX clock for the %u PHY\n",
			 ref_clk, cur_phy);

	return err;
}

#define ICE_REFCLK_USER_TO_AQ_IDX(x) ((x) + 1)

/**
 * ice_txclk_set_clk - Set Tx reference clock
 * @pf: pointer to pf structure
 * @clk: new Tx clock
 *
 * Return: 0 on success, negative value otherwise.
 */
int ice_txclk_set_clk(struct ice_pf *pf, enum ice_e825c_ref_clk clk)
{
	struct ice_pf *ctrl_pf = ice_get_ctrl_pf(pf);
	struct ice_port_info *port_info;
	u8 port_num, phy;
	int err;

	if (pf->ptp.port.tx_clk == clk)
		return 0;

	if (IS_ERR_OR_NULL(ctrl_pf)) {
		dev_err(ice_pf_to_dev(pf),
			"Can't set tx-clk: no controlling PF\n");
		return -EINVAL;
	}

	port_num = pf->ptp.port.port_num;
	phy = port_num / pf->hw.ptp.ports_per_phy;
	port_info = pf->hw.port_info;

	/* Check if the TX clk is enabled for this PHY, if not - enable it */
	if (!ice_txclk_any_port_uses(ctrl_pf, phy, clk)) {
		err = ice_cpi_ena_dis_clk_ref(&pf->hw, phy, clk, true);
		if (err) {
			dev_err(ice_hw_to_dev(&pf->hw), "Failed to enable the %u TX clock for the %u PHY\n",
				clk, phy);
			return err;
		}
		err = ice_txclk_enable_peer(pf, clk);
		if (err)
			return err;
	}

	pf->ptp.port.tx_clk_req = clk;

	/* We are ready to switch to the new TX clk. */
	err = ice_aq_set_link_restart_an(port_info, true, NULL,
					 ICE_REFCLK_USER_TO_AQ_IDX(clk));
	if (err)
		dev_err(ice_hw_to_dev(&pf->hw), "Failed to switch to %u TX clock for the %u PHY\n",
			clk, phy);

	return err;
}

/**
 * ice_txclk_verify - Validate TX reference clock switch and update usage state
 * @pf: pointer to PF structure
 *
 * After a link-up event, verify whether the previously requested TX reference
 * clock transition actually succeeded. The SERDES reference selector reflects
 * the effective hardware choice, which may differ from the requested clock
 * when Auto-Negotiation or firmware applies additional policy.
 *
 * If the hardware-selected clock differs from the requested one, update the
 * software state accordingly and stop further processing.
 *
 * When the switch is successful, update the per‑PHY usage bitmaps so that the
 * driver knows which reference clock is currently in use by this port. Using
 * these bitmaps, disable any reference clocks that are no longer required by
 * any port on the local PHY or, when applicable, on the peer PHY (according
 * to E825 clock‑routing rules).
 *
 * This function does not initiate a clock switch; it only validates the result
 * of a previously triggered transition and performs cleanup of unused clocks.
 */
void ice_txclk_verify(struct ice_pf *pf)
{
	struct ice_ptp_port *ptp_port = &pf->ptp.port;
	struct ice_pf *ctrl_pf = ice_get_ctrl_pf(pf);
	struct ice_hw *hw = &pf->hw;
	enum ice_e825c_ref_clk clk;
	int err;
	u8 phy;

	phy = ptp_port->port_num / hw->ptp.ports_per_phy;

	/* verify current Tx reference settings */
	err = ice_get_serdes_ref_sel_e825c(hw,
					   ptp_port->port_num,
					   &clk);
	if (err)
		return;

	if (clk != pf->ptp.port.tx_clk_req) {
		dev_warn(ice_pf_to_dev(pf),
			 "Failed to switch tx-clk for phy %d and clk %u (current: %u)\n",
			 phy, pf->ptp.port.tx_clk_req, clk);
		pf->ptp.port.tx_clk = clk;
		pf->ptp.port.tx_clk_req = clk;
		return;
	}

	if (IS_ERR_OR_NULL(ctrl_pf)) {
		dev_err(ice_pf_to_dev(pf),
			"Can't set tx-clk: no controlling PF\n");
		return;
	}

	/* update Tx reference clock usage map */
	for (int i = 0; i < ICE_REF_CLK_MAX; i++)
		(clk == i) ?
		 set_bit(ptp_port->port_num,
			 &ctrl_pf->ptp.tx_refclks[phy][i]) :
		 clear_bit(ptp_port->port_num,
			   &ctrl_pf->ptp.tx_refclks[phy][i]);

	ice_txclk_disable_unused(pf, pf->ptp.port.tx_clk);

	pf->ptp.port.tx_clk = clk;
	pf->ptp.port.tx_clk_req = clk;
}

