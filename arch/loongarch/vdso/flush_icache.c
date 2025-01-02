// SPDX-License-Identifier: GPL-2.0-only
/*
 * Fast user context implementation of getcpu()
 */

#include <asm/vdso.h>
#include <asm/unistd.h>

static int flush_icache_ibar(void)
{
	__asm__ __volatile__ ("\tibar 0\n"::);

	return 0;
}

static int flush_icache_fallback(uintptr_t start, uintptr_t end,
				  uintptr_t flags)
{
	register long _num  __asm__ ("a7") = __NR_loongarch_flush_icache;
	register long _arg1 __asm__ ("a0") = (long)(start);
	register long _arg2 __asm__ ("a1") = (long)(end);
	register long _arg3 __asm__ ("a2") = (long)(flags);

	__asm__ volatile (
		"syscall 0\n"
		: "+r"(_arg1)
		: "r"(_arg2), "r"(_arg3),
		  "r"(_num)
		: "memory", "$t0", "$t1", "$t2", "$t3", "$t4", "$t5",
		  "$t6", "$t7", "$t8"
	);

	return _arg1;
}

extern int __vdso_flush_icache(uintptr_t start, uintptr_t end,
			       uintptr_t flags);
int __vdso_flush_icache(uintptr_t start, uintptr_t end, uintptr_t flags)
{

	switch (_loongarch_data.icache_flush_data.mode) {
	case VDSO_ICACLE_FLUSH_IBAR:
		return flush_icache_ibar();
	case VDSO_ICACLE_FLUSH_FALLBACK:
	default:
		return flush_icache_fallback(start, end, flags);
	}

	return -EINVAL;
}
