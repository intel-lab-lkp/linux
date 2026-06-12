// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * AVX-512 optimized implementation of xor_gen()
 *
 * Copyright 2026 Google LLC
 */

#include <linux/compiler.h>
#include <linux/types.h>
#include <asm/fpu/api.h>
#include "xor_impl.h"
#include "xor_arch.h"

struct block64 {
	u8 x[64];
} __aligned(64);

/*
 * Use different registers for each unrolled iteration just in case it helps,
 * though the hardware register renamer should make it unnecessary.
 */

#define DO_XOR2(i, reg0)                                   \
	asm volatile("vmovdqa64 %0, %%" reg0 "\n"          \
		     "vpxorq %1, %%" reg0 ", %%" reg0 "\n" \
		     "vmovdqa64 %%" reg0 ", %0\n"          \
		     : "+m"(p0[i])                         \
		     : "m"(p1[i]))

#define DO_XOR3(i, reg0, reg1)                                        \
	asm volatile("vmovdqa64 %0, %%" reg0 "\n"                     \
		     "vmovdqa64 %1, %%" reg1 "\n"                     \
		     "vpternlogq $0x96, %2, %%" reg1 ", %%" reg0 "\n" \
		     "vmovdqa64 %%" reg0 ", %0\n"                     \
		     : "+m"(p0[i])                                    \
		     : "m"(p1[i]), "m"(p2[i]))

#define DO_XOR4(i, reg0, reg1)                                        \
	asm volatile("vmovdqa64 %0, %%" reg0 "\n"                     \
		     "vmovdqa64 %1, %%" reg1 "\n"                     \
		     "vpxorq %2, %%" reg0 ", %%" reg0 "\n"            \
		     "vpternlogq $0x96, %3, %%" reg1 ", %%" reg0 "\n" \
		     "vmovdqa64 %%" reg0 ", %0\n"                     \
		     : "+m"(p0[i])                                    \
		     : "m"(p1[i]), "m"(p2[i]), "m"(p3[i]))

#define DO_XOR5(i, reg0, reg1)                                        \
	asm volatile("vmovdqa64 %0, %%" reg0 "\n"                     \
		     "vmovdqa64 %1, %%" reg1 "\n"                     \
		     "vpternlogq $0x96, %2, %%" reg1 ", %%" reg0 "\n" \
		     "vmovdqa64 %3, %%" reg1 "\n"                     \
		     "vpternlogq $0x96, %4, %%" reg1 ", %%" reg0 "\n" \
		     "vmovdqa64 %%" reg0 ", %0\n"                     \
		     : "+m"(p0[i])                                    \
		     : "m"(p1[i]), "m"(p2[i]), "m"(p3[i]), "m"(p4[i]))

static void xor_avx512_2(size_t bytes, struct block64 *p0,
			 const struct block64 *p1)
{
	do {
		DO_XOR2(0, "zmm0");
		DO_XOR2(1, "zmm1");
		DO_XOR2(2, "zmm2");
		DO_XOR2(3, "zmm3");
		DO_XOR2(4, "zmm4");
		DO_XOR2(5, "zmm5");
		DO_XOR2(6, "zmm6");
		DO_XOR2(7, "zmm7");
		p0 += 512 / sizeof(*p0);
		p1 += 512 / sizeof(*p1);
		bytes -= 512;
	} while (bytes);
}

static void xor_avx512_3(size_t bytes, struct block64 *p0,
			 const struct block64 *p1, const struct block64 *p2)
{
	do {
		DO_XOR3(0, "zmm0", "zmm1");
		DO_XOR3(1, "zmm2", "zmm3");
		DO_XOR3(2, "zmm4", "zmm5");
		DO_XOR3(3, "zmm6", "zmm7");
		DO_XOR3(4, "zmm8", "zmm9");
		DO_XOR3(5, "zmm10", "zmm11");
		DO_XOR3(6, "zmm12", "zmm13");
		DO_XOR3(7, "zmm14", "zmm15");
		p0 += 512 / sizeof(*p0);
		p1 += 512 / sizeof(*p1);
		p2 += 512 / sizeof(*p2);
		bytes -= 512;
	} while (bytes);
}

static void xor_avx512_4(size_t bytes, struct block64 *p0,
			 const struct block64 *p1, const struct block64 *p2,
			 const struct block64 *p3)
{
	do {
		DO_XOR4(0, "zmm0", "zmm1");
		DO_XOR4(1, "zmm2", "zmm3");
		DO_XOR4(2, "zmm4", "zmm5");
		DO_XOR4(3, "zmm6", "zmm7");
		DO_XOR4(4, "zmm8", "zmm9");
		DO_XOR4(5, "zmm10", "zmm11");
		DO_XOR4(6, "zmm12", "zmm13");
		DO_XOR4(7, "zmm14", "zmm15");
		p0 += 512 / sizeof(*p0);
		p1 += 512 / sizeof(*p1);
		p2 += 512 / sizeof(*p2);
		p3 += 512 / sizeof(*p3);
		bytes -= 512;
	} while (bytes);
}

static void xor_avx512_5(size_t bytes, struct block64 *p0,
			 const struct block64 *p1, const struct block64 *p2,
			 const struct block64 *p3, const struct block64 *p4)
{
	do {
		DO_XOR5(0, "zmm0", "zmm1");
		DO_XOR5(1, "zmm2", "zmm3");
		DO_XOR5(2, "zmm4", "zmm5");
		DO_XOR5(3, "zmm6", "zmm7");
		DO_XOR5(4, "zmm8", "zmm9");
		DO_XOR5(5, "zmm10", "zmm11");
		DO_XOR5(6, "zmm12", "zmm13");
		DO_XOR5(7, "zmm14", "zmm15");
		p0 += 512 / sizeof(*p0);
		p1 += 512 / sizeof(*p1);
		p2 += 512 / sizeof(*p2);
		p3 += 512 / sizeof(*p3);
		p4 += 512 / sizeof(*p4);
		bytes -= 512;
	} while (bytes);
}

DO_XOR_BLOCKS(avx512_inner, xor_avx512_2, xor_avx512_3, xor_avx512_4,
	      xor_avx512_5);

/*
 * Preconditions: bytes is a nonzero multiple of 512, and all buffers are
 * 64-byte aligned.
 */
static void xor_gen_avx512(void *dest, void **srcs, unsigned int src_cnt,
			   unsigned int bytes)
{
	kernel_fpu_begin();
	xor_gen_avx512_inner(dest, srcs, src_cnt, bytes);
	kernel_fpu_end();
}

struct xor_block_template xor_block_avx512 = {
	.name = "avx512",
	.xor_gen = xor_gen_avx512,
};
