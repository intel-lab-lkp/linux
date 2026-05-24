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

#define CBQRI_CC_CAPABILITIES_VER_MINOR_MASK  GENMASK(3, 0)
#define CBQRI_CC_CAPABILITIES_VER_MAJOR_MASK  GENMASK(7, 4)
#define CBQRI_CC_CAPABILITIES_NCBLKS_MASK     GENMASK(23, 8)

#define CBQRI_CONTROL_REGISTERS_OP_MASK      GENMASK(4, 0)
#define CBQRI_CONTROL_REGISTERS_AT_MASK      GENMASK(7, 5)
#define CBQRI_CONTROL_REGISTERS_AT_DATA      0
#define CBQRI_CONTROL_REGISTERS_AT_CODE      1
#define CBQRI_CONTROL_REGISTERS_RCID_MASK    GENMASK(19, 8)
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
 * enum cbqri_at - capacity controller access type for CDP
 * @CBQRI_AT_DATA: data access (CBQRI Table 1, AT=0)
 * @CBQRI_AT_CODE: code access (CBQRI Table 1, AT=1)
 *
 * Selects between data and code halves on controllers that advertise
 * supports_alloc_at_code. The resctrl glue maps from CDP_DATA / CDP_CODE
 * to this enum at the boundary so cbqri_devices.c stays free of fs/resctrl
 * types.
 */
enum cbqri_at {
	CBQRI_AT_DATA = CBQRI_CONTROL_REGISTERS_AT_DATA,
	CBQRI_AT_CODE = CBQRI_CONTROL_REGISTERS_AT_CODE,
};

/**
 * struct cbqri_cc_config - desired capacity allocation state for one rcid
 * @cbm:         capacity block mask
 * @at:          AT half (data or code) the @cbm applies to
 * @cdp_enabled: when false and the controller supports AT, mirror @cbm
 *               into the other AT half so both stay in sync
 */
struct cbqri_cc_config {
	u64           cbm;
	enum cbqri_at at;
	bool          cdp_enabled;
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
	/*
	 * Set by cbqri_wait_busy_flag() on BUSY timeout, cleared on the
	 * next successful wait. Informational only, used for diagnostics.
	 */
	bool faulted;

	int ver_major;
	int ver_minor;

	struct riscv_cbqri_capacity_caps cc;

	bool alloc_capable;
	bool mon_capable;

	phys_addr_t addr;
	phys_addr_t size;
	enum cbqri_controller_type type;
	u32 rcid_count;
	u32 mcid_count;

	struct list_head list;

	struct cache_controller {
		u32 cache_level;
		u32 cache_size; /* in bytes */
		struct cpumask cpu_mask;
		/* Unique Cache ID from the PPTT table's Cache Type Structure */
		u32 cache_id;
	} cache;
};

extern struct list_head cbqri_controllers;

void cbqri_controller_destroy(struct cbqri_controller *ctrl);

int cbqri_apply_cache_config(struct cbqri_controller *ctrl, u32 closid,
			     const struct cbqri_cc_config *cfg);

int cbqri_read_cache_config(struct cbqri_controller *ctrl, u32 closid,
			    enum cbqri_at at, u32 *cbm_out);

#endif /* _DRIVERS_RESCTRL_CBQRI_INTERNAL_H */
