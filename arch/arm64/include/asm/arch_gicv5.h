/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2025 ARM Ltd.
 */
#ifndef __ASM_ARCH_GICV5_H
#define __ASM_ARCH_GICV5_H

#include <asm/cacheflush.h>
#include <asm/sysreg.h>

#ifndef __ASSEMBLY__

#define GICV5_OP_GIC_CDAFF		sys_insn(1, 0, 12, 1, 3)
#define GICV5_OP_GIC_CDDI		sys_insn(1, 0, 12, 2, 0)
#define GICV5_OP_GIC_CDDIS		sys_insn(1, 0, 12, 1, 0)
#define GICV5_OP_GIC_CDEN		sys_insn(1, 0, 12, 1, 1)
#define GICV5_OP_GIC_CDEOI		sys_insn(1, 0, 12, 1, 7)
#define GICV5_OP_GIC_CDPEND		sys_insn(1, 0, 12, 1, 4)
#define GICV5_OP_GIC_CDPRI		sys_insn(1, 0, 12, 1, 2)
#define GICV5_OP_GIC_CDRCFG		sys_insn(1, 0, 12, 1, 5)
#define GICV5_OP_GICR_CDIA		sys_insn(1, 0, 12, 3, 0)

#define gicr_insn(insn)			read_sysreg_s(insn)
#define gic_insn(v, insn)		write_sysreg_s(v, insn)

#define GSB_ACK				__emit_inst(0xd5000000 | sys_insn(1, 0, 12, 0, 1) | 31)
#define GSB_SYS				__emit_inst(0xd5000000 | sys_insn(1, 0, 12, 0, 0) | 31)

#define gsb_ack()			asm volatile(GSB_ACK : : : "memory")
#define gsb_sys()			asm volatile(GSB_SYS : : : "memory")

/* Shift and mask definitions for GIC CDAFF */
#define GICV5_GIC_CDAFF_IAFFID_MASK	GENMASK_ULL(47, 32)
#define GICV5_GIC_CDAFF_IAFFID(r)	FIELD_GET(GICV5_GIC_CDAFF_IAFFID_MASK, r)
#define GICV5_GIC_CDAFF_TYPE_MASK	GENMASK_ULL(31, 29)
#define GICV5_GIC_CDAFF_TYPE(r)		FIELD_GET(GICV5_GIC_CDAFF_TYPE_MASK, r)
#define GICV5_GIC_CDAFF_IRM_MASK	BIT_ULL(28)
#define GICV5_GIC_CDAFF_IRM(r)		FIELD_GET(GICV5_GIC_CDAFF_IRM_MASK, r)
#define GICV5_GIC_CDAFF_ID_MASK		GENMASK_ULL(23, 0)
#define GICV5_GIC_CDAFF_ID(r)		FIELD_GET(GICV5_GIC_CDAFF_ID_MASK, r)

/* Shift and mask definitions for GIC CDDI */
#define GICV5_GIC_CDDI_TYPE_MASK	GENMASK_ULL(31, 29)
#define GICV5_GIC_CDDI_TYPE(r)		FIELD_GET(GICV5_GIC_CDDI_TYPE_MASK, r)
#define GICV5_GIC_CDDI_ID_MASK		GENMASK_ULL(23, 0)
#define GICV5_GIC_CDDI_ID(r)		FIELD_GET(GICV5_GIC_CDDI_ID_MASK, r)

/* Shift and mask definitions for GIC CDDIS */
#define GICV5_GIC_CDDIS_TYPE_MASK	GENMASK_ULL(31, 29)
#define GICV5_GIC_CDDIS_TYPE(r)		FIELD_GET(GICV5_GIC_CDDIS_TYPE_MASK, r)
#define GICV5_GIC_CDDIS_ID_MASK		GENMASK_ULL(23, 0)
#define GICV5_GIC_CDDIS_ID(r)		FIELD_GET(GICV5_GIC_CDDIS_ID_MASK, r)

/* Shift and mask definitions for GIC CDEN */
#define GICV5_GIC_CDEN_TYPE_MASK	GENMASK_ULL(31, 29)
#define GICV5_GIC_CDEN_TYPE(r)		FIELD_GET(GICV5_GIC_CDEN_TYPE_MASK, r)
#define GICV5_GIC_CDEN_ID_MASK		GENMASK_ULL(23, 0)
#define GICV5_GIC_CDEN_ID(r)		FIELD_GET(GICV5_GIC_CDEN_ID_MASK, r)

/* Shift and mask definitions for GIC CDPEND */
#define GICV5_GIC_CDPEND_PENDING_MASK	BIT_ULL(32)
#define GICV5_GIC_CDPEND_PENDING(r)	FIELD_GET(GICV5_GIC_CDPEND_PENDING_MASK, r)
#define GICV5_GIC_CDPEND_TYPE_MASK	GENMASK_ULL(31, 29)
#define GICV5_GIC_CDPEND_TYPE(r)	FIELD_GET(GICV5_GIC_CDPEND_TYPE_MASK, r)
#define GICV5_GIC_CDPEND_ID_MASK	GENMASK_ULL(23, 0)
#define GICV5_GIC_CDPEND_ID(r)		FIELD_GET(GICV5_GIC_CDPEND_ID_MASK, r)

/* Shift and mask definitions for GIC CDPRI */
#define GICV5_GIC_CDPRI_PRIORITY_MASK	GENMASK_ULL(39, 35)
#define GICV5_GIC_CDPRI_PRIORITY(r)	FIELD_GET(GICV5_GIC_CDPRI_PRIORITY_MASK, r)
#define GICV5_GIC_CDPRI_TYPE_MASK	GENMASK_ULL(31, 29)
#define GICV5_GIC_CDPRI_TYPE(r)		FIELD_GET(GICV5_GIC_CDPRI_TYPE_MASK, r)
#define GICV5_GIC_CDPRI_ID_MASK		GENMASK_ULL(23, 0)
#define GICV5_GIC_CDPRI_ID(r)		FIELD_GET(GICV5_GIC_CDPRI_ID_MASK, r)

/* Shift and mask definitions for GIC CDRCFG */
#define GICV5_GIC_CDRCFG_TYPE_MASK	GENMASK_ULL(31, 29)
#define GICV5_GIC_CDRCFG_TYPE(r)	FIELD_GET(GICV5_GIC_CDRCFG_TYPE_MASK, r)
#define GICV5_GIC_CDRCFG_ID_MASK	GENMASK_ULL(23, 0)
#define GICV5_GIC_CDRCFG_ID(r)		FIELD_GET(GICV5_GIC_CDRCFG_ID_MASK, r)

/* Shift and mask definitions for GICR CDIA */
#define GICV5_GIC_CDIA_VALID_MASK	BIT_ULL(32)
#define GICV5_GIC_CDIA_VALID(r)		FIELD_GET(GICV5_GIC_CDIA_VALID_MASK, r)
#define GICV5_GIC_CDIA_TYPE_MASK	GENMASK_ULL(31, 29)
#define GICV5_GIC_CDIA_TYPE(r)		FIELD_GET(GICV5_GIC_CDIA_TYPE_MASK, r)
#define GICV5_GIC_CDIA_ID_MASK		GENMASK_ULL(23, 0)
#define GICV5_GIC_CDIA_ID(r)		FIELD_GET(GICV5_GIC_CDIA_ID_MASK, r)

#endif /* __ASSEMBLY__ */
#endif /* __ASM_ARCH_GICV5_H */
