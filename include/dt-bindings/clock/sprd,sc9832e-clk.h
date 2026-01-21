/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * Unisoc SC9832E platform clocks
 *
 * Copyright (C) 2015 Spreadtrum, Inc.
 */

#ifndef _DT_BINDINGS_CLK_SC9832E_H_
#define _DT_BINDINGS_CLK_SC9832E_H_

#define CLK_ISPPLL_GATE		0
#define CLK_MPLL_GATE		1
#define CLK_DPLL_GATE		2
#define CLK_LPLL_GATE		3
#define CLK_GPLL_GATE		4

#define CLK_TWPLL		0
#define CLK_TWPLL_768M		1
#define CLK_TWPLL_384M		2
#define CLK_TWPLL_192M		3
#define CLK_TWPLL_96M		4
#define CLK_TWPLL_48M		5
#define CLK_TWPLL_24M		6
#define CLK_TWPLL_12M		7
#define CLK_TWPLL_512M		8
#define CLK_TWPLL_256M		9
#define CLK_TWPLL_128M		10
#define CLK_TWPLL_64M		11
#define CLK_TWPLL_307M2		12
#define CLK_TWPLL_219M4		13
#define CLK_TWPLL_170M6		14
#define CLK_TWPLL_153M6		15
#define CLK_TWPLL_76M8		16
#define CLK_TWPLL_51M2		17
#define CLK_TWPLL_38M4		18
#define CLK_TWPLL_19M2		19
#define CLK_LPLL		20
#define CLK_LPLL_409M6		21
#define CLK_LPLL_245M76		22
#define CLK_GPLL		23
#define CLK_ISPPLL		24
#define CLK_ISPPLL_468M		25

#define CLK_MPLL		0
#define CLK_MPLL_50M		1

#define CLK_DPLL		0
#define CLK_DPLL_40M		1

#define CLK_AUDIO_GATE		0
#define CLK_RPLL		1
#define CLK_RPLL_390M		2
#define CLK_RPLL_260M		3
#define CLK_RPLL_195M		4
#define CLK_RPLL_26M		5

#define CLK_DSI_EB		0
#define CLK_DISPC_EB		1
#define CLK_VSP_EB		2
#define CLK_GSP_EB		3
#define CLK_OTG_EB		4
#define CLK_DMA_PUB_EB		5
#define CLK_CE_PUB_EB		6
#define CLK_AHB_CKG_EB		7
#define CLK_SDIO0_EB		8
#define CLK_SDIO1_EB		9
#define CLK_NANDC_EB		10
#define CLK_EMMC_EB		11
#define CLK_SPINLOCK_EB		12
#define CLK_CE_EFUSE_EB		13
#define CLK_EMMC_32K_EB		14
#define CLK_SDIO0_32K_EB	15
#define CLK_SDIO1_32K_EB	16

#define CLK_ADC_EB		0
#define CLK_FM_EB		1
#define CLK_TPC_EB		2
#define CLK_GPIO_EB		3
#define CLK_PWM0_EB		4
#define CLK_PWM1_EB		5
#define CLK_PWM2_EB		6
#define CLK_PWM3_EB		7
#define CLK_KPD_EB		8
#define CLK_AON_SYST_EB		9
#define CLK_AP_SYST_EB		10
#define CLK_AON_TMR_EB		11
#define CLK_AP_TMR0_EB		12
#define CLK_EFUSE_EB		13
#define CLK_EIC_EB		14
#define CLK_INTC_EB		15
#define CLK_ADI_EB		16
#define CLK_AUDIF_EB		17
#define CLK_AUD_EB		18
#define CLK_VBC_EB		19
#define CLK_PIN_EB		20
#define CLK_IPI_EB		21
#define CLK_SPLK_EB		22
#define CLK_AP_WDG_EB		23
#define CLK_MM_EB		24
#define CLK_AON_APB_CKG_EB	25
#define CLK_GPU_EB		26
#define CLK_CA7_TS0_EB		27
#define CLK_CA7_DAP_EB		28

#define CLK_AP_APB		0
#define CLK_NANDC_ECC		1
#define CLK_OTG_REF		2
#define CLK_OTG_UTMI		3
#define CLK_UART1		4
#define CLK_I2C0		5
#define CLK_I2C1		6
#define CLK_I2C2		7
#define CLK_I2C3		8
#define CLK_I2C4		9
#define CLK_SPI0		10
#define CLK_SPI2		11
#define CLK_HS_SPI		12
#define CLK_IIS0		13
#define CLK_CE			14
#define CLK_NANDC_2X		15
#define CLK_SDIO0_2X		16
#define CLK_SDIO1_2X		17
#define CLK_EMMC_2X		18
#define CLK_VSP			19
#define CLK_GSP			20
#define CLK_DISPC0		21
#define CLK_DISPC0_DPI		22
#define CLK_DSI_RXESC		23
#define CLK_DSI_LANEBYTE	24

#define CLK_AON_APB		0
#define CLK_ADI			1
#define CLK_AUX0		2
#define CLK_AUX1		3
#define CLK_PWM0		4
#define CLK_PWM1		5
#define CLK_PWM2		6
#define CLK_PWM3		7
#define CLK_THM0		8
#define CLK_THM1		9
#define CLK_AUDIF		10
#define CLK_AUD_IIS_DA0		11
#define CLK_AUD_IIS_AD0		12
#define CLK_CA53_DAP		13
#define CLK_CA53_DMTCK		14
#define CLK_CA53_TS		15
#define CLK_DJTAG_TCK		16
#define CLK_EMC_REF		17
#define CLK_CSSYS		18
#define CLK_TMR			19
#define CLK_DSI_TEST		20
#define CLK_SDPHY_APB		21
#define CLK_AIO_APB		22
#define CLK_DTCK_HW		23
#define CLK_AP_MM		24
#define CLK_AP_AXI		25
#define CLK_NIC_GPU		26
#define CLK_MM_ISP		27

#define CLK_SIM0_EB		0
#define CLK_IIS0_EB		1
#define CLK_APB_REG_EB		2
#define CLK_SPI0_EB		3
#define CLK_SPI2_EB		4
#define CLK_I2C0_EB		5
#define CLK_I2C1_EB		6
#define CLK_I2C2_EB		7
#define CLK_I2C3_EB		8
#define CLK_I2C4_EB		9
#define CLK_UART1_EB		10
#define CLK_SIM0_32K_EB		11
#define CLK_INTC0_EB		12
#define CLK_INTC1_EB		13
#define CLK_INTC2_EB		14
#define CLK_INTC3_EB		15

#endif /* _DT_BINDINGS_CLK_SC9832E_H_ */
