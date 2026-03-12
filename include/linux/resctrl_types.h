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

/**
 * struct resctrl_kmode - Resctrl kernel mode descriptor
 * @name:	Human-readable name of the kernel mode.
 * @val:	Bitmask value for the kernel mode (e.g. INHERIT_CTRL_AND_MON).
 */
struct resctrl_kmode {
	char    name[32];
	u32     val;
};

/**
 * struct resctrl_kmode_cfg - Resctrl kernel mode configuration
 * @kmode:	Requested kernel mode.
 * @kmode_cur:	Currently active kernel mode.
 * @k_rdtgrp:	Resource control structure in use, or NULL otherwise.
 */
struct resctrl_kmode_cfg {
	u32 kmode;
	u32 kmode_cur;
	struct rdtgroup *k_rdtgrp;
};

#define QOS_NUM_L3_MBM_EVENTS	(QOS_L3_MBM_LOCAL_EVENT_ID - QOS_L3_MBM_TOTAL_EVENT_ID + 1)
#define MBM_STATE_IDX(evt)	((evt) - QOS_L3_MBM_TOTAL_EVENT_ID)

/* Resctrl kernel mode bits (e.g. for PLZA). */
#define INHERIT_CTRL_AND_MON		BIT(0)	/* Kernel uses same CLOSID/RMID as user. */
/* One CLOSID for all kernel work; RMID inherited from user. */
#define GLOBAL_ASSIGN_CTRL_INHERIT_MON	BIT(1)
/* One resource group (CLOSID+RMID) for all kernel work. */
#define GLOBAL_ASSIGN_CTRL_ASSIGN_MON	BIT(2)
#define RESCTRL_KERNEL_MODES_NUM	3

#endif /* __LINUX_RESCTRL_TYPES_H */
