/* SPDX-License-Identifier: (GPL-2.0-only OR MIT) */
#ifndef _DT_BINDINGS_CLK_SUN8I_B288_H_
#define _DT_BINDINGS_CLK_SUN8I_B288_H_

#define CLK_PLL_CPUX	0
#define CLK_PLL_AUDIO_BASE	1
#define CLK_PLL_VIDEO0	2
#define CLK_PLL_DDR0	3
#define CLK_PLL_PERIPH0	4
#define CLK_PLL_VIDEO1	5
#define CLK_PLL_24M	6
#define CLK_PLL_PERIPH1	7
#define CLK_PLL_DE	8
#define CLK_PLL_DDR1	9

#define CLK_PLL_PERIPH0_2X	10
#define CLK_PLL_PERIPH1_2X	11
#define CLK_CPUX	12
#define CLK_AXI	13
#define CLK_CPUAPB	14
#define CLK_AHB1	15
#define CLK_APB1	16
#define CLK_APB2	17

#define CLK_NAND	18
#define CLK_MMC0	19
#define CLK_MMC1	20
#define CLK_MMC2	21
#define CLK_MMC3	22
#define CLK_SPI0	23
#define CLK_SPI1	24
#define CLK_SPI2	25
#define CLK_DRAM	26
#define CLK_DE	27
#define CLK_EE	28
#define CLK_EDMA	29
#define CLK_TCON0	30
#define CLK_MBUS	31

#define CLK_BUS_THS	32
#define CLK_BUS_NAND	33
#define CLK_BUS_SDMMC0_BUS	34
#define CLK_BUS_SDMMC1_BUS	35
#define CLK_BUS_SDMMC2_BUS	36
#define CLK_BUS_SDMMC3_BUS	37
#define CLK_BUS_SPI0	38
#define CLK_BUS_SPI1	39
#define CLK_BUS_SPI2	40
#define CLK_BUS_I2S0	41
#define CLK_BUS_I2S1	42
#define CLK_BUS_SPDIF	43
#define CLK_BUS_DSD	44
#define CLK_BUS_DMIC	45
#define CLK_BUS_USBOHCI0	46
#define CLK_BUS_USBEHCI0	47
#define CLK_BUS_USBOTG	48
#define CLK_BUS_DE	49
#define CLK_BUS_EE	50
#define CLK_BUS_TCON0	51
#define CLK_BUS_CSI	52
#define CLK_BUS_ADDA	53
#define CLK_BUS_SDRAM	54
#define CLK_BUS_PSRAM	55
#define CLK_BUS_DMA	56
#define CLK_BUS_UART0	57
#define CLK_BUS_UART1	58
#define CLK_BUS_UART2	59
#define CLK_BUS_UART3	60
#define CLK_BUS_UART4	61
#define CLK_BUS_UART5	62
#define CLK_BUS_TWI0	63
#define CLK_BUS_TWI1	64
#define CLK_BUS_TWI2	65
#define CLK_BUS_PIO	66
#define CLK_BUS_GPADC	67
#define CLK_BUS_KEYADC	68

/*
 * mmc0 and mmc1 are v4p1x controllers driven in the old timing mode, which
 * needs the sample and output phase clocks. mmc2 and mmc3 are newer blocks
 * that do their own delay calibration and have none.
 */
#define CLK_MMC0_SAMPLE	69
#define CLK_MMC0_OUTPUT	70
#define CLK_MMC1_SAMPLE	71
#define CLK_MMC1_OUTPUT	72

#endif
