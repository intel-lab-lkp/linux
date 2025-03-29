// SPDX-License-Identifier: GPL-2.0
/******************************************************************************
 *
 * Copyright(c) 2007 - 2011 Realtek Corporation. All rights reserved.
 *
 ******************************************************************************/

#include "odm_precomp.h"

static void odm_SetCrystalCap(void *dm_void, u8 CrystalCap)
{
	struct dm_odm_t *dm_odm = (struct dm_odm_t *)dm_void;
	struct cfo_tracking *cfo_track = &dm_odm->DM_CfoTrack;

	if (cfo_track->CrystalCap == CrystalCap)
		return;

	cfo_track->CrystalCap = CrystalCap;

	/*  0x2C[23:18] = 0x2C[17:12] = CrystalCap */
	CrystalCap = CrystalCap & 0x3F;
	PHY_SetBBReg(
		dm_odm->Adapter,
		REG_MAC_PHY_CTRL,
		0x00FFF000,
		(CrystalCap | (CrystalCap << 6))
	);
}

static u8 odm_GetDefaultCrytaltalCap(void *dm_void)
{
	struct dm_odm_t *dm_odm = (struct dm_odm_t *)dm_void;

	struct adapter *Adapter = dm_odm->Adapter;
	struct hal_com_data *pHalData = GET_HAL_DATA(Adapter);

	return pHalData->CrystalCap & 0x3f;
}

static void odm_SetATCStatus(void *dm_void, bool ATCStatus)
{
	struct dm_odm_t *dm_odm = (struct dm_odm_t *)dm_void;
	struct cfo_tracking *cfo_track = &dm_odm->DM_CfoTrack;

	if (cfo_track->bATCStatus == ATCStatus)
		return;

	PHY_SetBBReg(
		dm_odm->Adapter,
		ODM_REG(BB_ATC, dm_odm),
		ODM_BIT(BB_ATC, dm_odm),
		ATCStatus
	);
	cfo_track->bATCStatus = ATCStatus;
}

static bool odm_GetATCStatus(void *dm_void)
{
	bool ATCStatus;
	struct dm_odm_t *dm_odm = (struct dm_odm_t *)dm_void;

	ATCStatus = (bool)PHY_QueryBBReg(
		dm_odm->Adapter,
		ODM_REG(BB_ATC, dm_odm),
		ODM_BIT(BB_ATC, dm_odm)
	);
	return ATCStatus;
}

void ODM_CfoTrackingReset(void *dm_void)
{
	struct dm_odm_t *dm_odm = (struct dm_odm_t *)dm_void;
	struct cfo_tracking *cfo_track = &dm_odm->DM_CfoTrack;

	cfo_track->DefXCap = odm_GetDefaultCrytaltalCap(dm_odm);
	cfo_track->bAdjust = true;

	odm_SetCrystalCap(dm_odm, cfo_track->DefXCap);
	odm_SetATCStatus(dm_odm, true);
}

void ODM_CfoTrackingInit(void *dm_void)
{
	struct dm_odm_t *dm_odm = (struct dm_odm_t *)dm_void;
	struct cfo_tracking *cfo_track = &dm_odm->DM_CfoTrack;

	cfo_track->DefXCap =
		cfo_track->CrystalCap = odm_GetDefaultCrytaltalCap(dm_odm);
	cfo_track->bATCStatus = odm_GetATCStatus(dm_odm);
	cfo_track->bAdjust = true;
}

void ODM_CfoTracking(void *dm_void)
{
	struct dm_odm_t *dm_odm = (struct dm_odm_t *)dm_void;
	struct cfo_tracking *cfo_track = &dm_odm->DM_CfoTrack;
	int CFO_kHz_A, CFO_ave = 0;
	int CFO_ave_diff;
	int CrystalCap = (int)cfo_track->CrystalCap;
	u8 Adjust_Xtal = 1;

	/* 4 Support ability */
	if (!(dm_odm->SupportAbility & ODM_BB_CFO_TRACKING)) {
		return;
	}

	if (!dm_odm->bLinked || !dm_odm->bOneEntryOnly) {
		/* 4 No link or more than one entry */
		ODM_CfoTrackingReset(dm_odm);
	} else {
		/* 3 1. CFO Tracking */
		/* 4 1.1 No new packet */
		if (cfo_track->packetCount == cfo_track->packetCount_pre) {
			return;
		}
		cfo_track->packetCount_pre = cfo_track->packetCount;

		/* 4 1.2 Calculate CFO */
		CFO_kHz_A =  (int)(cfo_track->CFO_tail[0] * 3125)  / 1280;

		CFO_ave = CFO_kHz_A;

		/* 4 1.3 Avoid abnormal large CFO */
		CFO_ave_diff =
			(cfo_track->CFO_ave_pre >= CFO_ave) ?
			(cfo_track->CFO_ave_pre-CFO_ave) :
			(CFO_ave-cfo_track->CFO_ave_pre);

		if (
			CFO_ave_diff > 20 &&
			cfo_track->largeCFOHit == 0 &&
			!cfo_track->bAdjust
		) {
			cfo_track->largeCFOHit = 1;
			return;
		} else
			cfo_track->largeCFOHit = 0;
		cfo_track->CFO_ave_pre = CFO_ave;

		/* 4 1.4 Dynamic Xtal threshold */
		if (cfo_track->bAdjust == false) {
			if (CFO_ave > CFO_TH_XTAL_HIGH || CFO_ave < (-CFO_TH_XTAL_HIGH))
				cfo_track->bAdjust = true;
		} else {
			if (CFO_ave < CFO_TH_XTAL_LOW && CFO_ave > (-CFO_TH_XTAL_LOW))
				cfo_track->bAdjust = false;
		}

		/* 4 1.5 BT case: Disable CFO tracking */
		if (dm_odm->bBtEnabled) {
			cfo_track->bAdjust = false;
			odm_SetCrystalCap(dm_odm, cfo_track->DefXCap);
		}

		/* 4 1.6 Big jump */
		if (cfo_track->bAdjust) {
			if (CFO_ave > CFO_TH_XTAL_LOW)
				Adjust_Xtal = Adjust_Xtal+((CFO_ave-CFO_TH_XTAL_LOW)>>2);
			else if (CFO_ave < (-CFO_TH_XTAL_LOW))
				Adjust_Xtal = Adjust_Xtal+((CFO_TH_XTAL_LOW-CFO_ave)>>2);
		}

		/* 4 1.7 Adjust Crystal Cap. */
		if (cfo_track->bAdjust) {
			if (CFO_ave > CFO_TH_XTAL_LOW)
				CrystalCap = CrystalCap + Adjust_Xtal;
			else if (CFO_ave < (-CFO_TH_XTAL_LOW))
				CrystalCap = CrystalCap - Adjust_Xtal;

			if (CrystalCap > 0x3f)
				CrystalCap = 0x3f;
			else if (CrystalCap < 0)
				CrystalCap = 0;

			odm_SetCrystalCap(dm_odm, (u8)CrystalCap);
		}

		/* 3 2. Dynamic ATC switch */
		if (CFO_ave < CFO_TH_ATC && CFO_ave > -CFO_TH_ATC) {
			odm_SetATCStatus(dm_odm, false);
		} else {
			odm_SetATCStatus(dm_odm, true);
		}
	}
}

void odm_parsing_cfo(void *dm_void, void *pkt_info_void, s8 *cfotail)
{
	struct dm_odm_t *dm_odm = (struct dm_odm_t *)dm_void;
	struct odm_packet_info *pkt_info = pkt_info_void;
	struct cfo_tracking *cfo_track = &dm_odm->DM_CfoTrack;
	u8 i;

	if (!(dm_odm->SupportAbility & ODM_BB_CFO_TRACKING))
		return;

	if (pkt_info->station_id != 0) {
		/*
		 * 3 Update CFO report for path-A & path-B
		 * Only paht-A and path-B have CFO tail and short CFO
		 */
		for (i = RF_PATH_A; i <= RF_PATH_B; i++)
			cfo_track->CFO_tail[i] = (int)cfotail[i];

		/* 3 Update packet counter */
		if (cfo_track->packetCount == 0xffffffff)
			cfo_track->packetCount = 0;
		else
			cfo_track->packetCount++;
	}
}
