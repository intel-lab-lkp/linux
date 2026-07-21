/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_RISCV_SSPM_H
#define _ASM_RISCV_SSPM_H

#include <linux/errno.h>
#include <linux/init.h>
#include <linux/kconfig.h>
#include <linux/types.h>

#ifdef CONFIG_RISCV_SSPM
void __init riscv_sspm_boot_reset(void);
unsigned long riscv_sspm_hibernate_pmlen(void);
bool riscv_sspm_hibernate_pmlen_valid(unsigned long pmlen);
int riscv_sspm_hibernate_restore(unsigned long pmlen);
int riscv_sspm_prepare_cpu(void);
void __init riscv_sspm_smp_cpus_done(void);
bool riscv_sspm_may_be_active(void);
void riscv_sspm_reset_local_or_panic(const char *context);
#else
static inline void __init riscv_sspm_boot_reset(void)
{
}

static inline unsigned long riscv_sspm_hibernate_pmlen(void)
{
	return 0;
}

static inline bool riscv_sspm_hibernate_pmlen_valid(unsigned long pmlen)
{
	return pmlen == 0 || pmlen == 7;
}

static inline int riscv_sspm_hibernate_restore(unsigned long pmlen)
{
	if (!riscv_sspm_hibernate_pmlen_valid(pmlen))
		return -EINVAL;

	return pmlen ? -EOPNOTSUPP : 0;
}

static inline int riscv_sspm_prepare_cpu(void)
{
	return 0;
}

static inline void __init riscv_sspm_smp_cpus_done(void)
{
}

static inline bool riscv_sspm_may_be_active(void)
{
	return false;
}

static inline void riscv_sspm_reset_local_or_panic(const char *context)
{
}
#endif

#ifdef CONFIG_RISCV_ISA_SSPM
int riscv_sspm_enable(void);

/* True means the consumer contract is safe; false does not guarantee PMLEN=0. */
bool riscv_sspm_enabled(void);
#else
static inline int riscv_sspm_enable(void)
{
	return -EOPNOTSUPP;
}

static inline bool riscv_sspm_enabled(void)
{
	return false;
}
#endif

#endif /* _ASM_RISCV_SSPM_H */
