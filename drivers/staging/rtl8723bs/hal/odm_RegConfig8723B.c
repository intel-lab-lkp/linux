// SPDX-License-Identifier: GPL-2.0
/******************************************************************************
 *
 * Copyright(c) 2007 - 2011 Realtek Corporation. All rights reserved.
 *
 ******************************************************************************/

#include "odm_precomp.h"

void odm_ConfigRFReg_8723B(
	struct dm_odm_t *pDM_Odm,
	u32 Addr,
	u32 Data,
	enum rf_path RF_PATH,
	u32 RegAddr
)
{
	if (Addr == 0xfe || Addr == 0xffe)
		msleep(50);
	else {
		PHY_SetRFReg(pDM_Odm->Adapter, RF_PATH, RegAddr, bRFRegOffsetMask, Data);
		/*  Add 1us delay between BB/RF register setting. */
		usleep_range(1, 2);

		/* For disable/enable test in high temperature, the B6 value will fail to fill. Suggestion by BB Stanley, 2013.06.25. */
		if (Addr == 0xb6) {
			u32 getvalue = 0;
			u8 count = 0;

			getvalue = PHY_QueryRFReg(
				pDM_Odm->Adapter, RF_PATH, Addr, bMaskDWord
			);

			usleep_range(1, 2);

			while ((getvalue>>8) != (Data>>8)) {
				count++;
				PHY_SetRFReg(pDM_Odm->Adapter, RF_PATH, RegAddr, bRFRegOffsetMask, Data);
				usleep_range(1, 2);
				getvalue = PHY_QueryRFReg(pDM_Odm->Adapter, RF_PATH, Addr, bMaskDWord);
				if (count > 5)
					break;
			}
		}

		if (Addr == 0xb2) {
			u32 getvalue = 0;
			u8 count = 0;

			getvalue = PHY_QueryRFReg(
				pDM_Odm->Adapter, RF_PATH, Addr, bMaskDWord
			);

			usleep_range(1, 2);

			while (getvalue != Data) {
				count++;
				PHY_SetRFReg(
					pDM_Odm->Adapter,
					RF_PATH,
					RegAddr,
					bRFRegOffsetMask,
					Data
				);
				usleep_range(1, 2);
				/* Do LCK againg */
				PHY_SetRFReg(
					pDM_Odm->Adapter,
					RF_PATH,
					0x18,
					bRFRegOffsetMask,
					0x0fc07
				);
				usleep_range(1, 2);
				getvalue = PHY_QueryRFReg(
					pDM_Odm->Adapter, RF_PATH, Addr, bMaskDWord
				);

				if (count > 5)
					break;
			}
		}
	}
}


void odm_ConfigRF_RadioA_8723B(struct dm_odm_t *pDM_Odm, u32 Addr, u32 Data)
{
	u32  content = 0x1000; /*  RF_Content: radioa_txt */
	u32 maskforPhySet = (u32)(content&0xE000);

	odm_ConfigRFReg_8723B(
		pDM_Odm,
		Addr,
		Data,
		RF_PATH_A,
		Addr|maskforPhySet
	);
}

void odm_ConfigMAC_8723B(struct dm_odm_t *pDM_Odm, u32 Addr, u8 Data)
{
	rtw_write8(pDM_Odm->Adapter, Addr, Data);
}

void odm_ConfigBB_AGC_8723B(
	struct dm_odm_t *pDM_Odm,
	u32 Addr,
	u32 Bitmask,
	u32 Data
)
{
	PHY_SetBBReg(pDM_Odm->Adapter, Addr, Bitmask, Data);
	/*  Add 1us delay between BB/RF register setting. */
	usleep_range(1, 2);
}

void odm_ConfigBB_PHY_REG_PG_8723B(
	struct dm_odm_t *pDM_Odm,
	u32 RfPath,
	u32 Addr,
	u32 Bitmask,
	u32 Data
)
{
	if (Addr == 0xfe || Addr == 0xffe)
		msleep(50);
	else
		PHY_StoreTxPowerByRate(pDM_Odm->Adapter, RfPath, Addr, Bitmask, Data);
}

void odm_ConfigBB_PHY_8723B(
	struct dm_odm_t *pDM_Odm,
	u32 Addr,
	u32 Bitmask,
	u32 Data
)
{
	if (Addr == 0xfe)
		msleep(50);
	else if (Addr == 0xfd)
		mdelay(5);
	else if (Addr == 0xfc)
		mdelay(1);
	else if (Addr == 0xfb)
		usleep_range(50, 60);
	else if (Addr == 0xfa)
		usleep_range(5, 10);
	else if (Addr == 0xf9)
		usleep_range(1, 2);
	else
		PHY_SetBBReg(pDM_Odm->Adapter, Addr, Bitmask, Data);

	/*  Add 1us delay between BB/RF register setting. */
	usleep_range(1, 2);
}

void odm_ConfigBB_TXPWR_LMT_8723B(
	struct dm_odm_t *pDM_Odm,
	u8 *Regulation,
	u8 *Bandwidth,
	u8 *RateSection,
	u8 *RfPath,
	u8 *Channel,
	u8 *PowerLimit
)
{
	PHY_SetTxPowerLimit(
		pDM_Odm->Adapter,
		Regulation,
		Bandwidth,
		RateSection,
		RfPath,
		Channel,
		PowerLimit
	);
}
