/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause) */

#ifndef _DT_BINDINGS_CLK_RDA8810_H_
#define _DT_BINDINGS_CLK_RDA8810_H_

/* soc clocks */
#define CLK_CPU			0
#define CLK_BUS			1
#define CLK_MEM			2
#define CLK_USB			3
#define CLK_AXI			4
#define CLK_GCG			5
#define CLK_AHB1			6
#define CLK_APB1			7
#define CLK_APB2			8
#define CLK_GPU			9
#define CLK_VPU			10
#define CLK_VOC			11
#define CLK_SFLSH			12
#define CLK_UART1			13
#define CLK_UART2			14
#define CLK_UART3			15
#define CLK_VOC2			16
#define CLK_EMMC			17

/* resets */
#define RST_CPU			0

#define RST_AXI_VOC			1
#define RST_AXI_DMA			2
#define RST_AXI_CONNECT		3
#define RST_AXI_VPU			4

#define RST_GCG_GOUDA			5
#define RST_GCG_CAMERA			6
#define RST_GCG_LCDC			7

#define RST_AHB1_USBC			8
#define RST_AHB1_SPIFLASH		9
#define RST_APB1_TIMER			10
#define RST_APB1_KEYPAD		11
#define RST_APB1_GPIO			12
#define RST_APB1_PWM			13
#define RST_APB1_AIF			14
#define RST_APB1_AUIFC			15
#define RST_APB1_I2C1			16
#define RST_APB1_I2C2			17
#define RST_APB1_I2C3			18
#define RST_APB1_COMREGS		19
#define RST_APB1_DMC			20
#define RST_APB1_DDRPHY_P		21

#define RST_APB2_IFC			22
#define RST_APB2_UART1			23
#define RST_APB2_UART2			24
#define RST_APB2_UART3			25
#define RST_APB2_SPI1			26
#define RST_APB2_SPI2			27
#define RST_APB2_SPI3			28
#define RST_APB2_SDMMC1		29
#define RST_APB2_SDMMC2		30
#define RST_APB2_SDMMC3		31
#define RST_APB2_NAND			32

#define RST_MEM_GPU			33
#define RST_MEM_VPU			34
#define RST_MEM_DMC			35
#define RST_MEM_DDRPHY_P		36

#endif /* _DT_BINDINGS_CLK_RDA8810_H_ */
