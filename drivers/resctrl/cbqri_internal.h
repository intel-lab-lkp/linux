/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _DRIVERS_RESCTRL_CBQRI_INTERNAL_H
#define _DRIVERS_RESCTRL_CBQRI_INTERNAL_H

#include <linux/bitfield.h>
#include <linux/riscv_cbqri.h>
#include <linux/cpumask.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/types.h>

/* Capacity Controller (CC) MMIO register offsets. */
#define CBQRI_CC_CAPABILITIES_OFF 0
#define CBQRI_CC_ALLOC_CTL_OFF   24
#define CBQRI_CC_BLOCK_MASK_OFF  32

/*
 * Smallest MMIO span the driver actually accesses: highest defined
 * register offset (0x20) plus the 8-byte register width. Used by
 * cbqri_probe_controller() to reject undersized firmware-supplied
 * mappings before request_mem_region/ioremap, so a u64 access at
 * BLOCK_MASK does not walk past the end of the mapping.
 */
#define CBQRI_CTRL_MIN_REG_SPAN  0x28u

#define CBQRI_CC_CAPABILITIES_VER_MINOR_MASK  GENMASK_ULL(3, 0)
#define CBQRI_CC_CAPABILITIES_VER_MAJOR_MASK  GENMASK_ULL(7, 4)
#define CBQRI_CC_CAPABILITIES_NCBLKS_MASK     GENMASK_ULL(23, 8)

/*
 * CC control registers are 64-bit. Keep every field mask GENMASK_ULL so
 * FIELD_MODIFY() or ~mask on a u64 register never zero-extends a 32-bit
 * mask and clobbers STATUS/BUSY/WPRI in bits 63:32 if RV32 support is
 * added in the future.
 */
#define CBQRI_CONTROL_REGISTERS_OP_MASK      GENMASK_ULL(4, 0)
#define CBQRI_CONTROL_REGISTERS_AT_MASK      GENMASK_ULL(7, 5)
/* AT field values (CBQRI Table 1): data vs code half for CDP */
#define CBQRI_CONTROL_REGISTERS_AT_DATA      0
#define CBQRI_CONTROL_REGISTERS_AT_CODE      1
#define CBQRI_CONTROL_REGISTERS_RCID_MASK    GENMASK_ULL(19, 8)
#define CBQRI_CONTROL_REGISTERS_STATUS_MASK  GENMASK_ULL(38, 32)
#define CBQRI_CONTROL_REGISTERS_BUSY_MASK    GENMASK_ULL(39, 39)

#define CBQRI_CC_ALLOC_CTL_OP_CONFIG_LIMIT 1
#define CBQRI_CC_ALLOC_CTL_OP_READ_LIMIT   2
#define CBQRI_CC_ALLOC_CTL_STATUS_SUCCESS  1

/* Capacity Controller hardware capabilities */
struct riscv_cbqri_capacity_caps {
	u16 ncblks;
	bool supports_alloc_at_code;
};

/**
 * struct cbqri_cc_config - desired capacity allocation state for one rcid
 * @cbm:         capacity block mask
 * @at:          AT half the @cbm applies to (CBQRI_CONTROL_REGISTERS_AT_DATA
 *               or CBQRI_CONTROL_REGISTERS_AT_CODE)
 * @cdp_enabled: when false and the controller supports AT, mirror @cbm
 *               into the other AT half so both stay in sync
 */
struct cbqri_cc_config {
	u64  cbm;
	u32  at;
	bool cdp_enabled;
};

struct cbqri_controller {
	void __iomem *base;
	/*
	 * Serializes the write-then-poll-busy MMIO sequences on this
	 * controller. Each CBQRI op may busy-wait up to 1 ms on slow
	 * firmware, so use a sleeping mutex (paired with the sleeping
	 * readq_poll_timeout() in cbqri_wait_busy_flag()) to keep
	 * preemption enabled, which is required for PREEMPT_RT.
	 * All resctrl-arch entry points run in process context.
	 */
	struct mutex lock;

	struct riscv_cbqri_capacity_caps cc;

	bool alloc_capable;

	phys_addr_t addr;
	phys_addr_t size;
	enum cbqri_controller_type type;
	u32 rcid_count;

	struct list_head list;

	struct cache_controller {
		u32 cache_level;
		struct cpumask cpu_mask;
		/* Cache id used as the resctrl domain id */
		u32 cache_id;
	} cache;
};

extern struct list_head cbqri_controllers;

void cbqri_controller_destroy(struct cbqri_controller *ctrl);

int cbqri_apply_cache_config(struct cbqri_controller *ctrl, u32 closid,
			     const struct cbqri_cc_config *cfg);

int cbqri_read_cache_config(struct cbqri_controller *ctrl, u32 closid,
			    u32 at, u32 *cbm_out);

#endif /* _DRIVERS_RESCTRL_CBQRI_INTERNAL_H */
