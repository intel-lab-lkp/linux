/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _ASM_VERMAGIC_H
#define _ASM_VERMAGIC_H

#ifdef CONFIG_X86_64
/* X86_64 does not define MODULE_PROC_FAMILY */
#elif CONFIG_X86_MINIMUM_CPU_FAMILY == 6
#define MODULE_PROC_FAMILY "686 "
#else
#define MODULE_PROC_FAMILY "586 "
#endif

#ifdef CONFIG_X86_32
# define MODULE_ARCH_VERMAGIC MODULE_PROC_FAMILY
#else
# define MODULE_ARCH_VERMAGIC ""
#endif

#endif /* _ASM_VERMAGIC_H */
