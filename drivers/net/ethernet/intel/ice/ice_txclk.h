/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Intel Corporation */

#ifndef _ICE_TXCLK_H_
#define _ICE_TXCLK_H_

/**
 * ice_txclk_any_port_uses - check if any port on a PHY uses this TX refclk
 * @ctrl_pf: control PF (owner of the shared tx_refclks map)
 * @phy: PHY index
 * @clk: TX reference clock
 *
 * Return: true if any bit (port) is set for this clock on this PHY
 */
static inline bool
ice_txclk_any_port_uses(const struct ice_pf *ctrl_pf, u8 phy,
			enum ice_e825c_ref_clk clk)
{
	return find_first_bit(&ctrl_pf->ptp.tx_refclks[phy][clk],
			BITS_PER_LONG) < BITS_PER_LONG;
}

/**
 * ice_txclk_port_uses - check if a specific port uses this TX refclk
 * @ctrl_pf: control PF
 * @phy: PHY index
 * @clk: TX reference clock
 * @port: port number to test
 *
 * Return: true if this port uses the given clock
 */
static inline bool
ice_txclk_port_uses(const struct ice_pf *ctrl_pf, u8 phy,
		    enum ice_e825c_ref_clk clk, u8 port)
{
	return test_bit(port, &ctrl_pf->ptp.tx_refclks[phy][clk]);
}

int ice_txclk_set_clk(struct ice_pf *pf, enum ice_e825c_ref_clk clk);
void ice_txclk_verify(struct ice_pf *pf);
#endif /* _ICE_TXCLK_H_ */
