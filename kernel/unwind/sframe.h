/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * From https://www.sourceware.org/binutils/docs/sframe-spec.html
 */
#ifndef _SFRAME_H
#define _SFRAME_H

#include <linux/types.h>

#define SFRAME_VERSION_1			1
#define SFRAME_VERSION_2			2
#define SFRAME_MAGIC				0xdee2

#define SFRAME_F_FDE_SORTED			0x1
#define SFRAME_F_FRAME_POINTER			0x2

#define SFRAME_ABI_AARCH64_ENDIAN_BIG		1
#define SFRAME_ABI_AARCH64_ENDIAN_LITTLE	2
#define SFRAME_ABI_AMD64_ENDIAN_LITTLE		3

#define SFRAME_FDE_TYPE_PCINC			0
#define SFRAME_FDE_TYPE_PCMASK			1

struct sframe_preamble {
	u16	magic;
	u8	version;
	u8	flags;
} __packed;

struct sframe_header {
	struct sframe_preamble preamble;
	u8	abi_arch;
	s8	cfa_fixed_fp_offset;
	s8	cfa_fixed_ra_offset;
	u8	auxhdr_len;
	u32	num_fdes;
	u32	num_fres;
	u32	fre_len;
	u32	fdes_off;
	u32	fres_off;
} __packed;

#define SFRAME_HEADER_SIZE(header) \
	((sizeof(struct sframe_header) + header.auxhdr_len))

#define SFRAME_AARCH64_PAUTH_KEY_A		0
#define SFRAME_AARCH64_PAUTH_KEY_B		1

struct sframe_fde {
	s32	start_addr;
	u32	func_size;
	u32	fres_off;
	u32	fres_num;
	u8	info;
	u8	rep_size;
	u16 padding;
} __packed;

#define SFRAME_FUNC_FRE_TYPE(data)		(data & 0xf)
#define SFRAME_FUNC_FDE_TYPE(data)		((data >> 4) & 0x1)
#define SFRAME_FUNC_PAUTH_KEY(data)		((data >> 5) & 0x1)

#define SFRAME_BASE_REG_FP			0
#define SFRAME_BASE_REG_SP			1

#define SFRAME_FRE_CFA_BASE_REG_ID(data)	(data & 0x1)
#define SFRAME_FRE_OFFSET_COUNT(data)		((data >> 1) & 0xf)
#define SFRAME_FRE_OFFSET_SIZE(data)		((data >> 5) & 0x3)
#define SFRAME_FRE_MANGLED_RA_P(data)		((data >> 7) & 0x1)

#endif /* _SFRAME_H */
