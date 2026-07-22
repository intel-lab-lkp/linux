/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * ChaCha 256-bit cipher algorithm, x64 AVX-512 functions
 *
 * Copyright (C) 2026 Martin Willi
 */

#include <linux/linkage.h>

.section	.rodata.cst64.CTR16BL, "aM", @progbits, 64
.align 64
CTR16BL:	.octa 0x00000003000000020000000100000000
		.octa 0x00000007000000060000000500000004
		.octa 0x0000000b0000000a0000000900000008
		.octa 0x0000000f0000000e0000000d0000000c

.text

SYM_FUNC_START(chacha_16block_xor_avx512)
	# %rdi: Input state matrix, s
	# %rsi: up to 16 data blocks output, o
	# %rdx: up to 16 data blocks input, i
	# %rcx: input/output length in bytes
	# %r8d: nrounds

	# This function encrypts sixteen consecutive ChaCha blocks by loading
	# the state matrix in 512-bit AVX-512 (zmm) registers sixteen times.
	# Compared to AVX-512VL, this doubles the data-level parallelism using
	# full-width zmm registers, which pays off on CPUs that do not throttle
	# on 512-bit instructions.

	vzeroupper

	# x0..15[0-15] = s[0..15]
	vpbroadcastd	0x00(%rdi),%zmm0
	vpbroadcastd	0x04(%rdi),%zmm1
	vpbroadcastd	0x08(%rdi),%zmm2
	vpbroadcastd	0x0c(%rdi),%zmm3
	vpbroadcastd	0x10(%rdi),%zmm4
	vpbroadcastd	0x14(%rdi),%zmm5
	vpbroadcastd	0x18(%rdi),%zmm6
	vpbroadcastd	0x1c(%rdi),%zmm7
	vpbroadcastd	0x20(%rdi),%zmm8
	vpbroadcastd	0x24(%rdi),%zmm9
	vpbroadcastd	0x28(%rdi),%zmm10
	vpbroadcastd	0x2c(%rdi),%zmm11
	vpbroadcastd	0x30(%rdi),%zmm12
	vpbroadcastd	0x34(%rdi),%zmm13
	vpbroadcastd	0x38(%rdi),%zmm14
	vpbroadcastd	0x3c(%rdi),%zmm15

	# x12 += counter values 0-15
	vpaddd		CTR16BL(%rip),%zmm12,%zmm12

	# Only x12 carries a per-block value (the counter), so only it needs a
	# saved copy for the final state add. The other fifteen words are scalar
	# broadcasts of s[], re-added at the end via {1to16} embedded broadcast
	# straight from the state matrix, dropping fifteen vmovdqa64 copies.
	vmovdqa64	%zmm12,%zmm28

.Ldoubleround16:
	# x0 += x4, x12 = rotl32(x12 ^ x0, 16)
	vpaddd		%zmm0,%zmm4,%zmm0
	vpxord		%zmm0,%zmm12,%zmm12
	vprold		$16,%zmm12,%zmm12
	# x1 += x5, x13 = rotl32(x13 ^ x1, 16)
	vpaddd		%zmm1,%zmm5,%zmm1
	vpxord		%zmm1,%zmm13,%zmm13
	vprold		$16,%zmm13,%zmm13
	# x2 += x6, x14 = rotl32(x14 ^ x2, 16)
	vpaddd		%zmm2,%zmm6,%zmm2
	vpxord		%zmm2,%zmm14,%zmm14
	vprold		$16,%zmm14,%zmm14
	# x3 += x7, x15 = rotl32(x15 ^ x3, 16)
	vpaddd		%zmm3,%zmm7,%zmm3
	vpxord		%zmm3,%zmm15,%zmm15
	vprold		$16,%zmm15,%zmm15

	# x8 += x12, x4 = rotl32(x4 ^ x8, 12)
	vpaddd		%zmm12,%zmm8,%zmm8
	vpxord		%zmm8,%zmm4,%zmm4
	vprold		$12,%zmm4,%zmm4
	# x9 += x13, x5 = rotl32(x5 ^ x9, 12)
	vpaddd		%zmm13,%zmm9,%zmm9
	vpxord		%zmm9,%zmm5,%zmm5
	vprold		$12,%zmm5,%zmm5
	# x10 += x14, x6 = rotl32(x6 ^ x10, 12)
	vpaddd		%zmm14,%zmm10,%zmm10
	vpxord		%zmm10,%zmm6,%zmm6
	vprold		$12,%zmm6,%zmm6
	# x11 += x15, x7 = rotl32(x7 ^ x11, 12)
	vpaddd		%zmm15,%zmm11,%zmm11
	vpxord		%zmm11,%zmm7,%zmm7
	vprold		$12,%zmm7,%zmm7

	# x0 += x4, x12 = rotl32(x12 ^ x0, 8)
	vpaddd		%zmm0,%zmm4,%zmm0
	vpxord		%zmm0,%zmm12,%zmm12
	vprold		$8,%zmm12,%zmm12
	# x1 += x5, x13 = rotl32(x13 ^ x1, 8)
	vpaddd		%zmm1,%zmm5,%zmm1
	vpxord		%zmm1,%zmm13,%zmm13
	vprold		$8,%zmm13,%zmm13
	# x2 += x6, x14 = rotl32(x14 ^ x2, 8)
	vpaddd		%zmm2,%zmm6,%zmm2
	vpxord		%zmm2,%zmm14,%zmm14
	vprold		$8,%zmm14,%zmm14
	# x3 += x7, x15 = rotl32(x15 ^ x3, 8)
	vpaddd		%zmm3,%zmm7,%zmm3
	vpxord		%zmm3,%zmm15,%zmm15
	vprold		$8,%zmm15,%zmm15

	# x8 += x12, x4 = rotl32(x4 ^ x8, 7)
	vpaddd		%zmm12,%zmm8,%zmm8
	vpxord		%zmm8,%zmm4,%zmm4
	vprold		$7,%zmm4,%zmm4
	# x9 += x13, x5 = rotl32(x5 ^ x9, 7)
	vpaddd		%zmm13,%zmm9,%zmm9
	vpxord		%zmm9,%zmm5,%zmm5
	vprold		$7,%zmm5,%zmm5
	# x10 += x14, x6 = rotl32(x6 ^ x10, 7)
	vpaddd		%zmm14,%zmm10,%zmm10
	vpxord		%zmm10,%zmm6,%zmm6
	vprold		$7,%zmm6,%zmm6
	# x11 += x15, x7 = rotl32(x7 ^ x11, 7)
	vpaddd		%zmm15,%zmm11,%zmm11
	vpxord		%zmm11,%zmm7,%zmm7
	vprold		$7,%zmm7,%zmm7

	# x0 += x5, x15 = rotl32(x15 ^ x0, 16)
	vpaddd		%zmm0,%zmm5,%zmm0
	vpxord		%zmm0,%zmm15,%zmm15
	vprold		$16,%zmm15,%zmm15
	# x1 += x6, x12 = rotl32(x12 ^ x1, 16)
	vpaddd		%zmm1,%zmm6,%zmm1
	vpxord		%zmm1,%zmm12,%zmm12
	vprold		$16,%zmm12,%zmm12
	# x2 += x7, x13 = rotl32(x13 ^ x2, 16)
	vpaddd		%zmm2,%zmm7,%zmm2
	vpxord		%zmm2,%zmm13,%zmm13
	vprold		$16,%zmm13,%zmm13
	# x3 += x4, x14 = rotl32(x14 ^ x3, 16)
	vpaddd		%zmm3,%zmm4,%zmm3
	vpxord		%zmm3,%zmm14,%zmm14
	vprold		$16,%zmm14,%zmm14

	# x10 += x15, x5 = rotl32(x5 ^ x10, 12)
	vpaddd		%zmm15,%zmm10,%zmm10
	vpxord		%zmm10,%zmm5,%zmm5
	vprold		$12,%zmm5,%zmm5
	# x11 += x12, x6 = rotl32(x6 ^ x11, 12)
	vpaddd		%zmm12,%zmm11,%zmm11
	vpxord		%zmm11,%zmm6,%zmm6
	vprold		$12,%zmm6,%zmm6
	# x8 += x13, x7 = rotl32(x7 ^ x8, 12)
	vpaddd		%zmm13,%zmm8,%zmm8
	vpxord		%zmm8,%zmm7,%zmm7
	vprold		$12,%zmm7,%zmm7
	# x9 += x14, x4 = rotl32(x4 ^ x9, 12)
	vpaddd		%zmm14,%zmm9,%zmm9
	vpxord		%zmm9,%zmm4,%zmm4
	vprold		$12,%zmm4,%zmm4

	# x0 += x5, x15 = rotl32(x15 ^ x0, 8)
	vpaddd		%zmm0,%zmm5,%zmm0
	vpxord		%zmm0,%zmm15,%zmm15
	vprold		$8,%zmm15,%zmm15
	# x1 += x6, x12 = rotl32(x12 ^ x1, 8)
	vpaddd		%zmm1,%zmm6,%zmm1
	vpxord		%zmm1,%zmm12,%zmm12
	vprold		$8,%zmm12,%zmm12
	# x2 += x7, x13 = rotl32(x13 ^ x2, 8)
	vpaddd		%zmm2,%zmm7,%zmm2
	vpxord		%zmm2,%zmm13,%zmm13
	vprold		$8,%zmm13,%zmm13
	# x3 += x4, x14 = rotl32(x14 ^ x3, 8)
	vpaddd		%zmm3,%zmm4,%zmm3
	vpxord		%zmm3,%zmm14,%zmm14
	vprold		$8,%zmm14,%zmm14

	# x10 += x15, x5 = rotl32(x5 ^ x10, 7)
	vpaddd		%zmm15,%zmm10,%zmm10
	vpxord		%zmm10,%zmm5,%zmm5
	vprold		$7,%zmm5,%zmm5
	# x11 += x12, x6 = rotl32(x6 ^ x11, 7)
	vpaddd		%zmm12,%zmm11,%zmm11
	vpxord		%zmm11,%zmm6,%zmm6
	vprold		$7,%zmm6,%zmm6
	# x8 += x13, x7 = rotl32(x7 ^ x8, 7)
	vpaddd		%zmm13,%zmm8,%zmm8
	vpxord		%zmm8,%zmm7,%zmm7
	vprold		$7,%zmm7,%zmm7
	# x9 += x14, x4 = rotl32(x4 ^ x9, 7)
	vpaddd		%zmm14,%zmm9,%zmm9
	vpxord		%zmm9,%zmm4,%zmm4
	vprold		$7,%zmm4,%zmm4

	sub		$2,%r8d
	jnz		.Ldoubleround16

	# x0..15[0-15] += s[0..15]; all but x12 broadcast straight from s[]
	vpaddd		0x00(%rdi){1to16},%zmm0,%zmm0
	vpaddd		0x04(%rdi){1to16},%zmm1,%zmm1
	vpaddd		0x08(%rdi){1to16},%zmm2,%zmm2
	vpaddd		0x0c(%rdi){1to16},%zmm3,%zmm3
	vpaddd		0x10(%rdi){1to16},%zmm4,%zmm4
	vpaddd		0x14(%rdi){1to16},%zmm5,%zmm5
	vpaddd		0x18(%rdi){1to16},%zmm6,%zmm6
	vpaddd		0x1c(%rdi){1to16},%zmm7,%zmm7
	vpaddd		0x20(%rdi){1to16},%zmm8,%zmm8
	vpaddd		0x24(%rdi){1to16},%zmm9,%zmm9
	vpaddd		0x28(%rdi){1to16},%zmm10,%zmm10
	vpaddd		0x2c(%rdi){1to16},%zmm11,%zmm11
	vpaddd		%zmm28,%zmm12,%zmm12
	vpaddd		0x34(%rdi){1to16},%zmm13,%zmm13
	vpaddd		0x38(%rdi){1to16},%zmm14,%zmm14
	vpaddd		0x3c(%rdi){1to16},%zmm15,%zmm15

	# Transpose the 16x16 dword matrix: register n holds word n of all 16
	# blocks, but we need register n to hold all 16 words of block n. This
	# is the 8-block (vperm2i128) transpose extended by one level, since a
	# zmm holds four 128-bit lanes instead of two.

	# interleave 32-bit words in state n, n+1 -> zmm16..31
	vpunpckldq	%zmm1,%zmm0,%zmm16
	vpunpckhdq	%zmm1,%zmm0,%zmm17
	vpunpckldq	%zmm3,%zmm2,%zmm18
	vpunpckhdq	%zmm3,%zmm2,%zmm19
	vpunpckldq	%zmm5,%zmm4,%zmm20
	vpunpckhdq	%zmm5,%zmm4,%zmm21
	vpunpckldq	%zmm7,%zmm6,%zmm22
	vpunpckhdq	%zmm7,%zmm6,%zmm23
	vpunpckldq	%zmm9,%zmm8,%zmm24
	vpunpckhdq	%zmm9,%zmm8,%zmm25
	vpunpckldq	%zmm11,%zmm10,%zmm26
	vpunpckhdq	%zmm11,%zmm10,%zmm27
	vpunpckldq	%zmm13,%zmm12,%zmm28
	vpunpckhdq	%zmm13,%zmm12,%zmm29
	vpunpckldq	%zmm15,%zmm14,%zmm30
	vpunpckhdq	%zmm15,%zmm14,%zmm31

	# interleave 64-bit words in state n, n+2 -> zmm0..15
	vpunpcklqdq	%zmm18,%zmm16,%zmm0
	vpunpckhqdq	%zmm18,%zmm16,%zmm1
	vpunpcklqdq	%zmm19,%zmm17,%zmm2
	vpunpckhqdq	%zmm19,%zmm17,%zmm3
	vpunpcklqdq	%zmm22,%zmm20,%zmm4
	vpunpckhqdq	%zmm22,%zmm20,%zmm5
	vpunpcklqdq	%zmm23,%zmm21,%zmm6
	vpunpckhqdq	%zmm23,%zmm21,%zmm7
	vpunpcklqdq	%zmm26,%zmm24,%zmm8
	vpunpckhqdq	%zmm26,%zmm24,%zmm9
	vpunpcklqdq	%zmm27,%zmm25,%zmm10
	vpunpckhqdq	%zmm27,%zmm25,%zmm11
	vpunpcklqdq	%zmm30,%zmm28,%zmm12
	vpunpckhqdq	%zmm30,%zmm28,%zmm13
	vpunpcklqdq	%zmm31,%zmm29,%zmm14
	vpunpckhqdq	%zmm31,%zmm29,%zmm15

	# At this point lane L of zmm{r}, zmm{4+r}, zmm{8+r}, zmm{12+r} holds
	# word groups 0-3, 4-7, 8-11, 12-15 of block (4*L + r). Gather the four
	# 128-bit lanes of a block into one register with two levels of 128-bit
	# lane shuffles.

	# interleave 128-bit lanes in state n, n+4 -> zmm16..31
	vshufi64x2	$0x88,%zmm4,%zmm0,%zmm16
	vshufi64x2	$0x88,%zmm12,%zmm8,%zmm20
	vshufi64x2	$0xdd,%zmm4,%zmm0,%zmm24
	vshufi64x2	$0xdd,%zmm12,%zmm8,%zmm28
	vshufi64x2	$0x88,%zmm5,%zmm1,%zmm17
	vshufi64x2	$0x88,%zmm13,%zmm9,%zmm21
	vshufi64x2	$0xdd,%zmm5,%zmm1,%zmm25
	vshufi64x2	$0xdd,%zmm13,%zmm9,%zmm29
	vshufi64x2	$0x88,%zmm6,%zmm2,%zmm18
	vshufi64x2	$0x88,%zmm14,%zmm10,%zmm22
	vshufi64x2	$0xdd,%zmm6,%zmm2,%zmm26
	vshufi64x2	$0xdd,%zmm14,%zmm10,%zmm30
	vshufi64x2	$0x88,%zmm7,%zmm3,%zmm19
	vshufi64x2	$0x88,%zmm15,%zmm11,%zmm23
	vshufi64x2	$0xdd,%zmm7,%zmm3,%zmm27
	vshufi64x2	$0xdd,%zmm15,%zmm11,%zmm31

	# Fuse the final 256-bit interleave into the xor/store ladder.
	vshufi64x2	$0x88,%zmm20,%zmm16,%zmm0
	cmp		$0x0040,%rcx
	jl		.Lxorpart16
	vpxord		0x0000(%rdx),%zmm0,%zmm0
	vmovdqu64	%zmm0,0x0000(%rsi)

	vshufi64x2	$0x88,%zmm21,%zmm17,%zmm0
	cmp		$0x0080,%rcx
	jl		.Lxorpart16
	vpxord		0x0040(%rdx),%zmm0,%zmm0
	vmovdqu64	%zmm0,0x0040(%rsi)

	vshufi64x2	$0x88,%zmm22,%zmm18,%zmm0
	cmp		$0x00c0,%rcx
	jl		.Lxorpart16
	vpxord		0x0080(%rdx),%zmm0,%zmm0
	vmovdqu64	%zmm0,0x0080(%rsi)

	vshufi64x2	$0x88,%zmm23,%zmm19,%zmm0
	cmp		$0x0100,%rcx
	jl		.Lxorpart16
	vpxord		0x00c0(%rdx),%zmm0,%zmm0
	vmovdqu64	%zmm0,0x00c0(%rsi)

	vshufi64x2	$0x88,%zmm28,%zmm24,%zmm0
	cmp		$0x0140,%rcx
	jl		.Lxorpart16
	vpxord		0x0100(%rdx),%zmm0,%zmm0
	vmovdqu64	%zmm0,0x0100(%rsi)

	vshufi64x2	$0x88,%zmm29,%zmm25,%zmm0
	cmp		$0x0180,%rcx
	jl		.Lxorpart16
	vpxord		0x0140(%rdx),%zmm0,%zmm0
	vmovdqu64	%zmm0,0x0140(%rsi)

	vshufi64x2	$0x88,%zmm30,%zmm26,%zmm0
	cmp		$0x01c0,%rcx
	jl		.Lxorpart16
	vpxord		0x0180(%rdx),%zmm0,%zmm0
	vmovdqu64	%zmm0,0x0180(%rsi)

	vshufi64x2	$0x88,%zmm31,%zmm27,%zmm0
	cmp		$0x0200,%rcx
	jl		.Lxorpart16
	vpxord		0x01c0(%rdx),%zmm0,%zmm0
	vmovdqu64	%zmm0,0x01c0(%rsi)

	vshufi64x2	$0xdd,%zmm20,%zmm16,%zmm0
	cmp		$0x0240,%rcx
	jl		.Lxorpart16
	vpxord		0x0200(%rdx),%zmm0,%zmm0
	vmovdqu64	%zmm0,0x0200(%rsi)

	vshufi64x2	$0xdd,%zmm21,%zmm17,%zmm0
	cmp		$0x0280,%rcx
	jl		.Lxorpart16
	vpxord		0x0240(%rdx),%zmm0,%zmm0
	vmovdqu64	%zmm0,0x0240(%rsi)

	vshufi64x2	$0xdd,%zmm22,%zmm18,%zmm0
	cmp		$0x02c0,%rcx
	jl		.Lxorpart16
	vpxord		0x0280(%rdx),%zmm0,%zmm0
	vmovdqu64	%zmm0,0x0280(%rsi)

	vshufi64x2	$0xdd,%zmm23,%zmm19,%zmm0
	cmp		$0x0300,%rcx
	jl		.Lxorpart16
	vpxord		0x02c0(%rdx),%zmm0,%zmm0
	vmovdqu64	%zmm0,0x02c0(%rsi)

	vshufi64x2	$0xdd,%zmm28,%zmm24,%zmm0
	cmp		$0x0340,%rcx
	jl		.Lxorpart16
	vpxord		0x0300(%rdx),%zmm0,%zmm0
	vmovdqu64	%zmm0,0x0300(%rsi)

	vshufi64x2	$0xdd,%zmm29,%zmm25,%zmm0
	cmp		$0x0380,%rcx
	jl		.Lxorpart16
	vpxord		0x0340(%rdx),%zmm0,%zmm0
	vmovdqu64	%zmm0,0x0340(%rsi)

	vshufi64x2	$0xdd,%zmm30,%zmm26,%zmm0
	cmp		$0x03c0,%rcx
	jl		.Lxorpart16
	vpxord		0x0380(%rdx),%zmm0,%zmm0
	vmovdqu64	%zmm0,0x0380(%rsi)

	vshufi64x2	$0xdd,%zmm31,%zmm27,%zmm0
	cmp		$0x0400,%rcx
	jl		.Lxorpart16
	vpxord		0x03c0(%rdx),%zmm0,%zmm0
	vmovdqu64	%zmm0,0x03c0(%rsi)

.Ldone16:
	vzeroupper
	RET

.Lxorpart16:
	# xor remaining bytes from partial register into output
	mov		%rcx,%rax
	and		$0x3f,%rcx
	jz		.Ldone16
	mov		%rax,%r9
	and		$~0x3f,%r9

	mov		$1,%rax
	shld		%cl,%rax,%rax
	sub		$1,%rax
	kmovq		%rax,%k1

	vmovdqu8	(%rdx,%r9),%zmm1{%k1}{z}
	vpxord		%zmm0,%zmm1,%zmm1
	vmovdqu8	%zmm1,(%rsi,%r9){%k1}

	jmp		.Ldone16

SYM_FUNC_END(chacha_16block_xor_avx512)
