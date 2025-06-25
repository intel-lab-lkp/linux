/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_VDSO_PROCESSOR_H
#define __ASM_VDSO_PROCESSOR_H

#ifndef __ASSEMBLY__

#include <asm/hwprobe.h>
#include <sys/hwprobe.h>
#include <asm/vendor/mips.h>
#include <asm-generic/barrier.h>

static inline void cpu_relax(void)
{
	struct riscv_hwprobe pair;
	bool has_mipspause;
#ifdef __riscv_muldiv
	int dummy;
	/* In lieu of a halt instruction, induce a long-latency stall. */
	__asm__ __volatile__ ("div %0, %0, zero" : "=r" (dummy));
#endif

	pair.key = RISCV_HWPROBE_KEY_VENDOR_EXT_MIPS_0;
	__riscv_hwprobe(&pair, 1, 0, NULL, 0);
	has_mipspause = pair.value & RISCV_HWPROBE_VENDOR_EXT_XMIPSEXECTL;

	if (has_mipspause) {
		/* Encoding of the mips pause instruction */
		__asm__ __volatile__(".4byte 0x00501013");
	} else {
		/* Encoding of the pause instruction */
		__asm__ __volatile__(".4byte 0x100000F");
	}

	barrier();
}

#endif /* __ASSEMBLY__ */

#endif /* __ASM_VDSO_PROCESSOR_H */
