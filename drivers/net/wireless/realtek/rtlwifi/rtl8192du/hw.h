/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2009-2012  Realtek Corporation.*/

#ifndef __RTL92DU_HW_H__
#define __RTL92DU_HW_H__

void rtl92du_get_hw_reg(struct ieee80211_hw *hw, u8 variable, u8 *val);
void rtl92du_set_hw_reg(struct ieee80211_hw *hw, u8 variable, u8 *val);
void rtl92du_read_chip_version(struct ieee80211_hw *hw);
int rtl92du_hw_init(struct ieee80211_hw *hw);
void rtl92du_card_disable(struct ieee80211_hw *hw);
void rtl92de_enable_interrupt(struct ieee80211_hw *hw);
void rtl92de_disable_interrupt(struct ieee80211_hw *hw);
int rtl92de_set_network_type(struct ieee80211_hw *hw, enum nl80211_iftype type);
void rtl92de_set_check_bssid(struct ieee80211_hw *hw, bool check_bssid);
void rtl92de_set_beacon_related_registers(struct ieee80211_hw *hw);
void rtl92de_set_beacon_interval(struct ieee80211_hw *hw);
void rtl92de_update_interrupt_mask(struct ieee80211_hw *hw,
				   u32 add_msr, u32 rm_msr);
void rtl92de_suspend(struct ieee80211_hw *hw);
void rtl92de_resume(struct ieee80211_hw *hw);
void rtl92d_linked_set_reg(struct ieee80211_hw *hw);

#endif
