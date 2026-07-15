// SPDX-License-Identifier: GPL-2.0
/******************************************************************************
 *
 * Copyright(c) 2007 - 2012 Realtek Corporation. All rights reserved.
 *
 ******************************************************************************/
#include <drv_types.h>
#include <hal_data.h>

static void rtw_hal_init_opmode(struct adapter *padapter)
{
	enum ndis_802_11_network_infrastructure networkType = Ndis802_11InfrastructureMax;
	struct  mlme_priv *pmlmepriv = &(padapter->mlmepriv);
	signed int fw_state;

	fw_state = get_fwstate(pmlmepriv);

	if (fw_state & WIFI_ADHOC_STATE)
		networkType = Ndis802_11IBSS;
	else if (fw_state & WIFI_STATION_STATE)
		networkType = Ndis802_11Infrastructure;
	else if (fw_state & WIFI_AP_STATE)
		networkType = Ndis802_11APMode;
	else
		return;

	rtw_setopmode_cmd(padapter, networkType, false);
}

uint rtw_hal_init(struct adapter *padapter)
{
	uint status;
	struct dvobj_priv *dvobj = adapter_to_dvobj(padapter);

	status = rtl8723bs_hal_init(padapter);

	if (status == _SUCCESS) {
		rtw_hal_init_opmode(padapter);

		dvobj->padapters->hw_init_completed = true;

		if (padapter->registrypriv.notch_filter == 1)
			rtw_hal_notch_filter(padapter, 1);

		rtw_sec_restore_wep_key(dvobj->padapters);

		init_hw_mlme_ext(padapter);

		rtw_bb_rf_gain_offset(padapter);
	} else {
		dvobj->padapters->hw_init_completed = false;
	}

	return status;
}

uint rtw_hal_deinit(struct adapter *padapter)
{
	uint status = _SUCCESS;
	struct dvobj_priv *dvobj = adapter_to_dvobj(padapter);

	status = rtl8723bs_hal_deinit(padapter);

	if (status == _SUCCESS) {
		padapter = dvobj->padapters;
		padapter->hw_init_completed = false;
	}

	return status;
}

void rtw_hal_set_hwreg(struct adapter *padapter, u8 variable, u8 *val)
{
	u8 val8;

	switch (variable) {
	case HW_VAR_SET_RPWM:
		/*  rpwm value only use BIT0(clock bit) , BIT6(Ack bit), and BIT7(Toggle bit) */
		/*  BIT0 value - 1: 32k, 0:40MHz. */
		/*  BIT6 value - 1: report cpwm value after success set, 0:do not report. */
		/*  BIT7 value - Toggle bit change. */
		{
			val8 = *val;
			val8 &= 0xC1;
			rtw_write8(padapter, SDIO_LOCAL_BASE | SDIO_REG_HRPWM1, val8);
		}
		break;
	case HW_VAR_SET_REQ_FW_PS:
		{
			u8 req_fw_ps = 0;

			req_fw_ps = rtw_read8(padapter, 0x8f);
			req_fw_ps |= 0x10;
			rtw_write8(padapter, 0x8f, req_fw_ps);
		}
		break;
	case HW_VAR_RXDMA_AGG_PG_TH:
		val8 = *val;
		break;

	case HW_VAR_DM_IN_LPS:
		rtl8723b_hal_dm_in_lps(padapter);
		break;
	default:
		SetHwReg8723B(padapter, variable, val);
		break;
	}
}

void rtw_hal_get_hwreg(struct adapter *padapter, u8 variable, u8 *val)
{
	switch (variable) {
	case HW_VAR_CPWM:
		*val = rtw_read8(padapter, SDIO_LOCAL_BASE | SDIO_REG_HCPWM1_8723B);
		break;

	case HW_VAR_FW_PS_STATE:
		{
			/* 3. read dword 0x88               driver read fw ps state */
			*((u16 *)val) = rtw_read16(padapter, 0x88);
		}
		break;
	default:
		GetHwReg8723B(padapter, variable, val);
		break;
		}
}

void rtw_hal_set_hwreg_with_buf(struct adapter *padapter, u8 variable, u8 *pbuf, int len)
{
	switch (variable) {
	case HW_VAR_C2H_HANDLE:
		C2HPacketHandler_8723B(padapter, pbuf, len);
		break;
	default:
		break;
		}
}

u8 rtw_hal_get_def_var(struct adapter *padapter, enum hal_def_variable eVariable, void *pValue)
{
	u8 bResult = _SUCCESS;

	switch (eVariable) {
	case HAL_DEF_IS_SUPPORT_ANT_DIV:
		break;
	case HAL_DEF_CURRENT_ANTENNA:
		break;
	case HW_VAR_MAX_RX_AMPDU_FACTOR:
		/*  Stanley@BB.SD3 suggests 16K can get stable performance */
		/*  coding by Lucas@20130730 */
		*(u32 *)pValue = IEEE80211_HT_MAX_AMPDU_16K;
		break;
	default:
		bResult = GetHalDefVar8723B(padapter, eVariable, pValue);
		break;
	}

	return bResult;
}

void rtw_hal_set_odm_var(struct adapter *padapter, enum hal_odm_variable eVariable, void *pValue1, bool bSet)
{
	struct hal_com_data	*pHalData = GET_HAL_DATA(padapter);
	struct dm_odm_t *podmpriv = &pHalData->odmpriv;
	/* _irqL irqL; */
	switch (eVariable) {
	case HAL_ODM_STA_INFO:
		{
			struct sta_info *psta = pValue1;

			if (bSet) {
				ODM_CmnInfoPtrArrayHook(
					podmpriv, ODM_CMNINFO_STA_STATUS, psta->mac_id, psta
				);
			} else {
				/* spin_lock_bh(&pHalData->odm_stainfo_lock); */
				ODM_CmnInfoPtrArrayHook(
					podmpriv, ODM_CMNINFO_STA_STATUS, psta->mac_id, NULL
				);

				/* spin_unlock_bh(&pHalData->odm_stainfo_lock); */
			}
		}
		break;
	case HAL_ODM_P2P_STATE:
			ODM_CmnInfoUpdate(podmpriv, ODM_CMNINFO_WIFI_DIRECT, bSet);
		break;
	case HAL_ODM_WIFI_DISPLAY_STATE:
			ODM_CmnInfoUpdate(podmpriv, ODM_CMNINFO_WIFI_DISPLAY, bSet);
		break;

	default:
		break;
	};
}

u8 rtw_hal_check_ips_status(struct adapter *padapter)
{
	return rtw_read8(padapter, REG_FW_PWR_STATUS) == FW_PWR_STATUS_IPS_DONE;
}

int rtw_hal_xmitframe_enqueue(struct adapter *padapter, struct xmit_frame *pxmitframe)
{
	struct xmit_priv *pxmitpriv = &padapter->xmitpriv;
	s32 err;

	err = rtw_xmitframe_enqueue(padapter, pxmitframe);
	if (err) {
		rtw_free_xmitframe(pxmitpriv, pxmitframe);

		pxmitpriv->tx_drop++;
	} else {
		complete(&pxmitpriv->SdioXmitStart);
	}

	return err;
}

s32	rtw_hal_xmit(struct adapter *padapter, struct xmit_frame *pxmitframe)
{
	if (rtl8723bs_hal_xmit(padapter, pxmitframe))
		return _FAIL;

	return _SUCCESS;
}

/*
 * [IMPORTANT] This function would be run in interrupt context.
 */
s32	rtw_hal_mgnt_xmit(struct adapter *padapter, struct xmit_frame *pmgntframe)
{
	update_mgntframe_attrib_addr(padapter, pmgntframe);
	/* pframe = (u8 *)(pmgntframe->buf_addr) + TXDESC_OFFSET; */
	/* pwlanhdr = (struct rtw_ieee80211_hdr *)pframe; */
	/* memcpy(pmgntframe->attrib.ra, pwlanhdr->addr1, ETH_ALEN); */

	if (padapter->securitypriv.binstallBIPkey) {
		if (is_multicast_ether_addr(pmgntframe->attrib.ra)) {
			pmgntframe->attrib.encrypt = _BIP_;
			/* pmgntframe->attrib.bswenc = true; */
		} else {
			pmgntframe->attrib.encrypt = _AES_;
			pmgntframe->attrib.bswenc = true;
		}
		rtw_mgmt_xmitframe_coalesce(padapter, pmgntframe->pkt, pmgntframe);
	}

	return rtl8723bs_mgnt_xmit(padapter, pmgntframe);
}

s32	rtw_hal_init_xmit_priv(struct adapter *padapter)
{
	return rtl8723bs_init_xmit_priv(padapter);
}

void rtw_hal_free_xmit_priv(struct adapter *padapter)
{
	rtl8723bs_free_xmit_priv(padapter);
}

s32	rtw_hal_init_recv_priv(struct adapter *padapter)
{
	return rtl8723bs_init_recv_priv(padapter);
}

void rtw_hal_free_recv_priv(struct adapter *padapter)
{
	rtl8723bs_free_recv_priv(padapter);
}

void rtw_hal_update_ra_mask(struct sta_info *psta, u8 rssi_level)
{
	struct adapter *padapter;
	struct mlme_priv *pmlmepriv;

	if (!psta)
		return;

	padapter = psta->padapter;

	pmlmepriv = &(padapter->mlmepriv);

	if (check_fwstate(pmlmepriv, WIFI_AP_STATE))
		add_ratid(padapter, psta, rssi_level);
	else {
		UpdateHalRAMask8723B(padapter, psta->mac_id, rssi_level);
	}
}

void rtw_hal_add_ra_tid(struct adapter *padapter, u32 bitmap, u8 *arg, u8 rssi_level)
{
	rtl8723b_Add_RateATid(padapter, bitmap, arg, rssi_level);
}

u32 rtw_hal_read_bbreg(struct adapter *padapter, u32 RegAddr, u32 BitMask)
{
	return PHY_QueryBBReg_8723B(padapter, RegAddr, BitMask);
}
void rtw_hal_write_bbreg(struct adapter *padapter, u32 RegAddr, u32 BitMask, u32 Data)
{
	PHY_SetBBReg_8723B(padapter, RegAddr, BitMask, Data);
}

u32 rtw_hal_read_rfreg(struct adapter *padapter, u32 eRFPath, u32 RegAddr, u32 BitMask)
{
	return PHY_QueryRFReg_8723B(padapter, eRFPath, RegAddr, BitMask);
}
void rtw_hal_write_rfreg(struct adapter *padapter, u32 eRFPath, u32 RegAddr, u32 BitMask, u32 Data)
{
	PHY_SetRFReg_8723B(padapter, eRFPath, RegAddr, BitMask, Data);
}

void rtw_hal_set_chan(struct adapter *padapter, u8 channel)
{
	PHY_SwChnl8723B(padapter, channel);
}

void rtw_hal_set_chnl_bw(struct adapter *padapter, u8 channel,
			 enum channel_width Bandwidth, u8 Offset40, u8 Offset80)
{
	PHY_SetSwChnlBWMode8723B(padapter, channel, Bandwidth, Offset40, Offset80);
}

void rtw_hal_dm_watchdog(struct adapter *padapter)
{
	rtl8723b_HalDmWatchDog(padapter);
}

void rtw_hal_dm_watchdog_in_lps(struct adapter *padapter)
{
	if (adapter_to_pwrctl(padapter)->fw_current_in_ps_mode) {
		rtl8723b_HalDmWatchDog_in_LPS(padapter); /* this function caller is in interrupt context */
	}
}

void beacon_timing_control(struct adapter *padapter)
{
	rtl8723b_SetBeaconRelatedRegisters(padapter);
}

s32 rtw_hal_xmit_thread_handler(struct adapter *padapter)
{
	return rtl8723bs_xmit_buf_handler(padapter);
}

void rtw_hal_notch_filter(struct adapter *adapter, bool enable)
{
	hal_notch_filter_8723b(adapter, enable);
}

bool rtw_hal_c2h_valid(struct adapter *adapter, u8 *buf)
{
	return c2h_evt_valid((struct c2h_evt_hdr_88xx *)buf);
}

s32 rtw_hal_c2h_handler(struct adapter *adapter, u8 *c2h_evt)
{
	return c2h_handler_8723b(adapter, c2h_evt);
}

c2h_id_filter rtw_hal_c2h_id_filter_ccx(struct adapter *adapter)
{
	return c2h_id_filter_ccx_8723b;
}

s32 rtw_hal_macid_sleep(struct adapter *padapter, u32 macid)
{
	u8 support;

	support = false;
	rtw_hal_get_def_var(padapter, HAL_DEF_MACID_SLEEP, &support);
	if (false == support)
		return _FAIL;

	rtw_hal_set_hwreg(padapter, HW_VAR_MACID_SLEEP, (u8 *)&macid);

	return _SUCCESS;
}

s32 rtw_hal_macid_wakeup(struct adapter *padapter, u32 macid)
{
	u8 support;

	support = false;
	rtw_hal_get_def_var(padapter, HAL_DEF_MACID_SLEEP, &support);
	if (false == support)
		return _FAIL;

	rtw_hal_set_hwreg(padapter, HW_VAR_MACID_WAKEUP, (u8 *)&macid);

	return _SUCCESS;
}

s32 rtw_hal_fill_h2c_cmd(struct adapter *padapter, u8 ElementID, u32 CmdLen, u8 *pCmdBuffer)
{
	return FillH2CCmd8723B(padapter, ElementID, CmdLen, pCmdBuffer);
}
