/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __KVM_FPU_H_
#define __KVM_FPU_H_

#include <asm/fpu/api.h>
#include <asm/kvm_vcpu_regs.h>

typedef u32		__attribute__((vector_size(16))) sse128_t;
#define __sse128_u	union { sse128_t vec; u64 as_u64[2]; u32 as_u32[4]; }
#define sse128_lo(x)	({ __sse128_u t; t.vec = x; t.as_u64[0]; })
#define sse128_hi(x)	({ __sse128_u t; t.vec = x; t.as_u64[1]; })
#define sse128_l0(x)	({ __sse128_u t; t.vec = x; t.as_u32[0]; })
#define sse128_l1(x)	({ __sse128_u t; t.vec = x; t.as_u32[1]; })
#define sse128_l2(x)	({ __sse128_u t; t.vec = x; t.as_u32[2]; })
#define sse128_l3(x)	({ __sse128_u t; t.vec = x; t.as_u32[3]; })
#define sse128(lo, hi)	({ __sse128_u t; t.as_u64[0] = lo; t.as_u64[1] = hi; t.vec; })

static inline void _kvm_read_sse_reg(int reg, sse128_t *data)
{
	switch (reg) {
	case 0: asm("movdqa %%xmm0, %0" : "=m"(*data)); break;
	case 1: asm("movdqa %%xmm1, %0" : "=m"(*data)); break;
	case 2: asm("movdqa %%xmm2, %0" : "=m"(*data)); break;
	case 3: asm("movdqa %%xmm3, %0" : "=m"(*data)); break;
	case 4: asm("movdqa %%xmm4, %0" : "=m"(*data)); break;
	case 5: asm("movdqa %%xmm5, %0" : "=m"(*data)); break;
	case 6: asm("movdqa %%xmm6, %0" : "=m"(*data)); break;
	case 7: asm("movdqa %%xmm7, %0" : "=m"(*data)); break;
#ifdef CONFIG_X86_64
	case 8: asm("movdqa %%xmm8, %0" : "=m"(*data)); break;
	case 9: asm("movdqa %%xmm9, %0" : "=m"(*data)); break;
	case 10: asm("movdqa %%xmm10, %0" : "=m"(*data)); break;
	case 11: asm("movdqa %%xmm11, %0" : "=m"(*data)); break;
	case 12: asm("movdqa %%xmm12, %0" : "=m"(*data)); break;
	case 13: asm("movdqa %%xmm13, %0" : "=m"(*data)); break;
	case 14: asm("movdqa %%xmm14, %0" : "=m"(*data)); break;
	case 15: asm("movdqa %%xmm15, %0" : "=m"(*data)); break;
#endif
	default: BUG();
	}
}

static inline void _kvm_write_sse_reg(int reg, const sse128_t *data)
{
	switch (reg) {
	case 0: asm("movdqa %0, %%xmm0" : : "m"(*data)); break;
	case 1: asm("movdqa %0, %%xmm1" : : "m"(*data)); break;
	case 2: asm("movdqa %0, %%xmm2" : : "m"(*data)); break;
	case 3: asm("movdqa %0, %%xmm3" : : "m"(*data)); break;
	case 4: asm("movdqa %0, %%xmm4" : : "m"(*data)); break;
	case 5: asm("movdqa %0, %%xmm5" : : "m"(*data)); break;
	case 6: asm("movdqa %0, %%xmm6" : : "m"(*data)); break;
	case 7: asm("movdqa %0, %%xmm7" : : "m"(*data)); break;
#ifdef CONFIG_X86_64
	case 8: asm("movdqa %0, %%xmm8" : : "m"(*data)); break;
	case 9: asm("movdqa %0, %%xmm9" : : "m"(*data)); break;
	case 10: asm("movdqa %0, %%xmm10" : : "m"(*data)); break;
	case 11: asm("movdqa %0, %%xmm11" : : "m"(*data)); break;
	case 12: asm("movdqa %0, %%xmm12" : : "m"(*data)); break;
	case 13: asm("movdqa %0, %%xmm13" : : "m"(*data)); break;
	case 14: asm("movdqa %0, %%xmm14" : : "m"(*data)); break;
	case 15: asm("movdqa %0, %%xmm15" : : "m"(*data)); break;
#endif
	default: BUG();
	}
}

static inline void _kvm_read_mmx_reg(int reg, u64 *data)
{
	switch (reg) {
	case 0: asm("movq %%mm0, %0" : "=m"(*data)); break;
	case 1: asm("movq %%mm1, %0" : "=m"(*data)); break;
	case 2: asm("movq %%mm2, %0" : "=m"(*data)); break;
	case 3: asm("movq %%mm3, %0" : "=m"(*data)); break;
	case 4: asm("movq %%mm4, %0" : "=m"(*data)); break;
	case 5: asm("movq %%mm5, %0" : "=m"(*data)); break;
	case 6: asm("movq %%mm6, %0" : "=m"(*data)); break;
	case 7: asm("movq %%mm7, %0" : "=m"(*data)); break;
	default: BUG();
	}
}

static inline void _kvm_write_mmx_reg(int reg, const u64 *data)
{
	switch (reg) {
	case 0: asm("movq %0, %%mm0" : : "m"(*data)); break;
	case 1: asm("movq %0, %%mm1" : : "m"(*data)); break;
	case 2: asm("movq %0, %%mm2" : : "m"(*data)); break;
	case 3: asm("movq %0, %%mm3" : : "m"(*data)); break;
	case 4: asm("movq %0, %%mm4" : : "m"(*data)); break;
	case 5: asm("movq %0, %%mm5" : : "m"(*data)); break;
	case 6: asm("movq %0, %%mm6" : : "m"(*data)); break;
	case 7: asm("movq %0, %%mm7" : : "m"(*data)); break;
	default: BUG();
	}
}

#ifdef CONFIG_X86_64
/*
 * Accessors for extended general-purpose registers. binutils >= 2.43 can
 * recognize those register symbols.
 */

static inline void _kvm_read_egpr(int reg, unsigned long *data)
{
	/* mov %r16..%r31, %rax */
	switch (reg) {
	case __VCPU_XREG_R16: asm(".byte 0xd5, 0x48, 0x89, 0xc0" : "=a"(*data)); break;
	case __VCPU_XREG_R17: asm(".byte 0xd5, 0x48, 0x89, 0xc8" : "=a"(*data)); break;
	case __VCPU_XREG_R18: asm(".byte 0xd5, 0x48, 0x89, 0xd0" : "=a"(*data)); break;
	case __VCPU_XREG_R19: asm(".byte 0xd5, 0x48, 0x89, 0xd8" : "=a"(*data)); break;
	case __VCPU_XREG_R20: asm(".byte 0xd5, 0x48, 0x89, 0xe0" : "=a"(*data)); break;
	case __VCPU_XREG_R21: asm(".byte 0xd5, 0x48, 0x89, 0xe8" : "=a"(*data)); break;
	case __VCPU_XREG_R22: asm(".byte 0xd5, 0x48, 0x89, 0xf0" : "=a"(*data)); break;
	case __VCPU_XREG_R23: asm(".byte 0xd5, 0x48, 0x89, 0xf8" : "=a"(*data)); break;
	case __VCPU_XREG_R24: asm(".byte 0xd5, 0x4c, 0x89, 0xc0" : "=a"(*data)); break;
	case __VCPU_XREG_R25: asm(".byte 0xd5, 0x4c, 0x89, 0xc8" : "=a"(*data)); break;
	case __VCPU_XREG_R26: asm(".byte 0xd5, 0x4c, 0x89, 0xd0" : "=a"(*data)); break;
	case __VCPU_XREG_R27: asm(".byte 0xd5, 0x4c, 0x89, 0xd8" : "=a"(*data)); break;
	case __VCPU_XREG_R28: asm(".byte 0xd5, 0x4c, 0x89, 0xe0" : "=a"(*data)); break;
	case __VCPU_XREG_R29: asm(".byte 0xd5, 0x4c, 0x89, 0xe8" : "=a"(*data)); break;
	case __VCPU_XREG_R30: asm(".byte 0xd5, 0x4c, 0x89, 0xf0" : "=a"(*data)); break;
	case __VCPU_XREG_R31: asm(".byte 0xd5, 0x4c, 0x89, 0xf8" : "=a"(*data)); break;
	default: BUG();
	}
}

static inline void _kvm_write_egpr(int reg, unsigned long *data)
{
	/* mov %rax, %r16...%r31*/
	switch (reg) {
	case __VCPU_XREG_R16: asm(".byte 0xd5, 0x18, 0x89, 0xc0" : : "a"(*data)); break;
	case __VCPU_XREG_R17: asm(".byte 0xd5, 0x18, 0x89, 0xc1" : : "a"(*data)); break;
	case __VCPU_XREG_R18: asm(".byte 0xd5, 0x18, 0x89, 0xc2" : : "a"(*data)); break;
	case __VCPU_XREG_R19: asm(".byte 0xd5, 0x18, 0x89, 0xc3" : : "a"(*data)); break;
	case __VCPU_XREG_R20: asm(".byte 0xd5, 0x18, 0x89, 0xc4" : : "a"(*data)); break;
	case __VCPU_XREG_R21: asm(".byte 0xd5, 0x18, 0x89, 0xc5" : : "a"(*data)); break;
	case __VCPU_XREG_R22: asm(".byte 0xd5, 0x18, 0x89, 0xc6" : : "a"(*data)); break;
	case __VCPU_XREG_R23: asm(".byte 0xd5, 0x18, 0x89, 0xc7" : : "a"(*data)); break;
	case __VCPU_XREG_R24: asm(".byte 0xd5, 0x19, 0x89, 0xc0" : : "a"(*data)); break;
	case __VCPU_XREG_R25: asm(".byte 0xd5, 0x19, 0x89, 0xc1" : : "a"(*data)); break;
	case __VCPU_XREG_R26: asm(".byte 0xd5, 0x19, 0x89, 0xc2" : : "a"(*data)); break;
	case __VCPU_XREG_R27: asm(".byte 0xd5, 0x19, 0x89, 0xc3" : : "a"(*data)); break;
	case __VCPU_XREG_R28: asm(".byte 0xd5, 0x19, 0x89, 0xc4" : : "a"(*data)); break;
	case __VCPU_XREG_R29: asm(".byte 0xd5, 0x19, 0x89, 0xc5" : : "a"(*data)); break;
	case __VCPU_XREG_R30: asm(".byte 0xd5, 0x19, 0x89, 0xc6" : : "a"(*data)); break;
	case __VCPU_XREG_R31: asm(".byte 0xd5, 0x19, 0x89, 0xc7" : : "a"(*data)); break;
	default: BUG();
	}
}
#endif

static inline void kvm_fpu_get(void)
{
	fpregs_lock();

	fpregs_assert_state_consistent();
	if (test_thread_flag(TIF_NEED_FPU_LOAD))
		switch_fpu_return();
}

static inline void kvm_fpu_put(void)
{
	fpregs_unlock();
}

static inline void kvm_read_sse_reg(int reg, sse128_t *data)
{
	kvm_fpu_get();
	_kvm_read_sse_reg(reg, data);
	kvm_fpu_put();
}

static inline void kvm_write_sse_reg(int reg, const sse128_t *data)
{
	kvm_fpu_get();
	_kvm_write_sse_reg(reg, data);
	kvm_fpu_put();
}

static inline void kvm_read_mmx_reg(int reg, u64 *data)
{
	kvm_fpu_get();
	_kvm_read_mmx_reg(reg, data);
	kvm_fpu_put();
}

static inline void kvm_write_mmx_reg(int reg, const u64 *data)
{
	kvm_fpu_get();
	_kvm_write_mmx_reg(reg, data);
	kvm_fpu_put();
}

#ifdef CONFIG_X86_64
static inline unsigned long kvm_read_egpr(int reg)
{
	unsigned long data;

	if (WARN_ON_ONCE(!cpu_has_xfeatures(XFEATURE_MASK_APX, NULL)))
		return 0;

	kvm_fpu_get();
	_kvm_read_egpr(reg, &data);
	kvm_fpu_put();

	return data;
}

static inline void kvm_write_egpr(int reg, unsigned long data)
{
	if (WARN_ON_ONCE(!cpu_has_xfeatures(XFEATURE_MASK_APX, NULL)))
		return;

	kvm_fpu_get();
	_kvm_write_egpr(reg, &data);
	kvm_fpu_put();
}
#endif

#endif
