/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2023-2026 ARM Ltd.
 *
 * The values and structures in this file are from the Realm Management Monitor
 * specification (DEN0137) version 2.0-bet1:
 * https://developer.arm.com/documentation/den0137/2-0bet1/
 */

#ifndef __ASM_RMI_SMC_H
#define __ASM_RMI_SMC_H

#include <linux/arm-smccc.h>

#define SMC_RMI_CALL(func)				\
	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL,		\
			   ARM_SMCCC_SMC_64,		\
			   ARM_SMCCC_OWNER_STANDARD,	\
			   (func))

#define SMC_RMI_VERSION				SMC_RMI_CALL(0x0150)

#define SMC_RMI_RTT_DATA_MAP_INIT		SMC_RMI_CALL(0x0153)

#define SMC_RMI_REALM_ACTIVATE			SMC_RMI_CALL(0x0157)
#define SMC_RMI_REALM_CREATE			SMC_RMI_CALL(0x0158)
#define SMC_RMI_REALM_DESTROY			SMC_RMI_CALL(0x0159)
#define SMC_RMI_REC_CREATE			SMC_RMI_CALL(0x015a)
#define SMC_RMI_REC_DESTROY			SMC_RMI_CALL(0x015b)
#define SMC_RMI_REC_ENTER			SMC_RMI_CALL(0x015c)
#define SMC_RMI_RTT_CREATE			SMC_RMI_CALL(0x015d)
#define SMC_RMI_RTT_DESTROY			SMC_RMI_CALL(0x015e)

#define SMC_RMI_RTT_READ_ENTRY			SMC_RMI_CALL(0x0161)

#define SMC_RMI_RTT_DEV_VALIDATE		SMC_RMI_CALL(0x0163)
#define SMC_RMI_PSCI_COMPLETE			SMC_RMI_CALL(0x0164)
#define SMC_RMI_FEATURES			SMC_RMI_CALL(0x0165)
#define SMC_RMI_RTT_FOLD			SMC_RMI_CALL(0x0166)

#define SMC_RMI_RTT_INIT_RIPAS			SMC_RMI_CALL(0x0168)
#define SMC_RMI_RTT_SET_RIPAS			SMC_RMI_CALL(0x0169)
#define SMC_RMI_VSMMU_CREATE			SMC_RMI_CALL(0x016a)
#define SMC_RMI_VSMMU_DESTROY			SMC_RMI_CALL(0x016b)
#define SMC_RMI_RMM_CONFIG_SET			SMC_RMI_CALL(0x016e)
#define SMC_RMI_PSMMU_IRQ_NOTIFY		SMC_RMI_CALL(0x016f)

#define SMC_RMI_PDEV_ABORT			SMC_RMI_CALL(0x0174)
#define SMC_RMI_PDEV_COMMUNICATE		SMC_RMI_CALL(0x0175)
#define SMC_RMI_PDEV_CREATE			SMC_RMI_CALL(0x0176)
#define SMC_RMI_PDEV_DESTROY			SMC_RMI_CALL(0x0177)
#define SMC_RMI_PDEV_GET_STATE			SMC_RMI_CALL(0x0178)

#define SMC_RMI_PDEV_STREAM_KEY_REFRESH		SMC_RMI_CALL(0x017a)
#define SMC_RMI_PDEV_SET_PUBKEY			SMC_RMI_CALL(0x017b)
#define SMC_RMI_PDEV_STOP			SMC_RMI_CALL(0x017c)
#define SMC_RMI_RTT_AUX_CREATE			SMC_RMI_CALL(0x017d)
#define SMC_RMI_RTT_AUX_DESTROY			SMC_RMI_CALL(0x017e)
#define SMC_RMI_RTT_AUX_FOLD			SMC_RMI_CALL(0x017f)

#define SMC_RMI_VDEV_ABORT			SMC_RMI_CALL(0x0185)
#define SMC_RMI_VDEV_COMMUNICATE		SMC_RMI_CALL(0x0186)
#define SMC_RMI_VDEV_CREATE			SMC_RMI_CALL(0x0187)
#define SMC_RMI_VDEV_DESTROY			SMC_RMI_CALL(0x0188)
#define SMC_RMI_VDEV_GET_STATE			SMC_RMI_CALL(0x0189)
#define SMC_RMI_VDEV_UNLOCK			SMC_RMI_CALL(0x018a)
#define SMC_RMI_RTT_SET_S2AP			SMC_RMI_CALL(0x018b)
#define SMC_RMI_VDEV_COMPLETE			SMC_RMI_CALL(0x018e)

#define SMC_RMI_VDEV_GET_INTERFACE_REPORT	SMC_RMI_CALL(0x01d0)
#define SMC_RMI_VDEV_GET_MEASUREMENTS		SMC_RMI_CALL(0x01d1)
#define SMC_RMI_VDEV_LOCK			SMC_RMI_CALL(0x01d2)
#define SMC_RMI_VDEV_START			SMC_RMI_CALL(0x01d3)

#define SMC_RMI_VSMMU_EVENT_NOTIFY		SMC_RMI_CALL(0x01d6)
#define SMC_RMI_PSMMU_ACTIVATE			SMC_RMI_CALL(0x01d7)
#define SMC_RMI_PSMMU_DEACTIVATE		SMC_RMI_CALL(0x01d8)

#define SMC_RMI_PSMMU_ST_L2_CREATE		SMC_RMI_CALL(0x01db)
#define SMC_RMI_PSMMU_ST_L2_DESTROY		SMC_RMI_CALL(0x01dc)
#define SMC_RMI_DPT_L0_CREATE			SMC_RMI_CALL(0x01dd)
#define SMC_RMI_DPT_L0_DESTROY			SMC_RMI_CALL(0x01de)
#define SMC_RMI_DPT_L1_CREATE			SMC_RMI_CALL(0x01df)
#define SMC_RMI_DPT_L1_DESTROY			SMC_RMI_CALL(0x01e0)
#define SMC_RMI_GRANULE_TRACKING_GET		SMC_RMI_CALL(0x01e1)

#define SMC_RMI_GRANULE_TRACKING_SET		SMC_RMI_CALL(0x01e3)

#define SMC_RMI_RMM_CONFIG_GET			SMC_RMI_CALL(0x01ec)

#define SMC_RMI_RMM_STATE_GET			SMC_RMI_CALL(0x01ee)

#define SMC_RMI_PSMMU_EVENT_CONSUME		SMC_RMI_CALL(0x01f0)
#define SMC_RMI_GRANULE_RANGE_DELEGATE		SMC_RMI_CALL(0x01f1)
#define SMC_RMI_GRANULE_RANGE_UNDELEGATE	SMC_RMI_CALL(0x01f2)
#define SMC_RMI_GPT_L1_CREATE			SMC_RMI_CALL(0x01f3)
#define SMC_RMI_GPT_L1_DESTROY			SMC_RMI_CALL(0x01f4)
#define SMC_RMI_RTT_DATA_MAP			SMC_RMI_CALL(0x01f5)
#define SMC_RMI_RTT_DATA_UNMAP			SMC_RMI_CALL(0x01f6)
#define SMC_RMI_RTT_DEV_MAP			SMC_RMI_CALL(0x01f7)
#define SMC_RMI_RTT_DEV_UNMAP			SMC_RMI_CALL(0x01f8)
#define SMC_RMI_RTT_ARCH_DEV_MAP		SMC_RMI_CALL(0x01f9)
#define SMC_RMI_RTT_ARCH_DEV_UNMAP		SMC_RMI_CALL(0x01fa)
#define SMC_RMI_RTT_UNPROT_MAP			SMC_RMI_CALL(0x01fb)
#define SMC_RMI_RTT_UNPROT_UNMAP		SMC_RMI_CALL(0x01fc)
#define SMC_RMI_RTT_AUX_PROT_MAP		SMC_RMI_CALL(0x01fd)
#define SMC_RMI_RTT_AUX_PROT_UNMAP		SMC_RMI_CALL(0x01fe)
#define SMC_RMI_RTT_AUX_UNPROT_MAP		SMC_RMI_CALL(0x01ff)
#define SMC_RMI_RTT_AUX_UNPROT_UNMAP		SMC_RMI_CALL(0x0200)
#define SMC_RMI_REALM_TERMINATE			SMC_RMI_CALL(0x0201)
#define SMC_RMI_RMM_ACTIVATE			SMC_RMI_CALL(0x0202)
#define SMC_RMI_OP_CONTINUE			SMC_RMI_CALL(0x0203)
#define SMC_RMI_PDEV_STREAM_CONNECT		SMC_RMI_CALL(0x0204)
#define SMC_RMI_PDEV_STREAM_DISCONNECT		SMC_RMI_CALL(0x0205)
#define SMC_RMI_PDEV_STREAM_COMPLETE		SMC_RMI_CALL(0x0206)
#define SMC_RMI_PDEV_STREAM_KEY_PURGE		SMC_RMI_CALL(0x0207)
#define SMC_RMI_OP_MEM_DONATE			SMC_RMI_CALL(0x0208)
#define SMC_RMI_OP_MEM_RECLAIM			SMC_RMI_CALL(0x0209)
#define SMC_RMI_OP_CANCEL			SMC_RMI_CALL(0x020a)
#define SMC_RMI_VSMMU_FEATURES			SMC_RMI_CALL(0x020b)
#define SMC_RMI_VSMMU_CMD_GET			SMC_RMI_CALL(0x020c)
#define SMC_RMI_VSMMU_CMD_COMPLETE		SMC_RMI_CALL(0x020d)
#define SMC_RMI_PSMMU_INFO			SMC_RMI_CALL(0x020e)

#define RMI_ABI_MAJOR_VERSION	2
#define RMI_ABI_MINOR_VERSION	0

#define RMI_ABI_VERSION_GET_MAJOR(version) ((version) >> 16)
#define RMI_ABI_VERSION_GET_MINOR(version) ((version) & 0xFFFF)
#define RMI_ABI_VERSION(major, minor)      (((major) << 16) | (minor))

#define RMI_UNASSIGNED			0
#define RMI_ASSIGNED			1
#define RMI_TABLE			2

#define RMI_RETURN_STATUS(ret)		((ret) & 0xFF)
#define RMI_RETURN_INDEX(ret)		(((ret) >> 8) & 0xFF)
#define RMI_RETURN_MEMREQ(ret)		(((ret) >> 8) & 0x3)
#define RMI_RETURN_CAN_CANCEL(ret)	(((ret) >> 10) & 0x1)

#define RMI_SUCCESS			0
#define RMI_ERROR_INPUT			1
#define RMI_ERROR_REALM			2
#define RMI_ERROR_REC			3
#define RMI_ERROR_RTT			4
#define RMI_ERROR_NOT_SUPPORTED		5
#define RMI_ERROR_DEVICE		6
#define RMI_ERROR_RTT_AUX		7
#define RMI_ERROR_PSMMU_ST		8
#define RMI_ERROR_DPT			9
#define RMI_BUSY			10
#define RMI_ERROR_GLOBAL		11
#define RMI_ERROR_TRACKING		12
#define RMI_INCOMPLETE			13
#define RMI_BLOCKED			14
#define RMI_ERROR_GPT			15
#define RMI_ERROR_GRANULE		16

#define RMI_OP_MEM_REQ_NONE		0
#define RMI_OP_MEM_REQ_DONATE		1
#define RMI_OP_MEM_REQ_RECLAIM		2

#define RMI_DONATE_SIZE(req)		((req) & 0x3)
#define RMI_DONATE_COUNT_MASK		GENMASK(15, 2)
#define RMI_DONATE_COUNT(req)		(((req) & RMI_DONATE_COUNT_MASK) >> 2)
#define RMI_DONATE_CONTIG(req)		(!!((req) & BIT(16)))
#define RMI_DONATE_STATE(req)		(!!((req) & BIT(17)))

#define RMI_OP_MEM_DELEGATED		0
#define RMI_OP_MEM_UNDELEGATED		1

#define RMI_ADDR_TYPE_NONE		0
#define RMI_ADDR_TYPE_SINGLE		1
#define RMI_ADDR_TYPE_LIST		2

#define RMI_ADDR_RANGE_SIZE_MASK	GENMASK(1, 0)
#define RMI_ADDR_RANGE_COUNT_MASK	GENMASK(PAGE_SHIFT - 1, 2)
#define RMI_ADDR_RANGE_ADDR_MASK	(PAGE_MASK & GENMASK(51, 0))
#define RMI_ADDR_RANGE_STATE_MASK	BIT(63)

#define RMI_ADDR_RANGE_SIZE(ar)		(FIELD_GET(RMI_ADDR_RANGE_SIZE_MASK, \
						   (ar)))
#define RMI_ADDR_RANGE_COUNT(ar)	(FIELD_GET(RMI_ADDR_RANGE_COUNT_MASK, \
						   (ar)))
#define RMI_ADDR_RANGE_ADDR(ar)		((ar) & RMI_ADDR_RANGE_ADDR_MASK)
#define RMI_ADDR_RANGE_STATE(ar)	(FIELD_GET(RMI_ADDR_RANGE_STATE_MASK, \
						   (ar)))

enum rmi_ripas {
	RMI_EMPTY = 0,
	RMI_RAM = 1,
	RMI_DESTROYED = 2,
	RMI_DEV = 3,
};

#define RMI_NO_MEASURE_CONTENT	0
#define RMI_MEASURE_CONTENT	1

#define RMI_FEATURE_REGISTER_0_S2SZ		GENMASK(7, 0)
#define RMI_FEATURE_REGISTER_0_LPA2		BIT(8)
#define RMI_FEATURE_REGISTER_0_SVE		BIT(9)
#define RMI_FEATURE_REGISTER_0_SVE_VL		GENMASK(13, 10)
#define RMI_FEATURE_REGISTER_0_NUM_BPS		GENMASK(19, 14)
#define RMI_FEATURE_REGISTER_0_NUM_WPS		GENMASK(25, 20)
#define RMI_FEATURE_REGISTER_0_PMU		BIT(26)
#define RMI_FEATURE_REGISTER_0_PMU_NUM_CTRS	GENMASK(31, 27)

#define RMI_FEATURE_REGISTER_1_RMI_GRAN_SZ_4KB	BIT(0)
#define RMI_FEATURE_REGISTER_1_RMI_GRAN_SZ_16KB	BIT(1)
#define RMI_FEATURE_REGISTER_1_RMI_GRAN_SZ_64KB	BIT(2)
#define RMI_FEATURE_REGISTER_1_HASH_SHA_256	BIT(3)
#define RMI_FEATURE_REGISTER_1_HASH_SHA_384	BIT(4)
#define RMI_FEATURE_REGISTER_1_HASH_SHA_512	BIT(5)
#define RMI_FEATURE_REGISTER_1_MAX_RECS_ORDER	GENMASK(9, 6)
#define RMI_FEATURE_REGISTER_1_L0GPTSZ		GENMASK(13, 10)
#define RMI_FEATURE_REGISTER_1_PPS		GENMASK(16, 14)

#define RMI_FEATURE_REGISTER_2_DA		BIT(0)
#define RMI_FEATURE_REGISTER_2_DA_COH		BIT(1)
#define RMI_FEATURE_REGISTER_2_VSMMU		BIT(2)
#define RMI_FEATURE_REGISTER_2_ATS		BIT(3)
#define RMI_FEATURE_REGISTER_2_MAX_VDEVS_ORDER	GENMASK(7, 4)
#define RMI_FEATURE_REGISTER_2_VDEV_KROU	BIT(8)
#define RMI_FEATURE_REGISTER_2_NON_TEE_STREAM	BIT(9)

#define RMI_FEATURE_REGISTER_3_MAX_NUM_AUX_PLANES	GENMASK(3, 0)
#define RMI_FEATURE_REGISTER_3_RTT_PLAN			GENMASK(5, 4)
#define RMI_FEATURE_REGISTER_3_RTT_S2AP_INDIRECT	BIT(6)

#define RMI_FEATURE_REGISTER_4_MEC_COUNT		GENMASK(63, 0)

#define RMI_MEM_CATEGORY_CONVENTIONAL		0
#define RMI_MEM_CATEGORY_DEV_NCOH		1
#define RMI_MEM_CATEGORY_DEV_COH		2

#define RMI_TRACKING_RESERVED			0
#define RMI_TRACKING_NONE			1
#define RMI_TRACKING_FINE			2
#define RMI_TRACKING_COARSE			3

#define RMI_GRANULE_SIZE_4KB	0
#define RMI_GRANULE_SIZE_16KB	1
#define RMI_GRANULE_SIZE_64KB	2

/*
 * Note many of these fields are smaller than u64 but all fields have u64
 * alignment, so use u64 to ensure correct alignment.
 */
struct rmm_config {
	union { /* 0x0 */
		struct {
			u64 tracking_region_size;
			u64 rmi_granule_size;
		};
		u8 sizer[0x1000];
	};
};

#define RMI_REALM_PARAM_FLAG_LPA2		BIT(0)
#define RMI_REALM_PARAM_FLAG_SVE		BIT(1)
#define RMI_REALM_PARAM_FLAG_PMU		BIT(2)

struct realm_params {
	union { /* 0x0 */
		struct {
			u64 flags;
			u64 s2sz;
			u64 sve_vl;
			u64 num_bps;
			u64 num_wps;
			u64 pmu_num_ctrs;
			u64 hash_algo;
			u64 num_aux_planes;
		};
		u8 padding0[0x400];
	};
	union { /* 0x400 */
		struct {
			u8 rpv[64];
			u64 ats_plane;
		};
		u8 padding1[0x400];
	};
	union { /* 0x800 */
		struct {
			u64 padding;
			u64 rtt_base;
			s64 rtt_level_start;
			u64 rtt_num_start;
			u64 flags1;
			u64 aux_rtt_base[3];
		};
		u8 padding2[0x800];
	};
};

/*
 * The number of GPRs (starting from X0) that are
 * configured by the host when a REC is created.
 */
#define REC_CREATE_NR_GPRS		8

#define REC_PARAMS_FLAG_RUNNABLE	BIT_ULL(0)

struct rec_params {
	union { /* 0x0 */
		u64 flags;
		u8 padding0[0x100];
	};
	union { /* 0x100 */
		u64 mpidr;
		u8 padding1[0x100];
	};
	union { /* 0x200 */
		u64 pc;
		u8 padding2[0x100];
	};
	union { /* 0x300 */
		u64 gprs[REC_CREATE_NR_GPRS];
		u8 padding3[0xd00];
	};
};

#define REC_ENTER_FLAG_EMULATED_MMIO	BIT(0)
#define REC_ENTER_FLAG_INJECT_SEA	BIT(1)
#define REC_ENTER_FLAG_TRAP_WFI		BIT(2)
#define REC_ENTER_FLAG_TRAP_WFE		BIT(3)
#define REC_ENTER_FLAG_RIPAS_RESPONSE	BIT(4)
#define REC_ENTER_FLAG_S2AP_RESPONSE	BIT(5)
#define REC_ENTER_FLAG_DEV_MEM_RESPONSE	BIT(6)
#define REC_ENTER_FLAG_FORCE_P0		BIT(7)

#define REC_RUN_GPRS			31
#define REC_MAX_GIC_NUM_LRS		16

#define RMI_PERMITTED_GICV3_HCR_BITS	(ICH_HCR_EL2_UIE |		\
					 ICH_HCR_EL2_LRENPIE |		\
					 ICH_HCR_EL2_NPIE |		\
					 ICH_HCR_EL2_VGrp0EIE |		\
					 ICH_HCR_EL2_VGrp0DIE |		\
					 ICH_HCR_EL2_VGrp1EIE |		\
					 ICH_HCR_EL2_VGrp1DIE |		\
					 ICH_HCR_EL2_TDIR)

struct rec_enter {
	union { /* 0x000 */
		u64 flags;
		u8 padding0[0x200];
	};
	union { /* 0x200 */
		u64 gprs[REC_RUN_GPRS];
		u8 padding1[0x100];
	};
	u8 padding3[0x500];
};

#define RMI_EXIT_SYNC			0x00
#define RMI_EXIT_IRQ			0x01
#define RMI_EXIT_FIQ			0x02
#define RMI_EXIT_PSCI			0x03
#define RMI_EXIT_RIPAS_CHANGE		0x04
#define RMI_EXIT_HOST_CALL		0x05
#define RMI_EXIT_SERROR			0x06
#define RMI_EXIT_S2AP_CHANGE		0x07
#define RMI_EXIT_VDEV_REQUEST		0x08
#define RMI_EXIT_VDEV_VALIDATE_MAPPING	0x09
#define RMI_EXIT_VSMMU_COMMAND		0x0a

struct rec_exit {
	union { /* 0x000 */
		u8 exit_reason;
		u8 padding0[0x100];
	};
	union { /* 0x100 */
		struct {
			u64 esr;
			u64 far;
			u64 hpfar;
			u64 rtt_tree;
		};
		u8 padding1[0x100];
	};
	union { /* 0x200 */
		u64 gprs[REC_RUN_GPRS];
		u8 padding2[0x100];
	};
	union { /* 0x300 */
		u8 padding3[0x100];
	};
	union { /* 0x400 */
		struct {
			u64 cntp_ctl;
			u64 cntp_cval;
			u64 cntv_ctl;
			u64 cntv_cval;
		};
		u8 padding4[0x100];
	};
	union { /* 0x500 */
		struct {
			u64 ripas_base;
			u64 ripas_top;
			u8 ripas_value;
			u8 padding8[15];
			u64 s2ap_base;
			u64 s2ap_top;
			u64 vdev_id_1;
			u64 vdev_id_2;
			u64 dev_mem_base;
			u64 dev_mem_top;
			u64 dev_mem_pa;
		};
		u8 padding5[0x100];
	};
	union { /* 0x600 */
		struct {
			u16 imm;
			u16 padding9;
			u64 plane;
		};
		u8 padding6[0x100];
	};
	union { /* 0x700 */
		struct {
			u8 pmu_ovf_status;
			u8 padding10[15];
			u64 vsmmu;
		};
		u8 padding7[0x100];
	};
};

struct rec_run {
	struct rec_enter enter;
	struct rec_exit exit;
};

/* RMI_RTT_UNPROT_MAP_FLAGS definitions */
#define RMI_RTT_UNPROT_MAP_FLAGS_OADDR_TYPE	GENMASK(1, 0)
#define RMI_RTT_UNPROT_MAP_FLAGS_LIST_COUNT	GENMASK(15, 2)
#define RMI_RTT_UNPROT_MAP_FLAGS_MEMATTR	GENMASK(18, 16)
#define RMI_RTT_UNPROT_MAP_FLAGS_S2AP		GENMASK(22, 19)

/* S2AP Direct Encodings, used in RMI_RTT_UNPROT_MAP_FLAGS_S2AP */
#define RMI_S2AP_DIRECT_WRITE			BIT(0)
#define RMI_S2AP_DIRECT_READ			BIT(1)

#endif /* __ASM_RMI_SMC_H */
