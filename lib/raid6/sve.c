// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * RAID-6 syndrome calculation using ARM SVE instructions
 */

#include <linux/raid/pq.h>

#ifdef __KERNEL__
#include <asm/simd.h>
#include <linux/cpufeature.h>
#else
#define scoped_ksimd()
#define system_supports_sve() (1)
#endif

static void raid6_sve1_gen_syndrome_real(int disks, unsigned long bytes, void **ptrs)
{
	u8 **dptr = (u8 **)ptrs;
	u8 *p, *q;
	int z0 = disks - 3;

	p = dptr[z0 + 1];
	q = dptr[z0 + 2];

	asm volatile(
		".arch armv8.2-a+sve\n"
		"ptrue p0.b\n"
		"cntb x3\n"
		"mov w4, #0x1d\n"
		"dup z4.b, w4\n"
		"mov x5, #0\n"

		"0:\n"
		"ldr x6, [%[dptr], %[z0], lsl #3]\n"
		"ld1b z0.b, p0/z, [x6, x5]\n"
		"mov z1.d, z0.d\n"

		"mov w7, %w[z0]\n"
		"sub w7, w7, #1\n"

		"1:\n"
		"cmp w7, #0\n"
		"blt 2f\n"

		"mov z3.d, z1.d\n"
		"asr z3.b, p0/m, z3.b, #7\n"
		"lsl z1.b, p0/m, z1.b, #1\n"

		"and z3.d, z3.d, z4.d\n"
		"eor z1.d, z1.d, z3.d\n"

		"sxtw x8, w7\n"
		"ldr x6, [%[dptr], x8, lsl #3]\n"
		"ld1b z2.b, p0/z, [x6, x5]\n"

		"eor z1.d, z1.d, z2.d\n"
		"eor z0.d, z0.d, z2.d\n"

		"sub w7, w7, #1\n"
		"b 1b\n"
		"2:\n"

		"st1b z0.b, p0, [%[p], x5]\n"
		"st1b z1.b, p0, [%[q], x5]\n"

		"add x5, x5, x3\n"
		"cmp x5, %[bytes]\n"
		"blt 0b\n"
		:
		: [dptr] "r" (dptr), [z0] "r" (z0), [bytes] "r" (bytes),
		  [p] "r" (p), [q] "r" (q)
		: "memory", "p0", "x3", "x4", "x5", "x6", "x7", "x8",
		  "z0", "z1", "z2", "z3", "z4"
	);
}

static void raid6_sve1_xor_syndrome_real(int disks, int start, int stop,
					 unsigned long bytes, void **ptrs)
{
	u8 **dptr = (u8 **)ptrs;
	u8 *p, *q;
	int z0 = stop;

	p = dptr[disks - 2];
	q = dptr[disks - 1];

	asm volatile(
		".arch armv8.2-a+sve\n"
		"ptrue p0.b\n"
		"cntb x3\n"
		"mov w4, #0x1d\n"
		"dup z4.b, w4\n"
		"mov x5, #0\n"

		"0:\n"
		"ldr x6, [%[dptr], %[z0], lsl #3]\n"
		"ld1b z1.b, p0/z, [x6, x5]\n"
		"ld1b z0.b, p0/z, [%[p], x5]\n"
		"eor z0.d, z0.d, z1.d\n"

		"mov w7, %w[z0]\n"
		"sub w7, w7, #1\n"

		"1:\n"
		"cmp w7, %w[start]\n"
		"blt 2f\n"

		"mov z3.d, z1.d\n"
		"asr z3.b, p0/m, z3.b, #7\n"
		"lsl z1.b, p0/m, z1.b, #1\n"
		"and z3.d, z3.d, z4.d\n"
		"eor z1.d, z1.d, z3.d\n"

		"sxtw x8, w7\n"
		"ldr x6, [%[dptr], x8, lsl #3]\n"
		"ld1b z2.b, p0/z, [x6, x5]\n"

		"eor z1.d, z1.d, z2.d\n"
		"eor z0.d, z0.d, z2.d\n"

		"sub w7, w7, #1\n"
		"b 1b\n"
		"2:\n"

		"mov w7, %w[start]\n"
		"sub w7, w7, #1\n"
		"3:\n"
		"cmp w7, #0\n"
		"blt 4f\n"

		"mov z3.d, z1.d\n"
		"asr z3.b, p0/m, z3.b, #7\n"
		"lsl z1.b, p0/m, z1.b, #1\n"
		"and z3.d, z3.d, z4.d\n"
		"eor z1.d, z1.d, z3.d\n"

		"sub w7, w7, #1\n"
		"b 3b\n"
		"4:\n"

		"ld1b z2.b, p0/z, [%[q], x5]\n"
		"eor z1.d, z1.d, z2.d\n"

		"st1b z0.b, p0, [%[p], x5]\n"
		"st1b z1.b, p0, [%[q], x5]\n"

		"add x5, x5, x3\n"
		"cmp x5, %[bytes]\n"
		"blt 0b\n"
		:
		: [dptr] "r" (dptr), [z0] "r" (z0), [bytes] "r" (bytes),
		  [p] "r" (p), [q] "r" (q), [start] "r" (start)
		: "memory", "p0", "x3", "x4", "x5", "x6", "x7", "x8",
		  "z0", "z1", "z2", "z3", "z4"
	);
}

static void raid6_sve2_gen_syndrome_real(int disks, unsigned long bytes, void **ptrs)
{
	u8 **dptr = (u8 **)ptrs;
	u8 *p, *q;
	int z0 = disks - 3;

	p = dptr[z0 + 1];
	q = dptr[z0 + 2];

	asm volatile(
		".arch armv8.2-a+sve\n"
		"ptrue p0.b\n"
		"cntb x3\n"
		"mov w4, #0x1d\n"
		"dup z4.b, w4\n"
		"mov x5, #0\n"

		"0:\n"
		"ldr x6, [%[dptr], %[z0], lsl #3]\n"
		"ld1b z0.b, p0/z, [x6, x5]\n"
		"add x8, x5, x3\n"
		"ld1b z5.b, p0/z, [x6, x8]\n"
		"mov z1.d, z0.d\n"
		"mov z6.d, z5.d\n"

		"mov w7, %w[z0]\n"
		"sub w7, w7, #1\n"

		"1:\n"
		"cmp w7, #0\n"
		"blt 2f\n"

		"mov z3.d, z1.d\n"
		"asr z3.b, p0/m, z3.b, #7\n"
		"lsl z1.b, p0/m, z1.b, #1\n"
		"and z3.d, z3.d, z4.d\n"
		"eor z1.d, z1.d, z3.d\n"

		"mov z8.d, z6.d\n"
		"asr z8.b, p0/m, z8.b, #7\n"
		"lsl z6.b, p0/m, z6.b, #1\n"
		"and z8.d, z8.d, z4.d\n"
		"eor z6.d, z6.d, z8.d\n"

		"sxtw x8, w7\n"
		"ldr x6, [%[dptr], x8, lsl #3]\n"
		"ld1b z2.b, p0/z, [x6, x5]\n"
		"add x8, x5, x3\n"
		"ld1b z7.b, p0/z, [x6, x8]\n"

		"eor z1.d, z1.d, z2.d\n"
		"eor z0.d, z0.d, z2.d\n"

		"eor z6.d, z6.d, z7.d\n"
		"eor z5.d, z5.d, z7.d\n"

		"sub w7, w7, #1\n"
		"b 1b\n"
		"2:\n"

		"st1b z0.b, p0, [%[p], x5]\n"
		"st1b z1.b, p0, [%[q], x5]\n"
		"add x8, x5, x3\n"
		"st1b z5.b, p0, [%[p], x8]\n"
		"st1b z6.b, p0, [%[q], x8]\n"

		"add x5, x5, x3\n"
		"add x5, x5, x3\n"
		"cmp x5, %[bytes]\n"
		"blt 0b\n"
		:
		: [dptr] "r" (dptr), [z0] "r" (z0), [bytes] "r" (bytes),
		  [p] "r" (p), [q] "r" (q)
		: "memory", "p0", "x3", "x4", "x5", "x6", "x7", "x8",
		  "z0", "z1", "z2", "z3", "z4",
		  "z5", "z6", "z7", "z8"
	);
}

static void raid6_sve2_xor_syndrome_real(int disks, int start, int stop,
					 unsigned long bytes, void **ptrs)
{
	u8 **dptr = (u8 **)ptrs;
	u8 *p, *q;
	int z0 = stop;

	p = dptr[disks - 2];
	q = dptr[disks - 1];

	asm volatile(
		".arch armv8.2-a+sve\n"
		"ptrue p0.b\n"
		"cntb x3\n"
		"mov w4, #0x1d\n"
		"dup z4.b, w4\n"
		"mov x5, #0\n"

		"0:\n"
		"ldr x6, [%[dptr], %[z0], lsl #3]\n"
		"ld1b z1.b, p0/z, [x6, x5]\n"
		"add x8, x5, x3\n"
		"ld1b z6.b, p0/z, [x6, x8]\n"

		"ld1b z0.b, p0/z, [%[p], x5]\n"
		"ld1b z5.b, p0/z, [%[p], x8]\n"

		"eor z0.d, z0.d, z1.d\n"
		"eor z5.d, z5.d, z6.d\n"

		"mov w7, %w[z0]\n"
		"sub w7, w7, #1\n"

		"1:\n"
		"cmp w7, %w[start]\n"
		"blt 2f\n"

		"mov z3.d, z1.d\n"
		"asr z3.b, p0/m, z3.b, #7\n"
		"lsl z1.b, p0/m, z1.b, #1\n"
		"and z3.d, z3.d, z4.d\n"
		"eor z1.d, z1.d, z3.d\n"

		"mov z8.d, z6.d\n"
		"asr z8.b, p0/m, z8.b, #7\n"
		"lsl z6.b, p0/m, z6.b, #1\n"
		"and z8.d, z8.d, z4.d\n"
		"eor z6.d, z6.d, z8.d\n"

		"sxtw x8, w7\n"
		"ldr x6, [%[dptr], x8, lsl #3]\n"
		"ld1b z2.b, p0/z, [x6, x5]\n"
		"add x8, x5, x3\n"
		"ld1b z7.b, p0/z, [x6, x8]\n"

		"eor z1.d, z1.d, z2.d\n"
		"eor z0.d, z0.d, z2.d\n"

		"eor z6.d, z6.d, z7.d\n"
		"eor z5.d, z5.d, z7.d\n"

		"sub w7, w7, #1\n"
		"b 1b\n"
		"2:\n"

		"mov w7, %w[start]\n"
		"sub w7, w7, #1\n"
		"3:\n"
		"cmp w7, #0\n"
		"blt 4f\n"

		"mov z3.d, z1.d\n"
		"asr z3.b, p0/m, z3.b, #7\n"
		"lsl z1.b, p0/m, z1.b, #1\n"
		"and z3.d, z3.d, z4.d\n"
		"eor z1.d, z1.d, z3.d\n"

		"mov z8.d, z6.d\n"
		"asr z8.b, p0/m, z8.b, #7\n"
		"lsl z6.b, p0/m, z6.b, #1\n"
		"and z8.d, z8.d, z4.d\n"
		"eor z6.d, z6.d, z8.d\n"

		"sub w7, w7, #1\n"
		"b 3b\n"
		"4:\n"

		"ld1b z2.b, p0/z, [%[q], x5]\n"
		"eor z1.d, z1.d, z2.d\n"
		"st1b z0.b, p0, [%[p], x5]\n"
		"st1b z1.b, p0, [%[q], x5]\n"

		"add x8, x5, x3\n"
		"ld1b z7.b, p0/z, [%[q], x8]\n"
		"eor z6.d, z6.d, z7.d\n"
		"st1b z5.b, p0, [%[p], x8]\n"
		"st1b z6.b, p0, [%[q], x8]\n"

		"add x5, x5, x3\n"
		"add x5, x5, x3\n"
		"cmp x5, %[bytes]\n"
		"blt 0b\n"
		:
		: [dptr] "r" (dptr), [z0] "r" (z0), [bytes] "r" (bytes),
		  [p] "r" (p), [q] "r" (q), [start] "r" (start)
		: "memory", "p0", "x3", "x4", "x5", "x6", "x7", "x8",
		  "z0", "z1", "z2", "z3", "z4",
		  "z5", "z6", "z7", "z8"
	);
}

static void raid6_sve4_gen_syndrome_real(int disks, unsigned long bytes, void **ptrs)
{
	u8 **dptr = (u8 **)ptrs;
	u8 *p, *q;
	int z0 = disks - 3;

	p = dptr[z0 + 1];
	q = dptr[z0 + 2];

	asm volatile(
		".arch armv8.2-a+sve\n"
		"ptrue p0.b\n"
		"cntb x3\n"
		"mov w4, #0x1d\n"
		"dup z4.b, w4\n"
		"mov x5, #0\n"

		"0:\n"
		"ldr x6, [%[dptr], %[z0], lsl #3]\n"
		"ld1b z0.b, p0/z, [x6, x5]\n"
		"add x8, x5, x3\n"
		"ld1b z5.b, p0/z, [x6, x8]\n"
		"add x8, x8, x3\n"
		"ld1b z10.b, p0/z, [x6, x8]\n"
		"add x8, x8, x3\n"
		"ld1b z15.b, p0/z, [x6, x8]\n"

		"mov z1.d, z0.d\n"
		"mov z6.d, z5.d\n"
		"mov z11.d, z10.d\n"
		"mov z16.d, z15.d\n"

		"mov w7, %w[z0]\n"
		"sub w7, w7, #1\n"

		"1:\n"
		"cmp w7, #0\n"
		"blt 2f\n"

		// software pipelining: load data early
		"sxtw x8, w7\n"
		"ldr x6, [%[dptr], x8, lsl #3]\n"
		"ld1b z2.b, p0/z, [x6, x5]\n"
		"add x8, x5, x3\n"
		"ld1b z7.b, p0/z, [x6, x8]\n"
		"add x8, x8, x3\n"
		"ld1b z12.b, p0/z, [x6, x8]\n"
		"add x8, x8, x3\n"
		"ld1b z17.b, p0/z, [x6, x8]\n"

		// math block 1
		"mov z3.d, z1.d\n"
		"asr z3.b, p0/m, z3.b, #7\n"
		"lsl z1.b, p0/m, z1.b, #1\n"
		"and z3.d, z3.d, z4.d\n"
		"eor z1.d, z1.d, z3.d\n"
		"eor z1.d, z1.d, z2.d\n"
		"eor z0.d, z0.d, z2.d\n"

		// math block 2
		"mov z8.d, z6.d\n"
		"asr z8.b, p0/m, z8.b, #7\n"
		"lsl z6.b, p0/m, z6.b, #1\n"
		"and z8.d, z8.d, z4.d\n"
		"eor z6.d, z6.d, z8.d\n"
		"eor z6.d, z6.d, z7.d\n"
		"eor z5.d, z5.d, z7.d\n"

		// math block 3
		"mov z13.d, z11.d\n"
		"asr z13.b, p0/m, z13.b, #7\n"
		"lsl z11.b, p0/m, z11.b, #1\n"
		"and z13.d, z13.d, z4.d\n"
		"eor z11.d, z11.d, z13.d\n"
		"eor z11.d, z11.d, z12.d\n"
		"eor z10.d, z10.d, z12.d\n"

		// math block 4
		"mov z18.d, z16.d\n"
		"asr z18.b, p0/m, z18.b, #7\n"
		"lsl z16.b, p0/m, z16.b, #1\n"
		"and z18.d, z18.d, z4.d\n"
		"eor z16.d, z16.d, z18.d\n"
		"eor z16.d, z16.d, z17.d\n"
		"eor z15.d, z15.d, z17.d\n"

		"sub w7, w7, #1\n"
		"b 1b\n"
		"2:\n"

		"st1b z0.b, p0, [%[p], x5]\n"
		"st1b z1.b, p0, [%[q], x5]\n"
		"add x8, x5, x3\n"
		"st1b z5.b, p0, [%[p], x8]\n"
		"st1b z6.b, p0, [%[q], x8]\n"
		"add x8, x8, x3\n"
		"st1b z10.b, p0, [%[p], x8]\n"
		"st1b z11.b, p0, [%[q], x8]\n"
		"add x8, x8, x3\n"
		"st1b z15.b, p0, [%[p], x8]\n"
		"st1b z16.b, p0, [%[q], x8]\n"

		"add x8, x3, x3\n"
		"add x5, x5, x8, lsl #1\n"
		"cmp x5, %[bytes]\n"
		"blt 0b\n"
		:
		: [dptr] "r" (dptr), [z0] "r" (z0), [bytes] "r" (bytes),
		  [p] "r" (p), [q] "r" (q)
		: "memory", "p0", "x3", "x4", "x5", "x6", "x7", "x8",
		  "z0", "z1", "z2", "z3", "z4",
		  "z5", "z6", "z7", "z8",
		  "z10", "z11", "z12", "z13",
		  "z15", "z16", "z17", "z18"
	);
}

static void raid6_sve4_xor_syndrome_real(int disks, int start, int stop,
					 unsigned long bytes, void **ptrs)
{
	u8 **dptr = (u8 **)ptrs;
	u8 *p, *q;
	int z0 = stop;

	p = dptr[disks - 2];
	q = dptr[disks - 1];

	asm volatile(
		".arch armv8.2-a+sve\n"
		"ptrue p0.b\n"
		"cntb x3\n"
		"mov w4, #0x1d\n"
		"dup z4.b, w4\n"
		"mov x5, #0\n"

		"0:\n"
		"ldr x6, [%[dptr], %[z0], lsl #3]\n"
		"ld1b z1.b, p0/z, [x6, x5]\n"
		"add x8, x5, x3\n"
		"ld1b z6.b, p0/z, [x6, x8]\n"
		"add x8, x8, x3\n"
		"ld1b z11.b, p0/z, [x6, x8]\n"
		"add x8, x8, x3\n"
		"ld1b z16.b, p0/z, [x6, x8]\n"

		"ld1b z0.b, p0/z, [%[p], x5]\n"
		"add x8, x5, x3\n"
		"ld1b z5.b, p0/z, [%[p], x8]\n"
		"add x8, x8, x3\n"
		"ld1b z10.b, p0/z, [%[p], x8]\n"
		"add x8, x8, x3\n"
		"ld1b z15.b, p0/z, [%[p], x8]\n"

		"eor z0.d, z0.d, z1.d\n"
		"eor z5.d, z5.d, z6.d\n"
		"eor z10.d, z10.d, z11.d\n"
		"eor z15.d, z15.d, z16.d\n"

		"mov w7, %w[z0]\n"
		"sub w7, w7, #1\n"

		"1:\n"
		"cmp w7, %w[start]\n"
		"blt 2f\n"

		// software pipelining: load data early
		"sxtw x8, w7\n"
		"ldr x6, [%[dptr], x8, lsl #3]\n"
		"ld1b z2.b, p0/z, [x6, x5]\n"
		"add x8, x5, x3\n"
		"ld1b z7.b, p0/z, [x6, x8]\n"
		"add x8, x8, x3\n"
		"ld1b z12.b, p0/z, [x6, x8]\n"
		"add x8, x8, x3\n"
		"ld1b z17.b, p0/z, [x6, x8]\n"

		// math block 1
		"mov z3.d, z1.d\n"
		"asr z3.b, p0/m, z3.b, #7\n"
		"lsl z1.b, p0/m, z1.b, #1\n"
		"and z3.d, z3.d, z4.d\n"
		"eor z1.d, z1.d, z3.d\n"
		"eor z1.d, z1.d, z2.d\n"
		"eor z0.d, z0.d, z2.d\n"

		// math block 2
		"mov z8.d, z6.d\n"
		"asr z8.b, p0/m, z8.b, #7\n"
		"lsl z6.b, p0/m, z6.b, #1\n"
		"and z8.d, z8.d, z4.d\n"
		"eor z6.d, z6.d, z8.d\n"
		"eor z6.d, z6.d, z7.d\n"
		"eor z5.d, z5.d, z7.d\n"

		// math block 3
		"mov z13.d, z11.d\n"
		"asr z13.b, p0/m, z13.b, #7\n"
		"lsl z11.b, p0/m, z11.b, #1\n"
		"and z13.d, z13.d, z4.d\n"
		"eor z11.d, z11.d, z13.d\n"
		"eor z11.d, z11.d, z12.d\n"
		"eor z10.d, z10.d, z12.d\n"

		// math block 4
		"mov z18.d, z16.d\n"
		"asr z18.b, p0/m, z18.b, #7\n"
		"lsl z16.b, p0/m, z16.b, #1\n"
		"and z18.d, z18.d, z4.d\n"
		"eor z16.d, z16.d, z18.d\n"
		"eor z16.d, z16.d, z17.d\n"
		"eor z15.d, z15.d, z17.d\n"

		"sub w7, w7, #1\n"
		"b 1b\n"
		"2:\n"

		"mov w7, %w[start]\n"
		"sub w7, w7, #1\n"
		"3:\n"
		"cmp w7, #0\n"
		"blt 4f\n"

		// math block 1
		"mov z3.d, z1.d\n"
		"asr z3.b, p0/m, z3.b, #7\n"
		"lsl z1.b, p0/m, z1.b, #1\n"
		"and z3.d, z3.d, z4.d\n"
		"eor z1.d, z1.d, z3.d\n"

		// math block 2
		"mov z8.d, z6.d\n"
		"asr z8.b, p0/m, z8.b, #7\n"
		"lsl z6.b, p0/m, z6.b, #1\n"
		"and z8.d, z8.d, z4.d\n"
		"eor z6.d, z6.d, z8.d\n"

		// math block 3
		"mov z13.d, z11.d\n"
		"asr z13.b, p0/m, z13.b, #7\n"
		"lsl z11.b, p0/m, z11.b, #1\n"
		"and z13.d, z13.d, z4.d\n"
		"eor z11.d, z11.d, z13.d\n"

		// math block 4
		"mov z18.d, z16.d\n"
		"asr z18.b, p0/m, z18.b, #7\n"
		"lsl z16.b, p0/m, z16.b, #1\n"
		"and z18.d, z18.d, z4.d\n"
		"eor z16.d, z16.d, z18.d\n"

		"sub w7, w7, #1\n"
		"b 3b\n"
		"4:\n"

		// Load q and XOR
		"ld1b z2.b, p0/z, [%[q], x5]\n"
		"add x8, x5, x3\n"
		"ld1b z7.b, p0/z, [%[q], x8]\n"
		"add x8, x8, x3\n"
		"ld1b z12.b, p0/z, [%[q], x8]\n"
		"add x8, x8, x3\n"
		"ld1b z17.b, p0/z, [%[q], x8]\n"

		"eor z1.d, z1.d, z2.d\n"
		"eor z6.d, z6.d, z7.d\n"
		"eor z11.d, z11.d, z12.d\n"
		"eor z16.d, z16.d, z17.d\n"

		// Store results
		"st1b z0.b, p0, [%[p], x5]\n"
		"st1b z1.b, p0, [%[q], x5]\n"
		"add x8, x5, x3\n"
		"st1b z5.b, p0, [%[p], x8]\n"
		"st1b z6.b, p0, [%[q], x8]\n"
		"add x8, x8, x3\n"
		"st1b z10.b, p0, [%[p], x8]\n"
		"st1b z11.b, p0, [%[q], x8]\n"
		"add x8, x8, x3\n"
		"st1b z15.b, p0, [%[p], x8]\n"
		"st1b z16.b, p0, [%[q], x8]\n"

		"add x8, x3, x3\n"
		"add x5, x5, x8, lsl #1\n"
		"cmp x5, %[bytes]\n"
		"blt 0b\n"
		:
		: [dptr] "r" (dptr), [z0] "r" (z0), [bytes] "r" (bytes),
		  [p] "r" (p), [q] "r" (q), [start] "r" (start)
		: "memory", "p0", "x3", "x4", "x5", "x6", "x7", "x8",
		  "z0", "z1", "z2", "z3", "z4",
		  "z5", "z6", "z7", "z8",
		  "z10", "z11", "z12", "z13",
		  "z15", "z16", "z17", "z18"
	);
}

#define RAID6_SVE_WRAPPER(_n)						\
	static void raid6_sve ## _n ## _gen_syndrome(int disks,		\
					size_t bytes, void **ptrs)	\
	{								\
		scoped_ksimd()						\
		raid6_sve ## _n ## _gen_syndrome_real(disks,		\
					(unsigned long)bytes, ptrs);	\
	}								\
	static void raid6_sve ## _n ## _xor_syndrome(int disks,		\
					int start, int stop,		\
					size_t bytes, void **ptrs)	\
	{								\
		scoped_ksimd()						\
		raid6_sve ## _n ## _xor_syndrome_real(disks,		\
				start, stop, (unsigned long)bytes, ptrs);\
	}								\
	struct raid6_calls const raid6_svex ## _n = {			\
		raid6_sve ## _n ## _gen_syndrome,			\
		raid6_sve ## _n ## _xor_syndrome,			\
		raid6_have_sve,						\
		"svex" #_n,						\
		0							\
	}

static int raid6_have_sve(void)
{
	return system_supports_sve();
}

RAID6_SVE_WRAPPER(1);
RAID6_SVE_WRAPPER(2);
RAID6_SVE_WRAPPER(4);
