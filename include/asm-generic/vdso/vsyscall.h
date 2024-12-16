/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_GENERIC_VSYSCALL_H
#define __ASM_GENERIC_VSYSCALL_H

#ifndef __ASSEMBLY__

#ifndef __arch_update_vsyscall
static __always_inline void __arch_update_vsyscall(struct vdso_time_data *vdata)
{
}
#endif /* __arch_update_vsyscall */

#ifndef __arch_sync_vdso_time_data
static __always_inline void __arch_sync_vdso_time_data(struct vdso_time_data *vdata)
{
}
#endif /* __arch_sync_vdso_time_data */

#endif /* !__ASSEMBLY__ */

#endif /* __ASM_GENERIC_VSYSCALL_H */
