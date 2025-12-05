/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Type definitions for the Microsoft hypervisor.
 */
#ifndef _HV_HVHDK_H
#define _HV_HVHDK_H

#include <linux/build_bug.h>

#include "hvhdk_mini.h"
#include "hvgdk.h"

enum hv_stats_hypervisor_counters {		/* HV_HYPERVISOR_COUNTER */
	HvLogicalProcessors			= 1,
	HvPartitions				= 2,
	HvTotalPages				= 3,
	HvVirtualProcessors			= 4,
	HvMonitoredNotifications		= 5,
	HvModernStandbyEntries			= 6,
	HvPlatformIdleTransitions		= 7,
	HvHypervisorStartupCost			= 8,
	HvIOSpacePages				= 10,
	HvNonEssentialPagesForDump		= 11,
	HvSubsumedPages				= 12,
	HvStatsMaxCounter
};

enum hv_stats_partition_counters {		/* HV_PROCESS_COUNTER */
	PartitionVirtualProcessors		= 1,
	PartitionTlbSize			= 3,
	PartitionAddressSpaces			= 4,
	PartitionDepositedPages			= 5,
	PartitionGpaPages			= 6,
	PartitionGpaSpaceModifications		= 7,
	PartitionVirtualTlbFlushEntires		= 8,
	PartitionRecommendedTlbSize		= 9,
	PartitionGpaPages4K			= 10,
	PartitionGpaPages2M			= 11,
	PartitionGpaPages1G			= 12,
	PartitionGpaPages512G			= 13,
	PartitionDevicePages4K			= 14,
	PartitionDevicePages2M			= 15,
	PartitionDevicePages1G			= 16,
	PartitionDevicePages512G		= 17,
	PartitionAttachedDevices		= 18,
	PartitionDeviceInterruptMappings	= 19,
	PartitionIoTlbFlushes			= 20,
	PartitionIoTlbFlushCost			= 21,
	PartitionDeviceInterruptErrors		= 22,
	PartitionDeviceDmaErrors		= 23,
	PartitionDeviceInterruptThrottleEvents	= 24,
	PartitionSkippedTimerTicks		= 25,
	PartitionPartitionId			= 26,
#if IS_ENABLED(CONFIG_X86_64)
	PartitionNestedTlbSize			= 27,
	PartitionRecommendedNestedTlbSize	= 28,
	PartitionNestedTlbFreeListSize		= 29,
	PartitionNestedTlbTrimmedPages		= 30,
	PartitionPagesShattered			= 31,
	PartitionPagesRecombined		= 32,
	PartitionHwpRequestValue		= 33,
#elif IS_ENABLED(CONFIG_ARM64)
	PartitionHwpRequestValue		= 27,
#endif
	PartitionStatsMaxCounter
};

enum hv_stats_vp_counters {			/* HV_THREAD_COUNTER */
	VpTotalRunTime					= 1,
	VpHypervisorRunTime				= 2,
	VpRemoteNodeRunTime				= 3,
	VpNormalizedRunTime				= 4,
	VpIdealCpu					= 5,
	VpHypercallsCount				= 7,
	VpHypercallsTime				= 8,
#if IS_ENABLED(CONFIG_X86_64)
	VpPageInvalidationsCount			= 9,
	VpPageInvalidationsTime				= 10,
	VpControlRegisterAccessesCount			= 11,
	VpControlRegisterAccessesTime			= 12,
	VpIoInstructionsCount				= 13,
	VpIoInstructionsTime				= 14,
	VpHltInstructionsCount				= 15,
	VpHltInstructionsTime				= 16,
	VpMwaitInstructionsCount			= 17,
	VpMwaitInstructionsTime				= 18,
	VpCpuidInstructionsCount			= 19,
	VpCpuidInstructionsTime				= 20,
	VpMsrAccessesCount				= 21,
	VpMsrAccessesTime				= 22,
	VpOtherInterceptsCount				= 23,
	VpOtherInterceptsTime				= 24,
	VpExternalInterruptsCount			= 25,
	VpExternalInterruptsTime			= 26,
	VpPendingInterruptsCount			= 27,
	VpPendingInterruptsTime				= 28,
	VpEmulatedInstructionsCount			= 29,
	VpEmulatedInstructionsTime			= 30,
	VpDebugRegisterAccessesCount			= 31,
	VpDebugRegisterAccessesTime			= 32,
	VpPageFaultInterceptsCount			= 33,
	VpPageFaultInterceptsTime			= 34,
	VpGuestPageTableMaps				= 35,
	VpLargePageTlbFills				= 36,
	VpSmallPageTlbFills				= 37,
	VpReflectedGuestPageFaults			= 38,
	VpApicMmioAccesses				= 39,
	VpIoInterceptMessages				= 40,
	VpMemoryInterceptMessages			= 41,
	VpApicEoiAccesses				= 42,
	VpOtherMessages					= 43,
	VpPageTableAllocations				= 44,
	VpLogicalProcessorMigrations			= 45,
	VpAddressSpaceEvictions				= 46,
	VpAddressSpaceSwitches				= 47,
	VpAddressDomainFlushes				= 48,
	VpAddressSpaceFlushes				= 49,
	VpGlobalGvaRangeFlushes				= 50,
	VpLocalGvaRangeFlushes				= 51,
	VpPageTableEvictions				= 52,
	VpPageTableReclamations				= 53,
	VpPageTableResets				= 54,
	VpPageTableValidations				= 55,
	VpApicTprAccesses				= 56,
	VpPageTableWriteIntercepts			= 57,
	VpSyntheticInterrupts				= 58,
	VpVirtualInterrupts				= 59,
	VpApicIpisSent					= 60,
	VpApicSelfIpisSent				= 61,
	VpGpaSpaceHypercalls				= 62,
	VpLogicalProcessorHypercalls			= 63,
	VpLongSpinWaitHypercalls			= 64,
	VpOtherHypercalls				= 65,
	VpSyntheticInterruptHypercalls			= 66,
	VpVirtualInterruptHypercalls			= 67,
	VpVirtualMmuHypercalls				= 68,
	VpVirtualProcessorHypercalls			= 69,
	VpHardwareInterrupts				= 70,
	VpNestedPageFaultInterceptsCount		= 71,
	VpNestedPageFaultInterceptsTime			= 72,
	VpPageScans					= 73,
	VpLogicalProcessorDispatches			= 74,
	VpWaitingForCpuTime				= 75,
	VpExtendedHypercalls				= 76,
	VpExtendedHypercallInterceptMessages		= 77,
	VpMbecNestedPageTableSwitches			= 78,
	VpOtherReflectedGuestExceptions			= 79,
	VpGlobalIoTlbFlushes				= 80,
	VpGlobalIoTlbFlushCost				= 81,
	VpLocalIoTlbFlushes				= 82,
	VpLocalIoTlbFlushCost				= 83,
	VpHypercallsForwardedCount			= 84,
	VpHypercallsForwardingTime			= 85,
	VpPageInvalidationsForwardedCount		= 86,
	VpPageInvalidationsForwardingTime		= 87,
	VpControlRegisterAccessesForwardedCount		= 88,
	VpControlRegisterAccessesForwardingTime		= 89,
	VpIoInstructionsForwardedCount			= 90,
	VpIoInstructionsForwardingTime			= 91,
	VpHltInstructionsForwardedCount			= 92,
	VpHltInstructionsForwardingTime			= 93,
	VpMwaitInstructionsForwardedCount		= 94,
	VpMwaitInstructionsForwardingTime		= 95,
	VpCpuidInstructionsForwardedCount		= 96,
	VpCpuidInstructionsForwardingTime		= 97,
	VpMsrAccessesForwardedCount			= 98,
	VpMsrAccessesForwardingTime			= 99,
	VpOtherInterceptsForwardedCount			= 100,
	VpOtherInterceptsForwardingTime			= 101,
	VpExternalInterruptsForwardedCount		= 102,
	VpExternalInterruptsForwardingTime		= 103,
	VpPendingInterruptsForwardedCount		= 104,
	VpPendingInterruptsForwardingTime		= 105,
	VpEmulatedInstructionsForwardedCount		= 106,
	VpEmulatedInstructionsForwardingTime		= 107,
	VpDebugRegisterAccessesForwardedCount		= 108,
	VpDebugRegisterAccessesForwardingTime		= 109,
	VpPageFaultInterceptsForwardedCount		= 110,
	VpPageFaultInterceptsForwardingTime		= 111,
	VpVmclearEmulationCount				= 112,
	VpVmclearEmulationTime				= 113,
	VpVmptrldEmulationCount				= 114,
	VpVmptrldEmulationTime				= 115,
	VpVmptrstEmulationCount				= 116,
	VpVmptrstEmulationTime				= 117,
	VpVmreadEmulationCount				= 118,
	VpVmreadEmulationTime				= 119,
	VpVmwriteEmulationCount				= 120,
	VpVmwriteEmulationTime				= 121,
	VpVmxoffEmulationCount				= 122,
	VpVmxoffEmulationTime				= 123,
	VpVmxonEmulationCount				= 124,
	VpVmxonEmulationTime				= 125,
	VpNestedVMEntriesCount				= 126,
	VpNestedVMEntriesTime				= 127,
	VpNestedSLATSoftPageFaultsCount			= 128,
	VpNestedSLATSoftPageFaultsTime			= 129,
	VpNestedSLATHardPageFaultsCount			= 130,
	VpNestedSLATHardPageFaultsTime			= 131,
	VpInvEptAllContextEmulationCount		= 132,
	VpInvEptAllContextEmulationTime			= 133,
	VpInvEptSingleContextEmulationCount		= 134,
	VpInvEptSingleContextEmulationTime		= 135,
	VpInvVpidAllContextEmulationCount		= 136,
	VpInvVpidAllContextEmulationTime		= 137,
	VpInvVpidSingleContextEmulationCount		= 138,
	VpInvVpidSingleContextEmulationTime		= 139,
	VpInvVpidSingleAddressEmulationCount		= 140,
	VpInvVpidSingleAddressEmulationTime		= 141,
	VpNestedTlbPageTableReclamations		= 142,
	VpNestedTlbPageTableEvictions			= 143,
	VpFlushGuestPhysicalAddressSpaceHypercalls	= 144,
	VpFlushGuestPhysicalAddressListHypercalls	= 145,
	VpPostedInterruptNotifications			= 146,
	VpPostedInterruptScans				= 147,
	VpTotalCoreRunTime				= 148,
	VpMaximumRunTime				= 149,
	VpHwpRequestContextSwitches			= 150,
	VpWaitingForCpuTimeBucket0			= 151,
	VpWaitingForCpuTimeBucket1			= 152,
	VpWaitingForCpuTimeBucket2			= 153,
	VpWaitingForCpuTimeBucket3			= 154,
	VpWaitingForCpuTimeBucket4			= 155,
	VpWaitingForCpuTimeBucket5			= 156,
	VpWaitingForCpuTimeBucket6			= 157,
	VpVmloadEmulationCount				= 158,
	VpVmloadEmulationTime				= 159,
	VpVmsaveEmulationCount				= 160,
	VpVmsaveEmulationTime				= 161,
	VpGifInstructionEmulationCount			= 162,
	VpGifInstructionEmulationTime			= 163,
	VpEmulatedErrataSvmInstructions			= 164,
	VpPlaceholder1					= 165,
	VpPlaceholder2					= 166,
	VpPlaceholder3					= 167,
	VpPlaceholder4					= 168,
	VpPlaceholder5					= 169,
	VpPlaceholder6					= 170,
	VpPlaceholder7					= 171,
	VpPlaceholder8					= 172,
	VpPlaceholder9					= 173,
	VpPlaceholder10					= 174,
	VpSchedulingPriority				= 175,
	VpRdpmcInstructionsCount			= 176,
	VpRdpmcInstructionsTime				= 177,
	VpPerfmonPmuMsrAccessesCount			= 178,
	VpPerfmonLbrMsrAccessesCount			= 179,
	VpPerfmonIptMsrAccessesCount			= 180,
	VpPerfmonInterruptCount				= 181,
	VpVtl1DispatchCount				= 182,
	VpVtl2DispatchCount				= 183,
	VpVtl2DispatchBucket0				= 184,
	VpVtl2DispatchBucket1				= 185,
	VpVtl2DispatchBucket2				= 186,
	VpVtl2DispatchBucket3				= 187,
	VpVtl2DispatchBucket4				= 188,
	VpVtl2DispatchBucket5				= 189,
	VpVtl2DispatchBucket6				= 190,
	VpVtl1RunTime					= 191,
	VpVtl2RunTime					= 192,
	VpIommuHypercalls				= 193,
	VpCpuGroupHypercalls				= 194,
	VpVsmHypercalls					= 195,
	VpEventLogHypercalls				= 196,
	VpDeviceDomainHypercalls			= 197,
	VpDepositHypercalls				= 198,
	VpSvmHypercalls					= 199,
	VpBusLockAcquisitionCount			= 200,
	VpUnused					= 201,
	VpRootDispatchThreadBlocked			= 202,
#elif IS_ENABLED(CONFIG_ARM64)
	VpSysRegAccessesCount				= 9,
	VpSysRegAccessesTime				= 10,
	VpSmcInstructionsCount				= 11,
	VpSmcInstructionsTime				= 12,
	VpOtherInterceptsCount				= 13,
	VpOtherInterceptsTime				= 14,
	VpExternalInterruptsCount			= 15,
	VpExternalInterruptsTime			= 16,
	VpPendingInterruptsCount			= 17,
	VpPendingInterruptsTime				= 18,
	VpGuestPageTableMaps				= 19,
	VpLargePageTlbFills				= 20,
	VpSmallPageTlbFills				= 21,
	VpReflectedGuestPageFaults			= 22,
	VpMemoryInterceptMessages			= 23,
	VpOtherMessages					= 24,
	VpLogicalProcessorMigrations			= 25,
	VpAddressDomainFlushes				= 26,
	VpAddressSpaceFlushes				= 27,
	VpSyntheticInterrupts				= 28,
	VpVirtualInterrupts				= 29,
	VpApicSelfIpisSent				= 30,
	VpGpaSpaceHypercalls				= 31,
	VpLogicalProcessorHypercalls			= 32,
	VpLongSpinWaitHypercalls			= 33,
	VpOtherHypercalls				= 34,
	VpSyntheticInterruptHypercalls			= 35,
	VpVirtualInterruptHypercalls			= 36,
	VpVirtualMmuHypercalls				= 37,
	VpVirtualProcessorHypercalls			= 38,
	VpHardwareInterrupts				= 39,
	VpNestedPageFaultInterceptsCount		= 40,
	VpNestedPageFaultInterceptsTime			= 41,
	VpLogicalProcessorDispatches			= 42,
	VpWaitingForCpuTime				= 43,
	VpExtendedHypercalls				= 44,
	VpExtendedHypercallInterceptMessages		= 45,
	VpMbecNestedPageTableSwitches			= 46,
	VpOtherReflectedGuestExceptions			= 47,
	VpGlobalIoTlbFlushes				= 48,
	VpGlobalIoTlbFlushCost				= 49,
	VpLocalIoTlbFlushes				= 50,
	VpLocalIoTlbFlushCost				= 51,
	VpFlushGuestPhysicalAddressSpaceHypercalls	= 52,
	VpFlushGuestPhysicalAddressListHypercalls	= 53,
	VpPostedInterruptNotifications			= 54,
	VpPostedInterruptScans				= 55,
	VpTotalCoreRunTime				= 56,
	VpMaximumRunTime				= 57,
	VpWaitingForCpuTimeBucket0			= 58,
	VpWaitingForCpuTimeBucket1			= 59,
	VpWaitingForCpuTimeBucket2			= 60,
	VpWaitingForCpuTimeBucket3			= 61,
	VpWaitingForCpuTimeBucket4			= 62,
	VpWaitingForCpuTimeBucket5			= 63,
	VpWaitingForCpuTimeBucket6			= 64,
	VpHwpRequestContextSwitches			= 65,
	VpPlaceholder2					= 66,
	VpPlaceholder3					= 67,
	VpPlaceholder4					= 68,
	VpPlaceholder5					= 69,
	VpPlaceholder6					= 70,
	VpPlaceholder7					= 71,
	VpPlaceholder8					= 72,
	VpContentionTime				= 73,
	VpWakeUpTime					= 74,
	VpSchedulingPriority				= 75,
	VpVtl1DispatchCount				= 76,
	VpVtl2DispatchCount				= 77,
	VpVtl2DispatchBucket0				= 78,
	VpVtl2DispatchBucket1				= 79,
	VpVtl2DispatchBucket2				= 80,
	VpVtl2DispatchBucket3				= 81,
	VpVtl2DispatchBucket4				= 82,
	VpVtl2DispatchBucket5				= 83,
	VpVtl2DispatchBucket6				= 84,
	VpVtl1RunTime					= 85,
	VpVtl2RunTime					= 86,
	VpIommuHypercalls				= 87,
	VpCpuGroupHypercalls				= 88,
	VpVsmHypercalls					= 89,
	VpEventLogHypercalls				= 90,
	VpDeviceDomainHypercalls			= 91,
	VpDepositHypercalls				= 92,
	VpSvmHypercalls					= 93,
	VpLoadAvg					= 94,
	VpRootDispatchThreadBlocked			= 95,
#endif
	VpStatsMaxCounter
};

enum hv_stats_lp_counters {			/* HV_CPU_COUNTER */
	LpGlobalTime				= 1,
	LpTotalRunTime				= 2,
	LpHypervisorRunTime			= 3,
	LpHardwareInterrupts			= 4,
	LpContextSwitches			= 5,
	LpInterProcessorInterrupts		= 6,
	LpSchedulerInterrupts			= 7,
	LpTimerInterrupts			= 8,
	LpInterProcessorInterruptsSent		= 9,
	LpProcessorHalts			= 10,
	LpMonitorTransitionCost			= 11,
	LpContextSwitchTime			= 12,
	LpC1TransitionsCount			= 13,
	LpC1RunTime				= 14,
	LpC2TransitionsCount			= 15,
	LpC2RunTime				= 16,
	LpC3TransitionsCount			= 17,
	LpC3RunTime				= 18,
	LpRootVpIndex				= 19,
	LpIdleSequenceNumber			= 20,
	LpGlobalTscCount			= 21,
	LpActiveTscCount			= 22,
	LpIdleAccumulation			= 23,
	LpReferenceCycleCount0			= 24,
	LpActualCycleCount0			= 25,
	LpReferenceCycleCount1			= 26,
	LpActualCycleCount1			= 27,
	LpProximityDomainId			= 28,
	LpPostedInterruptNotifications		= 29,
	LpBranchPredictorFlushes		= 30,
#if IS_ENABLED(CONFIG_X86_64)
	LpL1DataCacheFlushes			= 31,
	LpImmediateL1DataCacheFlushes		= 32,
	LpMbFlushes				= 33,
	LpCounterRefreshSequenceNumber		= 34,
	LpCounterRefreshReferenceTime		= 35,
	LpIdleAccumulationSnapshot		= 36,
	LpActiveTscCountSnapshot		= 37,
	LpHwpRequestContextSwitches		= 38,
	LpPlaceholder1				= 39,
	LpPlaceholder2				= 40,
	LpPlaceholder3				= 41,
	LpPlaceholder4				= 42,
	LpPlaceholder5				= 43,
	LpPlaceholder6				= 44,
	LpPlaceholder7				= 45,
	LpPlaceholder8				= 46,
	LpPlaceholder9				= 47,
	LpPlaceholder10				= 48,
	LpReserveGroupId			= 49,
	LpRunningPriority			= 50,
	LpPerfmonInterruptCount			= 51,
#elif IS_ENABLED(CONFIG_ARM64)
	LpCounterRefreshSequenceNumber		= 31,
	LpCounterRefreshReferenceTime		= 32,
	LpIdleAccumulationSnapshot		= 33,
	LpActiveTscCountSnapshot		= 34,
	LpHwpRequestContextSwitches		= 35,
	LpPlaceholder2				= 36,
	LpPlaceholder3				= 37,
	LpPlaceholder4				= 38,
	LpPlaceholder5				= 39,
	LpPlaceholder6				= 40,
	LpPlaceholder7				= 41,
	LpPlaceholder8				= 42,
	LpPlaceholder9				= 43,
	LpSchLocalRunListSize			= 44,
	LpReserveGroupId			= 45,
	LpRunningPriority			= 46,
#endif
	LpStatsMaxCounter
};

/*
 * Hypervisor statsitics page format
 */
struct hv_stats_page {
	union {
		u64 hv_cntrs[HvStatsMaxCounter];		/* Hypervisor counters */
		u64 pt_cntrs[PartitionStatsMaxCounter];		/* Partition counters */
		u64 vp_cntrs[VpStatsMaxCounter];		/* VP counters */
		u64 lp_cntrs[LpStatsMaxCounter];		/* LP counters */
		u8 data[HV_HYP_PAGE_SIZE];
	};
} __packed;

/* Bits for dirty mask of hv_vp_register_page */
#define HV_X64_REGISTER_CLASS_GENERAL	0
#define HV_X64_REGISTER_CLASS_IP	1
#define HV_X64_REGISTER_CLASS_XMM	2
#define HV_X64_REGISTER_CLASS_SEGMENT	3
#define HV_X64_REGISTER_CLASS_FLAGS	4

#define HV_VP_REGISTER_PAGE_VERSION_1	1u

#define HV_VP_REGISTER_PAGE_MAX_VECTOR_COUNT		7

union hv_vp_register_page_interrupt_vectors {
	u64 as_uint64;
	struct {
		u8 vector_count;
		u8 vector[HV_VP_REGISTER_PAGE_MAX_VECTOR_COUNT];
	} __packed;
};

struct hv_vp_register_page {
	u16 version;
	u8 isvalid;
	u8 rsvdz;
	u32 dirty;

#if IS_ENABLED(CONFIG_X86)

	union {
		struct {
			/* General purpose registers
			 * (HV_X64_REGISTER_CLASS_GENERAL)
			 */
			union {
				struct {
					u64 rax;
					u64 rcx;
					u64 rdx;
					u64 rbx;
					u64 rsp;
					u64 rbp;
					u64 rsi;
					u64 rdi;
					u64 r8;
					u64 r9;
					u64 r10;
					u64 r11;
					u64 r12;
					u64 r13;
					u64 r14;
					u64 r15;
				} __packed;

				u64 gp_registers[16];
			};
			/* Instruction pointer (HV_X64_REGISTER_CLASS_IP) */
			u64 rip;
			/* Flags (HV_X64_REGISTER_CLASS_FLAGS) */
			u64 rflags;
		} __packed;

		u64 registers[18];
	};
	/* Volatile XMM registers (HV_X64_REGISTER_CLASS_XMM) */
	union {
		struct {
			struct hv_u128 xmm0;
			struct hv_u128 xmm1;
			struct hv_u128 xmm2;
			struct hv_u128 xmm3;
			struct hv_u128 xmm4;
			struct hv_u128 xmm5;
		} __packed;

		struct hv_u128 xmm_registers[6];
	};
	/* Segment registers (HV_X64_REGISTER_CLASS_SEGMENT) */
	union {
		struct {
			struct hv_x64_segment_register es;
			struct hv_x64_segment_register cs;
			struct hv_x64_segment_register ss;
			struct hv_x64_segment_register ds;
			struct hv_x64_segment_register fs;
			struct hv_x64_segment_register gs;
		} __packed;

		struct hv_x64_segment_register segment_registers[6];
	};
	/* Misc. control registers (cannot be set via this interface) */
	u64 cr0;
	u64 cr3;
	u64 cr4;
	u64 cr8;
	u64 efer;
	u64 dr7;
	union hv_x64_pending_interruption_register pending_interruption;
	union hv_x64_interrupt_state_register interrupt_state;
	u64 instruction_emulation_hints;
	u64 xfem;

	/*
	 * Fields from this point are not included in the register page save chunk.
	 * The reserved field is intended to maintain alignment for unsaved fields.
	 */
	u8 reserved1[0x100];

	/*
	 * Interrupts injected as part of HvCallDispatchVp.
	 */
	union hv_vp_register_page_interrupt_vectors interrupt_vectors;

#elif IS_ENABLED(CONFIG_ARM64)
	/* Not yet supported in ARM */
#endif
} __packed;

#define HV_PARTITION_PROCESSOR_FEATURES_BANKS 2

union hv_partition_processor_features {
	u64 as_uint64[HV_PARTITION_PROCESSOR_FEATURES_BANKS];
	struct {
		u64 sse3_support : 1;
		u64 lahf_sahf_support : 1;
		u64 ssse3_support : 1;
		u64 sse4_1_support : 1;
		u64 sse4_2_support : 1;
		u64 sse4a_support : 1;
		u64 xop_support : 1;
		u64 pop_cnt_support : 1;
		u64 cmpxchg16b_support : 1;
		u64 altmovcr8_support : 1;
		u64 lzcnt_support : 1;
		u64 mis_align_sse_support : 1;
		u64 mmx_ext_support : 1;
		u64 amd3dnow_support : 1;
		u64 extended_amd3dnow_support : 1;
		u64 page_1gb_support : 1;
		u64 aes_support : 1;
		u64 pclmulqdq_support : 1;
		u64 pcid_support : 1;
		u64 fma4_support : 1;
		u64 f16c_support : 1;
		u64 rd_rand_support : 1;
		u64 rd_wr_fs_gs_support : 1;
		u64 smep_support : 1;
		u64 enhanced_fast_string_support : 1;
		u64 bmi1_support : 1;
		u64 bmi2_support : 1;
		u64 hle_support_deprecated : 1;
		u64 rtm_support_deprecated : 1;
		u64 movbe_support : 1;
		u64 npiep1_support : 1;
		u64 dep_x87_fpu_save_support : 1;
		u64 rd_seed_support : 1;
		u64 adx_support : 1;
		u64 intel_prefetch_support : 1;
		u64 smap_support : 1;
		u64 hle_support : 1;
		u64 rtm_support : 1;
		u64 rdtscp_support : 1;
		u64 clflushopt_support : 1;
		u64 clwb_support : 1;
		u64 sha_support : 1;
		u64 x87_pointers_saved_support : 1;
		u64 invpcid_support : 1;
		u64 ibrs_support : 1;
		u64 stibp_support : 1;
		u64 ibpb_support: 1;
		u64 unrestricted_guest_support : 1;
		u64 mdd_support : 1;
		u64 fast_short_rep_mov_support : 1;
		u64 l1dcache_flush_support : 1;
		u64 rdcl_no_support : 1;
		u64 ibrs_all_support : 1;
		u64 skip_l1df_support : 1;
		u64 ssb_no_support : 1;
		u64 rsb_a_no_support : 1;
		u64 virt_spec_ctrl_support : 1;
		u64 rd_pid_support : 1;
		u64 umip_support : 1;
		u64 mbs_no_support : 1;
		u64 mb_clear_support : 1;
		u64 taa_no_support : 1;
		u64 tsx_ctrl_support : 1;
		/*
		 * N.B. The final processor feature bit in bank 0 is reserved to
		 * simplify potential downlevel backports.
		 */
		u64 reserved_bank0 : 1;

		/* N.B. Begin bank 1 processor features. */
		u64 acount_mcount_support : 1;
		u64 tsc_invariant_support : 1;
		u64 cl_zero_support : 1;
		u64 rdpru_support : 1;
		u64 la57_support : 1;
		u64 mbec_support : 1;
		u64 nested_virt_support : 1;
		u64 psfd_support : 1;
		u64 cet_ss_support : 1;
		u64 cet_ibt_support : 1;
		u64 vmx_exception_inject_support : 1;
		u64 enqcmd_support : 1;
		u64 umwait_tpause_support : 1;
		u64 movdiri_support : 1;
		u64 movdir64b_support : 1;
		u64 cldemote_support : 1;
		u64 serialize_support : 1;
		u64 tsc_deadline_tmr_support : 1;
		u64 tsc_adjust_support : 1;
		u64 fzlrep_movsb : 1;
		u64 fsrep_stosb : 1;
		u64 fsrep_cmpsb : 1;
		u64 reserved_bank1 : 42;
	} __packed;
};

union hv_partition_processor_xsave_features {
	struct {
		u64 xsave_support : 1;
		u64 xsaveopt_support : 1;
		u64 avx_support : 1;
		u64 reserved1 : 61;
	} __packed;
	u64 as_uint64;
};

struct hv_partition_creation_properties {
	union hv_partition_processor_features disabled_processor_features;
	union hv_partition_processor_xsave_features
		disabled_processor_xsave_features;
} __packed;

#define HV_PARTITION_SYNTHETIC_PROCESSOR_FEATURES_BANKS 1

union hv_partition_synthetic_processor_features {
	u64 as_uint64[HV_PARTITION_SYNTHETIC_PROCESSOR_FEATURES_BANKS];

	struct {
		u64 hypervisor_present : 1;
		/* Support for HV#1: (CPUID leaves 0x40000000 - 0x40000006)*/
		u64 hv1 : 1;
		u64 access_vp_run_time_reg : 1; /* HV_X64_MSR_VP_RUNTIME */
		u64 access_partition_reference_counter : 1; /* HV_X64_MSR_TIME_REF_COUNT */
		u64 access_synic_regs : 1; /* SINT-related registers */
		/*
		 * Access to HV_X64_MSR_STIMER0_CONFIG through
		 * HV_X64_MSR_STIMER3_COUNT.
		 */
		u64 access_synthetic_timer_regs : 1;
		u64 access_intr_ctrl_regs : 1; /* APIC MSRs and VP assist page*/
		/* HV_X64_MSR_GUEST_OS_ID and HV_X64_MSR_HYPERCALL */
		u64 access_hypercall_regs : 1;
		u64 access_vp_index : 1;
		u64 access_partition_reference_tsc : 1;
		u64 access_guest_idle_reg : 1;
		u64 access_frequency_regs : 1;
		u64 reserved_z12 : 1;
		u64 reserved_z13 : 1;
		u64 reserved_z14 : 1;
		u64 enable_extended_gva_ranges_for_flush_virtual_address_list : 1;
		u64 reserved_z16 : 1;
		u64 reserved_z17 : 1;
		/* Use fast hypercall output. Corresponds to privilege. */
		u64 fast_hypercall_output : 1;
		u64 reserved_z19 : 1;
		u64 start_virtual_processor : 1; /* Can start VPs */
		u64 reserved_z21 : 1;
		/* Synthetic timers in direct mode. */
		u64 direct_synthetic_timers : 1;
		u64 reserved_z23 : 1;
		u64 extended_processor_masks : 1;

		/* Enable various hypercalls */
		u64 tb_flush_hypercalls : 1;
		u64 synthetic_cluster_ipi : 1;
		u64 notify_long_spin_wait : 1;
		u64 query_numa_distance : 1;
		u64 signal_events : 1;
		u64 retarget_device_interrupt : 1;
		u64 restore_time : 1;

		/* EnlightenedVmcs nested enlightenment is supported. */
		u64 enlightened_vmcs : 1;
		u64 reserved : 31;
	} __packed;
};

#define HV_MAKE_COMPATIBILITY_VERSION(major_, minor_)	\
	((u32)((major_) << 8 | (minor_)))

#define HV_COMPATIBILITY_21_H2		HV_MAKE_COMPATIBILITY_VERSION(0X6, 0X9)

union hv_partition_isolation_properties {
	u64 as_uint64;
	struct {
		u64 isolation_type: 5;
		u64 isolation_host_type : 2;
		u64 rsvd_z: 5;
		u64 shared_gpa_boundary_page_number: 52;
	} __packed;
};

/*
 * Various isolation types supported by MSHV.
 */
#define HV_PARTITION_ISOLATION_TYPE_NONE            0
#define HV_PARTITION_ISOLATION_TYPE_SNP             2
#define HV_PARTITION_ISOLATION_TYPE_TDX             3

/*
 * Various host isolation types supported by MSHV.
 */
#define HV_PARTITION_ISOLATION_HOST_TYPE_NONE       0x0
#define HV_PARTITION_ISOLATION_HOST_TYPE_HARDWARE   0x1
#define HV_PARTITION_ISOLATION_HOST_TYPE_RESERVED   0x2

/* Note: Exo partition is enabled by default */
#define HV_PARTITION_CREATION_FLAG_GPA_SUPER_PAGES_ENABLED		BIT(4)
#define HV_PARTITION_CREATION_FLAG_EXO_PARTITION			BIT(8)
#define HV_PARTITION_CREATION_FLAG_LAPIC_ENABLED			BIT(13)
#define HV_PARTITION_CREATION_FLAG_INTERCEPT_MESSAGE_PAGE_ENABLED	BIT(19)
#define HV_PARTITION_CREATION_FLAG_X2APIC_CAPABLE			BIT(22)

struct hv_input_create_partition {
	u64 flags;
	struct hv_proximity_domain_info proximity_domain_info;
	u32 compatibility_version;
	u32 padding;
	struct hv_partition_creation_properties partition_creation_properties;
	union hv_partition_isolation_properties isolation_properties;
} __packed;

struct hv_output_create_partition {
	u64 partition_id;
} __packed;

struct hv_input_initialize_partition {
	u64 partition_id;
} __packed;

struct hv_input_finalize_partition {
	u64 partition_id;
} __packed;

struct hv_input_delete_partition {
	u64 partition_id;
} __packed;

struct hv_input_get_partition_property {
	u64 partition_id;
	u32 property_code; /* enum hv_partition_property_code */
	u32 padding;
} __packed;

struct hv_output_get_partition_property {
	u64 property_value;
} __packed;

struct hv_input_set_partition_property {
	u64 partition_id;
	u32 property_code; /* enum hv_partition_property_code */
	u32 padding;
	u64 property_value;
} __packed;

union hv_partition_property_arg {
	u64 as_uint64;
	struct {
		union {
			u32 arg;
			u32 vp_index;
		};
		u16 reserved0;
		u8 reserved1;
		u8 object_type;
	} __packed;
};

struct hv_input_get_partition_property_ex {
	u64 partition_id;
	u32 property_code; /* enum hv_partition_property_code */
	u32 padding;
	union {
		union hv_partition_property_arg arg_data;
		u64 arg;
	};
} __packed;

/*
 * NOTE: Should use hv_input_set_partition_property_ex_header to compute this
 * size, but hv_input_get_partition_property_ex is identical so it suffices
 */
#define HV_PARTITION_PROPERTY_EX_MAX_VAR_SIZE \
	(HV_HYP_PAGE_SIZE - sizeof(struct hv_input_get_partition_property_ex))

union hv_partition_property_ex {
	u8 buffer[HV_PARTITION_PROPERTY_EX_MAX_VAR_SIZE];
	struct hv_partition_property_vmm_capabilities vmm_capabilities;
	/* More fields to be filled in when needed */
};

struct hv_output_get_partition_property_ex {
	union hv_partition_property_ex property_value;
} __packed;

enum hv_vp_state_page_type {
	HV_VP_STATE_PAGE_REGISTERS = 0,
	HV_VP_STATE_PAGE_INTERCEPT_MESSAGE = 1,
	HV_VP_STATE_PAGE_GHCB = 2,
	HV_VP_STATE_PAGE_COUNT
};

struct hv_input_map_vp_state_page {
	u64 partition_id;
	u32 vp_index;
	u16 type; /* enum hv_vp_state_page_type */
	union hv_input_vtl input_vtl;
	union {
		u8 as_uint8;
		struct {
			u8 map_location_provided : 1;
			u8 reserved : 7;
		};
	} flags;
	u64 requested_map_location;
} __packed;

struct hv_output_map_vp_state_page {
	u64 map_location; /* GPA page number */
} __packed;

struct hv_input_unmap_vp_state_page {
	u64 partition_id;
	u32 vp_index;
	u16 type; /* enum hv_vp_state_page_type */
	union hv_input_vtl input_vtl;
	u8 reserved0;
} __packed;

struct hv_x64_apic_eoi_message {
	u32 vp_index;
	u32 interrupt_vector;
} __packed;

struct hv_opaque_intercept_message {
	u32 vp_index;
} __packed;

enum hv_port_type {
	HV_PORT_TYPE_MESSAGE = 1,
	HV_PORT_TYPE_EVENT   = 2,
	HV_PORT_TYPE_MONITOR = 3,
	HV_PORT_TYPE_DOORBELL = 4	/* Root Partition only */
};

struct hv_port_info {
	u32 port_type; /* enum hv_port_type */
	u32 padding;
	union {
		struct {
			u32 target_sint;
			u32 target_vp;
			u64 rsvdz;
		} message_port_info;
		struct {
			u32 target_sint;
			u32 target_vp;
			u16 base_flag_number;
			u16 flag_count;
			u32 rsvdz;
		} event_port_info;
		struct {
			u64 monitor_address;
			u64 rsvdz;
		} monitor_port_info;
		struct {
			u32 target_sint;
			u32 target_vp;
			u64 rsvdz;
		} doorbell_port_info;
	};
} __packed;

struct hv_connection_info {
	u32 port_type;
	u32 padding;
	union {
		struct {
			u64 rsvdz;
		} message_connection_info;
		struct {
			u64 rsvdz;
		} event_connection_info;
		struct {
			u64 monitor_address;
		} monitor_connection_info;
		struct {
			u64 gpa;
			u64 trigger_value;
			u64 flags;
		} doorbell_connection_info;
	};
} __packed;

/* Define synthetic interrupt controller flag constants. */
#define HV_EVENT_FLAGS_COUNT		(256 * 8)
#define HV_EVENT_FLAGS_BYTE_COUNT	(256)
#define HV_EVENT_FLAGS32_COUNT		(256 / sizeof(u32))

/* linux side we create long version of flags to use long bit ops on flags */
#define HV_EVENT_FLAGS_UL_COUNT		(256 / sizeof(ulong))

/* Define the synthetic interrupt controller event flags format. */
union hv_synic_event_flags {
	unsigned char flags8[HV_EVENT_FLAGS_BYTE_COUNT];
	u32 flags32[HV_EVENT_FLAGS32_COUNT];
	ulong flags[HV_EVENT_FLAGS_UL_COUNT];  /* linux only */
};

struct hv_synic_event_flags_page {
	volatile union hv_synic_event_flags event_flags[HV_SYNIC_SINT_COUNT];
};

#define HV_SYNIC_EVENT_RING_MESSAGE_COUNT 63

struct hv_synic_event_ring {
	u8  signal_masked;
	u8  ring_full;
	u16 reserved_z;
	u32 data[HV_SYNIC_EVENT_RING_MESSAGE_COUNT];
} __packed;

struct hv_synic_event_ring_page {
	struct hv_synic_event_ring sint_event_ring[HV_SYNIC_SINT_COUNT];
};

/* Define SynIC control register. */
union hv_synic_scontrol {
	u64 as_uint64;
	struct {
		u64 enable : 1;
		u64 reserved : 63;
	} __packed;
};

/* Define the format of the SIEFP register */
union hv_synic_siefp {
	u64 as_uint64;
	struct {
		u64 siefp_enabled : 1;
		u64 preserved : 11;
		u64 base_siefp_gpa : 52;
	} __packed;
};

union hv_synic_sirbp {
	u64 as_uint64;
	struct {
		u64 sirbp_enabled : 1;
		u64 preserved : 11;
		u64 base_sirbp_gpa : 52;
	} __packed;
};

union hv_interrupt_control {
	u64 as_uint64;
	struct {
		u32 interrupt_type; /* enum hv_interrupt_type */
#if IS_ENABLED(CONFIG_X86)
		u32 level_triggered : 1;
		u32 logical_dest_mode : 1;
		u32 rsvd : 30;
#elif IS_ENABLED(CONFIG_ARM64)
		u32 rsvd1 : 2;
		u32 asserted : 1;
		u32 rsvd2 : 29;
#endif
	} __packed;
};

struct hv_stimer_state {
	struct {
		u32 undelivered_msg_pending : 1;
		u32 reserved : 31;
	} __packed flags;
	u32 resvd;
	u64 config;
	u64 count;
	u64 adjustment;
	u64 undelivered_exp_time;
} __packed;

struct hv_synthetic_timers_state {
	struct hv_stimer_state timers[HV_SYNIC_STIMER_COUNT];
	u64 reserved[5];
} __packed;

struct hv_async_completion_message_payload {
	u64 partition_id;
	u32 status;
	u32 completion_count;
	u64 sub_status;
} __packed;

union hv_input_delete_vp {
	u64 as_uint64[2];
	struct {
		u64 partition_id;
		u32 vp_index;
		u8 reserved[4];
	} __packed;
} __packed;

struct hv_input_assert_virtual_interrupt {
	u64 partition_id;
	union hv_interrupt_control control;
	u64 dest_addr; /* cpu's apic id */
	u32 vector;
	u8 target_vtl;
	u8 rsvd_z0;
	u16 rsvd_z1;
} __packed;

struct hv_input_create_port {
	u64 port_partition_id;
	union hv_port_id port_id;
	u8 port_vtl;
	u8 min_connection_vtl;
	u16 padding;
	u64 connection_partition_id;
	struct hv_port_info port_info;
	struct hv_proximity_domain_info proximity_domain_info;
} __packed;

union hv_input_delete_port {
	u64 as_uint64[2];
	struct {
		u64 port_partition_id;
		union hv_port_id port_id;
		u32 reserved;
	};
} __packed;

struct hv_input_connect_port {
	u64 connection_partition_id;
	union hv_connection_id connection_id;
	u8 connection_vtl;
	u8 rsvdz0;
	u16 rsvdz1;
	u64 port_partition_id;
	union hv_port_id port_id;
	u32 reserved2;
	struct hv_connection_info connection_info;
	struct hv_proximity_domain_info proximity_domain_info;
} __packed;

union hv_input_disconnect_port {
	u64 as_uint64[2];
	struct {
		u64 connection_partition_id;
		union hv_connection_id connection_id;
		u32 is_doorbell: 1;
		u32 reserved: 31;
	} __packed;
} __packed;

union hv_input_notify_port_ring_empty {
	u64 as_uint64;
	struct {
		u32 sint_index;
		u32 reserved;
	};
} __packed;

struct hv_vp_state_data_xsave {
	u64 flags;
	union hv_x64_xsave_xfem_register states;
} __packed;

/*
 * For getting and setting VP state, there are two options based on the state type:
 *
 *     1.) Data that is accessed by PFNs in the input hypercall page. This is used
 *         for state which may not fit into the hypercall pages.
 *     2.) Data that is accessed directly in the input\output hypercall pages.
 *         This is used for state that will always fit into the hypercall pages.
 *
 * In the future this could be dynamic based on the size if needed.
 *
 * Note these hypercalls have an 8-byte aligned variable header size as per the tlfs
 */

#define HV_GET_SET_VP_STATE_TYPE_PFN	BIT(31)

enum hv_get_set_vp_state_type {
	/* HvGetSetVpStateLocalInterruptControllerState - APIC/GIC state */
	HV_GET_SET_VP_STATE_LAPIC_STATE	     = 0 | HV_GET_SET_VP_STATE_TYPE_PFN,
	HV_GET_SET_VP_STATE_XSAVE	     = 1 | HV_GET_SET_VP_STATE_TYPE_PFN,
	HV_GET_SET_VP_STATE_SIM_PAGE	     = 2 | HV_GET_SET_VP_STATE_TYPE_PFN,
	HV_GET_SET_VP_STATE_SIEF_PAGE	     = 3 | HV_GET_SET_VP_STATE_TYPE_PFN,
	HV_GET_SET_VP_STATE_SYNTHETIC_TIMERS = 4,
};

struct hv_vp_state_data {
	u32 type;
	u32 rsvd;
	struct hv_vp_state_data_xsave xsave;
} __packed;

struct hv_input_get_vp_state {
	u64 partition_id;
	u32 vp_index;
	u8 input_vtl;
	u8 rsvd0;
	u16 rsvd1;
	struct hv_vp_state_data state_data;
	u64 output_data_pfns[];
} __packed;

union hv_output_get_vp_state {
	struct hv_synthetic_timers_state synthetic_timers_state;
} __packed;

union hv_input_set_vp_state_data {
	u64 pfns;
	u8 bytes;
} __packed;

struct hv_input_set_vp_state {
	u64 partition_id;
	u32 vp_index;
	u8 input_vtl;
	u8 rsvd0;
	u16 rsvd1;
	struct hv_vp_state_data state_data;
	union hv_input_set_vp_state_data data[];
} __packed;

union hv_x64_vp_execution_state {
	u16 as_uint16;
	struct {
		u16 cpl:2;
		u16 cr0_pe:1;
		u16 cr0_am:1;
		u16 efer_lma:1;
		u16 debug_active:1;
		u16 interruption_pending:1;
		u16 vtl:4;
		u16 enclave_mode:1;
		u16 interrupt_shadow:1;
		u16 virtualization_fault_active:1;
		u16 reserved:2;
	} __packed;
};

struct hv_x64_intercept_message_header {
	u32 vp_index;
	u8 instruction_length:4;
	u8 cr8:4; /* Only set for exo partitions */
	u8 intercept_access_type;
	union hv_x64_vp_execution_state execution_state;
	struct hv_x64_segment_register cs_segment;
	u64 rip;
	u64 rflags;
} __packed;

union hv_x64_memory_access_info {
	u8 as_uint8;
	struct {
		u8 gva_valid:1;
		u8 gva_gpa_valid:1;
		u8 hypercall_output_pending:1;
		u8 tlb_locked_no_overlay:1;
		u8 reserved:4;
	} __packed;
};

struct hv_x64_memory_intercept_message {
	struct hv_x64_intercept_message_header header;
	u32 cache_type; /* enum hv_cache_type */
	u8 instruction_byte_count;
	union hv_x64_memory_access_info memory_access_info;
	u8 tpr_priority;
	u8 reserved1;
	u64 guest_virtual_address;
	u64 guest_physical_address;
	u8 instruction_bytes[16];
} __packed;

/*
 * Dispatch state for the VP communicated by the hypervisor to the
 * VP-dispatching thread in the root on return from HVCALL_DISPATCH_VP.
 */
enum hv_vp_dispatch_state {
	HV_VP_DISPATCH_STATE_INVALID	= 0,
	HV_VP_DISPATCH_STATE_BLOCKED	= 1,
	HV_VP_DISPATCH_STATE_READY	= 2,
};

/*
 * Dispatch event that caused the current dispatch state on return from
 * HVCALL_DISPATCH_VP.
 */
enum hv_vp_dispatch_event {
	HV_VP_DISPATCH_EVENT_INVALID	= 0x00000000,
	HV_VP_DISPATCH_EVENT_SUSPEND	= 0x00000001,
	HV_VP_DISPATCH_EVENT_INTERCEPT	= 0x00000002,
};

#define HV_ROOT_SCHEDULER_MAX_VPS_PER_CHILD_PARTITION   1024
/* The maximum array size of HV_GENERIC_SET (vp_set) buffer */
#define HV_GENERIC_SET_QWORD_COUNT(max) (((((max) - 1) >> 6) + 1) + 2)

struct hv_vp_signal_bitset_scheduler_message {
	u64 partition_id;
	u32 overflow_count;
	u16 vp_count;
	u16 reserved;

#define BITSET_BUFFER_SIZE \
	HV_GENERIC_SET_QWORD_COUNT(HV_ROOT_SCHEDULER_MAX_VPS_PER_CHILD_PARTITION)
	union {
		struct hv_vpset bitset;
		u64 bitset_buffer[BITSET_BUFFER_SIZE];
	} vp_bitset;
#undef BITSET_BUFFER_SIZE
} __packed;

static_assert(sizeof(struct hv_vp_signal_bitset_scheduler_message) <=
	(sizeof(struct hv_message) - sizeof(struct hv_message_header)));

#define HV_MESSAGE_MAX_PARTITION_VP_PAIR_COUNT \
	(((sizeof(struct hv_message) - sizeof(struct hv_message_header)) / \
	 (sizeof(u64 /* partition id */) + sizeof(u32 /* vp index */))) - 1)

struct hv_vp_signal_pair_scheduler_message {
	u32 overflow_count;
	u8 vp_count;
	u8 reserved1[3];

	u64 partition_ids[HV_MESSAGE_MAX_PARTITION_VP_PAIR_COUNT];
	u32 vp_indexes[HV_MESSAGE_MAX_PARTITION_VP_PAIR_COUNT];

	u8 reserved2[4];
} __packed;

static_assert(sizeof(struct hv_vp_signal_pair_scheduler_message) ==
	(sizeof(struct hv_message) - sizeof(struct hv_message_header)));

/* Input and output structures for HVCALL_DISPATCH_VP */
#define HV_DISPATCH_VP_FLAG_CLEAR_INTERCEPT_SUSPEND	0x1
#define HV_DISPATCH_VP_FLAG_ENABLE_CALLER_INTERRUPTS	0x2
#define HV_DISPATCH_VP_FLAG_SET_CALLER_SPEC_CTRL	0x4
#define HV_DISPATCH_VP_FLAG_SKIP_VP_SPEC_FLUSH		0x8
#define HV_DISPATCH_VP_FLAG_SKIP_CALLER_SPEC_FLUSH	0x10
#define HV_DISPATCH_VP_FLAG_SKIP_CALLER_USER_SPEC_FLUSH	0x20
#define HV_DISPATCH_VP_FLAG_SCAN_INTERRUPT_INJECTION	0x40

struct hv_input_dispatch_vp {
	u64 partition_id;
	u32 vp_index;
	u32 flags;
	u64 time_slice; /* in 100ns */
	u64 spec_ctrl;
} __packed;

struct hv_output_dispatch_vp {
	u32 dispatch_state; /* enum hv_vp_dispatch_state */
	u32 dispatch_event; /* enum hv_vp_dispatch_event */
} __packed;

struct hv_input_modify_sparse_spa_page_host_access {
	u32 host_access : 2;
	u32 reserved : 30;
	u32 flags;
	u64 partition_id;
	u64 spa_page_list[];
} __packed;

/* hv_input_modify_sparse_spa_page_host_access flags */
#define HV_MODIFY_SPA_PAGE_HOST_ACCESS_MAKE_EXCLUSIVE  0x1
#define HV_MODIFY_SPA_PAGE_HOST_ACCESS_MAKE_SHARED     0x2
#define HV_MODIFY_SPA_PAGE_HOST_ACCESS_LARGE_PAGE      0x4
#define HV_MODIFY_SPA_PAGE_HOST_ACCESS_HUGE_PAGE       0x8

#endif /* _HV_HVHDK_H */
