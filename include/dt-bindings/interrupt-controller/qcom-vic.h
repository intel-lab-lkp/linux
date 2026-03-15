/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * This header provides constants for the ARM GIC.
 */

#ifndef _DT_BINDINGS_INTERRUPT_CONTROLLER_QCOM_VIC_H
#define _DT_BINDINGS_INTERRUPT_CONTROLLER_QCOM_VIC_H

#include <dt-bindings/interrupt-controller/irq.h>

#define VIC_INT_TO_REG_ADDR(base, irq) (base + ((irq & 32) ? 4 : 0))
#define VIC_INT_TO_REG_INDEX(irq) ((irq >> 5) & 1)

#define VIC_INT_SELECT0     0x0000  /* 1: FIQ, 0: IRQ */
#define VIC_INT_SELECT1     0x0004  /* 1: FIQ, 0: IRQ */
#define VIC_INT_SELECT2     0x0008  /* 1: FIQ, 0: IRQ */
#define VIC_INT_SELECT3     0x000C  /* 1: FIQ, 0: IRQ */
#define VIC_INT_EN0         0x0010
#define VIC_INT_EN1         0x0014
#define VIC_INT_EN2         0x0018
#define VIC_INT_EN3         0x001C
#define VIC_INT_ENCLEAR0    0x0020
#define VIC_INT_ENCLEAR1    0x0024
#define VIC_INT_ENCLEAR2    0x0028
#define VIC_INT_ENCLEAR3    0x002C
#define VIC_INT_ENSET0      0x0030
#define VIC_INT_ENSET1      0x0034
#define VIC_INT_ENSET2      0x0038
#define VIC_INT_ENSET3      0x003C
#define VIC_INT_TYPE0       0x0040  /* 1: EDGE, 0: LEVEL  */
#define VIC_INT_TYPE1       0x0044  /* 1: EDGE, 0: LEVEL  */
#define VIC_INT_TYPE2       0x0048  /* 1: EDGE, 0: LEVEL  */
#define VIC_INT_TYPE3       0x004C  /* 1: EDGE, 0: LEVEL  */
#define VIC_INT_POLARITY0   0x0050  /* 1: NEG, 0: POS */
#define VIC_INT_POLARITY1   0x0054  /* 1: NEG, 0: POS */
#define VIC_INT_POLARITY2   0x0058  /* 1: NEG, 0: POS */
#define VIC_INT_POLARITY3   0x005C  /* 1: NEG, 0: POS */
#define VIC_NO_PEND_VAL     0x0060


#define VIC_NO_PEND_VAL_FIQ 0x0064
#define VIC_INT_MASTEREN    0x0068  /* 1: IRQ, 2: FIQ     */
#define VIC_CONFIG          0x006C  /* 1: USE SC VIC */


#define IRQF_VALID	(1 << 0)
#define IRQF_PROBE	(1 << 1)
#define IRQF_NOAUTOEN	(1 << 2)

#define VIC_IRQ_STATUS0     0x0080
#define VIC_IRQ_STATUS1     0x0084
#define VIC_IRQ_STATUS2     0x0088
#define VIC_IRQ_STATUS3     0x008C
#define VIC_FIQ_STATUS0     0x0090
#define VIC_FIQ_STATUS1     0x0094
#define VIC_FIQ_STATUS2     0x0098
#define VIC_FIQ_STATUS3     0x009C
#define VIC_RAW_STATUS0     0x00A0
#define VIC_RAW_STATUS1     0x00A4
#define VIC_RAW_STATUS2     0x00A8
#define VIC_RAW_STATUS3     0x00AC
#define VIC_INT_CLEAR0      0x00B0
#define VIC_INT_CLEAR1      0x00B4
#define VIC_INT_CLEAR2      0x00B8
#define VIC_INT_CLEAR3      0x00BC
#define VIC_SOFTINT0        0x00C0
#define VIC_SOFTINT1        0x00C4
#define VIC_SOFTINT2        0x00C8
#define VIC_SOFTINT3        0x00CC
#define VIC_IRQ_VEC_RD      0x00D0  /* pending int # */
#define VIC_IRQ_VEC_PEND_RD 0x00D4  /* pending vector addr */
#define VIC_IRQ_VEC_WR      0x00D8


#define VIC_FIQ_VEC_RD      0x00DC
#define VIC_FIQ_VEC_PEND_RD 0x00E0
#define VIC_FIQ_VEC_WR      0x00E4
#define VIC_IRQ_IN_SERVICE  0x00E8
#define VIC_IRQ_IN_STACK    0x00EC
#define VIC_FIQ_IN_SERVICE  0x00F0
#define VIC_FIQ_IN_STACK    0x00F4
#define VIC_TEST_BUS_SEL    0x00F8
#define VIC_IRQ_CTRL_CONFIG 0x00FC


#define VIC_VECTPRIORITY(n) 0x0200+((n) * 4)
#define VIC_VECTADDR(n)     0x0400+((n) * 4)

#define SMSM_FAKE_IRQ (0xff)

#define VIC_NUM_REGS	    2
#endif
