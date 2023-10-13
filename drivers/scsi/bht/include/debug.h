/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: debug.h
 *
 * Abstract: define log type mapping for debugging
 *
 * Version: 1.00
 *
 * Environment:	OS Independent
 *
 */

#ifndef _BHT_DBG_H

#define _BHT_DBG_H

/* host.c */
#define MODULE_SD_HOST			0x00000001
/* hostven.c */
#define MODULE_VEN_HOST			0x00000002
/* transhander.c */
#define MODULE_TRANS			0x00000004

/* uhs2.c */
#define MODULE_UHS2_CARD		0x00000008
/* mmc.c */
#define MODULE_MMC_CARD			0x00000010
/* sd.c */
#define MODULE_SD_CARD			0x00000020
/* sdio.c */
#define MODULE_OTHER_CARD		0x00000040

/* cardcommon.c cardinterface.c */
#define MODULE_ALL_CARD		(MODULE_UHS2_CARD | MODULE_MMC_CARD | \
	MODULE_SD_CARD | MODULE_OTHER_CARD)
#define MODULE_LEGACY_CARD	(MODULE_MMC_CARD | MODULE_SD_CARD | MODULE_OTHER_CARD)

#define MODULE_TQ_FLOW		0x00000080
#define MODULE_TQ_POLICY	0x00000100
#define MODULE_TQ_DMA		0x00000200

/* thread.c */
#define MODULE_MAIN_THR		0x00000400
/* reqmng.c */
#define MODULE_REQ_MNG		0x00000800
/* pmfunc.c */
#define MODULE_MAIN_PM		0x00001000
/* geniofunc.c */
#define MODULE_MAIN_GENIO	0x00002000
/* autotimer.c */
#define MODULE_AUTOTIMER	0x00004000
/* thermal.c */
#define MODULE_THERMAL		0x00008000
/* cfgmng.c */
#define MODULE_CFG_MNG		0x00010000

#define MODULE_OTHER		0x00020000

#define MODULE_OS_ENTRYAPI	0x00040000
#define MODULE_OS_ENTRY		0x00080000
#define MODULE_OS_API		0x00100000

#define MODULE_TEST		0x00200000

#define MODULE_ALL			0xFFFFFFFF

/*
 * This is used for Driver Entry and Driver Unload
 * req_global_init and uninit and sub call
 */
#define    FEATURE_DRIVER_INIT			0x00000001

/* For Card Init Flow	*/
#define    FEATURE_CARD_INIT			0x00000002

/* For Error Recovery Flow	*/
#define    FEATURE_ERROR_RECOVER		0x00000004

/* For Scsi RW, Card RW, TagQueure RW case */
#define    FEATURE_RW_TRACE			0x00000008

/* Other Card operation, such as cmd12, power off and etc */
#define    FEATURE_CARD_OPS			0x00000010

/* for Command handler and card_send_sdcmd only */
#define    FEATURE_CARDCMD_TRACE		0x00000020

/* For trace interrupt only */
#define    FEATURE_INTR_TRACE			0x00000040

/* For PCI and SD Bar0 and SD Bar1 */
#define    FEATURE_SDREG_TRACEW		        0x00000080
#define    FEATURE_SDREG_TRACER			0x00000100
#define    FEATURE_PCIREG_TRACEW		0x00000200
#define    FEATURE_PCIREG_TRACER		0x00000400
#define    FEATURE_VENREG_TRACEW		0x00000800
#define    FEATURE_VENREG_TRACER		0x00001000

/* Pnp and PM trace */
#define    FEATURE_PNP_TRACE			0x00002000
#define    FEATURE_PM_TRACE				0x00004000

/* Trace each scsi command and result */
#define    FEATURE_SCSICMD_TRACE		0x00008000

/* Main Thread Event trace */
#define    FEATURE_THREAD_TRACE			0x00010000

/* for Register Cfg setting only */
#define    FEATURE_CFG_TRACE			0x00020000

/* For other function not specified */
#define    FEATURE_FUNC_TRACE			0x00040000
#define    FEATURE_FUNC_DESC			0x00080000

/* Functions */
#define    FEATURE_TIMER_TRACE			0x00100000
#define    FEATURE_FUNC_THERMAL			0x00200000
#define    FEATURE_IOCTL_TRACE			0x00400000

#define   FEATURE_DBG_ALL			0xFFFFFFFF

#define DBG_CTRL_DUMP_HOST	BIT0
#define DBG_CTRL_DUMP_DESC  BIT1

#define DBG_CTRL_CONTROL	3

/* contorol which feature will be print for DbgInfo */
#define DBG_FEATURE_CONTROL	 (0xFFFFFFFF)
/* control which modules will be print for DbgWarn and DbgInfo  */
#define DBG_MODULE_CONTROL	 (0xFFFFFFFF)

#define TO_RAM         1
#define NOT_TO_RAM   0
void DbgRamInit(void);
void DbgRamInitNon(void);
void DbgRamFree(void);

void DbgErr(byte *info, ...);
void PrintMsg(byte *info, ...);

#ifdef DBG_PERFORMANCE
void calc_req_start(tPerTick *tick, u32 sec_cnt, bool bWrite);
void calc_thr_start(tPerTick *tick);
void calc_io_end(tPerTick *tick);
#else
#define calc_req_start(x, y, z)
#define calc_thr_start(x)
#define calc_io_end(x)
#endif

extern u32 g_dbg_ctrl;

#if DBG || _DEBUG
void x_assert(char *, unsigned int);
#define X_ASSERT(f) do { if (f) { } else x_assert(__FILE__, __LINE__); } while (0)

void DbgWarn(u32 module, byte toram, byte *info, ...);
void DbgInfo(u32 module, u32 feature, byte toram, byte *info, ...);

#else
#define DbgWarn(m, r, _x_, ...)
#define DbgInfo(m, f, r, _x_, ...)
#define X_ASSERT(f)
#endif

#endif
