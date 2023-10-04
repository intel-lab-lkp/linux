/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright 2023 Mobileye Vision Technologies Ltd.
 */
#ifndef _DT_BINDINGS_SOC_MOBILEYE_EYEQ5_H
#define _DT_BINDINGS_SOC_MOBILEYE_EYEQ5_H

/* EQ5 interrupts */
#define NUM_INT_I2C_A			1
#define NUM_INT_I2C_B			2
#define NUM_INT_I2C_C			3
#define NUM_INT_I2C_D			4
#define NUM_INT_I2C_E			5

#define NUM_INT_UART			6 /* same for all UARTs - A, B, C */
#define NUM_INT_PCIE0_INT0		7
#define NUM_INT_PCIE0_INT1		8

#define NUM_INT_CAN			9 /* same for all CANs A, B, C */

#define NUM_INT_EMMC			10
/* empty				11 */
#define NUM_INT_SPIA_B			12
#define NUM_INT_SPIC_D			13

#define NUM_INT_GPIO			14

#define NUM_INT_TIMER_0			15
#define NUM_INT_TIMER_1			16
#define NUM_INT_TIMER_2			17
#define NUM_INT_TIMER_3			18
#define NUM_INT_TIMER_4_ETIMER0_1	19
#define NUM_INT_OQSPI			20
#define NUM_INT_DDR_CTRL		21
#define NUM_INT_NOC			22

#define NUM_INT_GEM0			23
#define NUM_INT_GEM1			24

#define NUM_INT_VDI_0_VC0		25
#define NUM_INT_VDI_0_VC1		26
#define NUM_INT_VDI_0_VC2		27
#define NUM_INT_VDI_0_VC3		28
#define NUM_INT_VDI_0_ERR		29
#define NUM_INT_VDI_1_VC0		30
#define NUM_INT_VDI_1_VC1		31
#define NUM_INT_VDI_1_VC2		32
#define NUM_INT_VDI_1_VC3		33
#define NUM_INT_VDI_1_ERR		34

#define NUM_INT_MPC0			35
#define NUM_INT_MPC1			36
#define NUM_INT_MPC2			37
#define NUM_INT_MPC3			38
#define NUM_INT_MPC4			39
#define NUM_INT_VMP0			40
#define NUM_INT_VMP1			41
#define NUM_INT_VMP2			42
#define NUM_INT_VMP3			43
#define NUM_INT_PMA0			44
#define NUM_INT_PMA1			45
#define NUM_INT_PMAC0			46
#define NUM_INT_PMAC1			47

#define NUM_INT_PCIE1_INT0		48
#define NUM_INT_PCIE1_INT1		49

#define NUM_INT_HSM_C3			50

#define NUM_INT_MJPEG			51

#define NUM_INT_FCMU_OLB		52
#define NUM_INT_FCMU_NMI		53
#define NUM_INT_WDDOG_TIMER		54
#define NUM_INT_WDDOG_TIMER_1		55

#endif /* _DT_BINDINGS_SOC_MOBILEYE_EYEQ5_H */
