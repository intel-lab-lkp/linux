// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 1996, 2003 VIA Networking Technologies, Inc.
 * All rights reserved.
 *
 * Purpose:Implement functions to access eeprom
 *
 * Author: Jerry Chen
 *
 * Date: Jan 29, 2003
 *
 * Functions:
 *      SROMbyReadEmbedded - Embedded read eeprom via MAC
 *      SROMbWriteEmbedded - Embedded write eeprom via MAC
 *      SROMvRegBitsOn - Set Bits On in eeprom
 *      SROMvRegBitsOff - Clear Bits Off in eeprom
 *      SROMbIsRegBitsOn - Test if Bits On in eeprom
 *      SROMbIsRegBitsOff - Test if Bits Off in eeprom
 *      SROMvReadAllContents - Read all contents in eeprom
 *      SROMvWriteAllContents - Write all contents in eeprom
 *      SROMvReadEtherAddress - Read Ethernet Address in eeprom
 *      SROMvWriteEtherAddress - Write Ethernet Address in eeprom
 *      SROMvReadSubSysVenId - Read Sub_VID and Sub_SysId in eeprom
 *      SROMbAutoLoad - Auto Load eeprom to MAC register
 *
 * Revision History:
 *
 */

#include "device.h"
#include "mac.h"
#include "srom.h"

/*---------------------  Static Definitions -------------------------*/

/*---------------------  Static Classes  ----------------------------*/

/*---------------------  Static Variables  --------------------------*/

/*---------------------  Static Functions  --------------------------*/

/*---------------------  Export Variables  --------------------------*/

/*---------------------  Export Functions  --------------------------*/

/*
 * Description: Read a byte from EEPROM, by MAC I2C
 *
 * Parameters:
 *  In:
 *      iobase          - I/O base address
 *      byContntOffset  - address of EEPROM
 *  Out:
 *      none
 *
 * Return Value: data read
 *
 */
unsigned char SROMbyReadEmbedded(void __iomem *iobase,
				 unsigned char byContntOffset)
{
	unsigned short delay, noack;
	unsigned char wait;
	unsigned char data;
	unsigned char org;

	data = 0xFF;
	org = ioread8(iobase + MAC_REG_I2MCFG);
	/* turn off hardware retry for getting NACK */
	iowrite8(org & (~I2MCFG_NORETRY), iobase + MAC_REG_I2MCFG);
	for (noack = 0; noack < W_MAX_I2CRETRY; noack++) {
		iowrite8(EEP_I2C_DEV_ID, iobase + MAC_REG_I2MTGID);
		iowrite8(byContntOffset, iobase + MAC_REG_I2MTGAD);

		/* issue read command */
		iowrite8(I2MCSR_EEMR, iobase + MAC_REG_I2MCSR);
		/* wait DONE be set */
		for (delay = 0; delay < W_MAX_TIMEOUT; delay++) {
			wait = ioread8(iobase + MAC_REG_I2MCSR);
			if (wait & (I2MCSR_DONE | I2MCSR_NACK))
				break;
			udelay(CB_DELAY_LOOP_WAIT);
		}
		if ((delay < W_MAX_TIMEOUT) &&
		    (!(wait & I2MCSR_NACK))) {
			break;
		}
	}
	data = ioread8(iobase + MAC_REG_I2MDIPT);
	iowrite8(org, iobase + MAC_REG_I2MCFG);
	return data;
}

/*
 * Description: Read all contents of eeprom to buffer
 *
 * Parameters:
 *  In:
 *      iobase          - I/O base address
 *  Out:
 *      eepromregs   - EEPROM content Buffer
 *
 * Return Value: none
 *
 */
void SROMvReadAllContents(void __iomem *iobase, unsigned char *eepromregs)
{
	int     ii;

	/* ii = Rom Address */
	for (ii = 0; ii < EEP_MAX_CONTEXT_SIZE; ii++) {
		*eepromregs = SROMbyReadEmbedded(iobase,
						    (unsigned char)ii);
		eepromregs++;
	}
}

/*
 * Description: Read Ethernet Address from eeprom to buffer
 *
 * Parameters:
 *  In:
 *      iobase          - I/O base address
 *  Out:
 *      etheraddress - Ethernet Address buffer
 *
 * Return Value: none
 *
 */
void SROMvReadEtherAddress(void __iomem *iobase,
			   unsigned char *etheraddress)
{
	unsigned char ii;

	/* ii = Rom Address */
	for (ii = 0; ii < ETH_ALEN; ii++) {
		*etheraddress = SROMbyReadEmbedded(iobase, ii);
		etheraddress++;
	}
}
