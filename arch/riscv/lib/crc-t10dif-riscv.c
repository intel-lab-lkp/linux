// SPDX-License-Identifier: GPL-2.0-only
/*
 * Accelerated CRC-T10DIF implementation with RISC-V Zbc extension.
 *
 * Copyright (C) 2024 Institute of Software, CAS.
 */

#include <asm/alternative-macros.h>
#include <asm/byteorder.h>
#include <asm/hwcap.h>

#include <linux/crc-t10dif.h>
#include <linux/module.h>

/*
 * CRC-T10DIF is a 16-bit CRC that uses most-significant-bit-first bit order,
 * i.e. bit i contains the coefficient of x^i (not reflected).
 */

#define CRCT10DIF_POLY		0x18bb7 /* The generator polynomial G */

#if __riscv_xlen == 64
#define CRCT10DIF_QUOTIENT_POLY	0xf65a57f81d33a48a /* floor(x^80 / G) - x^64 */
#define load_be_long(x)		be64_to_cpup(x)
#elif __riscv_xlen == 32
#define CRCT10DIF_QUOTIENT_POLY	0xf65a57f8	   /* floor(x^48 / G) - x^32 */
#define load_be_long(x)		be32_to_cpup(x)
#else
#error "Unsupported __riscv_xlen"
#endif

/*
 * Multiply the XLEN-bit message polynomial @m by x^16 and reduce it modulo the
 * generator polynomial G.  This gives the CRC of the message polynomial @m.
 */
static inline u16 crct10dif_zbc(unsigned long m)
{
	u16 crc;

	asm volatile(".option push\n"
		     ".option arch,+zbc\n"
		     /*
		      * First step of Barrett reduction with integrated
		      * multiplication by x^16:
		      *
		      *    %0 := floor((m * floor(x^(XLEN+16) / G)) / x^XLEN)
		      *
		      * The resulting value is equal to floor((m * x^16) / G).
		      *
		      * The constant floor(x^(XLEN+16) / G) has degree x^XLEN,
		      * i.e. it has XLEN+1 bits.  The clmulh instruction
		      * multiplies m by the x^0 through x^(XLEN-1) terms of this
		      * constant and does the floored division by x^XLEN.  The
		      * xor instruction handles the x^XLEN term of the constant
		      * by adding an additional (m * x^XLEN) / x^XLEN = m.
		      */
		     "clmulh %0, %1, %2\n"
		     "xor    %0, %0, %1\n"
		     /*
		      * Second step of Barrett reduction:
		      *
		      *    crc := (m * x^16) + (G * floor((m * x^16) / G))
		      *
		      * This reduces (m * x^16) modulo G by adding the
		      * appropriate multiple of G to it.  The result uses only
		      * the x^0 through x^15 terms.  HOWEVER, since the
		      * unreduced value (m * x^16) is zero in those terms in the
		      * first place, it is more efficient to do the equivalent:
		      *
		      *    crc := (G * floor((m * x^16) / G)) mod x^16
		      */
		     "clmul  %0, %0, %3\n"
		     ".option pop\n"
		     : "=&r" (crc)
		     : "r" (m),
		     "r" (CRCT10DIF_QUOTIENT_POLY),
		     "r" (CRCT10DIF_POLY));
	return crc;
}

static inline u16 crct10dif_unaligned(u16 crc, const u8 *p, size_t len)
{
	unsigned long m;
	size_t i;

	if (len == 1)
		return crct10dif_zbc(p[0] ^ (crc >> 8)) ^ (crc << 8);

	/* assuming len >= 2 here */
	m = crc ^ (p[0] << 8) ^ p[1];
	for (i = 2; i < len; i++)
		m = (m << 8) ^ p[i];
	return crct10dif_zbc(m);
}

u16 crc_t10dif_arch(u16 crc, const u8 *p, size_t len)
{
	size_t align;
	unsigned long m;

	asm goto(ALTERNATIVE("j %l[fallback]", "nop", 0,
			     RISCV_ISA_EXT_ZBC, 1) : : : : fallback);

	align = -(unsigned long)p % sizeof(unsigned long);
	if (align && len) {
		align = min(align, len);
		crc = crct10dif_unaligned(crc, p, align);
		p += align;
		len -= align;
	}

	while (len >= sizeof(unsigned long)) {
		m = ((unsigned long)crc << (8 * sizeof(unsigned long) - 16)) ^
		    load_be_long((const void *)p);
		crc = crct10dif_zbc(m);
		p += sizeof(unsigned long);
		len -= sizeof(unsigned long);
	}

	if (len)
		crc = crct10dif_unaligned(crc, p, len);

	return crc;

fallback:
	return crc_t10dif_generic(crc, p, len);
}
EXPORT_SYMBOL(crc_t10dif_arch);

MODULE_DESCRIPTION("CRC-T10DIF using RISC-V ZBC Extension");
MODULE_LICENSE("GPL");
