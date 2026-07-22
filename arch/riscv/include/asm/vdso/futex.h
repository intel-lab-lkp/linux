/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __ASM_VDSO_FUTEX_H
#define __ASM_VDSO_FUTEX_H

#include <asm/cpufeature-macros.h>

static inline bool cpu_supports_zacas(void)
{
	return IS_ENABLED(CONFIG_RISCV_ISA_ZACAS) && IS_ENABLED(CONFIG_TOOLCHAIN_HAS_ZACAS) &&
		riscv_has_extension_unlikely(RISCV_ISA_EXT_ZACAS);
}

#endif /* __ASM_VDSO_FUTEX_H */
