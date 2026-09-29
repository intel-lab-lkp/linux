/* SPDX-License-Identifier: (GPL-2.0 OR MIT) */
/*
 * Copyright (C) 2026 Synaptics Incorporated
 *
 * Author: Jisheng Zhang <jszhang@kernel.org>
 */

#ifndef _DT_BINDINGS_SL261X_RESET_H
#define _DT_BINDINGS_SL261X_RESET_H

/* ACPU subsystem */
#define RST_SOC_SDIO0		0
#define RST_SOC_USB0		1
#define RST_SOC_EMMC		2
#define RST_SOC_GETH0		3
#define RST_SOC_SDIO1		4
#define RST_SOC_USB1		5
#define RST_SOC_GETH1		6
#define RST_SOC_USB0PHY		7
#define RST_SOC_USB0CORE	8
#define RST_SOC_USB0MAHB	9
#define RST_SOC_USB1PHY		10
#define RST_SOC_USB1CORE	11
#define RST_SOC_USB1MAHB	12
#define RST_SOC_UART0		13
#define RST_SOC_UART1		14
#define RST_SOC_UART2		15
#define RST_SOC_UART3		16
#define RST_SOC_I2C0		17
#define RST_SOC_I2C1		18
#define RST_SOC_SPI0		19
#define RST_SOC_SPI1		20
#define RST_SOC_SPI2		21
#define RST_SOC_SPI3		22
#define RST_SOC_APBTIMERS	23
#define RST_SOC_APBSYSCNT	24
#define RST_SOC_APBWDT		25
#define RST_SOC_APBGPIO		26
#define RST_SOC_APBDMA		27
#define RST_SOC_GPUCORE		28
#define RST_SOC_NPUCORE		29
#define RST_SOC_AVIOAIOG	30
#define RST_SOC_AVIOVPPG	31
#define RST_SOC_AVIOVIPG	32

/* system subsystem */
#define RST_SM_ADCCORE		0
#define RST_SM_ADCPRST		1
#define RST_SM_CAN0PRST		2
#define RST_SM_CAN0SRST		3
#define RST_SM_CAN1PRST		4
#define RST_SM_CAN1SRST		5
#define RST_SM_GPIOPRST		6
#define RST_SM_GPIOSRST		7
#define RST_SM_I2CM0PRST	8
#define RST_SM_I2CM0SRST	9
#define RST_SM_I2CM1PRST	10
#define RST_SM_I2CM1SRST	11
#define RST_SM_I3CPRST		12
#define RST_SM_I3CSRST		13
#define RST_SM_PDMPRST		14
#define RST_SM_PDMSRST		15
#define RST_SM_PVTHSRST		16
#define RST_SM_PVTPRST		17
#define RST_SM_PWMPERIRST	18
#define RST_SM_PWMPRST		19
#define RST_SM_SPIMPRST		20
#define RST_SM_SPIMSRST		21
#define RST_SM_SPISPRST		22
#define RST_SM_SPISSRST		23
#define RST_SM_UART0PRST	24
#define RST_SM_UART0SRST	25
#define RST_SM_UART1PRST	26
#define RST_SM_UART1SRST	27
#define RST_SM_UART2PRST	28
#define RST_SM_UART2SRST	29
#define RST_SM_UART3PRST	30
#define RST_SM_UART3SRST	31

#endif /* _DT_BINDINGS_SL261X_RESET_H */
