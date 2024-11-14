// SPDX-License-Identifier: GPL-2.0-only
/*
 * Accelerated CRC-T10DIF implementation with RISC-V Zbc extension.
 *
 * Copyright (C) 2024 Institute of Software, CAS.
 */

#include <asm/alternative-macros.h>
#include <asm/byteorder.h>
#include <asm/hwcap.h>

#include <crypto/internal/hash.h>

#include <linux/byteorder/generic.h>
#include <linux/crc-t10dif.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/types.h>

static u16 crc_t10dif_generic_zbc(u16 crc, unsigned char const *p, size_t len);

#define CRCT10DIF_POLY 0x8bb7

#if __riscv_xlen == 64
#define STEP_ORDER 3

#define CRCT10DIF_POLY_QT_BE 0xf65a57f81d33a48a

static inline u64 crct10dif_prep(u16 crc, unsigned long const *ptr)
{
	return ((u64)crc << 48) ^ (__force u64)__cpu_to_be64(*ptr);
}

#elif __riscv_xlen == 32
#define STEP_ORDER 2
#define CRCT10DIF_POLY_QT_BE 0xf65a57f8

static inline u32 crct10dif_prep(u16 crc, unsigned long const *ptr)
{
	return ((u32)crc << 16) ^ (__force u32)__cpu_to_be32(*ptr);
}

#else
#error "Unexpected __riscv_xlen"
#endif

static inline u16 crct10dif_zbc(unsigned long s)
{
	u16 crc;

	asm volatile   (".option push\n"
			".option arch,+zbc\n"
			"clmulh %0, %1, %2\n"
			"xor    %0, %0, %1\n"
			"clmul  %0, %0, %3\n"
			".option pop\n"
			: "=&r" (crc)
			: "r"(s),
			  "r"(CRCT10DIF_POLY_QT_BE),
			  "r"(CRCT10DIF_POLY)
			:);

	return crc;
}

#define STEP (1 << STEP_ORDER)
#define OFFSET_MASK (STEP - 1)

static inline u16 crct10dif_unaligned(u16 crc, unsigned char const *p, size_t len)
{
	size_t bits = len * 8;
	unsigned long s = 0;
	u16 crc_low = 0;

	for (int i = 0; i < len; i++)
		s = *p++ | (s << 8);

	if (len < sizeof(u16)) {
		s ^= crc >> (16 - bits);
		crc_low = crc << bits;
	} else {
		s ^= (unsigned long)crc << (bits - 16);
	}

	crc = crct10dif_zbc(s);
	crc ^= crc_low;

	return crc;
}

static u16 crc_t10dif_generic_zbc(u16 crc, unsigned char const *p, size_t len)
{
	size_t offset, head_len, tail_len;
	unsigned long const *p_ul;
	unsigned long s;

	offset = (unsigned long)p & OFFSET_MASK;
	if (offset && len) {
		head_len = min(STEP - offset, len);
		crc = crct10dif_unaligned(crc, p, head_len);
		p += head_len;
		len -= head_len;
	}

	tail_len = len & OFFSET_MASK;
	len = len >> STEP_ORDER;
	p_ul = (unsigned long const *)p;

	for (int i = 0; i < len; i++) {
		s = crct10dif_prep(crc, p_ul);
		crc = crct10dif_zbc(s);
		p_ul++;
	}

	p = (unsigned char const *)p_ul;
	if (tail_len)
		crc = crct10dif_unaligned(crc, p, tail_len);

	return crc;
}

static int crc_t10dif_init(struct shash_desc *desc)
{
	u16 *crc = shash_desc_ctx(desc);

	*crc = 0;

	return 0;
}

static int crc_t10dif_final(struct shash_desc *desc, u8 *out)
{
	u16 *crc = shash_desc_ctx(desc);

	*(u16 *)out = *crc;

	return 0;
}

static int crc_t10dif_update_zbc(struct shash_desc *desc, const u8 *data,
				unsigned int length)
{
	u16 *crc = shash_desc_ctx(desc);

	*crc = crc_t10dif_generic_zbc(*crc, data, length);

	return 0;
}

static struct shash_alg crc_t10dif_alg = {
	.digestsize		= CRC_T10DIF_DIGEST_SIZE,
	.init			= crc_t10dif_init,
	.update			= crc_t10dif_update_zbc,
	.final			= crc_t10dif_final,
	.descsize		= CRC_T10DIF_DIGEST_SIZE,

	.base.cra_name		= "crct10dif",
	.base.cra_driver_name	= "crct10dif-riscv-zbc",
	.base.cra_priority	= 150,
	.base.cra_blocksize	= CRC_T10DIF_BLOCK_SIZE,
	.base.cra_module	= THIS_MODULE,
};

static int __init crc_t10dif_mod_init(void)
{
	if (riscv_isa_extension_available(NULL, ZBC))
		return crypto_register_shash(&crc_t10dif_alg);

	return -ENODEV;
}

static void __exit crc_t10dif_mod_exit(void)
{
	crypto_unregister_shash(&crc_t10dif_alg);
}

module_init(crc_t10dif_mod_init);
module_exit(crc_t10dif_mod_exit);

MODULE_DESCRIPTION("CRC-T10DIF using RISC-V ZBC Extension");
MODULE_ALIAS_CRYPTO("crct10dif");
MODULE_LICENSE("GPL");
