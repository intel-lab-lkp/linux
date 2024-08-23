/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_X86_MADT_WAKEUP_H
#define __ASM_X86_MADT_WAKEUP_H

void asm_acpi_mp_play_dead(u64 reset_vector, u64 pgd_pa);

#if defined(CONFIG_OF) && defined(CONFIG_ACPI_MADT_WAKEUP)
u64 dtb_parse_mp_wake(void);
#else
static inline u64 dtb_parse_mp_wake(void)
{
	return 0;
}
#endif

#endif /* __ASM_X86_MADT_WAKEUP_H */
