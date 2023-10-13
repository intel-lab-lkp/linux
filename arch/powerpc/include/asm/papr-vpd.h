/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_POWERPC_PAPR_VPD_H
#define _ASM_POWERPC_PAPR_VPD_H

/*
 * This is here only so sys_rtas() can avoid disrupting VPD sequences
 * in progress in the papr-vpd driver. There's no other reason that
 * the rest of the kernel should call into papr-vpd.
 */
#ifdef CONFIG_PPC_PSERIES
void papr_vpd_mutex_lock(void);
void papr_vpd_mutex_unlock(void);
#else  /* CONFIG_PPC_PSERIES */
static inline void papr_vpd_mutex_lock(void) {}
static inline void papr_vpd_mutex_unlock(void) {}
#endif /* CONFIG_PPC_PSERIES */

#endif /* _ASM_POWERPC_PAPR_VPD_H */
