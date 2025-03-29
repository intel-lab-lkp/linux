// SPDX-License-Identifier: GPL-2.0
/*****************************************************************************
 *
 * Copyright(c) 2007 - 2011 Realtek Corporation. All rights reserved.
 *
 ******************************************************************************/

#include <drv_types.h>
#include "odm_precomp.h"

/*  MACRO definition for rf_calibrate_info->TxIQC_8723B[0] */
#define		PATH_S0							1 /*  RF_PATH_B */
#define		IDX_0xC94						0
#define		IDX_0xC80						1
#define		IDX_0xC14						0
#define		IDX_0xCA0						1
#define		KEY							0
#define		VAL							1

/*  MACRO definition for rf_calibrate_info->TxIQC_8723B[1] */
#define		PATH_S1							0 /*  RF_PATH_A */
#define		IDX_0xC4C						2

/*---------------------------Define Local Constant---------------------------*/

/* In the case that we fail to read TxPowerTrack.txt, we use the table for
 * 88E as the default table.
 */
static u8 DeltaSwingTableIdx_2GA_N_8188E[] = {
	0, 0, 0, 2, 2, 3, 3, 4,  4,  4,  4,  5,  5,  6,  6,
	7, 7, 7, 7, 8, 8, 9, 9, 10, 10, 10, 11, 11, 11, 11
};
static u8 DeltaSwingTableIdx_2GA_P_8188E[] = {
	0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 4, 4, 4,
	4, 4, 4, 5, 5, 7, 7, 8, 8, 8, 9, 9, 9, 9, 9
};

/* 3 ============================================================ */
/* 3 Tx Power Tracking */
/* 3 ============================================================ */


static void setIqkMatrix_8723B(
	struct dm_odm_t *dm_odm,
	u8 OFDM_index,
	u8 RFPath,
	s32 IqkResult_X,
	s32 IqkResult_Y
)
{
	s32 ele_A = 0, ele_D, ele_C = 0, value32;

	if (OFDM_index >= OFDM_TABLE_SIZE)
		OFDM_index = OFDM_TABLE_SIZE-1;

	ele_D = (OFDMSwingTable_New[OFDM_index] & 0xFFC00000)>>22;

	/* new element A = element D x X */
	if (IqkResult_X != 0) {
		if ((IqkResult_X & 0x00000200) != 0)	/* consider minus */
			IqkResult_X = IqkResult_X | 0xFFFFFC00;
		ele_A = ((IqkResult_X * ele_D)>>8)&0x000003FF;

		/* new element C = element D x Y */
		if ((IqkResult_Y & 0x00000200) != 0)
			IqkResult_Y = IqkResult_Y | 0xFFFFFC00;
		ele_C = ((IqkResult_Y * ele_D)>>8)&0x000003FF;

		/* if (RFPath == RF_PATH_A) */
		switch (RFPath) {
		case RF_PATH_A:
			/* write new elements A, C, D to regC80 and regC94,
			 * element B is always 0
			 */
			value32 = (ele_D<<22)|((ele_C&0x3F)<<16)|ele_A;
			PHY_SetBBReg(dm_odm->Adapter, rOFDM0_XATxIQImbalance, bMaskDWord, value32);

			value32 = (ele_C&0x000003C0)>>6;
			PHY_SetBBReg(dm_odm->Adapter, rOFDM0_XCTxAFE, bMaskH4Bits, value32);

			value32 = ((IqkResult_X * ele_D)>>7)&0x01;
			PHY_SetBBReg(dm_odm->Adapter, rOFDM0_ECCAThreshold, BIT24, value32);
			break;
		case RF_PATH_B:
			/* write new elements A, C, D to regC88 and regC9C,
			 * element B is always 0
			 */
			value32 = (ele_D<<22)|((ele_C&0x3F)<<16)|ele_A;
			PHY_SetBBReg(dm_odm->Adapter, rOFDM0_XBTxIQImbalance, bMaskDWord, value32);

			value32 = (ele_C&0x000003C0)>>6;
			PHY_SetBBReg(dm_odm->Adapter, rOFDM0_XDTxAFE, bMaskH4Bits, value32);

			value32 = ((IqkResult_X * ele_D)>>7)&0x01;
			PHY_SetBBReg(dm_odm->Adapter, rOFDM0_ECCAThreshold, BIT28, value32);

			break;
		default:
			break;
		}
	} else {
		switch (RFPath) {
		case RF_PATH_A:
			PHY_SetBBReg(dm_odm->Adapter, rOFDM0_XATxIQImbalance, bMaskDWord, OFDMSwingTable_New[OFDM_index]);
			PHY_SetBBReg(dm_odm->Adapter, rOFDM0_XCTxAFE, bMaskH4Bits, 0x00);
			PHY_SetBBReg(dm_odm->Adapter, rOFDM0_ECCAThreshold, BIT24, 0x00);
			break;

		case RF_PATH_B:
			PHY_SetBBReg(dm_odm->Adapter, rOFDM0_XBTxIQImbalance, bMaskDWord, OFDMSwingTable_New[OFDM_index]);
			PHY_SetBBReg(dm_odm->Adapter, rOFDM0_XDTxAFE, bMaskH4Bits, 0x00);
			PHY_SetBBReg(dm_odm->Adapter, rOFDM0_ECCAThreshold, BIT28, 0x00);
			break;

		default:
			break;
		}
	}
}


static void setCCKFilterCoefficient(struct dm_odm_t *dm_odm, u8 CCKSwingIndex)
{
	if (!dm_odm->RFCalibrateInfo.bCCKinCH14) {
		rtw_write8(dm_odm->Adapter, 0xa22, CCKSwingTable_Ch1_Ch13_New[CCKSwingIndex][0]);
		rtw_write8(dm_odm->Adapter, 0xa23, CCKSwingTable_Ch1_Ch13_New[CCKSwingIndex][1]);
		rtw_write8(dm_odm->Adapter, 0xa24, CCKSwingTable_Ch1_Ch13_New[CCKSwingIndex][2]);
		rtw_write8(dm_odm->Adapter, 0xa25, CCKSwingTable_Ch1_Ch13_New[CCKSwingIndex][3]);
		rtw_write8(dm_odm->Adapter, 0xa26, CCKSwingTable_Ch1_Ch13_New[CCKSwingIndex][4]);
		rtw_write8(dm_odm->Adapter, 0xa27, CCKSwingTable_Ch1_Ch13_New[CCKSwingIndex][5]);
		rtw_write8(dm_odm->Adapter, 0xa28, CCKSwingTable_Ch1_Ch13_New[CCKSwingIndex][6]);
		rtw_write8(dm_odm->Adapter, 0xa29, CCKSwingTable_Ch1_Ch13_New[CCKSwingIndex][7]);
	} else {
		rtw_write8(dm_odm->Adapter, 0xa22, CCKSwingTable_Ch14_New[CCKSwingIndex][0]);
		rtw_write8(dm_odm->Adapter, 0xa23, CCKSwingTable_Ch14_New[CCKSwingIndex][1]);
		rtw_write8(dm_odm->Adapter, 0xa24, CCKSwingTable_Ch14_New[CCKSwingIndex][2]);
		rtw_write8(dm_odm->Adapter, 0xa25, CCKSwingTable_Ch14_New[CCKSwingIndex][3]);
		rtw_write8(dm_odm->Adapter, 0xa26, CCKSwingTable_Ch14_New[CCKSwingIndex][4]);
		rtw_write8(dm_odm->Adapter, 0xa27, CCKSwingTable_Ch14_New[CCKSwingIndex][5]);
		rtw_write8(dm_odm->Adapter, 0xa28, CCKSwingTable_Ch14_New[CCKSwingIndex][6]);
		rtw_write8(dm_odm->Adapter, 0xa29, CCKSwingTable_Ch14_New[CCKSwingIndex][7]);
	}
}

void DoIQK_8723B(
	struct dm_odm_t *dm_odm,
	u8 DeltaThermalIndex,
	u8 ThermalValue,
	u8 Threshold
)
{
}

/*-----------------------------------------------------------------------------
 * Function:	odm_TxPwrTrackSetPwr88E()
 *
 * Overview:	88E change all channel tx power according to flag.
 *			OFDM & CCK are all different.
 *
 * Input:		NONE
 *
 * Output:		NONE
 *
 * Return:		NONE
 *
 * Revised History:
 *When		Who	Remark
 *04/23/2012	MHC	Create Version 0.
 *
 *---------------------------------------------------------------------------*/
void ODM_TxPwrTrackSetPwr_8723B(
	struct dm_odm_t *dm_odm,
	enum pwrtrack_method Method,
	u8 RFPath,
	u8 ChannelMappedIndex
)
{
	struct adapter *Adapter = dm_odm->Adapter;
	struct hal_com_data *hal_data = GET_HAL_DATA(Adapter);
	u8 PwrTrackingLimit_OFDM = 34; /* 0dB */
	u8 PwrTrackingLimit_CCK = 28; /* 2dB */
	u8 TxRate = 0xFF;
	u8 Final_OFDM_Swing_Index = 0;
	u8 Final_CCK_Swing_Index = 0;

	{
		u16 rate = *(dm_odm->pForcedDataRate);

		if (!rate) { /* auto rate */
			if (dm_odm->TxRate != 0xFF)
				TxRate = HwRateToMRate(dm_odm->TxRate);
		} else /* force rate */
			TxRate = (u8)rate;

	}

	if (TxRate != 0xFF) {
		/* 2 CCK */
		if ((TxRate >= MGN_1M) && (TxRate <= MGN_11M))
			PwrTrackingLimit_CCK = 28;	/* 2dB */
		/* 2 OFDM */
		else if ((TxRate >= MGN_6M) && (TxRate <= MGN_48M))
			PwrTrackingLimit_OFDM = 36; /* 3dB */
		else if (TxRate == MGN_54M)
			PwrTrackingLimit_OFDM = 34; /* 2dB */

		/* 2 HT */
		else if ((TxRate >= MGN_MCS0) && (TxRate <= MGN_MCS2)) /* QPSK/BPSK */
			PwrTrackingLimit_OFDM = 38; /* 4dB */
		else if ((TxRate >= MGN_MCS3) && (TxRate <= MGN_MCS4)) /* 16QAM */
			PwrTrackingLimit_OFDM = 36; /* 3dB */
		else if ((TxRate >= MGN_MCS5) && (TxRate <= MGN_MCS7)) /* 64QAM */
			PwrTrackingLimit_OFDM = 34; /* 2dB */

		else
			PwrTrackingLimit_OFDM =  dm_odm->DefaultOfdmIndex;   /* Default OFDM index = 30 */
	}

	if (Method == TXAGC) {
		struct adapter *Adapter = dm_odm->Adapter;

		dm_odm->Remnant_OFDMSwingIdx[RFPath] = dm_odm->Absolute_OFDMSwingIdx[RFPath];

		dm_odm->Modify_TxAGC_Flag_PathA = true;
		dm_odm->Modify_TxAGC_Flag_PathA_CCK = true;

		PHY_SetTxPowerIndexByRateSection(Adapter, RFPath, hal_data->CurrentChannel, CCK);
		PHY_SetTxPowerIndexByRateSection(Adapter, RFPath, hal_data->CurrentChannel, OFDM);
		PHY_SetTxPowerIndexByRateSection(Adapter, RFPath, hal_data->CurrentChannel, HT_MCS0_MCS7);
	} else if (Method == BBSWING) {
		Final_OFDM_Swing_Index = dm_odm->DefaultOfdmIndex + dm_odm->Absolute_OFDMSwingIdx[RFPath];
		Final_CCK_Swing_Index = dm_odm->DefaultCckIndex + dm_odm->Absolute_OFDMSwingIdx[RFPath];

		/*  Adjust BB swing by OFDM IQ matrix */
		if (Final_OFDM_Swing_Index >= PwrTrackingLimit_OFDM)
			Final_OFDM_Swing_Index = PwrTrackingLimit_OFDM;
		else if (Final_OFDM_Swing_Index <= 0)
			Final_OFDM_Swing_Index = 0;

		if (Final_CCK_Swing_Index >= CCK_TABLE_SIZE)
			Final_CCK_Swing_Index = CCK_TABLE_SIZE-1;
		else if (dm_odm->BbSwingIdxCck <= 0)
			Final_CCK_Swing_Index = 0;

		setIqkMatrix_8723B(dm_odm, Final_OFDM_Swing_Index, RFPath,
			dm_odm->RFCalibrateInfo.iqk_matrix_regs_setting_value[ChannelMappedIndex][0],
			dm_odm->RFCalibrateInfo.iqk_matrix_regs_setting_value[ChannelMappedIndex][1]);

		setCCKFilterCoefficient(dm_odm, Final_CCK_Swing_Index);

	} else if (Method == MIX_MODE) {
		Final_OFDM_Swing_Index = dm_odm->DefaultOfdmIndex + dm_odm->Absolute_OFDMSwingIdx[RFPath];
		Final_CCK_Swing_Index = dm_odm->DefaultCckIndex + dm_odm->Absolute_OFDMSwingIdx[RFPath];

		if (Final_OFDM_Swing_Index > PwrTrackingLimit_OFDM) { /* BBSwing higher then Limit */
			dm_odm->Remnant_OFDMSwingIdx[RFPath] = Final_OFDM_Swing_Index - PwrTrackingLimit_OFDM;

			setIqkMatrix_8723B(dm_odm, PwrTrackingLimit_OFDM, RFPath,
				dm_odm->RFCalibrateInfo.iqk_matrix_regs_setting_value[ChannelMappedIndex][0],
				dm_odm->RFCalibrateInfo.iqk_matrix_regs_setting_value[ChannelMappedIndex][1]);

			dm_odm->Modify_TxAGC_Flag_PathA = true;
			PHY_SetTxPowerIndexByRateSection(Adapter, RFPath, hal_data->CurrentChannel, OFDM);
			PHY_SetTxPowerIndexByRateSection(Adapter, RFPath, hal_data->CurrentChannel, HT_MCS0_MCS7);
		} else if (Final_OFDM_Swing_Index <= 0) {
			dm_odm->Remnant_OFDMSwingIdx[RFPath] = Final_OFDM_Swing_Index;

			setIqkMatrix_8723B(dm_odm, 0, RFPath,
				dm_odm->RFCalibrateInfo.iqk_matrix_regs_setting_value[ChannelMappedIndex][0],
				dm_odm->RFCalibrateInfo.iqk_matrix_regs_setting_value[ChannelMappedIndex][1]);

			dm_odm->Modify_TxAGC_Flag_PathA = true;
			PHY_SetTxPowerIndexByRateSection(Adapter, RFPath, hal_data->CurrentChannel, OFDM);
			PHY_SetTxPowerIndexByRateSection(Adapter, RFPath, hal_data->CurrentChannel, HT_MCS0_MCS7);
		} else {
			setIqkMatrix_8723B(dm_odm, Final_OFDM_Swing_Index, RFPath,
				dm_odm->RFCalibrateInfo.iqk_matrix_regs_setting_value[ChannelMappedIndex][0],
				dm_odm->RFCalibrateInfo.iqk_matrix_regs_setting_value[ChannelMappedIndex][1]);

			if (dm_odm->Modify_TxAGC_Flag_PathA) { /* If TxAGC has changed, reset TxAGC again */
				dm_odm->Remnant_OFDMSwingIdx[RFPath] = 0;
				PHY_SetTxPowerIndexByRateSection(Adapter, RFPath, hal_data->CurrentChannel, OFDM);
				PHY_SetTxPowerIndexByRateSection(Adapter, RFPath, hal_data->CurrentChannel, HT_MCS0_MCS7);
				dm_odm->Modify_TxAGC_Flag_PathA = false;
			}
		}

		if (Final_CCK_Swing_Index > PwrTrackingLimit_CCK) {
			dm_odm->Remnant_CCKSwingIdx = Final_CCK_Swing_Index - PwrTrackingLimit_CCK;
			setCCKFilterCoefficient(dm_odm, PwrTrackingLimit_CCK);
			dm_odm->Modify_TxAGC_Flag_PathA_CCK = true;
			PHY_SetTxPowerIndexByRateSection(Adapter, RFPath, hal_data->CurrentChannel, CCK);
		} else if (Final_CCK_Swing_Index <= 0) { /*  Lowest CCK Index = 0 */
			dm_odm->Remnant_CCKSwingIdx = Final_CCK_Swing_Index;
			setCCKFilterCoefficient(dm_odm, 0);
			dm_odm->Modify_TxAGC_Flag_PathA_CCK = true;
			PHY_SetTxPowerIndexByRateSection(Adapter, RFPath, hal_data->CurrentChannel, CCK);
		} else {
			setCCKFilterCoefficient(dm_odm, Final_CCK_Swing_Index);

			if (dm_odm->Modify_TxAGC_Flag_PathA_CCK) { /* If TxAGC has changed, reset TxAGC again */
				dm_odm->Remnant_CCKSwingIdx = 0;
				PHY_SetTxPowerIndexByRateSection(Adapter, RFPath, hal_data->CurrentChannel, CCK);
				dm_odm->Modify_TxAGC_Flag_PathA_CCK = false;
			}
		}
	} else
		return; /*  This method is not supported. */
}

static void GetDeltaSwingTable_8723B(
	struct dm_odm_t *dm_odm,
	u8 **TemperatureUP_A,
	u8 **TemperatureDOWN_A,
	u8 **TemperatureUP_B,
	u8 **TemperatureDOWN_B
)
{
	struct adapter *Adapter = dm_odm->Adapter;
	struct odm_rf_cal_t *rf_calibrate_info = &dm_odm->RFCalibrateInfo;
	struct hal_com_data *hal_data = GET_HAL_DATA(Adapter);
	u16 rate = *(dm_odm->pForcedDataRate);
	u8 channel = hal_data->CurrentChannel;

	if (1 <= channel && channel <= 14) {
		if (IS_CCK_RATE(rate)) {
			*TemperatureUP_A   = rf_calibrate_info->DeltaSwingTableIdx_2GCCKA_P;
			*TemperatureDOWN_A = rf_calibrate_info->DeltaSwingTableIdx_2GCCKA_N;
			*TemperatureUP_B   = rf_calibrate_info->DeltaSwingTableIdx_2GCCKB_P;
			*TemperatureDOWN_B = rf_calibrate_info->DeltaSwingTableIdx_2GCCKB_N;
		} else {
			*TemperatureUP_A   = rf_calibrate_info->DeltaSwingTableIdx_2GA_P;
			*TemperatureDOWN_A = rf_calibrate_info->DeltaSwingTableIdx_2GA_N;
			*TemperatureUP_B   = rf_calibrate_info->DeltaSwingTableIdx_2GB_P;
			*TemperatureDOWN_B = rf_calibrate_info->DeltaSwingTableIdx_2GB_N;
		}
	} else {
		*TemperatureUP_A   = (u8 *)DeltaSwingTableIdx_2GA_P_8188E;
		*TemperatureDOWN_A = (u8 *)DeltaSwingTableIdx_2GA_N_8188E;
		*TemperatureUP_B   = (u8 *)DeltaSwingTableIdx_2GA_P_8188E;
		*TemperatureDOWN_B = (u8 *)DeltaSwingTableIdx_2GA_N_8188E;
	}
}


void ConfigureTxpowerTrack_8723B(struct txpwrtrack_cfg *config)
{
	config->SwingTableSize_CCK = CCK_TABLE_SIZE;
	config->SwingTableSize_OFDM = OFDM_TABLE_SIZE;
	config->Threshold_IQK = IQK_THRESHOLD;
	config->AverageThermalNum = AVG_THERMAL_NUM_8723B;
	config->RfPathCount = MAX_PATH_NUM_8723B;
	config->ThermalRegAddr = RF_T_METER_8723B;

	config->ODM_TxPwrTrackSetPwr = ODM_TxPwrTrackSetPwr_8723B;
	config->DoIQK = DoIQK_8723B;
	config->PHY_LCCalibrate = PHY_LCCalibrate_8723B;
	config->GetDeltaSwingTable = GetDeltaSwingTable_8723B;
}

/* 1 7. IQK */
#define MAX_TOLERANCE		5

/* bit0 = 1 => Tx OK, bit1 = 1 => Rx OK */
static u8 phy_PathA_IQK_8723B(
	struct adapter *adapter, bool configPathB, u8 RF_Path
)
{
	u32 regEAC, regE94, regE9C, tmp, Path_SEL_BB /*, regEA4*/;
	u8 result = 0x00;

	struct hal_com_data	*hal_data = GET_HAL_DATA(adapter);
	struct dm_odm_t *dm_odm = &hal_data->odmpriv;

	/*  Save RF Path */
	Path_SEL_BB = PHY_QueryBBReg(dm_odm->Adapter, 0x948, bMaskDWord);

	/* leave IQK mode */
	PHY_SetBBReg(dm_odm->Adapter, rFPGA0_IQK, bMaskH3Bytes, 0x000000);

	/* 	enable path A PA in TXIQK mode */
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, RF_WE_LUT, 0x80000, 0x1);
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, RF_RCK_OS, bRFRegOffsetMask, 0x18000);
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, RF_TXPA_G1, bRFRegOffsetMask, 0x0003f);
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, RF_TXPA_G2, bRFRegOffsetMask, 0xc7f87);
	/* 	disable path B PA in TXIQK mode */
/* 	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, 0xed, bRFRegOffsetMask, 0x00020); */
/* 	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, 0x43, bRFRegOffsetMask, 0x40ec1); */

	/* 1 Tx IQK */
	/* IQK setting */
	PHY_SetBBReg(dm_odm->Adapter, rTx_IQK, bMaskDWord, 0x01007c00);
	PHY_SetBBReg(dm_odm->Adapter, rRx_IQK, bMaskDWord, 0x01004800);
	/* path-A IQK setting */
	PHY_SetBBReg(dm_odm->Adapter, rTx_IQK_Tone_A, bMaskDWord, 0x18008c1c);
	PHY_SetBBReg(dm_odm->Adapter, rRx_IQK_Tone_A, bMaskDWord, 0x38008c1c);
	PHY_SetBBReg(dm_odm->Adapter, rTx_IQK_Tone_B, bMaskDWord, 0x38008c1c);
	PHY_SetBBReg(dm_odm->Adapter, rRx_IQK_Tone_B, bMaskDWord, 0x38008c1c);
/* 	PHY_SetBBReg(dm_odm->Adapter, rTx_IQK_PI_A, bMaskDWord, 0x8214010a); */
	PHY_SetBBReg(dm_odm->Adapter, rTx_IQK_PI_A, bMaskDWord, 0x821303ea);
	PHY_SetBBReg(dm_odm->Adapter, rRx_IQK_PI_A, bMaskDWord, 0x28110000);
	PHY_SetBBReg(dm_odm->Adapter, rTx_IQK_PI_B, bMaskDWord, 0x82110000);
	PHY_SetBBReg(dm_odm->Adapter, rRx_IQK_PI_B, bMaskDWord, 0x28110000);

	/* LO calibration setting */
	PHY_SetBBReg(dm_odm->Adapter, rIQK_AGC_Rsp, bMaskDWord, 0x00462911);

	/* enter IQK mode */
	PHY_SetBBReg(dm_odm->Adapter, rFPGA0_IQK, bMaskH3Bytes, 0x808000);

	/* Ant switch */
	if (configPathB || (RF_Path == 0))
		/*  wifi switch to S1 */
		PHY_SetBBReg(dm_odm->Adapter, 0x948, bMaskDWord, 0x00000000);
	else
		/*  wifi switch to S0 */
		PHY_SetBBReg(dm_odm->Adapter, 0x948, bMaskDWord, 0x00000280);

	/* GNT_BT = 0 */
	PHY_SetBBReg(dm_odm->Adapter, 0x764, bMaskDWord, 0x00000800);

	/* One shot, path A LOK & IQK */
	PHY_SetBBReg(dm_odm->Adapter, rIQK_AGC_Pts, bMaskDWord, 0xf9000000);
	PHY_SetBBReg(dm_odm->Adapter, rIQK_AGC_Pts, bMaskDWord, 0xf8000000);

	/*  delay x ms */
	/* PlatformStallExecution(IQK_DELAY_TIME_8723B*1000); */
	mdelay(IQK_DELAY_TIME_8723B);

	/* restore Ant Path */
	PHY_SetBBReg(dm_odm->Adapter, 0x948, bMaskDWord, Path_SEL_BB);
	/* GNT_BT = 1 */
	PHY_SetBBReg(dm_odm->Adapter, 0x764, bMaskDWord, 0x00001800);

	/* leave IQK mode */
	PHY_SetBBReg(dm_odm->Adapter, rFPGA0_IQK, bMaskH3Bytes, 0x000000);


	/*  Check failed */
	regEAC = PHY_QueryBBReg(dm_odm->Adapter, rRx_Power_After_IQK_A_2, bMaskDWord);
	regE94 = PHY_QueryBBReg(dm_odm->Adapter, rTx_Power_Before_IQK_A, bMaskDWord);
	regE9C = PHY_QueryBBReg(dm_odm->Adapter, rTx_Power_After_IQK_A, bMaskDWord);


	/* Allen 20131125 */
	tmp = (regE9C & 0x03FF0000)>>16;
	if ((tmp & 0x200) > 0)
		tmp = 0x400 - tmp;

	if (
		!(regEAC & BIT28) &&
		(((regE94 & 0x03FF0000)>>16) != 0x142) &&
		(((regE9C & 0x03FF0000)>>16) != 0x42) &&
		(((regE94 & 0x03FF0000)>>16) < 0x110) &&
		(((regE94 & 0x03FF0000)>>16) > 0xf0) &&
		(tmp < 0xf)
	)
		result |= 0x01;
	else					/* if Tx not OK, ignore Rx */
		return result;

	return result;
}

/* bit0 = 1 => Tx OK, bit1 = 1 => Rx OK */
static u8 phy_PathA_RxIQK8723B(
	struct adapter *adapter, bool configPathB, u8 RF_Path
)
{
	u32 regEAC, regE94, regE9C, regEA4, u4tmp, tmp, Path_SEL_BB;
	u8 result = 0x00;
	struct hal_com_data *hal_data = GET_HAL_DATA(adapter);
	struct dm_odm_t *dm_odm = &hal_data->odmpriv;

	/*  Save RF Path */
	Path_SEL_BB = PHY_QueryBBReg(dm_odm->Adapter, 0x948, bMaskDWord);

	/* leave IQK mode */
	PHY_SetBBReg(dm_odm->Adapter, rFPGA0_IQK, bMaskH3Bytes, 0x000000);
	/* 1 Get TXIMR setting */
	/* modify RXIQK mode table */
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, RF_WE_LUT, 0x80000, 0x1);
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, RF_RCK_OS, bRFRegOffsetMask, 0x18000);
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, RF_TXPA_G1, bRFRegOffsetMask, 0x0001f);
	/* LNA2 off, PA on for Dcut */
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, RF_TXPA_G2, bRFRegOffsetMask, 0xf7fb7);
/* 	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, RF_WE_LUT, 0x80000, 0x0); */
	PHY_SetBBReg(dm_odm->Adapter, rFPGA0_IQK, bMaskH3Bytes, 0x808000);

	/* IQK setting */
	PHY_SetBBReg(dm_odm->Adapter, rTx_IQK, bMaskDWord, 0x01007c00);
	PHY_SetBBReg(dm_odm->Adapter, rRx_IQK, bMaskDWord, 0x01004800);

	/* path-A IQK setting */
	PHY_SetBBReg(dm_odm->Adapter, rTx_IQK_Tone_A, bMaskDWord, 0x18008c1c);
	PHY_SetBBReg(dm_odm->Adapter, rRx_IQK_Tone_A, bMaskDWord, 0x38008c1c);
	PHY_SetBBReg(dm_odm->Adapter, rTx_IQK_Tone_B, bMaskDWord, 0x38008c1c);
	PHY_SetBBReg(dm_odm->Adapter, rRx_IQK_Tone_B, bMaskDWord, 0x38008c1c);

/* 	PHY_SetBBReg(dm_odm->Adapter, rTx_IQK_PI_A, bMaskDWord, 0x82160c1f); */
	PHY_SetBBReg(dm_odm->Adapter, rTx_IQK_PI_A, bMaskDWord, 0x82130ff0);
	PHY_SetBBReg(dm_odm->Adapter, rRx_IQK_PI_A, bMaskDWord, 0x28110000);
	PHY_SetBBReg(dm_odm->Adapter, rTx_IQK_PI_B, bMaskDWord, 0x82110000);
	PHY_SetBBReg(dm_odm->Adapter, rRx_IQK_PI_B, bMaskDWord, 0x28110000);

	/* LO calibration setting */
	PHY_SetBBReg(dm_odm->Adapter, rIQK_AGC_Rsp, bMaskDWord, 0x0046a911);

	/* enter IQK mode */
	PHY_SetBBReg(dm_odm->Adapter, rFPGA0_IQK, bMaskH3Bytes, 0x808000);

	/* Ant switch */
	if (configPathB || (RF_Path == 0))
		/*  wifi switch to S1 */
		PHY_SetBBReg(dm_odm->Adapter, 0x948, bMaskDWord, 0x00000000);
	else
		/*  wifi switch to S0 */
		PHY_SetBBReg(dm_odm->Adapter, 0x948, bMaskDWord, 0x00000280);

	/* GNT_BT = 0 */
	PHY_SetBBReg(dm_odm->Adapter, 0x764, bMaskDWord, 0x00000800);

	/* One shot, path A LOK & IQK */
	PHY_SetBBReg(dm_odm->Adapter, rIQK_AGC_Pts, bMaskDWord, 0xf9000000);
	PHY_SetBBReg(dm_odm->Adapter, rIQK_AGC_Pts, bMaskDWord, 0xf8000000);

	/*  delay x ms */
	/* PlatformStallExecution(IQK_DELAY_TIME_8723B*1000); */
	mdelay(IQK_DELAY_TIME_8723B);

	/* restore Ant Path */
	PHY_SetBBReg(dm_odm->Adapter, 0x948, bMaskDWord, Path_SEL_BB);
	/* GNT_BT = 1 */
	PHY_SetBBReg(dm_odm->Adapter, 0x764, bMaskDWord, 0x00001800);

	/* leave IQK mode */
	PHY_SetBBReg(dm_odm->Adapter, rFPGA0_IQK, bMaskH3Bytes, 0x000000);

	/*  Check failed */
	regEAC = PHY_QueryBBReg(dm_odm->Adapter, rRx_Power_After_IQK_A_2, bMaskDWord);
	regE94 = PHY_QueryBBReg(dm_odm->Adapter, rTx_Power_Before_IQK_A, bMaskDWord);
	regE9C = PHY_QueryBBReg(dm_odm->Adapter, rTx_Power_After_IQK_A, bMaskDWord);

	/* Allen 20131125 */
	tmp = (regE9C & 0x03FF0000)>>16;
	if ((tmp & 0x200) > 0)
		tmp = 0x400 - tmp;

	if (
		!(regEAC & BIT28) &&
		(((regE94 & 0x03FF0000)>>16) != 0x142) &&
		(((regE9C & 0x03FF0000)>>16) != 0x42) &&
		(((regE94 & 0x03FF0000)>>16) < 0x110) &&
		(((regE94 & 0x03FF0000)>>16) > 0xf0) &&
		(tmp < 0xf)
	)
		result |= 0x01;
	else				/* if Tx not OK, ignore Rx */
		return result;

	u4tmp = 0x80007C00 | (regE94&0x3FF0000) | ((regE9C&0x3FF0000) >> 16);
	PHY_SetBBReg(dm_odm->Adapter, rTx_IQK, bMaskDWord, u4tmp);

	/* modify RXIQK mode table */
	PHY_SetBBReg(dm_odm->Adapter, rFPGA0_IQK, bMaskH3Bytes, 0x000000);
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, RF_WE_LUT, 0x80000, 0x1);
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, RF_RCK_OS, bRFRegOffsetMask, 0x18000);
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, RF_TXPA_G1, bRFRegOffsetMask, 0x0001f);
	/* LAN2 on, PA off for Dcut */
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, RF_TXPA_G2, bRFRegOffsetMask, 0xf7d77);
/* 	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, RF_WE_LUT, 0x80000, 0x0); */

	/* PA, PAD setting */
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, 0xdf, bRFRegOffsetMask, 0xf80);
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, 0x55, bRFRegOffsetMask, 0x4021f);


	/* IQK setting */
	PHY_SetBBReg(dm_odm->Adapter, rRx_IQK, bMaskDWord, 0x01004800);

	/* path-A IQK setting */
	PHY_SetBBReg(dm_odm->Adapter, rTx_IQK_Tone_A, bMaskDWord, 0x38008c1c);
	PHY_SetBBReg(dm_odm->Adapter, rRx_IQK_Tone_A, bMaskDWord, 0x18008c1c);
	PHY_SetBBReg(dm_odm->Adapter, rTx_IQK_Tone_B, bMaskDWord, 0x38008c1c);
	PHY_SetBBReg(dm_odm->Adapter, rRx_IQK_Tone_B, bMaskDWord, 0x38008c1c);

	PHY_SetBBReg(dm_odm->Adapter, rTx_IQK_PI_A, bMaskDWord, 0x82110000);
/* 	PHY_SetBBReg(dm_odm->Adapter, rRx_IQK_PI_A, bMaskDWord, 0x281604c2); */
	PHY_SetBBReg(dm_odm->Adapter, rRx_IQK_PI_A, bMaskDWord, 0x2813001f);
	PHY_SetBBReg(dm_odm->Adapter, rTx_IQK_PI_B, bMaskDWord, 0x82110000);
	PHY_SetBBReg(dm_odm->Adapter, rRx_IQK_PI_B, bMaskDWord, 0x28110000);

	/* LO calibration setting */
	PHY_SetBBReg(dm_odm->Adapter, rIQK_AGC_Rsp, bMaskDWord, 0x0046a8d1);

	/* enter IQK mode */
	PHY_SetBBReg(dm_odm->Adapter, rFPGA0_IQK, bMaskH3Bytes, 0x808000);

	/* Ant switch */
	if (configPathB || (RF_Path == 0))
		/*  wifi switch to S1 */
		PHY_SetBBReg(dm_odm->Adapter, 0x948, bMaskDWord, 0x00000000);
	else
		/*  wifi switch to S0 */
		PHY_SetBBReg(dm_odm->Adapter, 0x948, bMaskDWord, 0x00000280);

	/* GNT_BT = 0 */
	PHY_SetBBReg(dm_odm->Adapter, 0x764, bMaskDWord, 0x00000800);

	/* One shot, path A LOK & IQK */
	PHY_SetBBReg(dm_odm->Adapter, rIQK_AGC_Pts, bMaskDWord, 0xf9000000);
	PHY_SetBBReg(dm_odm->Adapter, rIQK_AGC_Pts, bMaskDWord, 0xf8000000);

	/*  delay x ms */
	/* PlatformStallExecution(IQK_DELAY_TIME_8723B*1000); */
	mdelay(IQK_DELAY_TIME_8723B);

	/* restore Ant Path */
	PHY_SetBBReg(dm_odm->Adapter, 0x948, bMaskDWord, Path_SEL_BB);
	/* GNT_BT = 1 */
	PHY_SetBBReg(dm_odm->Adapter, 0x764, bMaskDWord, 0x00001800);

    /* leave IQK mode */
	PHY_SetBBReg(dm_odm->Adapter, rFPGA0_IQK, bMaskH3Bytes, 0x000000);

	/*  Check failed */
	regEAC = PHY_QueryBBReg(dm_odm->Adapter, rRx_Power_After_IQK_A_2, bMaskDWord);
	regEA4 = PHY_QueryBBReg(dm_odm->Adapter, rRx_Power_Before_IQK_A_2, bMaskDWord);

	/* 	PA/PAD controlled by 0x0 */
	/* leave IQK mode */
	PHY_SetBBReg(dm_odm->Adapter, rFPGA0_IQK, bMaskH3Bytes, 0x000000);
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, 0xdf, bRFRegOffsetMask, 0x780);

	/* Allen 20131125 */
	tmp = (regEAC & 0x03FF0000)>>16;
	if ((tmp & 0x200) > 0)
		tmp = 0x400 - tmp;

	if (
		!(regEAC & BIT27) && /* if Tx is OK, check whether Rx is OK */
		(((regEA4 & 0x03FF0000)>>16) != 0x132) &&
		(((regEAC & 0x03FF0000)>>16) != 0x36) &&
		(((regEA4 & 0x03FF0000)>>16) < 0x110) &&
		(((regEA4 & 0x03FF0000)>>16) > 0xf0) &&
		(tmp < 0xf)
	)
		result |= 0x02;

	return result;
}

/* bit0 = 1 => Tx OK, bit1 = 1 => Rx OK */
static u8 phy_PathB_IQK_8723B(struct adapter *adapter)
{
	u32 regEAC, regE94, regE9C, tmp, Path_SEL_BB/*, regEC4, regECC, Path_SEL_BB*/;
	u8 result = 0x00;
	struct hal_com_data	*hal_data = GET_HAL_DATA(adapter);
	struct dm_odm_t *dm_odm = &hal_data->odmpriv;

	/*  Save RF Path */
	Path_SEL_BB = PHY_QueryBBReg(dm_odm->Adapter, 0x948, bMaskDWord);

    /* leave IQK mode */
	PHY_SetBBReg(dm_odm->Adapter, rFPGA0_IQK, bMaskH3Bytes, 0x000000);

	/* 	in TXIQK mode */
/* 	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, RF_WE_LUT, 0x80000, 0x1); */
/* 	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, RF_RCK_OS, bRFRegOffsetMask, 0x20000); */
/* 	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, RF_TXPA_G1, bRFRegOffsetMask, 0x0003f); */
/* 	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, RF_TXPA_G2, bRFRegOffsetMask, 0xc7f87); */
	/* 	enable path B PA in TXIQK mode */
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, 0xed, 0x20, 0x1);
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, 0x43, bRFRegOffsetMask, 0x30fc1);



	/* 1 Tx IQK */
	/* IQK setting */
	PHY_SetBBReg(dm_odm->Adapter, rTx_IQK, bMaskDWord, 0x01007c00);
	PHY_SetBBReg(dm_odm->Adapter, rRx_IQK, bMaskDWord, 0x01004800);
	/* path-A IQK setting */
	PHY_SetBBReg(dm_odm->Adapter, rTx_IQK_Tone_A, bMaskDWord, 0x18008c1c);
	PHY_SetBBReg(dm_odm->Adapter, rRx_IQK_Tone_A, bMaskDWord, 0x38008c1c);
	PHY_SetBBReg(dm_odm->Adapter, rTx_IQK_Tone_B, bMaskDWord, 0x38008c1c);
	PHY_SetBBReg(dm_odm->Adapter, rRx_IQK_Tone_B, bMaskDWord, 0x38008c1c);

/* 	PHY_SetBBReg(dm_odm->Adapter, rTx_IQK_PI_A, bMaskDWord, 0x82140114); */
	PHY_SetBBReg(dm_odm->Adapter, rTx_IQK_PI_A, bMaskDWord, 0x821303ea);
	PHY_SetBBReg(dm_odm->Adapter, rRx_IQK_PI_A, bMaskDWord, 0x28110000);
	PHY_SetBBReg(dm_odm->Adapter, rTx_IQK_PI_B, bMaskDWord, 0x82110000);
	PHY_SetBBReg(dm_odm->Adapter, rRx_IQK_PI_B, bMaskDWord, 0x28110000);

	/* LO calibration setting */
	PHY_SetBBReg(dm_odm->Adapter, rIQK_AGC_Rsp, bMaskDWord, 0x00462911);

	/* enter IQK mode */
	PHY_SetBBReg(dm_odm->Adapter, rFPGA0_IQK, bMaskH3Bytes, 0x808000);

	/* switch to path B */
	PHY_SetBBReg(dm_odm->Adapter, 0x948, bMaskDWord, 0x00000280);
/* 	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, 0xb0, bRFRegOffsetMask, 0xeffe0); */

	/* GNT_BT = 0 */
	PHY_SetBBReg(dm_odm->Adapter, 0x764, bMaskDWord, 0x00000800);

	/* One shot, path B LOK & IQK */
	PHY_SetBBReg(dm_odm->Adapter, rIQK_AGC_Pts, bMaskDWord, 0xf9000000);
	PHY_SetBBReg(dm_odm->Adapter, rIQK_AGC_Pts, bMaskDWord, 0xf8000000);

	/*  delay x ms */
	/* PlatformStallExecution(IQK_DELAY_TIME_88E*1000); */
	mdelay(IQK_DELAY_TIME_8723B);

	/* restore Ant Path */
	PHY_SetBBReg(dm_odm->Adapter, 0x948, bMaskDWord, Path_SEL_BB);
	/* GNT_BT = 1 */
	PHY_SetBBReg(dm_odm->Adapter, 0x764, bMaskDWord, 0x00001800);

    /* leave IQK mode */
	PHY_SetBBReg(dm_odm->Adapter, rFPGA0_IQK, bMaskH3Bytes, 0x000000);

	/*  Check failed */
	regEAC = PHY_QueryBBReg(dm_odm->Adapter, rRx_Power_After_IQK_A_2, bMaskDWord);
	regE94 = PHY_QueryBBReg(dm_odm->Adapter, rTx_Power_Before_IQK_A, bMaskDWord);
	regE9C = PHY_QueryBBReg(dm_odm->Adapter, rTx_Power_After_IQK_A, bMaskDWord);

	/* Allen 20131125 */
	tmp = (regE9C & 0x03FF0000)>>16;
	if ((tmp & 0x200) > 0)
		tmp = 0x400 - tmp;

	if (
		!(regEAC & BIT28) &&
		(((regE94 & 0x03FF0000)>>16) != 0x142) &&
		(((regE9C & 0x03FF0000)>>16) != 0x42) &&
		(((regE94 & 0x03FF0000)>>16) < 0x110) &&
		(((regE94 & 0x03FF0000)>>16) > 0xf0) &&
		(tmp < 0xf)
	)
		result |= 0x01;

	return result;
}

/* bit0 = 1 => Tx OK, bit1 = 1 => Rx OK */
static u8 phy_PathB_RxIQK8723B(struct adapter *adapter, bool configPathB)
{
	u32 regE94, regE9C, regEA4, regEAC, u4tmp, tmp, Path_SEL_BB;
	u8 result = 0x00;
	struct hal_com_data *hal_data = GET_HAL_DATA(adapter);
	struct dm_odm_t *dm_odm = &hal_data->odmpriv;

	/*  Save RF Path */
	Path_SEL_BB = PHY_QueryBBReg(dm_odm->Adapter, 0x948, bMaskDWord);
    /* leave IQK mode */
	PHY_SetBBReg(dm_odm->Adapter, rFPGA0_IQK, bMaskH3Bytes, 0x000000);

	/* switch to path B */
	PHY_SetBBReg(dm_odm->Adapter, 0x948, bMaskDWord, 0x00000280);
	/* modify RXIQK mode table */
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, RF_WE_LUT, 0x80000, 0x1);
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, RF_RCK_OS, bRFRegOffsetMask, 0x18000);
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, RF_TXPA_G1, bRFRegOffsetMask, 0x0001f);
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, RF_TXPA_G2, bRFRegOffsetMask, 0xf7fb7);
	/* open PA S1 & SMIXER */
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, 0xed, 0x20, 0x1);
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, 0x43, bRFRegOffsetMask, 0x30fcd);


	/* IQK setting */
	PHY_SetBBReg(dm_odm->Adapter, rTx_IQK, bMaskDWord, 0x01007c00);
	PHY_SetBBReg(dm_odm->Adapter, rRx_IQK, bMaskDWord, 0x01004800);


	/* path-B IQK setting */
	PHY_SetBBReg(dm_odm->Adapter, rTx_IQK_Tone_A, bMaskDWord, 0x18008c1c);
	PHY_SetBBReg(dm_odm->Adapter, rRx_IQK_Tone_A, bMaskDWord, 0x38008c1c);
	PHY_SetBBReg(dm_odm->Adapter, rTx_IQK_Tone_B, bMaskDWord, 0x38008c1c);
	PHY_SetBBReg(dm_odm->Adapter, rRx_IQK_Tone_B, bMaskDWord, 0x38008c1c);

/* 	PHY_SetBBReg(dm_odm->Adapter, rTx_IQK_PI_A, bMaskDWord, 0x82160c1f); */
	PHY_SetBBReg(dm_odm->Adapter, rTx_IQK_PI_A, bMaskDWord, 0x82130ff0);
	PHY_SetBBReg(dm_odm->Adapter, rRx_IQK_PI_A, bMaskDWord, 0x28110000);
	PHY_SetBBReg(dm_odm->Adapter, rTx_IQK_PI_B, bMaskDWord, 0x82110000);
	PHY_SetBBReg(dm_odm->Adapter, rRx_IQK_PI_B, bMaskDWord, 0x28110000);

	/* LO calibration setting */
	PHY_SetBBReg(dm_odm->Adapter, rIQK_AGC_Rsp, bMaskDWord, 0x0046a911);

    /* enter IQK mode */
	PHY_SetBBReg(dm_odm->Adapter, rFPGA0_IQK, bMaskH3Bytes, 0x808000);

	/* switch to path B */
	PHY_SetBBReg(dm_odm->Adapter, 0x948, bMaskDWord, 0x00000280);
/* 	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, 0xb0, bRFRegOffsetMask, 0xeffe0); */

	/* GNT_BT = 0 */
	PHY_SetBBReg(dm_odm->Adapter, 0x764, bMaskDWord, 0x00000800);

	/* One shot, path B TXIQK @ RXIQK */
	PHY_SetBBReg(dm_odm->Adapter, rIQK_AGC_Pts, bMaskDWord, 0xf9000000);
	PHY_SetBBReg(dm_odm->Adapter, rIQK_AGC_Pts, bMaskDWord, 0xf8000000);


	/*  delay x ms */
	/* PlatformStallExecution(IQK_DELAY_TIME_88E*1000); */
	mdelay(IQK_DELAY_TIME_8723B);

	/* restore Ant Path */
	PHY_SetBBReg(dm_odm->Adapter, 0x948, bMaskDWord, Path_SEL_BB);
	/* GNT_BT = 1 */
	PHY_SetBBReg(dm_odm->Adapter, 0x764, bMaskDWord, 0x00001800);

    /* leave IQK mode */
	PHY_SetBBReg(dm_odm->Adapter, rFPGA0_IQK, bMaskH3Bytes, 0x000000);

	/*  Check failed */
	regEAC = PHY_QueryBBReg(dm_odm->Adapter, rRx_Power_After_IQK_A_2, bMaskDWord);
	regE94 = PHY_QueryBBReg(dm_odm->Adapter, rTx_Power_Before_IQK_A, bMaskDWord);
	regE9C = PHY_QueryBBReg(dm_odm->Adapter, rTx_Power_After_IQK_A, bMaskDWord);

	/* Allen 20131125 */
	tmp = (regE9C & 0x03FF0000)>>16;
	if ((tmp & 0x200) > 0)
		tmp = 0x400 - tmp;

	if (
		!(regEAC & BIT28) &&
		(((regE94 & 0x03FF0000)>>16) != 0x142) &&
		(((regE9C & 0x03FF0000)>>16) != 0x42)  &&
		(((regE94 & 0x03FF0000)>>16) < 0x110) &&
		(((regE94 & 0x03FF0000)>>16) > 0xf0) &&
		(tmp < 0xf)
	)
			result |= 0x01;
	else	/* if Tx not OK, ignore Rx */
		return result;

	u4tmp = 0x80007C00 | (regE94&0x3FF0000)  | ((regE9C&0x3FF0000) >> 16);
	PHY_SetBBReg(dm_odm->Adapter, rTx_IQK, bMaskDWord, u4tmp);

	/* modify RXIQK mode table */
	/* 20121009, Kordan> RF Mode = 3 */
	PHY_SetBBReg(dm_odm->Adapter, rFPGA0_IQK, bMaskH3Bytes, 0x000000);
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, RF_WE_LUT, 0x80000, 0x1);
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, RF_RCK_OS, bRFRegOffsetMask, 0x18000);
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, RF_TXPA_G1, bRFRegOffsetMask, 0x0001f);
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, RF_TXPA_G2, bRFRegOffsetMask, 0xf7d77);
/* 	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, RF_WE_LUT, 0x80000, 0x0); */

	/* open PA S1 & close SMIXER */
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, 0xed, 0x20, 0x1);
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, 0x43, bRFRegOffsetMask, 0x30ebd);

	/* PA, PAD setting */
/* 	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, 0xdf, bRFRegOffsetMask, 0xf80); */
/* 	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, 0x56, bRFRegOffsetMask, 0x51000); */

	/* IQK setting */
	PHY_SetBBReg(dm_odm->Adapter, rRx_IQK, bMaskDWord, 0x01004800);

	/* path-B IQK setting */
	PHY_SetBBReg(dm_odm->Adapter, rTx_IQK_Tone_A, bMaskDWord, 0x38008c1c);
	PHY_SetBBReg(dm_odm->Adapter, rRx_IQK_Tone_A, bMaskDWord, 0x18008c1c);
	PHY_SetBBReg(dm_odm->Adapter, rTx_IQK_Tone_B, bMaskDWord, 0x38008c1c);
	PHY_SetBBReg(dm_odm->Adapter, rRx_IQK_Tone_B, bMaskDWord, 0x38008c1c);

	PHY_SetBBReg(dm_odm->Adapter, rTx_IQK_PI_A, bMaskDWord, 0x82110000);
/* 	PHY_SetBBReg(dm_odm->Adapter, rRx_IQK_PI_A, bMaskDWord, 0x281604c2); */
	PHY_SetBBReg(dm_odm->Adapter, rRx_IQK_PI_A, bMaskDWord, 0x2813001f);
	PHY_SetBBReg(dm_odm->Adapter, rTx_IQK_PI_B, bMaskDWord, 0x82110000);
	PHY_SetBBReg(dm_odm->Adapter, rRx_IQK_PI_B, bMaskDWord, 0x28110000);

	/* LO calibration setting */
	PHY_SetBBReg(dm_odm->Adapter, rIQK_AGC_Rsp, bMaskDWord, 0x0046a8d1);

    /* enter IQK mode */
	PHY_SetBBReg(dm_odm->Adapter, rFPGA0_IQK, bMaskH3Bytes, 0x808000);

	/* switch to path B */
	PHY_SetBBReg(dm_odm->Adapter, 0x948, bMaskDWord, 0x00000280);
/* 	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, 0xb0, bRFRegOffsetMask, 0xeffe0); */

	/* GNT_BT = 0 */
	PHY_SetBBReg(dm_odm->Adapter, 0x764, bMaskDWord, 0x00000800);

	/* One shot, path B LOK & IQK */
	PHY_SetBBReg(dm_odm->Adapter, rIQK_AGC_Pts, bMaskDWord, 0xf9000000);
	PHY_SetBBReg(dm_odm->Adapter, rIQK_AGC_Pts, bMaskDWord, 0xf8000000);

	/*  delay x ms */
	/* PlatformStallExecution(IQK_DELAY_TIME_88E*1000); */
	mdelay(IQK_DELAY_TIME_8723B);

	/* restore Ant Path */
	PHY_SetBBReg(dm_odm->Adapter, 0x948, bMaskDWord, Path_SEL_BB);
	/* GNT_BT = 1 */
	PHY_SetBBReg(dm_odm->Adapter, 0x764, bMaskDWord, 0x00001800);

    /* leave IQK mode */
	PHY_SetBBReg(dm_odm->Adapter, rFPGA0_IQK, bMaskH3Bytes, 0x000000);

	/*  Check failed */
	regEAC = PHY_QueryBBReg(dm_odm->Adapter, rRx_Power_After_IQK_A_2, bMaskDWord);
	regEA4 = PHY_QueryBBReg(dm_odm->Adapter, rRx_Power_Before_IQK_A_2, bMaskDWord);

	/* 	PA/PAD controlled by 0x0 */
	/* leave IQK mode */
/* 	PHY_SetBBReg(dm_odm->Adapter, rFPGA0_IQK, 0xffffff00, 0x00000000); */
/* 	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_B, 0xdf, bRFRegOffsetMask, 0x180); */



	/* Allen 20131125 */
	tmp = (regEAC & 0x03FF0000)>>16;
	if ((tmp & 0x200) > 0)
		tmp = 0x400 - tmp;

	if (
		!(regEAC & BIT27) && /* if Tx is OK, check whether Rx is OK */
		(((regEA4 & 0x03FF0000)>>16) != 0x132) &&
		(((regEAC & 0x03FF0000)>>16) != 0x36) &&
		(((regEA4 & 0x03FF0000)>>16) < 0x110) &&
		(((regEA4 & 0x03FF0000)>>16) > 0xf0) &&
		(tmp < 0xf)
	)
		result |= 0x02;

	return result;
}

static void _PHY_PathAFillIQKMatrix8723B(
	struct adapter *adapter,
	bool iqkok,
	s32 result[][8],
	u8 final_candidate,
	bool tx_only
)
{
	u32 Oldval_0, X, TX0_A, reg;
	s32 Y, TX0_C;
	struct hal_com_data	*hal_data = GET_HAL_DATA(adapter);
	struct dm_odm_t *dm_odm = &hal_data->odmpriv;

	struct odm_rf_cal_t *rf_calibrate_info = &dm_odm->RFCalibrateInfo;

	if (final_candidate == 0xFF)
		return;

	else if (iqkok) {
		Oldval_0 = (PHY_QueryBBReg(dm_odm->Adapter, rOFDM0_XATxIQImbalance, bMaskDWord) >> 22) & 0x3FF;

		X = result[final_candidate][0];
		if ((X & 0x00000200) != 0)
			X = X | 0xFFFFFC00;
		TX0_A = (X * Oldval_0) >> 8;
		PHY_SetBBReg(dm_odm->Adapter, rOFDM0_XATxIQImbalance, 0x3FF, TX0_A);

		PHY_SetBBReg(dm_odm->Adapter, rOFDM0_ECCAThreshold, BIT(31), ((X*Oldval_0>>7) & 0x1));

		Y = result[final_candidate][1];
		if ((Y & 0x00000200) != 0)
			Y = Y | 0xFFFFFC00;

		/* 2 Tx IQC */
		TX0_C = (Y * Oldval_0) >> 8;
		PHY_SetBBReg(dm_odm->Adapter, rOFDM0_XCTxAFE, 0xF0000000, ((TX0_C&0x3C0)>>6));
		rf_calibrate_info->TxIQC_8723B[PATH_S1][IDX_0xC94][KEY] = rOFDM0_XCTxAFE;
		rf_calibrate_info->TxIQC_8723B[PATH_S1][IDX_0xC94][VAL] = PHY_QueryBBReg(dm_odm->Adapter, rOFDM0_XCTxAFE, bMaskDWord);

		PHY_SetBBReg(dm_odm->Adapter, rOFDM0_XATxIQImbalance, 0x003F0000, (TX0_C&0x3F));
		rf_calibrate_info->TxIQC_8723B[PATH_S1][IDX_0xC80][KEY] = rOFDM0_XATxIQImbalance;
		rf_calibrate_info->TxIQC_8723B[PATH_S1][IDX_0xC80][VAL] = PHY_QueryBBReg(dm_odm->Adapter, rOFDM0_XATxIQImbalance, bMaskDWord);

		PHY_SetBBReg(dm_odm->Adapter, rOFDM0_ECCAThreshold, BIT(29), ((Y*Oldval_0>>7) & 0x1));
		rf_calibrate_info->TxIQC_8723B[PATH_S1][IDX_0xC4C][KEY] = rOFDM0_ECCAThreshold;
		rf_calibrate_info->TxIQC_8723B[PATH_S1][IDX_0xC4C][VAL] = PHY_QueryBBReg(dm_odm->Adapter, rOFDM0_ECCAThreshold, bMaskDWord);

		if (tx_only) {
			/*  <20130226, Kordan> Saving RxIQC, otherwise not initialized. */
			rf_calibrate_info->RxIQC_8723B[PATH_S1][IDX_0xCA0][KEY] = rOFDM0_RxIQExtAnta;
			rf_calibrate_info->RxIQC_8723B[PATH_S1][IDX_0xCA0][VAL] = 0xfffffff & PHY_QueryBBReg(dm_odm->Adapter, rOFDM0_RxIQExtAnta, bMaskDWord);
			rf_calibrate_info->RxIQC_8723B[PATH_S1][IDX_0xC14][KEY] = rOFDM0_XARxIQImbalance;
/* 			rf_calibrate_info->RxIQC_8723B[PATH_S1][IDX_0xC14][VAL] = PHY_QueryBBReg(dm_odm->Adapter, rOFDM0_XARxIQImbalance, bMaskDWord); */
			rf_calibrate_info->RxIQC_8723B[PATH_S1][IDX_0xC14][VAL] = 0x40000100;
			return;
		}

		reg = result[final_candidate][2];

		/* 2 Rx IQC */
		PHY_SetBBReg(dm_odm->Adapter, rOFDM0_XARxIQImbalance, 0x3FF, reg);
		reg = result[final_candidate][3] & 0x3F;
		PHY_SetBBReg(dm_odm->Adapter, rOFDM0_XARxIQImbalance, 0xFC00, reg);
		rf_calibrate_info->RxIQC_8723B[PATH_S1][IDX_0xC14][KEY] = rOFDM0_XARxIQImbalance;
		rf_calibrate_info->RxIQC_8723B[PATH_S1][IDX_0xC14][VAL] = PHY_QueryBBReg(dm_odm->Adapter, rOFDM0_XARxIQImbalance, bMaskDWord);

		reg = (result[final_candidate][3] >> 6) & 0xF;
		PHY_SetBBReg(dm_odm->Adapter, rOFDM0_RxIQExtAnta, 0xF0000000, reg);
		rf_calibrate_info->RxIQC_8723B[PATH_S1][IDX_0xCA0][KEY] = rOFDM0_RxIQExtAnta;
		rf_calibrate_info->RxIQC_8723B[PATH_S1][IDX_0xCA0][VAL] = PHY_QueryBBReg(dm_odm->Adapter, rOFDM0_RxIQExtAnta, bMaskDWord);

	}
}

static void _PHY_PathBFillIQKMatrix8723B(
	struct adapter *adapter,
	bool iqkok,
	s32 result[][8],
	u8 final_candidate,
	bool tx_only /* do Tx only */
)
{
	u32 Oldval_1, X, TX1_A, reg;
	s32	Y, TX1_C;
	struct hal_com_data	*hal_data = GET_HAL_DATA(adapter);
	struct dm_odm_t *dm_odm = &hal_data->odmpriv;

	struct odm_rf_cal_t *rf_calibrate_info = &dm_odm->RFCalibrateInfo;

	if (final_candidate == 0xFF)
		return;

	else if (iqkok) {
		Oldval_1 = (PHY_QueryBBReg(dm_odm->Adapter, rOFDM0_XBTxIQImbalance, bMaskDWord) >> 22) & 0x3FF;

		X = result[final_candidate][4];
		if ((X & 0x00000200) != 0)
			X = X | 0xFFFFFC00;
		TX1_A = (X * Oldval_1) >> 8;

		PHY_SetBBReg(dm_odm->Adapter, rOFDM0_XBTxIQImbalance, 0x3FF, TX1_A);

		PHY_SetBBReg(dm_odm->Adapter, rOFDM0_ECCAThreshold, BIT(27), ((X*Oldval_1>>7) & 0x1));

		Y = result[final_candidate][5];
		if ((Y & 0x00000200) != 0)
			Y = Y | 0xFFFFFC00;

		TX1_C = (Y * Oldval_1) >> 8;

		/* 2 Tx IQC */
		PHY_SetBBReg(dm_odm->Adapter, rOFDM0_XDTxAFE, 0xF0000000, ((TX1_C&0x3C0)>>6));
/* 		rf_calibrate_info->TxIQC_8723B[PATH_S0][IDX_0xC9C][KEY] = rOFDM0_XDTxAFE; */
/* 		rf_calibrate_info->TxIQC_8723B[PATH_S0][IDX_0xC9C][VAL] = PHY_QueryBBReg(dm_odm->Adapter, rOFDM0_XDTxAFE, bMaskDWord); */
		rf_calibrate_info->TxIQC_8723B[PATH_S0][IDX_0xC94][KEY] = rOFDM0_XCTxAFE;
		rf_calibrate_info->TxIQC_8723B[PATH_S0][IDX_0xC94][VAL] = PHY_QueryBBReg(dm_odm->Adapter, rOFDM0_XDTxAFE, bMaskDWord);

		PHY_SetBBReg(dm_odm->Adapter, rOFDM0_XBTxIQImbalance, 0x003F0000, (TX1_C&0x3F));
		rf_calibrate_info->TxIQC_8723B[PATH_S0][IDX_0xC80][KEY] = rOFDM0_XATxIQImbalance;
		rf_calibrate_info->TxIQC_8723B[PATH_S0][IDX_0xC80][VAL] = PHY_QueryBBReg(dm_odm->Adapter, rOFDM0_XBTxIQImbalance, bMaskDWord);

		PHY_SetBBReg(dm_odm->Adapter, rOFDM0_ECCAThreshold, BIT(25), ((Y*Oldval_1>>7) & 0x1));
		rf_calibrate_info->TxIQC_8723B[PATH_S0][IDX_0xC4C][KEY] = rOFDM0_ECCAThreshold;
		rf_calibrate_info->TxIQC_8723B[PATH_S0][IDX_0xC4C][VAL] = PHY_QueryBBReg(dm_odm->Adapter, rOFDM0_ECCAThreshold, bMaskDWord);

		if (tx_only) {
			rf_calibrate_info->RxIQC_8723B[PATH_S0][IDX_0xC14][KEY] = rOFDM0_XARxIQImbalance;
/* 			rf_calibrate_info->RxIQC_8723B[PATH_S0][IDX_0xC14][VAL] = PHY_QueryBBReg(dm_odm->Adapter, rOFDM0_XARxIQImbalance, bMaskDWord); */
			rf_calibrate_info->RxIQC_8723B[PATH_S0][IDX_0xC14][VAL] = 0x40000100;
			rf_calibrate_info->RxIQC_8723B[PATH_S0][IDX_0xCA0][KEY] = rOFDM0_RxIQExtAnta;
			rf_calibrate_info->RxIQC_8723B[PATH_S0][IDX_0xCA0][VAL] = 0x0fffffff & PHY_QueryBBReg(dm_odm->Adapter, rOFDM0_RxIQExtAnta, bMaskDWord);
			return;
		}

		/* 2 Rx IQC */
		reg = result[final_candidate][6];
		PHY_SetBBReg(dm_odm->Adapter, rOFDM0_XBRxIQImbalance, 0x3FF, reg);
		reg = result[final_candidate][7] & 0x3F;
		PHY_SetBBReg(dm_odm->Adapter, rOFDM0_XBRxIQImbalance, 0xFC00, reg);
		rf_calibrate_info->RxIQC_8723B[PATH_S0][IDX_0xC14][KEY] = rOFDM0_XARxIQImbalance;
		rf_calibrate_info->RxIQC_8723B[PATH_S0][IDX_0xC14][VAL] = PHY_QueryBBReg(dm_odm->Adapter, rOFDM0_XBRxIQImbalance, bMaskDWord);

		reg = (result[final_candidate][7] >> 6) & 0xF;
/* 		PHY_SetBBReg(dm_odm->Adapter, rOFDM0_AGCRSSITable, 0x0000F000, reg); */
		rf_calibrate_info->RxIQC_8723B[PATH_S0][IDX_0xCA0][KEY] = rOFDM0_RxIQExtAnta;
		rf_calibrate_info->RxIQC_8723B[PATH_S0][IDX_0xCA0][VAL] = (reg << 28)|(PHY_QueryBBReg(dm_odm->Adapter, rOFDM0_RxIQExtAnta, bMaskDWord)&0x0fffffff);
	}
}

/*  */
/*  2011/07/26 MH Add an API for testing IQK fail case. */
/*  */
/*  MP Already declare in odm.c */

void ODM_SetIQCbyRFpath(struct dm_odm_t *dm_odm, u32 RFpath)
{

	struct odm_rf_cal_t *rf_calibrate_info = &dm_odm->RFCalibrateInfo;

	if (
		(rf_calibrate_info->TxIQC_8723B[PATH_S0][IDX_0xC80][VAL] != 0x0) &&
		(rf_calibrate_info->RxIQC_8723B[PATH_S0][IDX_0xC14][VAL] != 0x0) &&
		(rf_calibrate_info->TxIQC_8723B[PATH_S1][IDX_0xC80][VAL] != 0x0) &&
		(rf_calibrate_info->RxIQC_8723B[PATH_S1][IDX_0xC14][VAL] != 0x0)
	) {
		if (RFpath) { /* S1: RFpath = 0, S0:RFpath = 1 */
			/* S0 TX IQC */
			PHY_SetBBReg(dm_odm->Adapter, rf_calibrate_info->TxIQC_8723B[PATH_S0][IDX_0xC94][KEY], bMaskDWord, rf_calibrate_info->TxIQC_8723B[PATH_S0][IDX_0xC94][VAL]);
			PHY_SetBBReg(dm_odm->Adapter, rf_calibrate_info->TxIQC_8723B[PATH_S0][IDX_0xC80][KEY], bMaskDWord, rf_calibrate_info->TxIQC_8723B[PATH_S0][IDX_0xC80][VAL]);
			PHY_SetBBReg(dm_odm->Adapter, rf_calibrate_info->TxIQC_8723B[PATH_S0][IDX_0xC4C][KEY], bMaskDWord, rf_calibrate_info->TxIQC_8723B[PATH_S0][IDX_0xC4C][VAL]);
			/* S0 RX IQC */
			PHY_SetBBReg(dm_odm->Adapter, rf_calibrate_info->RxIQC_8723B[PATH_S0][IDX_0xC14][KEY], bMaskDWord, rf_calibrate_info->RxIQC_8723B[PATH_S0][IDX_0xC14][VAL]);
			PHY_SetBBReg(dm_odm->Adapter, rf_calibrate_info->RxIQC_8723B[PATH_S0][IDX_0xCA0][KEY], bMaskDWord, rf_calibrate_info->RxIQC_8723B[PATH_S0][IDX_0xCA0][VAL]);
		} else {
			/* S1 TX IQC */
			PHY_SetBBReg(dm_odm->Adapter, rf_calibrate_info->TxIQC_8723B[PATH_S1][IDX_0xC94][KEY], bMaskDWord, rf_calibrate_info->TxIQC_8723B[PATH_S1][IDX_0xC94][VAL]);
			PHY_SetBBReg(dm_odm->Adapter, rf_calibrate_info->TxIQC_8723B[PATH_S1][IDX_0xC80][KEY], bMaskDWord, rf_calibrate_info->TxIQC_8723B[PATH_S1][IDX_0xC80][VAL]);
			PHY_SetBBReg(dm_odm->Adapter, rf_calibrate_info->TxIQC_8723B[PATH_S1][IDX_0xC4C][KEY], bMaskDWord, rf_calibrate_info->TxIQC_8723B[PATH_S1][IDX_0xC4C][VAL]);
			/* S1 RX IQC */
			PHY_SetBBReg(dm_odm->Adapter, rf_calibrate_info->RxIQC_8723B[PATH_S1][IDX_0xC14][KEY], bMaskDWord, rf_calibrate_info->RxIQC_8723B[PATH_S1][IDX_0xC14][VAL]);
			PHY_SetBBReg(dm_odm->Adapter, rf_calibrate_info->RxIQC_8723B[PATH_S1][IDX_0xCA0][KEY], bMaskDWord, rf_calibrate_info->RxIQC_8723B[PATH_S1][IDX_0xCA0][VAL]);
		}
	}
}

static bool ODM_CheckPowerStatus(struct adapter *Adapter)
{
	return true;
}

static void _PHY_SaveADDARegisters8723B(
	struct adapter *adapter,
	u32 *ADDAReg,
	u32 *ADDABackup,
	u32 RegisterNum
)
{
	u32 i;
	struct hal_com_data *hal_data = GET_HAL_DATA(adapter);
	struct dm_odm_t *dm_odm = &hal_data->odmpriv;

	if (!ODM_CheckPowerStatus(adapter))
		return;

	for (i = 0 ; i < RegisterNum ; i++) {
		ADDABackup[i] = PHY_QueryBBReg(dm_odm->Adapter, ADDAReg[i], bMaskDWord);
	}
}


static void _PHY_SaveMACRegisters8723B(
	struct adapter *adapter, u32 *MACReg, u32 *MACBackup
)
{
	u32 i;
	struct hal_com_data	*hal_data = GET_HAL_DATA(adapter);
	struct dm_odm_t *dm_odm = &hal_data->odmpriv;

	for (i = 0 ; i < (IQK_MAC_REG_NUM - 1); i++) {
		MACBackup[i] = rtw_read8(dm_odm->Adapter, MACReg[i]);
	}
	MACBackup[i] = rtw_read32(dm_odm->Adapter, MACReg[i]);

}


static void _PHY_ReloadADDARegisters8723B(
	struct adapter *adapter,
	u32 *ADDAReg,
	u32 *ADDABackup,
	u32 RegiesterNum
)
{
	u32 i;
	struct hal_com_data	*hal_data = GET_HAL_DATA(adapter);
	struct dm_odm_t *dm_odm = &hal_data->odmpriv;

	for (i = 0 ; i < RegiesterNum; i++) {
		PHY_SetBBReg(dm_odm->Adapter, ADDAReg[i], bMaskDWord, ADDABackup[i]);
	}
}

static void _PHY_ReloadMACRegisters8723B(
	struct adapter *adapter, u32 *MACReg, u32 *MACBackup
)
{
	u32 i;

	for (i = 0 ; i < (IQK_MAC_REG_NUM - 1); i++) {
		rtw_write8(adapter, MACReg[i], (u8)MACBackup[i]);
	}
	rtw_write32(adapter, MACReg[i], MACBackup[i]);
}


static void _PHY_PathADDAOn8723B(
	struct adapter *adapter,
	u32 *ADDAReg,
	bool is2T
)
{
	u32 pathOn;
	u32 i;
	struct hal_com_data *hal_data = GET_HAL_DATA(adapter);
	struct dm_odm_t *dm_odm = &hal_data->odmpriv;

	pathOn = 0x01c00014;
	if (!is2T) {
		pathOn = 0x01c00014;
		PHY_SetBBReg(dm_odm->Adapter, ADDAReg[0], bMaskDWord, 0x01c00014);
	} else {
		PHY_SetBBReg(dm_odm->Adapter, ADDAReg[0], bMaskDWord, pathOn);
	}

	for (i = 1 ; i < IQK_ADDA_REG_NUM ; i++) {
		PHY_SetBBReg(dm_odm->Adapter, ADDAReg[i], bMaskDWord, pathOn);
	}

}

static void _PHY_MACSettingCalibration8723B(
	struct adapter *adapter, u32 *MACReg, u32 *MACBackup
)
{
	u32 i = 0;
	struct hal_com_data	*hal_data = GET_HAL_DATA(adapter);
	struct dm_odm_t *dm_odm = &hal_data->odmpriv;

	rtw_write8(dm_odm->Adapter, MACReg[i], 0x3F);

	for (i = 1 ; i < (IQK_MAC_REG_NUM - 1); i++) {
		rtw_write8(dm_odm->Adapter, MACReg[i], (u8)(MACBackup[i]&(~BIT3)));
	}
	rtw_write8(dm_odm->Adapter, MACReg[i], (u8)(MACBackup[i]&(~BIT5)));

}

static bool phy_SimularityCompare_8723B(
	struct adapter *adapter,
	s32 result[][8],
	u8  c1,
	u8  c2
)
{
	u32 i, j, diff, SimularityBitMap, bound = 0;
	u8 final_candidate[2] = {0xFF, 0xFF}; /* for path A and path B */
	bool bResult = true;
	s32 tmp1 = 0, tmp2 = 0;

	bound = 8;
	SimularityBitMap = 0;

	for (i = 0; i < bound; i++) {

		if ((i == 1) || (i == 3) || (i == 5) || (i == 7)) {
			if ((result[c1][i] & 0x00000200) != 0)
				tmp1 = result[c1][i] | 0xFFFFFC00;
			else
				tmp1 = result[c1][i];

			if ((result[c2][i] & 0x00000200) != 0)
				tmp2 = result[c2][i] | 0xFFFFFC00;
			else
				tmp2 = result[c2][i];
		} else {
			tmp1 = result[c1][i];
			tmp2 = result[c2][i];
		}

		diff = (tmp1 > tmp2) ? (tmp1 - tmp2) : (tmp2 - tmp1);

		if (diff > MAX_TOLERANCE) {
			if ((i == 2 || i == 6) && !SimularityBitMap) {
				if (result[c1][i]+result[c1][i+1] == 0)
					final_candidate[(i/4)] = c2;
				else if (result[c2][i]+result[c2][i+1] == 0)
					final_candidate[(i/4)] = c1;
				else
					SimularityBitMap = SimularityBitMap|(1<<i);
			} else
				SimularityBitMap = SimularityBitMap|(1<<i);
		}
	}

	if (SimularityBitMap == 0) {
		for (i = 0; i < (bound/4); i++) {
			if (final_candidate[i] != 0xFF) {
				for (j = i*4; j < (i+1)*4-2; j++)
					result[3][j] = result[final_candidate[i]][j];
				bResult = false;
			}
		}
		return bResult;
	} else {

		if (!(SimularityBitMap & 0x03)) { /* path A TX OK */
			for (i = 0; i < 2; i++)
				result[3][i] = result[c1][i];
		}

		if (!(SimularityBitMap & 0x0c)) { /* path A RX OK */
			for (i = 2; i < 4; i++)
				result[3][i] = result[c1][i];
		}

		if (!(SimularityBitMap & 0x30)) { /* path B TX OK */
			for (i = 4; i < 6; i++)
				result[3][i] = result[c1][i];
		}

		if (!(SimularityBitMap & 0xc0)) { /* path B RX OK */
			for (i = 6; i < 8; i++)
				result[3][i] = result[c1][i];
		}
		return false;
	}
}



static void phy_IQCalibrate_8723B(
	struct adapter *adapter,
	s32 result[][8],
	u8 t,
	bool is2T,
	u8 RF_Path
)
{
	struct hal_com_data	*hal_data = GET_HAL_DATA(adapter);
	struct dm_odm_t *dm_odm = &hal_data->odmpriv;

	u32 i;
	u8 PathAOK, PathBOK;
	u8 tmp0xc50 = (u8)PHY_QueryBBReg(dm_odm->Adapter, 0xC50, bMaskByte0);
	u8 tmp0xc58 = (u8)PHY_QueryBBReg(dm_odm->Adapter, 0xC58, bMaskByte0);
	u32 ADDA_REG[IQK_ADDA_REG_NUM] = {
		rFPGA0_XCD_SwitchControl,
		rBlue_Tooth,
		rRx_Wait_CCA,
		rTx_CCK_RFON,
		rTx_CCK_BBON,
		rTx_OFDM_RFON,
		rTx_OFDM_BBON,
		rTx_To_Rx,
		rTx_To_Tx,
		rRx_CCK,
		rRx_OFDM,
		rRx_Wait_RIFS,
		rRx_TO_Rx,
		rStandby,
		rSleep,
		rPMPD_ANAEN
	};
	u32 IQK_MAC_REG[IQK_MAC_REG_NUM] = {
		REG_TXPAUSE,
		REG_BCN_CTRL,
		REG_BCN_CTRL_1,
		REG_GPIO_MUXCFG
	};

	/* since 92C & 92D have the different define in IQK_BB_REG */
	u32 IQK_BB_REG_92C[IQK_BB_REG_NUM] = {
		rOFDM0_TRxPathEnable,
		rOFDM0_TRMuxPar,
		rFPGA0_XCD_RFInterfaceSW,
		rConfig_AntA,
		rConfig_AntB,
		rFPGA0_XAB_RFInterfaceSW,
		rFPGA0_XA_RFInterfaceOE,
		rFPGA0_XB_RFInterfaceOE,
		rCCK0_AFESetting
	};
	const u32 retryCount = 2;

	/*  Note: IQ calibration must be performed after loading */
	/* 		PHY_REG.txt , and radio_a, radio_b.txt */

	/* u32 bbvalue; */

	if (t == 0) {

		/*  Save ADDA parameters, turn Path A ADDA on */
		_PHY_SaveADDARegisters8723B(adapter, ADDA_REG, dm_odm->RFCalibrateInfo.ADDA_backup, IQK_ADDA_REG_NUM);
		_PHY_SaveMACRegisters8723B(adapter, IQK_MAC_REG, dm_odm->RFCalibrateInfo.IQK_MAC_backup);
		_PHY_SaveADDARegisters8723B(adapter, IQK_BB_REG_92C, dm_odm->RFCalibrateInfo.IQK_BB_backup, IQK_BB_REG_NUM);
	}

	_PHY_PathADDAOn8723B(adapter, ADDA_REG, is2T);

/* no serial mode */

	/* save RF path for 8723B */
/* 	Path_SEL_BB = PHY_QueryBBReg(dm_odm->Adapter, 0x948, bMaskDWord); */
/* 	Path_SEL_RF = PHY_QueryRFReg(dm_odm->Adapter, RF_PATH_A, 0xb0, 0xfffff); */

	/* MAC settings */
	_PHY_MACSettingCalibration8723B(adapter, IQK_MAC_REG, dm_odm->RFCalibrateInfo.IQK_MAC_backup);

	/* BB setting */
	/* PHY_SetBBReg(dm_odm->Adapter, rFPGA0_RFMOD, BIT24, 0x00); */
	PHY_SetBBReg(dm_odm->Adapter, rCCK0_AFESetting, 0x0f000000, 0xf);
	PHY_SetBBReg(dm_odm->Adapter, rOFDM0_TRxPathEnable, bMaskDWord, 0x03a05600);
	PHY_SetBBReg(dm_odm->Adapter, rOFDM0_TRMuxPar, bMaskDWord, 0x000800e4);
	PHY_SetBBReg(dm_odm->Adapter, rFPGA0_XCD_RFInterfaceSW, bMaskDWord, 0x22204000);


/* 	PHY_SetBBReg(dm_odm->Adapter, rFPGA0_XAB_RFInterfaceSW, BIT10, 0x01); */
/* 	PHY_SetBBReg(dm_odm->Adapter, rFPGA0_XAB_RFInterfaceSW, BIT26, 0x01); */
/* 	PHY_SetBBReg(dm_odm->Adapter, rFPGA0_XA_RFInterfaceOE, BIT10, 0x00); */
/* 	PHY_SetBBReg(dm_odm->Adapter, rFPGA0_XB_RFInterfaceOE, BIT10, 0x00); */


/* RX IQ calibration setting for 8723B D cut large current issue when leaving IPS */

	PHY_SetBBReg(dm_odm->Adapter, rFPGA0_IQK, bMaskH3Bytes, 0x000000);
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, RF_WE_LUT, 0x80000, 0x1);
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, RF_RCK_OS, bRFRegOffsetMask, 0x30000);
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, RF_TXPA_G1, bRFRegOffsetMask, 0x0001f);
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, RF_TXPA_G2, bRFRegOffsetMask, 0xf7fb7);
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, 0xed, 0x20, 0x1);
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, 0x43, bRFRegOffsetMask, 0x60fbd);

/* path A TX IQK */
	for (i = 0 ; i < retryCount ; i++) {
		PathAOK = phy_PathA_IQK_8723B(adapter, is2T, RF_Path);
/* 		if (PathAOK == 0x03) { */
		if (PathAOK == 0x01) {
			/*  Path A Tx IQK Success */
			PHY_SetBBReg(dm_odm->Adapter, rFPGA0_IQK, bMaskH3Bytes, 0x000000);
			dm_odm->RFCalibrateInfo.TxLOK[RF_PATH_A] = PHY_QueryRFReg(dm_odm->Adapter, RF_PATH_A, 0x8, bRFRegOffsetMask);

				result[t][0] = (PHY_QueryBBReg(dm_odm->Adapter, rTx_Power_Before_IQK_A, bMaskDWord)&0x3FF0000)>>16;
				result[t][1] = (PHY_QueryBBReg(dm_odm->Adapter, rTx_Power_After_IQK_A, bMaskDWord)&0x3FF0000)>>16;
			break;
		}
	}

/* path A RXIQK */
	for (i = 0 ; i < retryCount ; i++) {
		PathAOK = phy_PathA_RxIQK8723B(adapter, is2T, RF_Path);
		if (PathAOK == 0x03) {
/* 				result[t][0] = (PHY_QueryBBReg(dm_odm->Adapter, rTx_Power_Before_IQK_A, bMaskDWord)&0x3FF0000)>>16; */
/* 				result[t][1] = (PHY_QueryBBReg(dm_odm->Adapter, rTx_Power_After_IQK_A, bMaskDWord)&0x3FF0000)>>16; */
				result[t][2] = (PHY_QueryBBReg(dm_odm->Adapter, rRx_Power_Before_IQK_A_2, bMaskDWord)&0x3FF0000)>>16;
				result[t][3] = (PHY_QueryBBReg(dm_odm->Adapter, rRx_Power_After_IQK_A_2, bMaskDWord)&0x3FF0000)>>16;
			break;
		}
	}

	if (0x00 == PathAOK) {
	}

/* path B IQK */
	if (is2T) {

		/* path B TX IQK */
		for (i = 0 ; i < retryCount ; i++) {
			PathBOK = phy_PathB_IQK_8723B(adapter);
			if (PathBOK == 0x01) {
				/*  Path B Tx IQK Success */
				PHY_SetBBReg(dm_odm->Adapter, rFPGA0_IQK, bMaskH3Bytes, 0x000000);
				dm_odm->RFCalibrateInfo.TxLOK[RF_PATH_B] = PHY_QueryRFReg(dm_odm->Adapter, RF_PATH_B, 0x8, bRFRegOffsetMask);

				result[t][4] = (PHY_QueryBBReg(dm_odm->Adapter, rTx_Power_Before_IQK_A, bMaskDWord)&0x3FF0000)>>16;
				result[t][5] = (PHY_QueryBBReg(dm_odm->Adapter, rTx_Power_After_IQK_A, bMaskDWord)&0x3FF0000)>>16;
				break;
			}
		}

/* path B RX IQK */
		for (i = 0 ; i < retryCount ; i++) {
			PathBOK = phy_PathB_RxIQK8723B(adapter, is2T);
			if (PathBOK == 0x03) {
/* 				result[t][0] = (PHY_QueryBBReg(dm_odm->Adapter, rTx_Power_Before_IQK_A, bMaskDWord)&0x3FF0000)>>16; */
/* 				result[t][1] = (PHY_QueryBBReg(dm_odm->Adapter, rTx_Power_After_IQK_A, bMaskDWord)&0x3FF0000)>>16; */
				result[t][6] = (PHY_QueryBBReg(dm_odm->Adapter, rRx_Power_Before_IQK_A_2, bMaskDWord)&0x3FF0000)>>16;
				result[t][7] = (PHY_QueryBBReg(dm_odm->Adapter, rRx_Power_After_IQK_A_2, bMaskDWord)&0x3FF0000)>>16;
				break;
			}
		}

/* Allen end */
	}

	/* Back to BB mode, load original value */
	PHY_SetBBReg(dm_odm->Adapter, rFPGA0_IQK, bMaskH3Bytes, 0);

	if (t != 0) {
		/*  Reload ADDA power saving parameters */
		_PHY_ReloadADDARegisters8723B(adapter, ADDA_REG, dm_odm->RFCalibrateInfo.ADDA_backup, IQK_ADDA_REG_NUM);

		/*  Reload MAC parameters */
		_PHY_ReloadMACRegisters8723B(adapter, IQK_MAC_REG, dm_odm->RFCalibrateInfo.IQK_MAC_backup);

		_PHY_ReloadADDARegisters8723B(adapter, IQK_BB_REG_92C, dm_odm->RFCalibrateInfo.IQK_BB_backup, IQK_BB_REG_NUM);

		/* Reload RF path */
/* 		PHY_SetBBReg(dm_odm->Adapter, 0x948, bMaskDWord, Path_SEL_BB); */
/* 		PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, 0xb0, 0xfffff, Path_SEL_RF); */

		/* Allen initial gain 0xc50 */
		/*  Restore RX initial gain */
		PHY_SetBBReg(dm_odm->Adapter, 0xc50, bMaskByte0, 0x50);
		PHY_SetBBReg(dm_odm->Adapter, 0xc50, bMaskByte0, tmp0xc50);
		if (is2T) {
			PHY_SetBBReg(dm_odm->Adapter, 0xc58, bMaskByte0, 0x50);
			PHY_SetBBReg(dm_odm->Adapter, 0xc58, bMaskByte0, tmp0xc58);
		}

		/* load 0xe30 IQC default value */
		PHY_SetBBReg(dm_odm->Adapter, rTx_IQK_Tone_A, bMaskDWord, 0x01008c00);
		PHY_SetBBReg(dm_odm->Adapter, rRx_IQK_Tone_A, bMaskDWord, 0x01008c00);

	}

}


static void phy_LCCalibrate_8723B(struct dm_odm_t *dm_odm, bool is2T)
{
	u8 tmpReg;
	u32 RF_Amode = 0, RF_Bmode = 0, LC_Cal;
	struct adapter *adapter = dm_odm->Adapter;

	/* Check continuous TX and Packet TX */
	tmpReg = rtw_read8(dm_odm->Adapter, 0xd03);

	if ((tmpReg&0x70) != 0)			/* Deal with contisuous TX case */
		rtw_write8(dm_odm->Adapter, 0xd03, tmpReg&0x8F);	/* disable all continuous TX */
	else							/*  Deal with Packet TX case */
		rtw_write8(dm_odm->Adapter, REG_TXPAUSE, 0xFF);		/*  block all queues */

	if ((tmpReg&0x70) != 0) {
		/* 1. Read original RF mode */
		/* Path-A */
		RF_Amode = PHY_QueryRFReg(adapter, RF_PATH_A, RF_AC, bMask12Bits);

		/* Path-B */
		if (is2T)
			RF_Bmode = PHY_QueryRFReg(adapter, RF_PATH_B, RF_AC, bMask12Bits);

		/* 2. Set RF mode = standby mode */
		/* Path-A */
		PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, RF_AC, bMask12Bits, (RF_Amode&0x8FFFF)|0x10000);

		/* Path-B */
		if (is2T)
			PHY_SetRFReg(dm_odm->Adapter, RF_PATH_B, RF_AC, bMask12Bits, (RF_Bmode&0x8FFFF)|0x10000);
	}

	/* 3. Read RF reg18 */
	LC_Cal = PHY_QueryRFReg(adapter, RF_PATH_A, RF_CHNLBW, bMask12Bits);

	/* 4. Set LC calibration begin	bit15 */
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, 0xB0, bRFRegOffsetMask, 0xDFBE0); /*  LDO ON */
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, RF_CHNLBW, bMask12Bits, LC_Cal|0x08000);

	mdelay(100);

	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, 0xB0, bRFRegOffsetMask, 0xDFFE0); /*  LDO OFF */

	/*  Channel 10 LC calibration issue for 8723bs with 26M xtal */
	if (dm_odm->SupportInterface == ODM_ITRF_SDIO && dm_odm->PackageType >= 0x2) {
		PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, RF_CHNLBW, bMask12Bits, LC_Cal);
	}

	/* Restore original situation */
	if ((tmpReg&0x70) != 0) { /* Deal with contisuous TX case */
		/* Path-A */
		rtw_write8(dm_odm->Adapter, 0xd03, tmpReg);
		PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, RF_AC, bMask12Bits, RF_Amode);

		/* Path-B */
		if (is2T)
			PHY_SetRFReg(dm_odm->Adapter, RF_PATH_B, RF_AC, bMask12Bits, RF_Bmode);
	} else /*  Deal with Packet TX case */
		rtw_write8(dm_odm->Adapter, REG_TXPAUSE, 0x00);
}

/* IQK version:V2.5    20140123 */
/* IQK is controlled by Is2ant, RF path */
void PHY_IQCalibrate_8723B(
	struct adapter *adapter,
	bool recovery,
	bool restore,
	bool Is2ant,	/* false:1ant, true:2-ant */
	u8 RF_Path	/* 0:S1, 1:S0 */
)
{
	struct hal_com_data	*hal_data = GET_HAL_DATA(adapter);

	struct dm_odm_t *dm_odm = &hal_data->odmpriv;

	s32 result[4][8];	/* last is final result */
	u8 i, final_candidate;
	bool path_a_ok, path_b_ok;
	s32 RegE94, RegE9C, RegEA4, RegEB4, RegEBC, RegEC4, RegTmp = 0;
	bool is12simular, is13simular, is23simular;
	bool single_tone = false, carrier_suppression = false;
	u32 IQK_BB_REG_92C[IQK_BB_REG_NUM] = {
		rOFDM0_XARxIQImbalance,
		rOFDM0_XBRxIQImbalance,
		rOFDM0_ECCAThreshold,
		rOFDM0_AGCRSSITable,
		rOFDM0_XATxIQImbalance,
		rOFDM0_XBTxIQImbalance,
		rOFDM0_XCTxAFE,
		rOFDM0_XDTxAFE,
		rOFDM0_RxIQExtAnta
	};
/* 	u32 		Path_SEL_BB = 0; */
	u32 		GNT_BT_default;

	if (!ODM_CheckPowerStatus(adapter))
		return;

	if (!(dm_odm->SupportAbility & ODM_RF_CALIBRATION))
		return;

	/*  20120213<Kordan> Turn on when continuous Tx to pass lab testing. (required by Edlu) */
	if (single_tone || carrier_suppression)
		return;

	if (dm_odm->RFCalibrateInfo.bIQKInProgress)
		return;


	dm_odm->RFCalibrateInfo.bIQKInProgress = true;

	if (restore) {
		u32 offset, data;
		u8 path, bResult = SUCCESS;
		struct odm_rf_cal_t *rf_calibrate_info = &dm_odm->RFCalibrateInfo;

		path = (PHY_QueryBBReg(dm_odm->Adapter, rS0S1_PathSwitch, bMaskByte0) == 0x00) ? RF_PATH_A : RF_PATH_B;

		/*  Restore TX IQK */
		for (i = 0; i < 3; ++i) {
			offset = rf_calibrate_info->TxIQC_8723B[path][i][0];
			data = rf_calibrate_info->TxIQC_8723B[path][i][1];
			if ((offset == 0) || (data == 0)) {
				bResult = FAIL;
				break;
			}
			PHY_SetBBReg(dm_odm->Adapter, offset, bMaskDWord, data);
		}

		/*  Restore RX IQK */
		for (i = 0; i < 2; ++i) {
			offset = rf_calibrate_info->RxIQC_8723B[path][i][0];
			data = rf_calibrate_info->RxIQC_8723B[path][i][1];
			if ((offset == 0) || (data == 0)) {
				bResult = FAIL;
				break;
			}
			PHY_SetBBReg(dm_odm->Adapter, offset, bMaskDWord, data);
		}

		if (dm_odm->RFCalibrateInfo.TxLOK[RF_PATH_A] == 0) {
			bResult = FAIL;
		} else {
			PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, RF_TXM_IDAC, bRFRegOffsetMask, dm_odm->RFCalibrateInfo.TxLOK[RF_PATH_A]);
			PHY_SetRFReg(dm_odm->Adapter, RF_PATH_B, RF_TXM_IDAC, bRFRegOffsetMask, dm_odm->RFCalibrateInfo.TxLOK[RF_PATH_B]);
		}

		if (bResult == SUCCESS)
			return;
	}

	if (recovery) {
		_PHY_ReloadADDARegisters8723B(adapter, IQK_BB_REG_92C, dm_odm->RFCalibrateInfo.IQK_BB_backup_recover, 9);
		return;
	}

	/* save default GNT_BT */
	GNT_BT_default = PHY_QueryBBReg(dm_odm->Adapter, 0x764, bMaskDWord);
	/*  Save RF Path */
/* 	Path_SEL_BB = PHY_QueryBBReg(dm_odm->Adapter, 0x948, bMaskDWord); */
/* 	Path_SEL_RF = PHY_QueryRFReg(dm_odm->Adapter, RF_PATH_A, 0xb0, 0xfffff); */

    /* set GNT_BT = 0, pause BT traffic */
/* 	PHY_SetBBReg(dm_odm->Adapter, 0x764, BIT12, 0x0); */
/* 	PHY_SetBBReg(dm_odm->Adapter, 0x764, BIT11, 0x1); */


	for (i = 0; i < 8; i++) {
		result[0][i] = 0;
		result[1][i] = 0;
		result[2][i] = 0;
		result[3][i] = 0;
	}

	final_candidate = 0xff;
	path_a_ok = false;
	path_b_ok = false;
	is12simular = false;
	is23simular = false;
	is13simular = false;


	for (i = 0; i < 3; i++) {
		phy_IQCalibrate_8723B(adapter, result, i, Is2ant, RF_Path);

		if (i == 1) {
			is12simular = phy_SimularityCompare_8723B(adapter, result, 0, 1);
			if (is12simular) {
				final_candidate = 0;
				break;
			}
		}

		if (i == 2) {
			is13simular = phy_SimularityCompare_8723B(adapter, result, 0, 2);
			if (is13simular) {
				final_candidate = 0;

				break;
			}

			is23simular = phy_SimularityCompare_8723B(adapter, result, 1, 2);
			if (is23simular) {
				final_candidate = 1;
			} else {
				for (i = 0; i < 8; i++)
					RegTmp += result[3][i];

				if (RegTmp != 0)
					final_candidate = 3;
				else
					final_candidate = 0xFF;
			}
		}
	}

	for (i = 0; i < 4; i++) {
		RegE94 = result[i][0];
		RegE9C = result[i][1];
		RegEA4 = result[i][2];
		RegEB4 = result[i][4];
		RegEBC = result[i][5];
		RegEC4 = result[i][6];
	}

	if (final_candidate != 0xff) {
		dm_odm->RFCalibrateInfo.RegE94 = RegE94 = result[final_candidate][0];
		dm_odm->RFCalibrateInfo.RegE9C = RegE9C = result[final_candidate][1];
		RegEA4 = result[final_candidate][2];
		dm_odm->RFCalibrateInfo.RegEB4 = RegEB4 = result[final_candidate][4];
		dm_odm->RFCalibrateInfo.RegEBC = RegEBC = result[final_candidate][5];
		RegEC4 = result[final_candidate][6];
		path_a_ok = path_b_ok = true;
	} else {
		dm_odm->RFCalibrateInfo.RegE94 = dm_odm->RFCalibrateInfo.RegEB4 = 0x100;	/* X default value */
		dm_odm->RFCalibrateInfo.RegE9C = dm_odm->RFCalibrateInfo.RegEBC = 0x0;		/* Y default value */
	}

	{
		if (RegE94 != 0)
			_PHY_PathAFillIQKMatrix8723B(adapter, path_a_ok, result, final_candidate, (RegEA4 == 0));
	}
	{
		if (RegEB4 != 0)
			_PHY_PathBFillIQKMatrix8723B(adapter, path_b_ok, result, final_candidate, (RegEC4 == 0));
	}

/* To Fix BSOD when final_candidate is 0xff */
/* by sherry 20120321 */
	if (final_candidate < 4) {
		for (i = 0; i < IQK_MATRIX_REG_NUM; i++)
			dm_odm->RFCalibrateInfo.iqk_matrix_regs_setting_value[0][i] = result[final_candidate][i];
	}

	_PHY_SaveADDARegisters8723B(adapter, IQK_BB_REG_92C, dm_odm->RFCalibrateInfo.IQK_BB_backup_recover, 9);

	/* restore GNT_BT */
	PHY_SetBBReg(dm_odm->Adapter, 0x764, bMaskDWord, GNT_BT_default);
	/*  Restore RF Path */
/* 	PHY_SetBBReg(dm_odm->Adapter, 0x948, bMaskDWord, Path_SEL_BB); */
/* 	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, 0xb0, 0xfffff, Path_SEL_RF); */

	/* Resotr RX mode table parameter */
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, RF_WE_LUT, 0x80000, 0x1);
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, RF_RCK_OS, bRFRegOffsetMask, 0x18000);
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, RF_TXPA_G1, bRFRegOffsetMask, 0x0001f);
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, RF_TXPA_G2, bRFRegOffsetMask, 0xe6177);
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, 0xed, 0x20, 0x1);
	PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, 0x43, bRFRegOffsetMask, 0x300bd);

	/* set GNT_BT = HW control */
/* 	PHY_SetBBReg(dm_odm->Adapter, 0x764, BIT12, 0x0); */
/* 	PHY_SetBBReg(dm_odm->Adapter, 0x764, BIT11, 0x0); */

	if (Is2ant) {
		if (RF_Path == 0x0)	/* S1 */
			ODM_SetIQCbyRFpath(dm_odm, 0);
		else	/* S0 */
			ODM_SetIQCbyRFpath(dm_odm, 1);
	}

	dm_odm->RFCalibrateInfo.bIQKInProgress = false;
}


void PHY_LCCalibrate_8723B(struct dm_odm_t *dm_odm)
{
	bool		single_tone = false, carrier_suppression = false;
	u32 		timeout = 2000, timecount = 0;

	if (!(dm_odm->SupportAbility & ODM_RF_CALIBRATION))
		return;

	/*  20120213<Kordan> Turn on when continuous Tx to pass lab testing. (required by Edlu) */
	if (single_tone || carrier_suppression)
		return;

	while (*(dm_odm->pbScanInProcess) && timecount < timeout) {
		mdelay(50);
		timecount += 50;
	}

	dm_odm->RFCalibrateInfo.bLCKInProgress = true;


	phy_LCCalibrate_8723B(dm_odm, false);


	dm_odm->RFCalibrateInfo.bLCKInProgress = false;
}
