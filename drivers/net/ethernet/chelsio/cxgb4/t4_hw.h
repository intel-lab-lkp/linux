/*
 * This file is part of the Chelsio T4 Ethernet driver for Linux.
 *
 * Copyright (c) 2003-2014 Chelsio Communications, Inc. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef __T4_HW_H
#define __T4_HW_H

#include <linux/types.h>

enum {
	NCHAN           = 4,    /* # of HW channels */
	MAX_MTU         = 9600, /* max MAC MTU, excluding header + FCS */
	EEPROMSIZE      = 17408,/* Serial EEPROM physical size */
	EEPROMVSIZE     = 32768,/* Serial EEPROM virtual address space size */
	EEPROMPFSIZE    = 1024, /* EEPROM writable area size for PFn, n>0 */
	RSS_NENTRIES    = 2048, /* # of entries in RSS mapping table */
	T6_RSS_NENTRIES = 4096, /* # of entries in RSS mapping table */
	TCB_SIZE        = 128,  /* TCB size */
	NMTUS           = 16,   /* size of MTU table */
	NCCTRL_WIN      = 32,   /* # of congestion control windows */
	NTX_SCHED       = 8,    /* # of HW Tx scheduling queues */
	PM_NSTATS       = 5,    /* # of PM stats */
	T6_PM_NSTATS    = 7,    /* # of PM stats in T6 */
	MBOX_LEN        = 64,   /* mailbox size in bytes */
	TRACE_LEN       = 112,  /* length of trace data and mask */
	FILTER_OPT_LEN  = 36,   /* filter tuple width for optional components */
	T7_PM_RX_CACHE_NSTATS = 27, /* # of PM Rx Cache stats in T7 */
	MAX_UP_CORES    = 8,    /* Max # of uP cores that can be enabled */
};

enum {
	CIM_NUM_IBQ    = 6,     /* # of CIM IBQs */
	CIM_NUM_IBQ_T7 = 16,    /* # of CIM IBQs for T7 */
	CIM_NUM_OBQ    = 6,     /* # of CIM OBQs */
	CIM_NUM_OBQ_T5 = 8,     /* # of CIM OBQs for T5 adapter */
	CIM_NUM_OBQ_T7 = 16,    /* # of CIM OBQs for T7 adapter */
	CIMLA_SIZE     = (256 * 8),  /* 256 rows * ceil(235/32) 32-bit words */
	CIMLA_SIZE_T6  = (256 * 10), /* 256 rows * ceil(311/32) 32-bit words */
	CIM_PIFLA_SIZE = 64,    /* # of 192-bit words in CIM PIF LA */
	CIM_MALA_SIZE  = 64,    /* # of 160-bit words in CIM MA LA */
	CIM_IBQ_SIZE   = 128,   /* # of 128-bit words in a CIM IBQ */
	CIM_OBQ_SIZE   = 128,   /* # of 128-bit words in a CIM OBQ */
	TPLA_SIZE      = 128,   /* # of 64-bit words in TP LA */
	ULPRX_LA_SIZE  = 512,   /* # of 256-bit words in ULP_RX LA */
};

/* SGE context types */
enum ctxt_type {
	CTXT_EGRESS,
	CTXT_INGRESS,
	CTXT_FLM,
	CTXT_CNM,
};

enum {
	SF_PAGE_SIZE = 256,           /* serial flash page size */
	SF_SEC_SIZE = 64 * 1024,      /* serial flash sector size */
};

enum { RSP_TYPE_FLBUF, RSP_TYPE_CPL, RSP_TYPE_INTR }; /* response entry types */

enum { MBOX_OWNER_NONE, MBOX_OWNER_FW, MBOX_OWNER_DRV };    /* mailbox owners */

enum {
	SGE_MAX_WR_LEN = 512,     /* max WR size in bytes */
	SGE_CTXT_SIZE = 24,       /* size of SGE context */
	SGE_CTXT_SIZE_T7 = 28,    /* size of SGE context for T7 */
	SGE_NTIMERS = 6,          /* # of interrupt holdoff timer values */
	SGE_NCOUNTERS = 4,        /* # of interrupt packet counter values */
	SGE_NDBQTIMERS = 8,       /* # of Doorbell Queue Timer values */
	SGE_MAX_IQ_SIZE = 65520,

	SGE_TIMER_RSTRT_CNTR = 6, /* restart RX packet threshold counter */
	SGE_TIMER_UPD_CIDX = 7,   /* update cidx only */

	SGE_EQ_IDXSIZE = 64,      /* egress queue pidx/cidx unit size */

	SGE_INTRDST_PCI = 0,      /* interrupt destination is PCI-E */
	SGE_INTRDST_IQ = 1,       /*   destination is an ingress queue */

	SGE_UPDATEDEL_NONE = 0,   /* ingress queue pidx update delivery */
	SGE_UPDATEDEL_INTR = 1,   /*   interrupt */
	SGE_UPDATEDEL_STPG = 2,   /*   status page */
	SGE_UPDATEDEL_BOTH = 3,   /*   interrupt and status page */

	SGE_HOSTFCMODE_NONE = 0,  /* egress queue cidx updates */
	SGE_HOSTFCMODE_IQ = 1,    /*   sent to ingress queue */
	SGE_HOSTFCMODE_STPG = 2,  /*   sent to status page */
	SGE_HOSTFCMODE_BOTH = 3,  /*   ingress queue and status page */

	SGE_FETCHBURSTMIN_16B = 0,/* egress queue descriptor fetch minimum */
	SGE_FETCHBURSTMIN_32B = 1,
	SGE_FETCHBURSTMIN_64B = 2,
	SGE_FETCHBURSTMIN_128B = 3,

	SGE_FETCHBURSTMAX_64B = 0,/* egress queue descriptor fetch maximum */
	SGE_FETCHBURSTMAX_128B = 1,
	SGE_FETCHBURSTMAX_256B = 2,
	SGE_FETCHBURSTMAX_512B = 3,

	SGE_CIDXFLUSHTHRESH_1 = 0,/* egress queue cidx flush threshold */
	SGE_CIDXFLUSHTHRESH_2 = 1,
	SGE_CIDXFLUSHTHRESH_4 = 2,
	SGE_CIDXFLUSHTHRESH_8 = 3,
	SGE_CIDXFLUSHTHRESH_16 = 4,
	SGE_CIDXFLUSHTHRESH_32 = 5,
	SGE_CIDXFLUSHTHRESH_64 = 6,
	SGE_CIDXFLUSHTHRESH_128 = 7,

	SGE_INGPADBOUNDARY_SHIFT = 5,/* ingress queue pad boundary */
};

/* PCI-e memory window access */
enum pcie_memwin {
	MEMWIN_NIC      = 0,
	MEMWIN_RSVD1    = 1,
	MEMWIN_RSVD2    = 2,
	MEMWIN_RDMA     = 3,
	MEMWIN_RSVD4    = 4,
	MEMWIN_FOISCSI  = 5,
	MEMWIN_CSIOSTOR = 6,
	MEMWIN_RSVD7    = 7,
};

struct sge_qstat {                /* data written to SGE queue status entries */
	__be32 qid;
	__be16 cidx;
	__be16 pidx;
};

/*
 * Structure for last 128 bits of response descriptors
 */
struct rsp_ctrl {
	__be32 hdrbuflen_pidx;
	__be32 pldbuflen_qid;
	union {
		u8 type_gen;
		__be64 last_flit;
	};
};

#define RSPD_NEWBUF_S    31
#define RSPD_NEWBUF_V(x) ((x) << RSPD_NEWBUF_S)
#define RSPD_NEWBUF_F    RSPD_NEWBUF_V(1U)

#define RSPD_LEN_S    0
#define RSPD_LEN_M    0x7fffffff
#define RSPD_LEN_G(x) (((x) >> RSPD_LEN_S) & RSPD_LEN_M)

#define RSPD_QID_S    RSPD_LEN_S
#define RSPD_QID_M    RSPD_LEN_M
#define RSPD_QID_G(x) RSPD_LEN_G(x)

#define RSPD_GEN_S    7

#define RSPD_TYPE_S    4
#define RSPD_TYPE_M    0x3
#define RSPD_TYPE_G(x) (((x) >> RSPD_TYPE_S) & RSPD_TYPE_M)

/* Rx queue interrupt deferral fields: counter enable and timer index */
#define QINTR_CNT_EN_S    0
#define QINTR_CNT_EN_V(x) ((x) << QINTR_CNT_EN_S)
#define QINTR_CNT_EN_F    QINTR_CNT_EN_V(1U)

#define QINTR_TIMER_IDX_S    1
#define QINTR_TIMER_IDX_M    0x7
#define QINTR_TIMER_IDX_V(x) ((x) << QINTR_TIMER_IDX_S)
#define QINTR_TIMER_IDX_G(x) (((x) >> QINTR_TIMER_IDX_S) & QINTR_TIMER_IDX_M)

/*
 * Flash layout.
 */
#define FLASH_START(start)	((start) * SF_SEC_SIZE)
#define FLASH_MAX_SIZE(nsecs)	((nsecs) * SF_SEC_SIZE)

enum {
	FLASH_FW_START_SEC = 8,
	FLASH_FW_NSECS = 16,
	FLASH_FW_START = FLASH_START(FLASH_FW_START_SEC),
	FLASH_FW_MAX_SIZE = FLASH_MAX_SIZE(FLASH_FW_NSECS),

	FLASH_CFG_START_SEC = 31,
	FLASH_CFG_NSECS = 1,
	FLASH_CFG_START = FLASH_START(FLASH_CFG_START_SEC),
	FLASH_CFG_MAX_SIZE = FLASH_MAX_SIZE(FLASH_CFG_NSECS),

	FLASH_MIN_SIZE = FLASH_CFG_START + FLASH_CFG_MAX_SIZE,
};

enum t4_flash_loc {
	/*
	 * Various Expansion-ROM boot images, etc.
	 */
	FLASH_LOC_EXP_ROM = 0,

	/*
	 * iSCSI Boot Firmware Table (iBFT) and other driver-related
	 * parameters ...
	 */
	FLASH_LOC_IBFT,

	/*
	 * Boot configuration data.
	 */
	FLASH_LOC_BOOTCFG,

	/*
	 * Location of firmware image in FLASH.
	 */
	FLASH_LOC_FW,

	/* Location of bootstrap firmware image in FLASH.
	 */
	FLASH_LOC_FWBOOTSTRAP,

	/*
	 * iSCSI persistent/crash information.
	 */
	FLASH_LOC_ISCSI_CRASH,

	/*
	 * FCoE persistent/crash information.
	 */
	FLASH_LOC_FCOE_CRASH,

	/*
	 * Location of Firmware Configuration File in FLASH.
	 */
	FLASH_LOC_CFG,

	/*
	 * CUDBG chip dump.
	 */
	FLASH_LOC_CUDBG,

	/*
	 * FW chip dump.
	 */
	FLASH_LOC_CHIP_DUMP,

	/*
	 * DPU boot information store.
	 */
	FLASH_LOC_DPU_BOOT,

	/*
	 * DPU peristent information store.
	 */
	FLASH_LOC_DPU_AREA,

	/*
	 * VPD location.
	 */
	FLASH_LOC_VPD,

	/*
	 * Backup init/vpd.
	 */
	FLASH_LOC_VPD_BACKUP,

	/*
	 * Sectors 32-63 for CUDBG.
	 * Backup firmware image.
	 */
	FLASH_LOC_FW_BACKUP,

	/*
	 * Backup bootstrap firmware image.
	 */
	FLASH_LOC_FWBOOTSTRAP_BACKUP,

	/*
	 * Backup Location of Firmware Configuration File in FLASH.
	 */
	FLASH_LOC_CFG_BACK,

	/*
	 * Helper to retrieve info that spans the entire Boot related area.
	 */
	FLASH_LOC_BOOT_AREA,

	/*
	 * Helper to determine minimum standard set of sections needed for
	 * normal operations.
	 */
	FLASH_LOC_MIN_SIZE,

	/*
	 * End of FLASH regions.
	 */
	FLASH_LOC_END
};

struct t4_flash_loc_entry {
	u16 start_sec;
	u16 nsecs;
};

#define SGE_TIMESTAMP_S 0
#define SGE_TIMESTAMP_M 0xfffffffffffffffULL
#define SGE_TIMESTAMP_V(x) ((__u64)(x) << SGE_TIMESTAMP_S)
#define SGE_TIMESTAMP_G(x) (((__u64)(x) >> SGE_TIMESTAMP_S) & SGE_TIMESTAMP_M)

#define I2C_DEV_ADDR_A0		0xa0
#define I2C_DEV_ADDR_A2		0xa2
#define I2C_PAGE_SIZE		0x100
#define SFP_DIAG_TYPE_ADDR	0x5c
#define SFP_DIAG_TYPE_LEN	0x1
#define SFP_DIAG_ADDRMODE	BIT(2)
#define SFP_DIAG_IMPLEMENTED	BIT(6)
#define SFF_8472_COMP_ADDR	0x5e
#define SFF_8472_COMP_LEN	0x1
#define SFF_REV_ADDR		0x1
#define SFF_REV_LEN		0x1

#endif /* __T4_HW_H */
