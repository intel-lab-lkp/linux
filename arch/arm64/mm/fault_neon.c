/*
 * These functions require asimd, which is not accepted by Clang in normal
 * kernel code, which is compiled with -mgeneral-regs-only. GCC will somehow
 * eat it regardless, but we want it to be portable, so move these in their
 * own translation unit. This allows us to turn off -mgeneral-regs-only for
 * these (where it should be harmless) without risking the compiler doing
 * wrong things in places where we don't want it to.
 *
 * Otherwise this is identical to the original patch.
 *
 * -- q66 <q66@chimera-linux.org>
 *
 */

#include <linux/types.h>

u64 __arm64_get_vn_dt(int n, int t) {
	u64 res;

	switch (n) {
#define V(n)						\
	case n:						\
		asm("cbnz %w1, 1f\n\t"			\
		    "mov %0, v"#n".d[0]\n\t"		\
		    "b 2f\n\t"				\
		    "1: mov %0, v"#n".d[1]\n\t"		\
		    "2:" : "=r" (res) : "r" (t));	\
		break
	V( 0); V( 1); V( 2); V( 3); V( 4); V( 5); V( 6); V( 7);
	V( 8); V( 9); V(10); V(11); V(12); V(13); V(14); V(15);
	V(16); V(17); V(18); V(19); V(20); V(21); V(22); V(23);
	V(24); V(25); V(26); V(27); V(28); V(29); V(30); V(31);
#undef V
	default:
		res = 0;
		break;
	}
	return res;
}

void __arm64_set_vn_dt(int n, int t, u64 val) {
	switch (n) {
#define V(n)						\
	case n:						\
		asm("cbnz %w1, 1f\n\t"			\
		    "mov v"#n".d[0], %0\n\t"		\
		    "b 2f\n\t"				\
		    "1: mov v"#n".d[1], %0\n\t"		\
		    "2:" :: "r" (val), "r" (t));	\
		break
	V( 0); V( 1); V( 2); V( 3); V( 4); V( 5); V( 6); V( 7);
	V( 8); V( 9); V(10); V(11); V(12); V(13); V(14); V(15);
	V(16); V(17); V(18); V(19); V(20); V(21); V(22); V(23);
	V(24); V(25); V(26); V(27); V(28); V(29); V(30); V(31);
#undef Q
	default:
		break;
	}
}
