/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#ifndef __CMDQ_SEC_IWC_COMMON_H__
#define __CMDQ_SEC_IWC_COMMON_H__

/**
 * CMDQ_SEC_SHARED_THR_CNT_OFFSET - shared memory offset to store thread count.
 */
#define CMDQ_SEC_SHARED_THR_CNT_OFFSET		0x100

/**
 * CMDQ_TZ_CMD_BLOCK_SIZE - total command buffer size copy from normal world to secure world.
 * Maximum 1 pages will be requested for each command buffer.
 * This size could be adjusted when command buffer size is not enough.
 */
#define CMDQ_TZ_CMD_BLOCK_SIZE		(4096)

/**
 * CMDQ_IWC_MAX_CMD_LENGTH - max length of u32 array to store commanad buffer.
 */
#define CMDQ_IWC_MAX_CMD_LENGTH		(CMDQ_TZ_CMD_BLOCK_SIZE / sizeof(u32))

/**
 * CMDQ_IWC_MAX_ADDR_LIST_LENGTH - max length of addr metadata list.
 */
#define CMDQ_IWC_MAX_ADDR_LIST_LENGTH	(30)

/**
 * CMDQ_IWC_CLIENT_NAME - length for caller_name in iwc_cmdq_command_t.
 */
#define CMDQ_IWC_CLIENT_NAME		(16)

/**
 * CMDQ_MAX_READBACK_ENG - length for readback_engs in iwc_cmdq_command_t.
 */
#define CMDQ_MAX_READBACK_ENG		(8)

/**
 * CMDQ_SEC_MESSAGE_INST_LEN - length for sec_inst in iwc_cmdq_sec_status_t.
 */
#define CMDQ_SEC_MESSAGE_INST_LEN	(8)

/**
 * CMDQ_SEC_DISPATCH_LEN - length for dispatch in iwc_cmdq_sec_status_t.
 */
#define CMDQ_SEC_DISPATCH_LEN		(8)

/*
 * IWC Command IDs - ID for normal world(TLC or linux kernel) to secure world.
 */
#define CMD_CMDQ_IWC_SUBMIT_TASK	(1) /* submit current task */
#define CMD_CMDQ_IWC_CANCEL_TASK	(3) /* cancel current task */
#define CMD_CMDQ_IWC_PATH_RES_ALLOCATE	(4) /* create global resource for secure path */

/**
 * enum cmdq_iwc_addr_metadata_type - address medadata type to be converted in secure world.
 * @CMDQ_IWC_H_2_PA: secure handle to sec PA.
 * @CMDQ_IWC_H_2_MVA: secure handle to sec MVA.
 * @CMDQ_IWC_NMVA_2_MVA: map normal MVA to secure world.
 * @CMDQ_IWC_PH_2_MVA: session protected handle to sec MVA.
 *
 * To tell secure world waht operation to use for converting address in metadata list.
 */
enum cmdq_iwc_addr_metadata_type {
	CMDQ_IWC_H_2_PA		= 0,
	CMDQ_IWC_H_2_MVA	= 1,
	CMDQ_IWC_NMVA_2_MVA	= 2,
	CMDQ_IWC_PH_2_MVA	= 3,
};

/*
 * enum cmdq_sec_engine_enum - the flag for HW engines need to be proteced in secure world.
 * Each enum is a bit in a u64 engine flag variable.
 */
enum cmdq_sec_engine_enum {
	/* MDP */
	CMDQ_SEC_MDP_RDMA0		= 0,
	CMDQ_SEC_MDP_RDMA1		= 1,
	CMDQ_SEC_MDP_WDMA		= 2,
	CMDQ_SEC_MDP_RDMA2		= 3,
	CMDQ_SEC_MDP_RDMA3		= 4,
	CMDQ_SEC_MDP_WROT0		= 5,
	CMDQ_SEC_MDP_WROT1		= 6,
	CMDQ_SEC_MDP_WROT2		= 7,
	CMDQ_SEC_MDP_WROT3		= 8,
	CMDQ_SEC_MDP_HDR0		= 9,
	CMDQ_SEC_MDP_HDR1		= 10,
	CMDQ_SEC_MDP_HDR2		= 11,
	CMDQ_SEC_MDP_HDR3		= 12,
	CMDQ_SEC_MDP_AAL0		= 13,
	CMDQ_SEC_MDP_AAL1		= 14,
	CMDQ_SEC_MDP_AAL2		= 15,
	CMDQ_SEC_MDP_AAL3		= 16,

	/* DISP (VDOSYS0) */
	CMDQ_SEC_DISP_RDMA0		= 17,
	CMDQ_SEC_DISP_RDMA1		= 18,
	CMDQ_SEC_DISP_WDMA0		= 19,
	CMDQ_SEC_DISP_WDMA1		= 20,
	CMDQ_SEC_DISP_OVL0		= 21,
	CMDQ_SEC_DISP_OVL1		= 22,
	CMDQ_SEC_DISP_OVL2		= 23,
	CMDQ_SEC_DISP_2L_OVL0		= 24,
	CMDQ_SEC_DISP_2L_OVL1		= 25,
	CMDQ_SEC_DISP_2L_OVL2		= 26,

	/* DSIP (VDOSYS1) */
	CMDQ_SEC_VDO1_DISP_RDMA_L0	= 27,
	CMDQ_SEC_VDO1_DISP_RDMA_L1	= 28,
	CMDQ_SEC_VDO1_DISP_RDMA_L2	= 29,
	CMDQ_SEC_VDO1_DISP_RDMA_L3	= 30,

	/* VENC */
	CMDQ_SEC_VENC_BSDMA		= 31,
	CMDQ_SEC_VENC_CUR_LUMA		= 32,
	CMDQ_SEC_VENC_CUR_CHROMA	= 33,
	CMDQ_SEC_VENC_REF_LUMA		= 34,
	CMDQ_SEC_VENC_REF_CHROMA	= 35,
	CMDQ_SEC_VENC_REC		= 36,
	CMDQ_SEC_VENC_SUB_R_LUMA	= 37,
	CMDQ_SEC_VENC_SUB_W_LUMA	= 38,
	CMDQ_SEC_VENC_SV_COMV		= 39,
	CMDQ_SEC_VENC_RD_COMV		= 40,
	CMDQ_SEC_VENC_NBM_RDMA		= 41,
	CMDQ_SEC_VENC_NBM_WDMA		= 42,
	CMDQ_SEC_VENC_NBM_RDMA_LITE	= 43,
	CMDQ_SEC_VENC_NBM_WDMA_LITE	= 44,
	CMDQ_SEC_VENC_FCS_NBM_RDMA	= 45,
	CMDQ_SEC_VENC_FCS_NBM_WDMA	= 46,

	CMDQ_SEC_MAX_ENG_COUNT
};

/**
 * struct iwc_cmdq_addr_metadata_t - metadata structure for converting address of secure buffer.
 * @type: addr metadata type.
 * @base_handle: secure address handle.
 * @block_offset: block offset from handle(PA) to current block(plane).
 * @offset: buffser offset to secure handle.
 */
struct iwc_cmdq_addr_metadata_t {
	/**
	 * @type: address medadata type to be converted in secure world.
	 */
	u32 type;

	/**
	 * @base_handle:
	 * @block_offset:
	 * @offset:
	 * these members are used to store the buffer and offset relationship.
	 *
	 *   -------------
	 *   |     |     |
	 *   -------------
	 *   ^     ^  ^  ^
	 *   A     B  C  D
	 *
	 *  A: base_handle
	 *  B: base_handle + block_offset
	 *  C: base_handle + block_offset + offset
	 */
	u64 base_handle;
	u32 block_offset;
	u32 offset;
};

/**
 * struct iwc_cmdq_metadata_t - metadata structure for converting a list of secure buffer address.
 * @addr_list_length: length of metadata address list.
 * @addr_list: array of metadata address list.
 */
struct iwc_cmdq_metadata_t {
	u32 addr_list_length;
	struct iwc_cmdq_addr_metadata_t addr_list[CMDQ_IWC_MAX_ADDR_LIST_LENGTH];
};

/**
 * enum sec_extension_iwc - extension HW engine flag to be protcted in secure world.
 * @IWC_MDP_AAL: for MDP AAL engine.
 * @IWC_MDP_TDSHP: for MDP TDSHP engine.
 */
enum sec_extension_iwc {
	IWC_MDP_AAL = 0,
	IWC_MDP_TDSHP,
};

/**
 * struct readback_engine - readback engine parameters.
 * @engine: HW engine flag for readback.
 * @start: start address pa of readback buffer.
 * @count: u32 size count of readback buffer.
 * @param: other parameters need in secure world.
 */
struct readback_engine {
	u32 engine;
	u32 start;
	u32 count;
	u32 param;
};

/**
 * struct iwc_cmdq_command_t - structure for excuting cmdq task in secure world.
 * @thread: GCE secure thread index to execute command.
 * @scenario: scenario to execute command.
 * @priority: priority of GCE secure thread.
 * @cmd_size: command size used in command buffer.
 * @va_base: command buffer
 * @wait_cookie: index in thread's task list, it should be (nextCookie - 1).
 * @reset_exec: reset HW thread.
 * @metadata: metadata structure for converting a list of secure buffer address.
 * @normal_task_handle: handle to reference task in normal world.
 */
struct iwc_cmdq_command_t {
	/* basic execution data */
	u32 thread;
	u32 scenario;
	u32 priority;
	u32 cmd_size;
	u32 va_base[CMDQ_IWC_MAX_CMD_LENGTH];

	/* exec order data */
	u32 wait_cookie;
	bool reset_exec;

	/* metadata */
	struct iwc_cmdq_metadata_t metadata;

	/* debug */
	u64 normal_task_handle;
};

/**
 * struct iwc_cmdq_cancel_task_t - structure for canceling cmdq task in the secure world.
 * @thread: [IN] GCE secure thread index.
 * @wait_cookie: [IN] execute count cookie to wait.
 * @throw_aee: [OUT] AEE has thrown.
 * @has_reset: [OUT] current secure thread has been reset
 * @irq_status: [OUT] global secure IRQ flag.
 * @irq_flag: [OUT] thread IRQ flag.
 * @err_instr: [OUT] err_instr[0] = instruction low bits, err_instr[1] = instruction high bits.
 * @reg_value: [OUT] value of error register.
 * @pc: [OUT] current pc.
 *
 * used to allocate share memory from secure world.
 */
struct iwc_cmdq_cancel_task_t {
	s32 thread;
	u32 wait_cookie;
	bool throw_aee;
	bool has_reset;
	s32 irq_status;
	s32 irq_flag;
	u32 err_instr[2];
	u32 reg_value;
	u32 pc;
};

/**
 * struct iwc_cmdq_path_resource_t - Inter-World Communication resource allocation structure.
 * @share_memoy_pa: use long long for 64 bit compatible support.
 * @size: size of share memory.
 * @use_normal_irq: use normal IRQ in secure world.
 *
 * used to allocate share memory from secure world.
 */
struct iwc_cmdq_path_resource_t {
	long long share_memoy_pa;
	u32 size;
	bool use_normal_irq;
};

/**
 * struct iwc_cmdq_debug_config_t - debug config structure for secure debug log.
 *
 * @log_level: log level in secure world.
 * @enable_profile: enable profile in secure world.
 */
struct iwc_cmdq_debug_config_t {
	s32 log_level;
	s32 enable_profile;
};

/**
 * struct iwc_cmdq_sec_status_t - secure status from secure world.
 *
 * @step: the step in secure cmdq TA.
 * @status: the status in secure cmdq TA.
 * @args: the status arguments in secure cmdq TA.
 * @sec_inst: current instruction in secure cmdq TA.
 * @inst_index: current instruction index in secure cmdq TA.
 * @dispatch: current HW engine configuring in secure cmdq TA.
 */
struct iwc_cmdq_sec_status_t {
	u32 step;
	s32 status;
	u32 args[4];
	u32 sec_inst[CMDQ_SEC_MESSAGE_INST_LEN];
	u32 inst_index;
	char dispatch[CMDQ_SEC_DISPATCH_LEN];
};

/**
 * struct iwc_cmdq_message_t - Inter-World Communication message structure.
 * @cmd: [IN] iwc command id.
 * @rsp: [OUT] respond from secureworld, 0 for success, < 0 for error.
 * @command: [IN] structure for excuting cmdq task in secure world.
 * @cancel_task: [IN] structure for canceling cmdq task in the secure world.
 * @path_resource: [IN]
 * @debug: [IN] debug config structure for secure debug log.
 * @sec_status: [OUT] secure status from secure world.
 * @cmdq_id: [IN] GCE core id.
 *
 * Both Linex kernel and mobicore have their own MMU tables for mapping
 * world shared memory and physical addresses, so mobicore does not understand
 * linux virtual address mapping.
 * If we want to transact a large buffer in TCI/DCI, there are 2 ways (both require 1 copy):
 * 1. Ue mc_map to map the normal world buffer to WSM and pass secure_virt_addr in TCI/DCI buffer.
 *    Note that mc_map implies a memcopy to copy the content from normal world to WSM.
 * 2. Declare a fixed-length array in TCI/DCI struct and its size must be < 1M.
 */
struct iwc_cmdq_message_t {
	union {
		u32 cmd;
		s32 rsp;
	};

	union {
		struct iwc_cmdq_command_t command;
		struct iwc_cmdq_cancel_task_t cancel_task;
		struct iwc_cmdq_path_resource_t path_resource;
	};

	struct iwc_cmdq_debug_config_t debug;
	struct iwc_cmdq_sec_status_t sec_status;

	u8 cmdq_id;
};
#endif /* __CMDQ_SEC_IWC_COMMON_H__ */
