/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Helpers used for Secure AVIC guests
 *
 */
#ifndef SELFTEST_KVM_SAVIC_H
#define SELFTEST_KVM_SAVIC_H

#define APIC_REG_OFF(VEC)		(VEC / 32 * 16)
#define APIC_VEC_POS(VEC)		(VEC % 32)

struct guest_apic_page;

void guest_apic_pages_init(struct kvm_vm *vm);
void set_savic_control_msr(struct guest_apic_page *apic_page, bool enable, bool enable_nmi);
void savic_write_reg(struct guest_apic_page *apic_page, uint32_t reg, uint64_t val);
uint64_t savic_read_reg(struct guest_apic_page *apic_page, uint32_t reg);
void savic_hv_write_reg(uint32_t reg, uint64_t val);
uint64_t savic_hv_read_reg(uint32_t reg);
void savic_enable(void);
int savic_nr_pages_required(uint64_t page_size);
void savic_vc_handler(struct ex_regs *regs);
struct guest_apic_page *get_guest_apic_page(void);
void savic_allow_vector(int vec);
#endif
