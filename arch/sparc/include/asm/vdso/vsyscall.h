/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _ASM_SPARC_VDSO_VSYSCALL_H
#define _ASM_SPARC_VDSO_VSYSCALL_H

#define __VDSO_PAGES 4

#ifndef __ASSEMBLER__

/* The user mapping may alias the kernel one in the VIPT D-cache. */
struct vdso_time_data;
void __arch_sync_vdso_time_data(struct vdso_time_data *vdata);
#define __arch_sync_vdso_time_data __arch_sync_vdso_time_data

#endif /* !__ASSEMBLER__ */

/* The asm-generic header needs to be included after the definitions above */
#include <asm-generic/vdso/vsyscall.h>

#endif /* _ASM_SPARC_VDSO_VSYSCALL_H */
