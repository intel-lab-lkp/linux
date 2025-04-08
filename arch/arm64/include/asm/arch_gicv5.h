/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2025 ARM Ltd.
 */
#ifndef __ASM_ARCH_GICV5_H
#define __ASM_ARCH_GICV5_H

#include <asm/sysreg.h>

#ifndef __ASSEMBLY__

#define GICV5_OP_GIC_CDDI		sys_insn(1, 0, 12, 2, 0)
#define GICV5_OP_GIC_CDEOI		sys_insn(1, 0, 12, 1, 7)
#define GICV5_OP_GICR_CDIA		sys_insn(1, 0, 12, 3, 0)

#define gicr_insn(insn)			read_sysreg_s(insn)
#define gic_insn(v, insn)		write_sysreg_s(v, insn)

#define GSB_ACK				__emit_inst(0xd5000000 | sys_insn(1, 0, 12, 0, 1) | 31)

#define gsb_ack()			asm volatile(GSB_ACK : : : "memory")

/* Shift and mask definitions for GIC CDDI */
#define GICV5_GIC_CDDI_TYPE_MASK	GENMASK_ULL(31, 29)
#define GICV5_GIC_CDDI_TYPE(r)		FIELD_GET(GICV5_GIC_CDDI_TYPE_MASK, r)
#define GICV5_GIC_CDDI_ID_MASK		GENMASK_ULL(23, 0)
#define GICV5_GIC_CDDI_ID(r)		FIELD_GET(GICV5_GIC_CDDI_ID_MASK, r)

/* Shift and mask definitions for GICR CDIA */
#define GICV5_GIC_CDIA_VALID_MASK	BIT_ULL(32)
#define GICV5_GIC_CDIA_VALID(r)		FIELD_GET(GICV5_GIC_CDIA_VALID_MASK, r)
#define GICV5_GIC_CDIA_TYPE_MASK	GENMASK_ULL(31, 29)
#define GICV5_GIC_CDIA_TYPE(r)		FIELD_GET(GICV5_GIC_CDIA_TYPE_MASK, r)
#define GICV5_GIC_CDIA_ID_MASK		GENMASK_ULL(23, 0)
#define GICV5_GIC_CDIA_ID(r)		FIELD_GET(GICV5_GIC_CDIA_ID_MASK, r)

#endif /* __ASSEMBLY__ */
#endif /* __ASM_ARCH_GICV5_H */
