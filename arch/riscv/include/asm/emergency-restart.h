/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _ASM_RISCV_EMERGENCY_RESTART_H
#define _ASM_RISCV_EMERGENCY_RESTART_H

#include <asm/sbi.h>

static inline void machine_emergency_restart(void)
{
	machine_restart(RISCV_EMERGENCY_RESTART_REASON);
}

#endif /* _ASM_RISCV_EMERGENCY_RESTART_H */
