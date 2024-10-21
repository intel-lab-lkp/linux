/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LOCAL_SYSREG_H_
#define __LOCAL_SYSREG_H_
#include <asm/sysreg.h>

#ifndef SYS_RMR_EL3
#define SYS_RMR_EL3			sys_reg(3, 6, 12, 0, 2)
#endif

#define SYS_IMP_CPUECTLR_EL1		sys_reg(3, 0, 15, 1, 4)
#define SYS_IMP_CPUACTLR_EL3		sys_reg(3, 6, 15, 4, 0)
#define SYS_IMP_CPUPPMCR_EL3		sys_reg(3, 6, 15, 2, 0)
#define SYS_IMP_CPUPPMCR2_EL3		sys_reg(3, 6, 15, 2, 1)
#define SYS_IMP_CPUPPMCR4_EL3		sys_reg(3, 6, 15, 2, 4)
#define SYS_IMP_CPUPPMCR5_EL3		sys_reg(3, 6, 15, 2, 5)
#define SYS_IMP_CPUPPMCR6_EL3		sys_reg(3, 6, 15, 2, 6)

#define SYS_VNCR_EL2			sys_reg(3, 4, 2, 2, 0)

#endif
