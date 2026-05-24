/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Public registration API for the RISC-V Capacity and Bandwidth QoS
 * Register Interface (CBQRI) driver. Discovery layers (ACPI RQSC, future
 * device tree) call riscv_cbqri_register_controller() to hand a controller
 * descriptor to the driver, which owns all subsequent state.
 */
#ifndef _LINUX_RISCV_CBQRI_H
#define _LINUX_RISCV_CBQRI_H

#include <linux/types.h>

enum cbqri_controller_type {
	CBQRI_CONTROLLER_TYPE_CAPACITY,
	CBQRI_CONTROLLER_TYPE_BANDWIDTH,
};

/*
 * Sanity caps on per-controller RCID/MCID counts from firmware (RQSC, DT).
 * Per-id MMIO init loops busy-wait up to ~1-2 ms each, so a malformed table
 * claiming the full u16 range (65535) would block boot long enough to trip
 * the soft-lockup watchdog. Real CBQRI hardware advertises tens to a few
 * hundred ids.
 */
#define CBQRI_MAX_RCID	1024
#define CBQRI_MAX_MCID	1024

/**
 * struct cbqri_controller_info - registration descriptor
 * @addr:        MMIO base address of the controller's register interface
 * @size:        size of the MMIO region
 * @type:        capacity or bandwidth controller
 * @rcid_count:  number of supported RCIDs (per RQSC table)
 * @mcid_count:  number of supported MCIDs (per RQSC table)
 * @cache_id:    PPTT cache id. Only meaningful for CAPACITY controllers
 * @prox_dom:    SRAT proximity domain. Only meaningful for BANDWIDTH
 *               controllers
 *
 * Discovery layers populate one of @cache_id / @prox_dom according to
 * @type. The CBQRI driver resolves the matching cpumask internally so
 * callers do not need to know about cacheinfo/NUMA topology.
 */
struct cbqri_controller_info {
	phys_addr_t			addr;
	phys_addr_t			size;
	enum cbqri_controller_type	type;
	u32				rcid_count;
	u32				mcid_count;
	u32				cache_id;
	u32				prox_dom;
};

#if IS_ENABLED(CONFIG_RISCV_CBQRI_DRIVER)
int riscv_cbqri_register_controller(const struct cbqri_controller_info *info);
void riscv_cbqri_unregister_last(unsigned int n);
#else
static inline int
riscv_cbqri_register_controller(const struct cbqri_controller_info *info)
{
	return -ENODEV;
}

static inline void riscv_cbqri_unregister_last(unsigned int n) { }
#endif

#endif /* _LINUX_RISCV_CBQRI_H */
