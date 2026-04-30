/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2025 Arm Ltd.
 * Based on arch/x86/kernel/cpu/resctrl/internal.h
 */

#ifndef __LINUX_RESCTRL_TYPES_H
#define __LINUX_RESCTRL_TYPES_H

#define MAX_MBA_BW			100u
#define MBM_OVERFLOW_INTERVAL		1000

/* Reads to Local DRAM Memory */
#define READS_TO_LOCAL_MEM		BIT(0)

/* Reads to Remote DRAM Memory */
#define READS_TO_REMOTE_MEM		BIT(1)

/* Non-Temporal Writes to Local Memory */
#define NON_TEMP_WRITE_TO_LOCAL_MEM	BIT(2)

/* Non-Temporal Writes to Remote Memory */
#define NON_TEMP_WRITE_TO_REMOTE_MEM	BIT(3)

/* Reads to Local Memory the system identifies as "Slow Memory" */
#define READS_TO_LOCAL_S_MEM		BIT(4)

/* Reads to Remote Memory the system identifies as "Slow Memory" */
#define READS_TO_REMOTE_S_MEM		BIT(5)

/* Dirty Victims to All Types of Memory */
#define DIRTY_VICTIMS_TO_ALL_MEM	BIT(6)

/* Max event bits supported */
#define MAX_EVT_CONFIG_BITS		GENMASK(6, 0)

/* Number of memory transactions that an MBM event can be configured with */
#define NUM_MBM_TRANSACTIONS		7

/* Event IDs */
enum resctrl_event_id {
	/* Must match value of first event below */
	QOS_FIRST_EVENT			= 0x01,

	/*
	 * These values match those used to program IA32_QM_EVTSEL before
	 * reading IA32_QM_CTR on RDT systems.
	 */
	QOS_L3_OCCUP_EVENT_ID		= 0x01,
	QOS_L3_MBM_TOTAL_EVENT_ID	= 0x02,
	QOS_L3_MBM_LOCAL_EVENT_ID	= 0x03,

	/* Intel Telemetry Events */
	PMT_EVENT_ENERGY,
	PMT_EVENT_ACTIVITY,
	PMT_EVENT_STALLS_LLC_HIT,
	PMT_EVENT_C1_RES,
	PMT_EVENT_UNHALTED_CORE_CYCLES,
	PMT_EVENT_STALLS_LLC_MISS,
	PMT_EVENT_AUTO_C6_RES,
	PMT_EVENT_UNHALTED_REF_CYCLES,
	PMT_EVENT_UOPS_RETIRED,

	/* Must be the last */
	QOS_NUM_EVENTS,
};

#define QOS_NUM_L3_MBM_EVENTS	(QOS_L3_MBM_LOCAL_EVENT_ID - QOS_L3_MBM_TOTAL_EVENT_ID + 1)
#define MBM_STATE_IDX(evt)	((evt) - QOS_L3_MBM_TOTAL_EVENT_ID)

/**
 * enum resctrl_kernel_modes - Kernel versus user CLOSID/RMID policy
 *
 * Enumeration values are contiguous indices from 0 through
 * @RESCTRL_KMODE_LAST inclusive. Global-assign modes treat all online CPUs as
 * in scope by default; a subset of CPUs may be selected by using resctrl
 * group's interface.
 *
 * @INHERIT_CTRL_AND_MON:
 *	User and kernel tasks use the same CLOSID and RMID.
 * @GLOBAL_ASSIGN_CTRL_INHERIT_MON_PER_CPU:
 *	A CLOSID may be assigned for kernel work while RMID selection for
 *	monitoring follows the same inheritance rules as for user contexts.
 *	Default scope is all online CPUs: subset of CPUs may be selected by
 *	using resctrl group's interface.
 * @GLOBAL_ASSIGN_CTRL_ASSIGN_MON_PER_CPU:
 *	A single resource group (CLOSID and RMID together) may be assigned to
 *	kernel work. Default scope is all online CPUs: subset of CPUs may be
 *	selected by using resctrl group's interface.
 * @RESCTRL_KMODE_LAST:
 *	Highest enumerator that names a policy mode. Use RESCTRL_NUM_KERNEL_MODES
 *	to size static tables indexed by mode.
 */
enum resctrl_kernel_modes {
	INHERIT_CTRL_AND_MON,
	GLOBAL_ASSIGN_CTRL_INHERIT_MON_PER_CPU,
	GLOBAL_ASSIGN_CTRL_ASSIGN_MON_PER_CPU,
	RESCTRL_KMODE_LAST = GLOBAL_ASSIGN_CTRL_ASSIGN_MON_PER_CPU,
};

#define RESCTRL_NUM_KERNEL_MODES (RESCTRL_KMODE_LAST + 1)

/**
 * struct resctrl_kmode_cfg - Kernel-mode policy snapshot from architecture
 * @kmode:	Hardware- or policy-supported modes: each enumerator from
 *		&enum resctrl_kernel_modes is represented by BIT(mode index).
 * @kmode_cur:	Effective mode(s) in the same BIT(index) form as @kmode.
 * @k_rdtgrp:	Resource group backing global-assign modes when applicable;
 *		initialized to the default group at boot.
 */
struct resctrl_kmode_cfg {
	u32 kmode;
	u32 kmode_cur;
	struct rdtgroup *k_rdtgrp;
};

#endif /* __LINUX_RESCTRL_TYPES_H */
