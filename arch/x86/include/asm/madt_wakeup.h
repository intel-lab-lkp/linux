/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_X86_MADT_WAKEUP_H
#define __ASM_X86_MADT_WAKEUP_H

void asm_acpi_mp_play_dead(u64 reset_vector, u64 pgd_pa);

#if defined(CONFIG_OF) && defined(CONFIG_ACPI_MADT_WAKEUP)
int dtb_parse_mp_wake(u64 *wake_mailbox_paddr);
#else
static inline int dtb_parse_mp_wake(u64 *wake_mailbox_paddr)
{
	return -ENODEV;
}
#endif

#endif /* __ASM_X86_MADT_WAKEUP_H */
