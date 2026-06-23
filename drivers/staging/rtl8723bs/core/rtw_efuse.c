// SPDX-License-Identifier: GPL-2.0
/******************************************************************************
 *
 * Copyright(c) 2007 - 2011 Realtek Corporation. All rights reserved.
 *
 ******************************************************************************/
#include <drv_types.h>
#include <hal_data.h>
#include <linux/jiffies.h>

/*  11/16/2008 MH Add description. Get current efuse area enabled word!!. */
u8
rtw_efuse_calculate_word_counts(u8 word_en)
{
	u8 word_cnts = 0;

	if (!(word_en & BIT(0)))
		word_cnts++; /*  0 : write enable */
	if (!(word_en & BIT(1)))
		word_cnts++;
	if (!(word_en & BIT(2)))
		word_cnts++;
	if (!(word_en & BIT(3)))
		word_cnts++;
	return word_cnts;
}

/*-----------------------------------------------------------------------------
 * Function:	rtw_efuse_read_1_byte
 *
 * Overview:	Copy from WMAC fot EFUSE read 1 byte.
 *
 * Input:       NONE
 *
 * Output:      NONE
 *
 * Return:      NONE
 *
 * Revised History:
 * When			Who		Remark
 * 09/23/2008	MHC		Copy from WMAC.
 *
 */
u8
rtw_efuse_read_1_byte(
struct adapter *Adapter,
u16		address)
{
	u8 byte_temp = {0x00};
	u8 temp = {0x00};
	u32 k = 0;
	u16 content_len = 0;

	Hal_GetEfuseDefinition(Adapter, EFUSE_WIFI, TYPE_EFUSE_REAL_CONTENT_LEN, &content_len);

	if (address < content_len) {/* E-fuse 512Byte */
		/* Write E-fuse Register address bit0~7 */
		temp = address & 0xFF;
		rtw_write8(Adapter, EFUSE_CTRL + 1, temp);
		byte_temp = rtw_read8(Adapter, EFUSE_CTRL + 2);
		/* Write E-fuse Register address bit8~9 */
		temp = ((address >> 8) & 0x03) | (byte_temp & 0xFC);
		rtw_write8(Adapter, EFUSE_CTRL + 2, temp);

		/* Write 0x30[31]= 0 */
		byte_temp = rtw_read8(Adapter, EFUSE_CTRL + 3);
		temp = byte_temp & 0x7F;
		rtw_write8(Adapter, EFUSE_CTRL + 3, temp);

		/* Wait Write-ready (0x30[31]= 1) */
		byte_temp = rtw_read8(Adapter, EFUSE_CTRL + 3);
		while (!(byte_temp & 0x80)) {
			byte_temp = rtw_read8(Adapter, EFUSE_CTRL + 3);
			k++;
			if (k == 1000)
				break;
		}
		return rtw_read8(Adapter, EFUSE_CTRL);
	} else {
		return 0xFF;
	}
} /* rtw_efuse_read_1_byte */

/*  11/16/2008 MH Read one byte from real Efuse. */
u8
rtw_efuse_one_byte_read(
struct adapter *padapter,
u16	addr,
u8	*data)
{
	u32 tmpidx = 0;
	u8 result;
	u8 readbyte;

	/*  <20130121, Kordan> For SMIC EFUSE specificatoin. */
	/* 0x34[11]: SW force PGMEN input of efuse to high. (for the bank selected by 0x34[9:8]) */
	/* PHY_SetMacReg(padapter, 0x34, BIT11, 0); */
	rtw_write16(padapter, 0x34, rtw_read16(padapter, 0x34) & (~BIT(11)));

	/*  -----------------e-fuse reg ctrl --------------------------------- */
	/* address */
	rtw_write8(padapter, EFUSE_CTRL + 1, (u8)(addr & 0xff));
	rtw_write8(padapter, EFUSE_CTRL + 2, ((u8)((addr >> 8) & 0x03)) |
	(rtw_read8(padapter, EFUSE_CTRL + 2) & 0xFC));

	/* rtw_write8(padapter, EFUSE_CTRL+3,  0x72); read cmd */
	/* Write bit 32 0 */
	readbyte = rtw_read8(padapter, EFUSE_CTRL + 3);
	rtw_write8(padapter, EFUSE_CTRL + 3, (readbyte & 0x7f));

	while (!(0x80 & rtw_read8(padapter, EFUSE_CTRL + 3)) && (tmpidx < 1000)) {
		mdelay(1);
		tmpidx++;
	}
	if (tmpidx < 100) {
		*data = rtw_read8(padapter, EFUSE_CTRL);
		result = true;
	} else {
		*data = 0xff;
		result = false;
	}

	return result;
}

/*-----------------------------------------------------------------------------
 * Function:	Efuse_ReadAllMap
 *
 * Overview:	Read All Efuse content
 *
 * Input:       NONE
 *
 * Output:      NONE
 *
 * Return:      NONE
 *
 * Revised History:
 * When			Who		Remark
 * 11/11/2008	MHC		Create Version 0.
 *
 */
static void Efuse_ReadAllMap(struct adapter *padapter, u8 efuse_type, u8 *Efuse)
{
	u16 map_len = 0;

	Hal_EfusePowerSwitch(padapter, true);

	Hal_GetEfuseDefinition(padapter, efuse_type, TYPE_EFUSE_MAP_LEN, &map_len);

	Hal_ReadEFuse(padapter, efuse_type, 0, map_len, Efuse);

	Hal_EfusePowerSwitch(padapter, false);
}

/*-----------------------------------------------------------------------------
 * Function:	efuse_ShadowRead1Byte
 *		efuse_ShadowRead2Byte
 *		efuse_ShadowRead4Byte
 *
 * Overview:	Read from efuse init map by one/two/four bytes !!!!!
 *
 * Input:       NONE
 *
 * Output:      NONE
 *
 * Return:      NONE
 *
 * Revised History:
 * When			Who		Remark
 * 11/12/2008	MHC		Create Version 0.
 *
 */
static void efuse_ShadowRead1Byte(struct adapter *padapter, u16 Offset, u8 *Value)
{
	struct eeprom_priv *pEEPROM = GET_EEPROM_EFUSE_PRIV(padapter);

	*Value = pEEPROM->efuse_eeprom_data[Offset];

}	/*  EFUSE_ShadowRead1Byte */

/* Read Two Bytes */
static void efuse_ShadowRead2Byte(struct adapter *padapter, u16 Offset, u16 *Value)
{
	struct eeprom_priv *pEEPROM = GET_EEPROM_EFUSE_PRIV(padapter);

	*Value = pEEPROM->efuse_eeprom_data[Offset];
	*Value |= pEEPROM->efuse_eeprom_data[Offset + 1] << 8;

}	/*  EFUSE_ShadowRead2Byte */

/* Read Four Bytes */
static void efuse_ShadowRead4Byte(struct adapter *padapter, u16 Offset, u32 *Value)
{
	struct eeprom_priv *pEEPROM = GET_EEPROM_EFUSE_PRIV(padapter);

	*Value = pEEPROM->efuse_eeprom_data[Offset];
	*Value |= pEEPROM->efuse_eeprom_data[Offset + 1] << 8;
	*Value |= pEEPROM->efuse_eeprom_data[Offset + 2] << 16;
	*Value |= pEEPROM->efuse_eeprom_data[Offset + 3] << 24;

}	/*  efuse_ShadowRead4Byte */

/*-----------------------------------------------------------------------------
 * Function:	rtw_efuse_shadow_map_update
 *
 * Overview:	Transfer current EFUSE content to shadow init and modify map.
 *
 * Input:       NONE
 *
 * Output:      NONE
 *
 * Return:      NONE
 *
 * Revised History:
 * When			Who		Remark
 * 11/13/2008	MHC		Create Version 0.
 *
 */
void rtw_efuse_shadow_map_update(struct adapter *padapter, u8 efuse_type)
{
	struct eeprom_priv *pEEPROM = GET_EEPROM_EFUSE_PRIV(padapter);
	u16 map_len = 0;

	Hal_GetEfuseDefinition(padapter, efuse_type, TYPE_EFUSE_MAP_LEN, &map_len);

	if (pEEPROM->bautoload_fail_flag)
		memset(pEEPROM->efuse_eeprom_data, 0xFF, map_len);
	else
		Efuse_ReadAllMap(padapter, efuse_type, pEEPROM->efuse_eeprom_data);

	/* PlatformMoveMemory((void *)&pHalData->EfuseMap[EFUSE_MODIFY_MAP][0], */
	/* void *)&pHalData->EfuseMap[EFUSE_INIT_MAP][0], map_len); */
} /*  rtw_efuse_shadow_map_update */

/*-----------------------------------------------------------------------------
 * Function:	rtw_efuse_shadow_read
 *
 * Overview:	Read from efuse init map !!!!!
 *
 * Input:       NONE
 *
 * Output:      NONE
 *
 * Return:      NONE
 *
 * Revised History:
 * When			Who		Remark
 * 11/12/2008	MHC		Create Version 0.
 *
 */
void rtw_efuse_shadow_read(struct adapter *padapter, u8 Type, u16 Offset, u32 *Value)
{
	if (Type == 1)
		efuse_ShadowRead1Byte(padapter, Offset, (u8 *)Value);
	else if (Type == 2)
		efuse_ShadowRead2Byte(padapter, Offset, (u16 *)Value);
	else if (Type == 4)
		efuse_ShadowRead4Byte(padapter, Offset, (u32 *)Value);

} /* rtw_efuse_shadow_read*/
