/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_EFI_H
#define _ASM_EFI_H

#include <asm/boot.h>
#include <asm/cpufeature.h>
#include <asm/fpsimd.h>
#include <asm/io.h>
#include <asm/memory.h>
#include <asm/mmu_context.h>
#include <asm/neon.h>
#include <asm/ptrace.h>
#include <asm/tlbflush.h>

#ifdef CONFIG_EFI
extern void efi_init(void);
extern void arm64_efi_init(void);

bool efi_runtime_fixup_exception(struct pt_regs *regs, const char *msg);
#else
#define efi_init()
#define arm64_efi_init()

static inline
bool efi_runtime_fixup_exception(struct pt_regs *regs, const char *msg)
{
	return false;
}
#endif

int efi_create_mapping(struct mm_struct *mm, efi_memory_desc_t *md);
int efi_set_mapping_permissions(struct mm_struct *mm, efi_memory_desc_t *md,
				bool has_bti);

#undef arch_efi_call_virt
#define arch_efi_call_virt(p, f, args...)				\
	__efi_rt_asm_wrapper((p)->f, #f, args)

extern u64 *efi_rt_stack_top;
efi_status_t __efi_rt_asm_wrapper(void *, const char *, ...);

void arch_efi_call_virt_setup(void);
void arch_efi_call_virt_teardown(void);

/*
 * efi_rt_stack_top[-1] contains the value the stack pointer had before
 * switching to the EFI runtime stack.
 */
#define current_in_efi()						\
	(efi_rt_stack_top != NULL &&					\
	 on_task_stack(current, READ_ONCE(efi_rt_stack_top[-1]), 1))

#define ARCH_EFI_IRQ_FLAGS_MASK (PSR_D_BIT | PSR_A_BIT | PSR_I_BIT | PSR_F_BIT)

/*
 * Even when Linux uses IRQ priorities for IRQ disabling, EFI does not.
 * And EFI shouldn't really play around with priority masking as it is not aware
 * which priorities the OS has assigned to its interrupts.
 */
#define arch_efi_save_flags(state_flags)		\
	((void)((state_flags) = read_sysreg(daif)))

#define arch_efi_restore_flags(state_flags)	write_sysreg(state_flags, daif)


/* arch specific definitions used by the stub code */

/*
 * In some configurations (e.g. VMAP_STACK && 64K pages), stacks built into the
 * kernel need greater alignment than we require the segments to be padded to.
 */
#define EFI_KIMG_ALIGN	\
	(SEGMENT_ALIGN > THREAD_ALIGN ? SEGMENT_ALIGN : THREAD_ALIGN)

/*
 * On arm64, we have to ensure that the initrd ends up in the linear region,
 * which is a 1 GB aligned region of size '1UL << (VA_BITS_MIN - 1)' that is
 * guaranteed to cover the kernel Image.
 *
 * Since the EFI stub is part of the kernel Image, we can relax the
 * usual requirements in Documentation/arch/arm64/booting.rst, which still
 * apply to other bootloaders, and are required for some kernel
 * configurations.
 */
static inline unsigned long efi_get_max_initrd_addr(unsigned long image_addr)
{
	return (image_addr & ~(SZ_1G - 1UL)) + (1UL << (VA_BITS_MIN - 1));
}

static inline unsigned long efi_get_kimg_min_align(void)
{
	extern bool efi_nokaslr;

	/*
	 * Although relocatable kernels can fix up the misalignment with
	 * respect to MIN_KIMG_ALIGN, the resulting virtual text addresses are
	 * subtly out of sync with those recorded in the vmlinux when kaslr is
	 * disabled but the image required relocation anyway. Therefore retain
	 * 2M alignment if KASLR was explicitly disabled, even if it was not
	 * going to be activated to begin with.
	 */
	return efi_nokaslr ? MIN_KIMG_ALIGN : EFI_KIMG_ALIGN;
}

#define EFI_ALLOC_ALIGN		SZ_64K
#define EFI_ALLOC_LIMIT		((1UL << 48) - 1)

extern unsigned long primary_entry_offset(void);

/*
 * On ARM systems, virtually remapped UEFI runtime services are set up in two
 * distinct stages:
 * - The stub retrieves the final version of the memory map from UEFI, populates
 *   the virt_addr fields and calls the SetVirtualAddressMap() [SVAM] runtime
 *   service to communicate the new mapping to the firmware (Note that the new
 *   mapping is not live at this time)
 * - During an early initcall(), the EFI system table is permanently remapped
 *   and the virtual remapping of the UEFI Runtime Services regions is loaded
 *   into a private set of page tables. If this all succeeds, the Runtime
 *   Services are enabled and the EFI_RUNTIME_SERVICES bit set.
 */

static inline void efi_set_pgd(struct mm_struct *mm)
{
	__switch_mm(mm);

	if (system_uses_ttbr0_pan()) {
		if (mm != current->active_mm) {
			/*
			 * Update the current thread's saved ttbr0 since it is
			 * restored as part of a return from exception.
			 */
			update_saved_ttbr0(current, mm);
		} else {
			/*
			 * Restore the current thread's saved ttbr0
			 * corresponding to its active_mm
			 */
			update_saved_ttbr0(current, current->active_mm);
		}
	}
}

void efi_virtmap_load(void);
void efi_virtmap_unload(void);

static inline void efi_capsule_flush_cache_range(void *addr, int size)
{
	dcache_clean_inval_poc((unsigned long)addr, (unsigned long)addr + size);
}

efi_status_t efi_handle_corrupted_x18(efi_status_t s, const char *f);

void efi_icache_sync(unsigned long start, unsigned long end);

/*
 * PSCI handler exposed by the firmware through
 * LINUX_EFI_ARM_PSCI_HANDLER_TABLE_GUID.
 *
 * Unlike the regular EFI runtime services, this handler is invoked as a plain
 * call under the EFI virtual mapping and not through the runtime services
 * dispatch path. The OS therefore does not set up the usual runtime call
 * environment around it, which imposes two requirements on the firmware:
 *
 *  - The handler must not corrupt FP/SIMD/SVE or any other lazily-saved CPU
 *    state.
 *
 *  - The handler must be reentrant and independent of the other EFI runtime
 *    services. It is only guaranteed to run with interrupts disabled on the
 *    core it is called.
 */
typedef unsigned long efi_psci_handler_t(unsigned long function_id,
					 unsigned long arg0,
					 unsigned long arg1,
					 unsigned long arg2);

#define EFI_PSCI_MAX_FN 0x20

/**
 * struct efi_psci_table - firmware-provided PSCI conduit table
 * @version:      PSCI version implemented by the firmware, in the format
 *                returned by PSCI_VERSION (major in bits 16-30, minor in
 *                bits 0-15).
 * @num_features: Number of valid entries in @features. Function numbers at
 *                or above this value are treated as not supported. Guaranteed
 *                to be at least 0x20.
 * @psci_handler: Firmware entry point invoked for PSCI calls that are not
 *                served from the cached fields above. Called via
 *                arm64_efi_psci_call() with the EFI runtime mapping active.
 * @features:     PSCI_FEATURES results indexed by PSCI function number (the
 *                low byte of the function ID, which is identical for the
 *                SMC32 and SMC64 variants).
 *
 * Populated at boot from the firmware's PSCI handler configuration table
 * (LINUX_EFI_ARM_PSCI_HANDLER_TABLE_GUID). @version, @num_features and
 * @features cache the answers to the PSCI calls that must be serviced before
 * EFI runtime services can be invoked. All other calls are forwarded to
 * @psci_handler using arm64_efi_psci_call().
 */
extern struct efi_psci_table {
	u32 version;
	u32 num_features;
	efi_psci_handler_t *psci_handler;
	s32 features[EFI_PSCI_MAX_FN];
} efi_psci;

unsigned long arm64_efi_psci_call(unsigned long function_id, unsigned long arg0,
				  unsigned long arg1, unsigned long arg2);

#endif /* _ASM_EFI_H */
