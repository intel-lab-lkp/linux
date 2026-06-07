/* SPDX-License-Identifier: GPL-2.0-only */
/*
 *  Copyright (C) 2017 Chelsio Communications.  All rights reserved.
 */

#ifndef __CUDBG_IF_H__
#define __CUDBG_IF_H__

/* Error codes */
#define CUDBG_STATUS_NO_MEM -19
#define CUDBG_STATUS_ENTITY_NOT_FOUND -24
#define CUDBG_STATUS_NOT_IMPLEMENTED -28
#define CUDBG_SYSTEM_ERROR -29
#define CUDBG_STATUS_CCLK_NOT_DEFINED -32
#define CUDBG_STATUS_PARTIAL_DATA -41

#define CUDBG_MAJOR_VERSION 1
#define CUDBG_MINOR_VERSION 14

#define CUDBG_MAX_PARAMS 16

enum {
	CUDBG_UP_COREID_PARAM = 13,
};

enum cudbg_dbg_entity_type {
	CUDBG_REG_DUMP = 1,
	CUDBG_DEV_LOG = 2,
	CUDBG_CIM_LA = 3,
	CUDBG_CIM_MA_LA = 4,
	CUDBG_CIM_QCFG = 5,
	CUDBG_CIM_IBQ_TP0 = 6,
	CUDBG_CIM_IBQ_TP1 = 7,
	CUDBG_CIM_IBQ_ULP = 8,
	CUDBG_CIM_IBQ_SGE0 = 9,
	CUDBG_CIM_IBQ_SGE1 = 10,
	CUDBG_CIM_IBQ_NCSI = 11,
	CUDBG_CIM_OBQ_ULP0 = 12,
	CUDBG_CIM_OBQ_ULP1 = 13,
	CUDBG_CIM_OBQ_ULP2 = 14,
	CUDBG_CIM_OBQ_ULP3 = 15,
	CUDBG_CIM_OBQ_SGE = 16,
	CUDBG_CIM_OBQ_NCSI = 17,
	CUDBG_EDC0 = 18,
	CUDBG_EDC1 = 19,
	CUDBG_MC0 = 20,
	CUDBG_MC1 = 21,
	CUDBG_RSS = 22,
	CUDBG_RSS_VF_CONF = 25,
	CUDBG_PATH_MTU = 27,
	CUDBG_PM_STATS = 30,
	CUDBG_HW_SCHED = 31,
	CUDBG_TP_INDIRECT = 36,
	CUDBG_SGE_INDIRECT = 37,
	CUDBG_ULPRX_LA = 41,
	CUDBG_TP_LA = 43,
	CUDBG_MEMINFO = 44,
	CUDBG_CIM_PIF_LA = 45,
	CUDBG_CLK = 46,
	CUDBG_CIM_OBQ_RXQ0 = 47,
	CUDBG_CIM_OBQ_RXQ1 = 48,
	CUDBG_PCIE_INDIRECT = 50,
	CUDBG_PM_INDIRECT = 51,
	CUDBG_TID_INFO = 54,
	CUDBG_PCIE_CONFIG = 55,
	CUDBG_DUMP_CONTEXT = 56,
	CUDBG_MPS_TCAM = 57,
	CUDBG_VPD_DATA = 58,
	CUDBG_LE_TCAM = 59,
	CUDBG_CCTRL = 60,
	CUDBG_MA_INDIRECT = 61,
	CUDBG_ULPTX_LA = 62,
	CUDBG_UP_CIM_INDIRECT = 64,
	CUDBG_PBT_TABLE = 65,
	CUDBG_MBOX_LOG = 66,
	CUDBG_HMA_INDIRECT = 67,
	CUDBG_HMA = 68,
	CUDBG_QDESC = 70,
	CUDBG_FLASH = 71,
	CUDBG_CIM_IBQ_TP2  = 73,
	CUDBG_CIM_IBQ_TP3  = 74,
	CUDBG_CIM_IBQ_IPC1 = 75,
	CUDBG_CIM_IBQ_IPC2 = 76,
	CUDBG_CIM_IBQ_IPC3 = 77,
	CUDBG_CIM_IBQ_IPC4 = 78,
	CUDBG_CIM_IBQ_IPC5 = 79,
	CUDBG_CIM_IBQ_IPC6 = 80,
	CUDBG_CIM_IBQ_IPC7 = 81,
	CUDBG_CIM_OBQ_IPC1 = 82,
	CUDBG_CIM_OBQ_IPC2 = 83,
	CUDBG_CIM_OBQ_IPC3 = 84,
	CUDBG_CIM_OBQ_IPC4 = 85,
	CUDBG_CIM_OBQ_IPC5 = 86,
	CUDBG_CIM_OBQ_IPC6 = 87,
	CUDBG_CIM_OBQ_IPC7 = 88,
	CUDBG_MAX_ENTITY,
};

struct cudbg_param {
	u16 param_type;
	u16 reserved;
	union {
		struct {
			u32 memtype; /* which memory (EDC0, EDC1, MC) */
			u32 start; /* start of log in firmware memory */
			u32 size; /* size of log */
		} devlog_param;
		struct {
			struct mbox_cmd_log *log;
			u16 mbox_cmds;
		} mboxlog_param;
		struct {
			const char *caller_string;
			u8 os_type;
		} sw_state_param;
		struct {
			u32 itr;
		} yield_param;
		u64 time;
		u8 tcb_bit_param;
		void *adap;
		u8 coreid;
	} u;
};

struct cudbg_init {
	struct adapter *adap; /* Pointer to adapter structure */
	u16                dbg_params_cnt;
	u16                dbg_reserved;
	struct cudbg_param dbg_params[CUDBG_MAX_PARAMS];
	void *outbuf; /* Output buffer */
	u32 outbuf_size;  /* Output buffer size */
	u8 compress_type; /* Type of compression to use */
	void *compress_buff; /* Compression buffer */
	u32 compress_buff_size; /* Compression buffer size */
	void *workspace; /* Workspace for zlib */
};

static inline unsigned int cudbg_mbytes_to_bytes(unsigned int size)
{
	return size * 1024 * 1024;
}
#endif /* __CUDBG_IF_H__ */
