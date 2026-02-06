/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_GENERIC_X86_VFM
#define _ASM_GENERIC_X86_VFM

/*
 * Can't use <linux/bitfield.h> because it generates expressions that
 * cannot be used in structure initializers. Bitfield construction
 * here must match the union in struct cpuinfo_86:
 *	union {
 *		struct {
 *			__u8	x86_model;
 *			__u8	x86;
 *			__u8	x86_vendor;
 *			__u8	x86_reserved;
 *		};
 *		__u32		x86_vfm;
 *	};
 */
#define VFM_MODEL_BIT	0
#define VFM_FAMILY_BIT	8
#define VFM_VENDOR_BIT	16
#define VFM_RSVD_BIT	24

#define	VFM_MODEL_MASK	GENMASK(VFM_FAMILY_BIT - 1, VFM_MODEL_BIT)
#define	VFM_FAMILY_MASK	GENMASK(VFM_VENDOR_BIT - 1, VFM_FAMILY_BIT)
#define	VFM_VENDOR_MASK	GENMASK(VFM_RSVD_BIT - 1, VFM_VENDOR_BIT)

#define VFM_MODEL(vfm)	(((vfm) & VFM_MODEL_MASK) >> VFM_MODEL_BIT)
#define VFM_FAMILY(vfm)	(((vfm) & VFM_FAMILY_MASK) >> VFM_FAMILY_BIT)
#define VFM_VENDOR(vfm)	(((vfm) & VFM_VENDOR_MASK) >> VFM_VENDOR_BIT)

#define	VFM_MAKE(_vendor, _family, _model) (	\
	((_model) << VFM_MODEL_BIT) |		\
	((_family) << VFM_FAMILY_BIT) |		\
	((_vendor) << VFM_VENDOR_BIT)		\
)

#endif /* _ASM_GENERIC_X86_VFM */
