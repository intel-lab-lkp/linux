/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _ASM_RISCV_NTL_HINTS_H
#define _ASM_RISCV_NTL_HINTS_H

#define ntl_all()	({ __asm__ __volatile__("add x0, x0, x5" ::: ); })
#define ntl_p1()	({ __asm__ __volatile__("add x0, x0, x2" ::: ); })
#define ntl_pall()	({ __asm__ __volatile__("add x0, x0, x3" ::: ); })
#define ntl_s1()	({ __asm__ __volatile__("add x0, x0, x4" ::: ); })

#endif /* _ASM_RISCV_NTL_HINTS_H */
