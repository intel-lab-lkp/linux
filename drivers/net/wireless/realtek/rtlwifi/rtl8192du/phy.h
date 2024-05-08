/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2009-2012  Realtek Corporation.*/

#ifndef __RTL92DU_PHY_H__
#define __RTL92DU_PHY_H__

u32 rtl92d_phy_query_bb_reg(struct ieee80211_hw *hw,
			    u32 regaddr, u32 bitmask);
void rtl92d_phy_set_bb_reg(struct ieee80211_hw *hw,
			   u32 regaddr, u32 bitmask, u32 data);
bool rtl92d_phy_mac_config(struct ieee80211_hw *hw);
bool rtl92d_phy_bb_config(struct ieee80211_hw *hw);
bool rtl92d_phy_rf_config(struct ieee80211_hw *hw);
void rtl92d_phy_set_bw_mode(struct ieee80211_hw *hw,
			    enum nl80211_channel_type ch_type);
u8 rtl92d_phy_sw_chnl(struct ieee80211_hw *hw);
bool rtl92d_phy_config_rf_with_headerfile(struct ieee80211_hw *hw,
					  enum rf_content content,
					  enum radio_path rfpath);
bool rtl92d_phy_set_rf_power_state(struct ieee80211_hw *hw,
				   enum rf_pwrstate rfpwr_state);

void rtl92d_phy_set_poweron(struct ieee80211_hw *hw);
bool rtl92d_phy_check_poweroff(struct ieee80211_hw *hw);
void rtl92d_phy_lc_calibrate(struct ieee80211_hw *hw, bool is2t);
void rtl92d_update_bbrf_configuration(struct ieee80211_hw *hw);
void rtl92d_phy_ap_calibrate(struct ieee80211_hw *hw, s8 delta);
void rtl92d_phy_iq_calibrate(struct ieee80211_hw *hw);
void rtl92d_phy_reload_iqk_setting(struct ieee80211_hw *hw, u8 channel);
void rtl92du_phy_init_pa_bias(struct ieee80211_hw *hw);

#endif
