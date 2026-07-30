/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __RAS_H__
#define __RAS_H__

#include <asm/errno.h>
#include <linux/bits.h>
#include <linux/stddef.h>
#include <linux/uuid.h>
#include <linux/cper.h>

#ifdef CONFIG_DEBUG_FS
int ras_userspace_consumers(void);
void ras_debugfs_init(void);
int ras_add_daemon_trace(void);
#else
static inline int ras_userspace_consumers(void) { return 0; }
static inline void ras_debugfs_init(void) { }
static inline int ras_add_daemon_trace(void) { return 0; }
#endif

#ifdef CONFIG_RAS_CEC
int __init parse_cec_param(char *str);
#endif

#ifdef CONFIG_RAS
void log_non_standard_event(const guid_t *sec_type,
			    const guid_t *fru_id, const char *fru_text,
			    const u8 sev, const u8 *err, const u32 len);
void log_arm_hw_error(struct cper_sec_proc_arm *err, const u8 sev);
#else
static inline void
log_non_standard_event(const guid_t *sec_type,
		       const guid_t *fru_id, const char *fru_text,
		       const u8 sev, const u8 *err, const u32 len)
{ return; }
static inline void
log_arm_hw_error(struct cper_sec_proc_arm *err, const u8 sev) { return; }
#endif

/* Operations requested and completed through amd_translate_umc_mca_addr(). */
#define ATL_OP_SPA		BIT(0)	/* System Physical Address */

struct atl_err {
	/* Identifiers; layout mirrors the PRM parameter buffer inputs */
	__struct_group(atl_umc_addr, umc_addr, __packed,
		u64 addr;
		u8 socket_id;		/* Filled by the library from @cpu */
		u64 ipid;
	);
	u32 cpu;

	/* Requested operations (input) */
	u8 requested;

	/* Completed operations (output) */
	u8 valid;

	/* Outputs */
	u64 spa;			/* Valid if (@valid & ATL_OP_SPA) */
};

#if IS_ENABLED(CONFIG_AMD_ATL)
void amd_retire_dram_row(struct atl_err *err);

void amd_atl_register_umc_translator(void (*f)(struct atl_err *));
void amd_atl_unregister_umc_translator(void);
void amd_translate_umc_mca_addr(struct atl_err *err);
#else
static inline void amd_retire_dram_row(struct atl_err *err) { }
static inline void
amd_translate_umc_mca_addr(struct atl_err *err) { err->valid = 0; }
#endif /* CONFIG_AMD_ATL */

#if defined(CONFIG_ARM) || defined(CONFIG_ARM64)
#include <asm/smp_plat.h>
/*
 * Include ARM-specific SMP header which provides a function mapping mpidr to
 * CPU logical index.
 */
#define GET_LOGICAL_INDEX(mpidr) get_logical_index(mpidr & MPIDR_HWID_BITMASK)
#else
#define GET_LOGICAL_INDEX(mpidr) -EINVAL
#endif /* CONFIG_ARM || CONFIG_ARM64 */

#endif /* __RAS_H__ */
