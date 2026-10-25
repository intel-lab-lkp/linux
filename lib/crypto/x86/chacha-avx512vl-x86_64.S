/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * ChaCha 256-bit cipher algorithm, x64 AVX-512VL and AVX-512F (zmm) functions
 *
 * Copyright (C) 2018 Martin Willi
 */

#include <linux/linkage.h>

.section	.rodata.cst32.CTR2BL, "aM", @progbits, 32
.align 32
CTR2BL:	.octa 0x00000000000000000000000000000000
	.octa 0x00000000000000000000000000000001

.section	.rodata.cst32.CTR4BL, "aM", @progbits, 32
.align 32
CTR4BL:	.octa 0x00000000000000000000000000000002
	.octa 0x00000000000000000000000000000003

.section	.rodata.cst32.CTR8BL, "aM", @progbits, 32
.align 32
CTR8BL:	.octa 0x00000003000000020000000100000000
	.octa 0x00000007000000060000000500000004

.section	.rodata.cst64.CTR16BL, "aM", @progbits, 64
.align 64
CTR16BL:.octa 0x00000003000000020000000100000000
	.octa 0x00000007000000060000000500000004
	.octa 0x0000000b0000000a0000000900000008
	.octa 0x0000000f0000000e0000000d0000000c

.text

SYM_FUNC_START(chacha_2block_xor_avx512vl)
	# %rdi: Input state matrix, s
	# %rsi: up to 2 data blocks output, o
	# %rdx: up to 2 data blocks input, i
	# %rcx: input/output length in bytes
	# %r8d: nrounds

	# This function encrypts two ChaCha blocks by loading the state
	# matrix twice across four AVX registers. It performs matrix operations
	# on four words in each matrix in parallel, but requires shuffling to
	# rearrange the words after each round.

	vzeroupper

	# x0..3[0-2] = s0..3
	vbroadcasti128	0x00(%rdi),%ymm0
	vbroadcasti128	0x10(%rdi),%ymm1
	vbroadcasti128	0x20(%rdi),%ymm2
	vbroadcasti128	0x30(%rdi),%ymm3

	vpaddd		CTR2BL(%rip),%ymm3,%ymm3

	vmovdqa		%ymm0,%ymm8
	vmovdqa		%ymm1,%ymm9
	vmovdqa		%ymm2,%ymm10
	vmovdqa		%ymm3,%ymm11

.Ldoubleround:

	# x0 += x1, x3 = rotl32(x3 ^ x0, 16)
	vpaddd		%ymm1,%ymm0,%ymm0
	vpxord		%ymm0,%ymm3,%ymm3
	vprold		$16,%ymm3,%ymm3

	# x2 += x3, x1 = rotl32(x1 ^ x2, 12)
	vpaddd		%ymm3,%ymm2,%ymm2
	vpxord		%ymm2,%ymm1,%ymm1
	vprold		$12,%ymm1,%ymm1

	# x0 += x1, x3 = rotl32(x3 ^ x0, 8)
	vpaddd		%ymm1,%ymm0,%ymm0
	vpxord		%ymm0,%ymm3,%ymm3
	vprold		$8,%ymm3,%ymm3

	# x2 += x3, x1 = rotl32(x1 ^ x2, 7)
	vpaddd		%ymm3,%ymm2,%ymm2
	vpxord		%ymm2,%ymm1,%ymm1
	vprold		$7,%ymm1,%ymm1

	# x1 = shuffle32(x1, MASK(0, 3, 2, 1))
	vpshufd		$0x39,%ymm1,%ymm1
	# x2 = shuffle32(x2, MASK(1, 0, 3, 2))
	vpshufd		$0x4e,%ymm2,%ymm2
	# x3 = shuffle32(x3, MASK(2, 1, 0, 3))
	vpshufd		$0x93,%ymm3,%ymm3

	# x0 += x1, x3 = rotl32(x3 ^ x0, 16)
	vpaddd		%ymm1,%ymm0,%ymm0
	vpxord		%ymm0,%ymm3,%ymm3
	vprold		$16,%ymm3,%ymm3

	# x2 += x3, x1 = rotl32(x1 ^ x2, 12)
	vpaddd		%ymm3,%ymm2,%ymm2
	vpxord		%ymm2,%ymm1,%ymm1
	vprold		$12,%ymm1,%ymm1

	# x0 += x1, x3 = rotl32(x3 ^ x0, 8)
	vpaddd		%ymm1,%ymm0,%ymm0
	vpxord		%ymm0,%ymm3,%ymm3
	vprold		$8,%ymm3,%ymm3

	# x2 += x3, x1 = rotl32(x1 ^ x2, 7)
	vpaddd		%ymm3,%ymm2,%ymm2
	vpxord		%ymm2,%ymm1,%ymm1
	vprold		$7,%ymm1,%ymm1

	# x1 = shuffle32(x1, MASK(2, 1, 0, 3))
	vpshufd		$0x93,%ymm1,%ymm1
	# x2 = shuffle32(x2, MASK(1, 0, 3, 2))
	vpshufd		$0x4e,%ymm2,%ymm2
	# x3 = shuffle32(x3, MASK(0, 3, 2, 1))
	vpshufd		$0x39,%ymm3,%ymm3

	sub		$2,%r8d
	jnz		.Ldoubleround

	# o0 = i0 ^ (x0 + s0)
	vpaddd		%ymm8,%ymm0,%ymm7
	cmp		$0x10,%rcx
	jl		.Lxorpart2
	vpxord		0x00(%rdx),%xmm7,%xmm6
	vmovdqu		%xmm6,0x00(%rsi)
	vextracti128	$1,%ymm7,%xmm0
	# o1 = i1 ^ (x1 + s1)
	vpaddd		%ymm9,%ymm1,%ymm7
	cmp		$0x20,%rcx
	jl		.Lxorpart2
	vpxord		0x10(%rdx),%xmm7,%xmm6
	vmovdqu		%xmm6,0x10(%rsi)
	vextracti128	$1,%ymm7,%xmm1
	# o2 = i2 ^ (x2 + s2)
	vpaddd		%ymm10,%ymm2,%ymm7
	cmp		$0x30,%rcx
	jl		.Lxorpart2
	vpxord		0x20(%rdx),%xmm7,%xmm6
	vmovdqu		%xmm6,0x20(%rsi)
	vextracti128	$1,%ymm7,%xmm2
	# o3 = i3 ^ (x3 + s3)
	vpaddd		%ymm11,%ymm3,%ymm7
	cmp		$0x40,%rcx
	jl		.Lxorpart2
	vpxord		0x30(%rdx),%xmm7,%xmm6
	vmovdqu		%xmm6,0x30(%rsi)
	vextracti128	$1,%ymm7,%xmm3

	# xor and write second block
	vmovdqa		%xmm0,%xmm7
	cmp		$0x50,%rcx
	jl		.Lxorpart2
	vpxord		0x40(%rdx),%xmm7,%xmm6
	vmovdqu		%xmm6,0x40(%rsi)

	vmovdqa		%xmm1,%xmm7
	cmp		$0x60,%rcx
	jl		.Lxorpart2
	vpxord		0x50(%rdx),%xmm7,%xmm6
	vmovdqu		%xmm6,0x50(%rsi)

	vmovdqa		%xmm2,%xmm7
	cmp		$0x70,%rcx
	jl		.Lxorpart2
	vpxord		0x60(%rdx),%xmm7,%xmm6
	vmovdqu		%xmm6,0x60(%rsi)

	vmovdqa		%xmm3,%xmm7
	cmp		$0x80,%rcx
	jl		.Lxorpart2
	vpxord		0x70(%rdx),%xmm7,%xmm6
	vmovdqu		%xmm6,0x70(%rsi)

.Ldone2:
	vzeroupper
	RET

.Lxorpart2:
	# xor remaining bytes from partial register into output
	mov		%rcx,%rax
	and		$0xf,%rcx
	jz		.Ldone2
	mov		%rax,%r9
	and		$~0xf,%r9

	mov		$1,%rax
	shld		%cl,%rax,%rax
	sub		$1,%rax
	kmovq		%rax,%k1

	vmovdqu8	(%rdx,%r9),%xmm1{%k1}{z}
	vpxord		%xmm7,%xmm1,%xmm1
	vmovdqu8	%xmm1,(%rsi,%r9){%k1}

	jmp		.Ldone2

SYM_FUNC_END(chacha_2block_xor_avx512vl)

SYM_FUNC_START(chacha_4block_xor_avx512vl)
	# %rdi: Input state matrix, s
	# %rsi: up to 4 data blocks output, o
	# %rdx: up to 4 data blocks input, i
	# %rcx: input/output length in bytes
	# %r8d: nrounds

	# This function encrypts four ChaCha blocks by loading the state
	# matrix four times across eight AVX registers. It performs matrix
	# operations on four words in two matrices in parallel, sequentially
	# to the operations on the four words of the other two matrices. The
	# required word shuffling has a rather high latency, we can do the
	# arithmetic on two matrix-pairs without much slowdown.

	vzeroupper

	# x0..3[0-4] = s0..3
	vbroadcasti128	0x00(%rdi),%ymm0
	vbroadcasti128	0x10(%rdi),%ymm1
	vbroadcasti128	0x20(%rdi),%ymm2
	vbroadcasti128	0x30(%rdi),%ymm3

	vmovdqa		%ymm0,%ymm4
	vmovdqa		%ymm1,%ymm5
	vmovdqa		%ymm2,%ymm6
	vmovdqa		%ymm3,%ymm7

	vpaddd		CTR2BL(%rip),%ymm3,%ymm3
	vpaddd		CTR4BL(%rip),%ymm7,%ymm7

	vmovdqa		%ymm0,%ymm11
	vmovdqa		%ymm1,%ymm12
	vmovdqa		%ymm2,%ymm13
	vmovdqa		%ymm3,%ymm14
	vmovdqa		%ymm7,%ymm15

.Ldoubleround4:

	# x0 += x1, x3 = rotl32(x3 ^ x0, 16)
	vpaddd		%ymm1,%ymm0,%ymm0
	vpxord		%ymm0,%ymm3,%ymm3
	vprold		$16,%ymm3,%ymm3

	vpaddd		%ymm5,%ymm4,%ymm4
	vpxord		%ymm4,%ymm7,%ymm7
	vprold		$16,%ymm7,%ymm7

	# x2 += x3, x1 = rotl32(x1 ^ x2, 12)
	vpaddd		%ymm3,%ymm2,%ymm2
	vpxord		%ymm2,%ymm1,%ymm1
	vprold		$12,%ymm1,%ymm1

	vpaddd		%ymm7,%ymm6,%ymm6
	vpxord		%ymm6,%ymm5,%ymm5
	vprold		$12,%ymm5,%ymm5

	# x0 += x1, x3 = rotl32(x3 ^ x0, 8)
	vpaddd		%ymm1,%ymm0,%ymm0
	vpxord		%ymm0,%ymm3,%ymm3
	vprold		$8,%ymm3,%ymm3

	vpaddd		%ymm5,%ymm4,%ymm4
	vpxord		%ymm4,%ymm7,%ymm7
	vprold		$8,%ymm7,%ymm7

	# x2 += x3, x1 = rotl32(x1 ^ x2, 7)
	vpaddd		%ymm3,%ymm2,%ymm2
	vpxord		%ymm2,%ymm1,%ymm1
	vprold		$7,%ymm1,%ymm1

	vpaddd		%ymm7,%ymm6,%ymm6
	vpxord		%ymm6,%ymm5,%ymm5
	vprold		$7,%ymm5,%ymm5

	# x1 = shuffle32(x1, MASK(0, 3, 2, 1))
	vpshufd		$0x39,%ymm1,%ymm1
	vpshufd		$0x39,%ymm5,%ymm5
	# x2 = shuffle32(x2, MASK(1, 0, 3, 2))
	vpshufd		$0x4e,%ymm2,%ymm2
	vpshufd		$0x4e,%ymm6,%ymm6
	# x3 = shuffle32(x3, MASK(2, 1, 0, 3))
	vpshufd		$0x93,%ymm3,%ymm3
	vpshufd		$0x93,%ymm7,%ymm7

	# x0 += x1, x3 = rotl32(x3 ^ x0, 16)
	vpaddd		%ymm1,%ymm0,%ymm0
	vpxord		%ymm0,%ymm3,%ymm3
	vprold		$16,%ymm3,%ymm3

	vpaddd		%ymm5,%ymm4,%ymm4
	vpxord		%ymm4,%ymm7,%ymm7
	vprold		$16,%ymm7,%ymm7

	# x2 += x3, x1 = rotl32(x1 ^ x2, 12)
	vpaddd		%ymm3,%ymm2,%ymm2
	vpxord		%ymm2,%ymm1,%ymm1
	vprold		$12,%ymm1,%ymm1

	vpaddd		%ymm7,%ymm6,%ymm6
	vpxord		%ymm6,%ymm5,%ymm5
	vprold		$12,%ymm5,%ymm5

	# x0 += x1, x3 = rotl32(x3 ^ x0, 8)
	vpaddd		%ymm1,%ymm0,%ymm0
	vpxord		%ymm0,%ymm3,%ymm3
	vprold		$8,%ymm3,%ymm3

	vpaddd		%ymm5,%ymm4,%ymm4
	vpxord		%ymm4,%ymm7,%ymm7
	vprold		$8,%ymm7,%ymm7

	# x2 += x3, x1 = rotl32(x1 ^ x2, 7)
	vpaddd		%ymm3,%ymm2,%ymm2
	vpxord		%ymm2,%ymm1,%ymm1
	vprold		$7,%ymm1,%ymm1

	vpaddd		%ymm7,%ymm6,%ymm6
	vpxord		%ymm6,%ymm5,%ymm5
	vprold		$7,%ymm5,%ymm5

	# x1 = shuffle32(x1, MASK(2, 1, 0, 3))
	vpshufd		$0x93,%ymm1,%ymm1
	vpshufd		$0x93,%ymm5,%ymm5
	# x2 = shuffle32(x2, MASK(1, 0, 3, 2))
	vpshufd		$0x4e,%ymm2,%ymm2
	vpshufd		$0x4e,%ymm6,%ymm6
	# x3 = shuffle32(x3, MASK(0, 3, 2, 1))
	vpshufd		$0x39,%ymm3,%ymm3
	vpshufd		$0x39,%ymm7,%ymm7

	sub		$2,%r8d
	jnz		.Ldoubleround4

	# o0 = i0 ^ (x0 + s0), first block
	vpaddd		%ymm11,%ymm0,%ymm10
	cmp		$0x10,%rcx
	jl		.Lxorpart4
	vpxord		0x00(%rdx),%xmm10,%xmm9
	vmovdqu		%xmm9,0x00(%rsi)
	vextracti128	$1,%ymm10,%xmm0
	# o1 = i1 ^ (x1 + s1), first block
	vpaddd		%ymm12,%ymm1,%ymm10
	cmp		$0x20,%rcx
	jl		.Lxorpart4
	vpxord		0x10(%rdx),%xmm10,%xmm9
	vmovdqu		%xmm9,0x10(%rsi)
	vextracti128	$1,%ymm10,%xmm1
	# o2 = i2 ^ (x2 + s2), first block
	vpaddd		%ymm13,%ymm2,%ymm10
	cmp		$0x30,%rcx
	jl		.Lxorpart4
	vpxord		0x20(%rdx),%xmm10,%xmm9
	vmovdqu		%xmm9,0x20(%rsi)
	vextracti128	$1,%ymm10,%xmm2
	# o3 = i3 ^ (x3 + s3), first block
	vpaddd		%ymm14,%ymm3,%ymm10
	cmp		$0x40,%rcx
	jl		.Lxorpart4
	vpxord		0x30(%rdx),%xmm10,%xmm9
	vmovdqu		%xmm9,0x30(%rsi)
	vextracti128	$1,%ymm10,%xmm3

	# xor and write second block
	vmovdqa		%xmm0,%xmm10
	cmp		$0x50,%rcx
	jl		.Lxorpart4
	vpxord		0x40(%rdx),%xmm10,%xmm9
	vmovdqu		%xmm9,0x40(%rsi)

	vmovdqa		%xmm1,%xmm10
	cmp		$0x60,%rcx
	jl		.Lxorpart4
	vpxord		0x50(%rdx),%xmm10,%xmm9
	vmovdqu		%xmm9,0x50(%rsi)

	vmovdqa		%xmm2,%xmm10
	cmp		$0x70,%rcx
	jl		.Lxorpart4
	vpxord		0x60(%rdx),%xmm10,%xmm9
	vmovdqu		%xmm9,0x60(%rsi)

	vmovdqa		%xmm3,%xmm10
	cmp		$0x80,%rcx
	jl		.Lxorpart4
	vpxord		0x70(%rdx),%xmm10,%xmm9
	vmovdqu		%xmm9,0x70(%rsi)

	# o0 = i0 ^ (x0 + s0), third block
	vpaddd		%ymm11,%ymm4,%ymm10
	cmp		$0x90,%rcx
	jl		.Lxorpart4
	vpxord		0x80(%rdx),%xmm10,%xmm9
	vmovdqu		%xmm9,0x80(%rsi)
	vextracti128	$1,%ymm10,%xmm4
	# o1 = i1 ^ (x1 + s1), third block
	vpaddd		%ymm12,%ymm5,%ymm10
	cmp		$0xa0,%rcx
	jl		.Lxorpart4
	vpxord		0x90(%rdx),%xmm10,%xmm9
	vmovdqu		%xmm9,0x90(%rsi)
	vextracti128	$1,%ymm10,%xmm5
	# o2 = i2 ^ (x2 + s2), third block
	vpaddd		%ymm13,%ymm6,%ymm10
	cmp		$0xb0,%rcx
	jl		.Lxorpart4
	vpxord		0xa0(%rdx),%xmm10,%xmm9
	vmovdqu		%xmm9,0xa0(%rsi)
	vextracti128	$1,%ymm10,%xmm6
	# o3 = i3 ^ (x3 + s3), third block
	vpaddd		%ymm15,%ymm7,%ymm10
	cmp		$0xc0,%rcx
	jl		.Lxorpart4
	vpxord		0xb0(%rdx),%xmm10,%xmm9
	vmovdqu		%xmm9,0xb0(%rsi)
	vextracti128	$1,%ymm10,%xmm7

	# xor and write fourth block
	vmovdqa		%xmm4,%xmm10
	cmp		$0xd0,%rcx
	jl		.Lxorpart4
	vpxord		0xc0(%rdx),%xmm10,%xmm9
	vmovdqu		%xmm9,0xc0(%rsi)

	vmovdqa		%xmm5,%xmm10
	cmp		$0xe0,%rcx
	jl		.Lxorpart4
	vpxord		0xd0(%rdx),%xmm10,%xmm9
	vmovdqu		%xmm9,0xd0(%rsi)

	vmovdqa		%xmm6,%xmm10
	cmp		$0xf0,%rcx
	jl		.Lxorpart4
	vpxord		0xe0(%rdx),%xmm10,%xmm9
	vmovdqu		%xmm9,0xe0(%rsi)

	vmovdqa		%xmm7,%xmm10
	cmp		$0x100,%rcx
	jl		.Lxorpart4
	vpxord		0xf0(%rdx),%xmm10,%xmm9
	vmovdqu		%xmm9,0xf0(%rsi)

.Ldone4:
	vzeroupper
	RET

.Lxorpart4:
	# xor remaining bytes from partial register into output
	mov		%rcx,%rax
	and		$0xf,%rcx
	jz		.Ldone4
	mov		%rax,%r9
	and		$~0xf,%r9

	mov		$1,%rax
	shld		%cl,%rax,%rax
	sub		$1,%rax
	kmovq		%rax,%k1

	vmovdqu8	(%rdx,%r9),%xmm1{%k1}{z}
	vpxord		%xmm10,%xmm1,%xmm1
	vmovdqu8	%xmm1,(%rsi,%r9){%k1}

	jmp		.Ldone4

SYM_FUNC_END(chacha_4block_xor_avx512vl)

SYM_FUNC_START(chacha_8block_xor_avx512vl)
	# %rdi: Input state matrix, s
	# %rsi: up to 8 data blocks output, o
	# %rdx: up to 8 data blocks input, i
	# %rcx: input/output length in bytes
	# %r8d: nrounds

	# This function encrypts eight consecutive ChaCha blocks by loading
	# the state matrix in AVX registers eight times. Compared to AVX2, this
	# mostly benefits from the new rotate instructions in VL and the
	# additional registers.

	vzeroupper

	# x0..15[0-7] = s[0..15]
	vpbroadcastd	0x00(%rdi),%ymm0
	vpbroadcastd	0x04(%rdi),%ymm1
	vpbroadcastd	0x08(%rdi),%ymm2
	vpbroadcastd	0x0c(%rdi),%ymm3
	vpbroadcastd	0x10(%rdi),%ymm4
	vpbroadcastd	0x14(%rdi),%ymm5
	vpbroadcastd	0x18(%rdi),%ymm6
	vpbroadcastd	0x1c(%rdi),%ymm7
	vpbroadcastd	0x20(%rdi),%ymm8
	vpbroadcastd	0x24(%rdi),%ymm9
	vpbroadcastd	0x28(%rdi),%ymm10
	vpbroadcastd	0x2c(%rdi),%ymm11
	vpbroadcastd	0x30(%rdi),%ymm12
	vpbroadcastd	0x34(%rdi),%ymm13
	vpbroadcastd	0x38(%rdi),%ymm14
	vpbroadcastd	0x3c(%rdi),%ymm15

	# x12 += counter values 0-3
	vpaddd		CTR8BL(%rip),%ymm12,%ymm12

	vmovdqa64	%ymm0,%ymm16
	vmovdqa64	%ymm1,%ymm17
	vmovdqa64	%ymm2,%ymm18
	vmovdqa64	%ymm3,%ymm19
	vmovdqa64	%ymm4,%ymm20
	vmovdqa64	%ymm5,%ymm21
	vmovdqa64	%ymm6,%ymm22
	vmovdqa64	%ymm7,%ymm23
	vmovdqa64	%ymm8,%ymm24
	vmovdqa64	%ymm9,%ymm25
	vmovdqa64	%ymm10,%ymm26
	vmovdqa64	%ymm11,%ymm27
	vmovdqa64	%ymm12,%ymm28
	vmovdqa64	%ymm13,%ymm29
	vmovdqa64	%ymm14,%ymm30
	vmovdqa64	%ymm15,%ymm31

.Ldoubleround8:
	# x0 += x4, x12 = rotl32(x12 ^ x0, 16)
	vpaddd		%ymm0,%ymm4,%ymm0
	vpxord		%ymm0,%ymm12,%ymm12
	vprold		$16,%ymm12,%ymm12
	# x1 += x5, x13 = rotl32(x13 ^ x1, 16)
	vpaddd		%ymm1,%ymm5,%ymm1
	vpxord		%ymm1,%ymm13,%ymm13
	vprold		$16,%ymm13,%ymm13
	# x2 += x6, x14 = rotl32(x14 ^ x2, 16)
	vpaddd		%ymm2,%ymm6,%ymm2
	vpxord		%ymm2,%ymm14,%ymm14
	vprold		$16,%ymm14,%ymm14
	# x3 += x7, x15 = rotl32(x15 ^ x3, 16)
	vpaddd		%ymm3,%ymm7,%ymm3
	vpxord		%ymm3,%ymm15,%ymm15
	vprold		$16,%ymm15,%ymm15

	# x8 += x12, x4 = rotl32(x4 ^ x8, 12)
	vpaddd		%ymm12,%ymm8,%ymm8
	vpxord		%ymm8,%ymm4,%ymm4
	vprold		$12,%ymm4,%ymm4
	# x9 += x13, x5 = rotl32(x5 ^ x9, 12)
	vpaddd		%ymm13,%ymm9,%ymm9
	vpxord		%ymm9,%ymm5,%ymm5
	vprold		$12,%ymm5,%ymm5
	# x10 += x14, x6 = rotl32(x6 ^ x10, 12)
	vpaddd		%ymm14,%ymm10,%ymm10
	vpxord		%ymm10,%ymm6,%ymm6
	vprold		$12,%ymm6,%ymm6
	# x11 += x15, x7 = rotl32(x7 ^ x11, 12)
	vpaddd		%ymm15,%ymm11,%ymm11
	vpxord		%ymm11,%ymm7,%ymm7
	vprold		$12,%ymm7,%ymm7

	# x0 += x4, x12 = rotl32(x12 ^ x0, 8)
	vpaddd		%ymm0,%ymm4,%ymm0
	vpxord		%ymm0,%ymm12,%ymm12
	vprold		$8,%ymm12,%ymm12
	# x1 += x5, x13 = rotl32(x13 ^ x1, 8)
	vpaddd		%ymm1,%ymm5,%ymm1
	vpxord		%ymm1,%ymm13,%ymm13
	vprold		$8,%ymm13,%ymm13
	# x2 += x6, x14 = rotl32(x14 ^ x2, 8)
	vpaddd		%ymm2,%ymm6,%ymm2
	vpxord		%ymm2,%ymm14,%ymm14
	vprold		$8,%ymm14,%ymm14
	# x3 += x7, x15 = rotl32(x15 ^ x3, 8)
	vpaddd		%ymm3,%ymm7,%ymm3
	vpxord		%ymm3,%ymm15,%ymm15
	vprold		$8,%ymm15,%ymm15

	# x8 += x12, x4 = rotl32(x4 ^ x8, 7)
	vpaddd		%ymm12,%ymm8,%ymm8
	vpxord		%ymm8,%ymm4,%ymm4
	vprold		$7,%ymm4,%ymm4
	# x9 += x13, x5 = rotl32(x5 ^ x9, 7)
	vpaddd		%ymm13,%ymm9,%ymm9
	vpxord		%ymm9,%ymm5,%ymm5
	vprold		$7,%ymm5,%ymm5
	# x10 += x14, x6 = rotl32(x6 ^ x10, 7)
	vpaddd		%ymm14,%ymm10,%ymm10
	vpxord		%ymm10,%ymm6,%ymm6
	vprold		$7,%ymm6,%ymm6
	# x11 += x15, x7 = rotl32(x7 ^ x11, 7)
	vpaddd		%ymm15,%ymm11,%ymm11
	vpxord		%ymm11,%ymm7,%ymm7
	vprold		$7,%ymm7,%ymm7

	# x0 += x5, x15 = rotl32(x15 ^ x0, 16)
	vpaddd		%ymm0,%ymm5,%ymm0
	vpxord		%ymm0,%ymm15,%ymm15
	vprold		$16,%ymm15,%ymm15
	# x1 += x6, x12 = rotl32(x12 ^ x1, 16)
	vpaddd		%ymm1,%ymm6,%ymm1
	vpxord		%ymm1,%ymm12,%ymm12
	vprold		$16,%ymm12,%ymm12
	# x2 += x7, x13 = rotl32(x13 ^ x2, 16)
	vpaddd		%ymm2,%ymm7,%ymm2
	vpxord		%ymm2,%ymm13,%ymm13
	vprold		$16,%ymm13,%ymm13
	# x3 += x4, x14 = rotl32(x14 ^ x3, 16)
	vpaddd		%ymm3,%ymm4,%ymm3
	vpxord		%ymm3,%ymm14,%ymm14
	vprold		$16,%ymm14,%ymm14

	# x10 += x15, x5 = rotl32(x5 ^ x10, 12)
	vpaddd		%ymm15,%ymm10,%ymm10
	vpxord		%ymm10,%ymm5,%ymm5
	vprold		$12,%ymm5,%ymm5
	# x11 += x12, x6 = rotl32(x6 ^ x11, 12)
	vpaddd		%ymm12,%ymm11,%ymm11
	vpxord		%ymm11,%ymm6,%ymm6
	vprold		$12,%ymm6,%ymm6
	# x8 += x13, x7 = rotl32(x7 ^ x8, 12)
	vpaddd		%ymm13,%ymm8,%ymm8
	vpxord		%ymm8,%ymm7,%ymm7
	vprold		$12,%ymm7,%ymm7
	# x9 += x14, x4 = rotl32(x4 ^ x9, 12)
	vpaddd		%ymm14,%ymm9,%ymm9
	vpxord		%ymm9,%ymm4,%ymm4
	vprold		$12,%ymm4,%ymm4

	# x0 += x5, x15 = rotl32(x15 ^ x0, 8)
	vpaddd		%ymm0,%ymm5,%ymm0
	vpxord		%ymm0,%ymm15,%ymm15
	vprold		$8,%ymm15,%ymm15
	# x1 += x6, x12 = rotl32(x12 ^ x1, 8)
	vpaddd		%ymm1,%ymm6,%ymm1
	vpxord		%ymm1,%ymm12,%ymm12
	vprold		$8,%ymm12,%ymm12
	# x2 += x7, x13 = rotl32(x13 ^ x2, 8)
	vpaddd		%ymm2,%ymm7,%ymm2
	vpxord		%ymm2,%ymm13,%ymm13
	vprold		$8,%ymm13,%ymm13
	# x3 += x4, x14 = rotl32(x14 ^ x3, 8)
	vpaddd		%ymm3,%ymm4,%ymm3
	vpxord		%ymm3,%ymm14,%ymm14
	vprold		$8,%ymm14,%ymm14

	# x10 += x15, x5 = rotl32(x5 ^ x10, 7)
	vpaddd		%ymm15,%ymm10,%ymm10
	vpxord		%ymm10,%ymm5,%ymm5
	vprold		$7,%ymm5,%ymm5
	# x11 += x12, x6 = rotl32(x6 ^ x11, 7)
	vpaddd		%ymm12,%ymm11,%ymm11
	vpxord		%ymm11,%ymm6,%ymm6
	vprold		$7,%ymm6,%ymm6
	# x8 += x13, x7 = rotl32(x7 ^ x8, 7)
	vpaddd		%ymm13,%ymm8,%ymm8
	vpxord		%ymm8,%ymm7,%ymm7
	vprold		$7,%ymm7,%ymm7
	# x9 += x14, x4 = rotl32(x4 ^ x9, 7)
	vpaddd		%ymm14,%ymm9,%ymm9
	vpxord		%ymm9,%ymm4,%ymm4
	vprold		$7,%ymm4,%ymm4

	sub		$2,%r8d
	jnz		.Ldoubleround8

	# x0..15[0-3] += s[0..15]
	vpaddd		%ymm16,%ymm0,%ymm0
	vpaddd		%ymm17,%ymm1,%ymm1
	vpaddd		%ymm18,%ymm2,%ymm2
	vpaddd		%ymm19,%ymm3,%ymm3
	vpaddd		%ymm20,%ymm4,%ymm4
	vpaddd		%ymm21,%ymm5,%ymm5
	vpaddd		%ymm22,%ymm6,%ymm6
	vpaddd		%ymm23,%ymm7,%ymm7
	vpaddd		%ymm24,%ymm8,%ymm8
	vpaddd		%ymm25,%ymm9,%ymm9
	vpaddd		%ymm26,%ymm10,%ymm10
	vpaddd		%ymm27,%ymm11,%ymm11
	vpaddd		%ymm28,%ymm12,%ymm12
	vpaddd		%ymm29,%ymm13,%ymm13
	vpaddd		%ymm30,%ymm14,%ymm14
	vpaddd		%ymm31,%ymm15,%ymm15

	# interleave 32-bit words in state n, n+1
	vpunpckldq	%ymm1,%ymm0,%ymm16
	vpunpckhdq	%ymm1,%ymm0,%ymm17
	vpunpckldq	%ymm3,%ymm2,%ymm18
	vpunpckhdq	%ymm3,%ymm2,%ymm19
	vpunpckldq	%ymm5,%ymm4,%ymm20
	vpunpckhdq	%ymm5,%ymm4,%ymm21
	vpunpckldq	%ymm7,%ymm6,%ymm22
	vpunpckhdq	%ymm7,%ymm6,%ymm23
	vpunpckldq	%ymm9,%ymm8,%ymm24
	vpunpckhdq	%ymm9,%ymm8,%ymm25
	vpunpckldq	%ymm11,%ymm10,%ymm26
	vpunpckhdq	%ymm11,%ymm10,%ymm27
	vpunpckldq	%ymm13,%ymm12,%ymm28
	vpunpckhdq	%ymm13,%ymm12,%ymm29
	vpunpckldq	%ymm15,%ymm14,%ymm30
	vpunpckhdq	%ymm15,%ymm14,%ymm31

	# interleave 64-bit words in state n, n+2
	vpunpcklqdq	%ymm18,%ymm16,%ymm0
	vpunpcklqdq	%ymm19,%ymm17,%ymm1
	vpunpckhqdq	%ymm18,%ymm16,%ymm2
	vpunpckhqdq	%ymm19,%ymm17,%ymm3
	vpunpcklqdq	%ymm22,%ymm20,%ymm4
	vpunpcklqdq	%ymm23,%ymm21,%ymm5
	vpunpckhqdq	%ymm22,%ymm20,%ymm6
	vpunpckhqdq	%ymm23,%ymm21,%ymm7
	vpunpcklqdq	%ymm26,%ymm24,%ymm8
	vpunpcklqdq	%ymm27,%ymm25,%ymm9
	vpunpckhqdq	%ymm26,%ymm24,%ymm10
	vpunpckhqdq	%ymm27,%ymm25,%ymm11
	vpunpcklqdq	%ymm30,%ymm28,%ymm12
	vpunpcklqdq	%ymm31,%ymm29,%ymm13
	vpunpckhqdq	%ymm30,%ymm28,%ymm14
	vpunpckhqdq	%ymm31,%ymm29,%ymm15

	# interleave 128-bit words in state n, n+4
	# xor/write first four blocks
	vmovdqa64	%ymm0,%ymm16
	vperm2i128	$0x20,%ymm4,%ymm0,%ymm0
	cmp		$0x0020,%rcx
	jl		.Lxorpart8
	vpxord		0x0000(%rdx),%ymm0,%ymm0
	vmovdqu64	%ymm0,0x0000(%rsi)
	vmovdqa64	%ymm16,%ymm0
	vperm2i128	$0x31,%ymm4,%ymm0,%ymm4

	vperm2i128	$0x20,%ymm12,%ymm8,%ymm0
	cmp		$0x0040,%rcx
	jl		.Lxorpart8
	vpxord		0x0020(%rdx),%ymm0,%ymm0
	vmovdqu64	%ymm0,0x0020(%rsi)
	vperm2i128	$0x31,%ymm12,%ymm8,%ymm12

	vperm2i128	$0x20,%ymm6,%ymm2,%ymm0
	cmp		$0x0060,%rcx
	jl		.Lxorpart8
	vpxord		0x0040(%rdx),%ymm0,%ymm0
	vmovdqu64	%ymm0,0x0040(%rsi)
	vperm2i128	$0x31,%ymm6,%ymm2,%ymm6

	vperm2i128	$0x20,%ymm14,%ymm10,%ymm0
	cmp		$0x0080,%rcx
	jl		.Lxorpart8
	vpxord		0x0060(%rdx),%ymm0,%ymm0
	vmovdqu64	%ymm0,0x0060(%rsi)
	vperm2i128	$0x31,%ymm14,%ymm10,%ymm14

	vperm2i128	$0x20,%ymm5,%ymm1,%ymm0
	cmp		$0x00a0,%rcx
	jl		.Lxorpart8
	vpxord		0x0080(%rdx),%ymm0,%ymm0
	vmovdqu64	%ymm0,0x0080(%rsi)
	vperm2i128	$0x31,%ymm5,%ymm1,%ymm5

	vperm2i128	$0x20,%ymm13,%ymm9,%ymm0
	cmp		$0x00c0,%rcx
	jl		.Lxorpart8
	vpxord		0x00a0(%rdx),%ymm0,%ymm0
	vmovdqu64	%ymm0,0x00a0(%rsi)
	vperm2i128	$0x31,%ymm13,%ymm9,%ymm13

	vperm2i128	$0x20,%ymm7,%ymm3,%ymm0
	cmp		$0x00e0,%rcx
	jl		.Lxorpart8
	vpxord		0x00c0(%rdx),%ymm0,%ymm0
	vmovdqu64	%ymm0,0x00c0(%rsi)
	vperm2i128	$0x31,%ymm7,%ymm3,%ymm7

	vperm2i128	$0x20,%ymm15,%ymm11,%ymm0
	cmp		$0x0100,%rcx
	jl		.Lxorpart8
	vpxord		0x00e0(%rdx),%ymm0,%ymm0
	vmovdqu64	%ymm0,0x00e0(%rsi)
	vperm2i128	$0x31,%ymm15,%ymm11,%ymm15

	# xor remaining blocks, write to output
	vmovdqa64	%ymm4,%ymm0
	cmp		$0x0120,%rcx
	jl		.Lxorpart8
	vpxord		0x0100(%rdx),%ymm0,%ymm0
	vmovdqu64	%ymm0,0x0100(%rsi)

	vmovdqa64	%ymm12,%ymm0
	cmp		$0x0140,%rcx
	jl		.Lxorpart8
	vpxord		0x0120(%rdx),%ymm0,%ymm0
	vmovdqu64	%ymm0,0x0120(%rsi)

	vmovdqa64	%ymm6,%ymm0
	cmp		$0x0160,%rcx
	jl		.Lxorpart8
	vpxord		0x0140(%rdx),%ymm0,%ymm0
	vmovdqu64	%ymm0,0x0140(%rsi)

	vmovdqa64	%ymm14,%ymm0
	cmp		$0x0180,%rcx
	jl		.Lxorpart8
	vpxord		0x0160(%rdx),%ymm0,%ymm0
	vmovdqu64	%ymm0,0x0160(%rsi)

	vmovdqa64	%ymm5,%ymm0
	cmp		$0x01a0,%rcx
	jl		.Lxorpart8
	vpxord		0x0180(%rdx),%ymm0,%ymm0
	vmovdqu64	%ymm0,0x0180(%rsi)

	vmovdqa64	%ymm13,%ymm0
	cmp		$0x01c0,%rcx
	jl		.Lxorpart8
	vpxord		0x01a0(%rdx),%ymm0,%ymm0
	vmovdqu64	%ymm0,0x01a0(%rsi)

	vmovdqa64	%ymm7,%ymm0
	cmp		$0x01e0,%rcx
	jl		.Lxorpart8
	vpxord		0x01c0(%rdx),%ymm0,%ymm0
	vmovdqu64	%ymm0,0x01c0(%rsi)

	vmovdqa64	%ymm15,%ymm0
	cmp		$0x0200,%rcx
	jl		.Lxorpart8
	vpxord		0x01e0(%rdx),%ymm0,%ymm0
	vmovdqu64	%ymm0,0x01e0(%rsi)

.Ldone8:
	vzeroupper
	RET

.Lxorpart8:
	# xor remaining bytes from partial register into output
	mov		%rcx,%rax
	and		$0x1f,%rcx
	jz		.Ldone8
	mov		%rax,%r9
	and		$~0x1f,%r9

	mov		$1,%rax
	shld		%cl,%rax,%rax
	sub		$1,%rax
	kmovq		%rax,%k1

	vmovdqu8	(%rdx,%r9),%ymm1{%k1}{z}
	vpxord		%ymm0,%ymm1,%ymm1
	vmovdqu8	%ymm1,(%rsi,%r9){%k1}

	jmp		.Ldone8

SYM_FUNC_END(chacha_8block_xor_avx512vl)

SYM_FUNC_START(chacha_16block_xor_avx512)
	# %rdi: Input state matrix, s
	# %rsi: up to 16 data blocks output, o
	# %rdx: up to 16 data blocks input, i
	# %rcx: input/output length in bytes
	# %r8d: nrounds

	# This function encrypts sixteen consecutive ChaCha blocks by loading
	# the state matrix in 512-bit AVX-512 registers sixteen times. It is
	# the same algorithm as the eight block AVX-512VL variant, but using
	# zmm registers to double the number of blocks processed in parallel.

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

	vmovdqa64	%zmm0,%zmm16
	vmovdqa64	%zmm1,%zmm17
	vmovdqa64	%zmm2,%zmm18
	vmovdqa64	%zmm3,%zmm19
	vmovdqa64	%zmm4,%zmm20
	vmovdqa64	%zmm5,%zmm21
	vmovdqa64	%zmm6,%zmm22
	vmovdqa64	%zmm7,%zmm23
	vmovdqa64	%zmm8,%zmm24
	vmovdqa64	%zmm9,%zmm25
	vmovdqa64	%zmm10,%zmm26
	vmovdqa64	%zmm11,%zmm27
	vmovdqa64	%zmm12,%zmm28
	vmovdqa64	%zmm13,%zmm29
	vmovdqa64	%zmm14,%zmm30
	vmovdqa64	%zmm15,%zmm31

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

	# x0..15[0-15] += s[0..15]
	vpaddd		%zmm16,%zmm0,%zmm0
	vpaddd		%zmm17,%zmm1,%zmm1
	vpaddd		%zmm18,%zmm2,%zmm2
	vpaddd		%zmm19,%zmm3,%zmm3
	vpaddd		%zmm20,%zmm4,%zmm4
	vpaddd		%zmm21,%zmm5,%zmm5
	vpaddd		%zmm22,%zmm6,%zmm6
	vpaddd		%zmm23,%zmm7,%zmm7
	vpaddd		%zmm24,%zmm8,%zmm8
	vpaddd		%zmm25,%zmm9,%zmm9
	vpaddd		%zmm26,%zmm10,%zmm10
	vpaddd		%zmm27,%zmm11,%zmm11
	vpaddd		%zmm28,%zmm12,%zmm12
	vpaddd		%zmm29,%zmm13,%zmm13
	vpaddd		%zmm30,%zmm14,%zmm14
	vpaddd		%zmm31,%zmm15,%zmm15

	# interleave 32-bit words in state n, n+1
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

	# interleave 64-bit words in state n, n+2; afterwards, 128-bit lane l
	# of register 4*g+j holds words 4*g..4*g+3 of block 4*l+j
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

	# transpose 128-bit lanes across registers j, j+4, j+8, j+12, so that
	# register k holds the full keystream block k
	vshufi32x4	$0x44,%zmm4,%zmm0,%zmm16
	vshufi32x4	$0xee,%zmm4,%zmm0,%zmm17
	vshufi32x4	$0x44,%zmm12,%zmm8,%zmm18
	vshufi32x4	$0xee,%zmm12,%zmm8,%zmm19
	vshufi32x4	$0x88,%zmm18,%zmm16,%zmm0
	vshufi32x4	$0xdd,%zmm18,%zmm16,%zmm4
	vshufi32x4	$0x88,%zmm19,%zmm17,%zmm8
	vshufi32x4	$0xdd,%zmm19,%zmm17,%zmm12
	vshufi32x4	$0x44,%zmm5,%zmm1,%zmm20
	vshufi32x4	$0xee,%zmm5,%zmm1,%zmm21
	vshufi32x4	$0x44,%zmm13,%zmm9,%zmm22
	vshufi32x4	$0xee,%zmm13,%zmm9,%zmm23
	vshufi32x4	$0x88,%zmm22,%zmm20,%zmm1
	vshufi32x4	$0xdd,%zmm22,%zmm20,%zmm5
	vshufi32x4	$0x88,%zmm23,%zmm21,%zmm9
	vshufi32x4	$0xdd,%zmm23,%zmm21,%zmm13
	vshufi32x4	$0x44,%zmm6,%zmm2,%zmm24
	vshufi32x4	$0xee,%zmm6,%zmm2,%zmm25
	vshufi32x4	$0x44,%zmm14,%zmm10,%zmm26
	vshufi32x4	$0xee,%zmm14,%zmm10,%zmm27
	vshufi32x4	$0x88,%zmm26,%zmm24,%zmm2
	vshufi32x4	$0xdd,%zmm26,%zmm24,%zmm6
	vshufi32x4	$0x88,%zmm27,%zmm25,%zmm10
	vshufi32x4	$0xdd,%zmm27,%zmm25,%zmm14
	vshufi32x4	$0x44,%zmm7,%zmm3,%zmm28
	vshufi32x4	$0xee,%zmm7,%zmm3,%zmm29
	vshufi32x4	$0x44,%zmm15,%zmm11,%zmm30
	vshufi32x4	$0xee,%zmm15,%zmm11,%zmm31
	vshufi32x4	$0x88,%zmm30,%zmm28,%zmm3
	vshufi32x4	$0xdd,%zmm30,%zmm28,%zmm7
	vshufi32x4	$0x88,%zmm31,%zmm29,%zmm11
	vshufi32x4	$0xdd,%zmm31,%zmm29,%zmm15

	# xor/write blocks, jump to partial block handling with the block in
	# zmm0 if the length ends before the end of the block
	cmp		$0x0040,%rcx
	jl		.Lxorpart16
	vpxord		0x0000(%rdx),%zmm0,%zmm0
	vmovdqu64	%zmm0,0x0000(%rsi)

	vmovdqa64	%zmm1,%zmm0
	cmp		$0x0080,%rcx
	jl		.Lxorpart16
	vpxord		0x0040(%rdx),%zmm0,%zmm0
	vmovdqu64	%zmm0,0x0040(%rsi)

	vmovdqa64	%zmm2,%zmm0
	cmp		$0x00c0,%rcx
	jl		.Lxorpart16
	vpxord		0x0080(%rdx),%zmm0,%zmm0
	vmovdqu64	%zmm0,0x0080(%rsi)

	vmovdqa64	%zmm3,%zmm0
	cmp		$0x0100,%rcx
	jl		.Lxorpart16
	vpxord		0x00c0(%rdx),%zmm0,%zmm0
	vmovdqu64	%zmm0,0x00c0(%rsi)

	vmovdqa64	%zmm4,%zmm0
	cmp		$0x0140,%rcx
	jl		.Lxorpart16
	vpxord		0x0100(%rdx),%zmm0,%zmm0
	vmovdqu64	%zmm0,0x0100(%rsi)

	vmovdqa64	%zmm5,%zmm0
	cmp		$0x0180,%rcx
	jl		.Lxorpart16
	vpxord		0x0140(%rdx),%zmm0,%zmm0
	vmovdqu64	%zmm0,0x0140(%rsi)

	vmovdqa64	%zmm6,%zmm0
	cmp		$0x01c0,%rcx
	jl		.Lxorpart16
	vpxord		0x0180(%rdx),%zmm0,%zmm0
	vmovdqu64	%zmm0,0x0180(%rsi)

	vmovdqa64	%zmm7,%zmm0
	cmp		$0x0200,%rcx
	jl		.Lxorpart16
	vpxord		0x01c0(%rdx),%zmm0,%zmm0
	vmovdqu64	%zmm0,0x01c0(%rsi)

	vmovdqa64	%zmm8,%zmm0
	cmp		$0x0240,%rcx
	jl		.Lxorpart16
	vpxord		0x0200(%rdx),%zmm0,%zmm0
	vmovdqu64	%zmm0,0x0200(%rsi)

	vmovdqa64	%zmm9,%zmm0
	cmp		$0x0280,%rcx
	jl		.Lxorpart16
	vpxord		0x0240(%rdx),%zmm0,%zmm0
	vmovdqu64	%zmm0,0x0240(%rsi)

	vmovdqa64	%zmm10,%zmm0
	cmp		$0x02c0,%rcx
	jl		.Lxorpart16
	vpxord		0x0280(%rdx),%zmm0,%zmm0
	vmovdqu64	%zmm0,0x0280(%rsi)

	vmovdqa64	%zmm11,%zmm0
	cmp		$0x0300,%rcx
	jl		.Lxorpart16
	vpxord		0x02c0(%rdx),%zmm0,%zmm0
	vmovdqu64	%zmm0,0x02c0(%rsi)

	vmovdqa64	%zmm12,%zmm0
	cmp		$0x0340,%rcx
	jl		.Lxorpart16
	vpxord		0x0300(%rdx),%zmm0,%zmm0
	vmovdqu64	%zmm0,0x0300(%rsi)

	vmovdqa64	%zmm13,%zmm0
	cmp		$0x0380,%rcx
	jl		.Lxorpart16
	vpxord		0x0340(%rdx),%zmm0,%zmm0
	vmovdqu64	%zmm0,0x0340(%rsi)

	vmovdqa64	%zmm14,%zmm0
	cmp		$0x03c0,%rcx
	jl		.Lxorpart16
	vpxord		0x0380(%rdx),%zmm0,%zmm0
	vmovdqu64	%zmm0,0x0380(%rsi)

	vmovdqa64	%zmm15,%zmm0
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
