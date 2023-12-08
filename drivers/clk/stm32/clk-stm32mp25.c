// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) STMicroelectronics 2023 - All Rights Reserved
 * Author: Gabriel Fernandez <gabriel.fernandez@foss.st.com> for STMicroelectronics.
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "clk-stm32-core.h"
#include "reset-stm32.h"

#include <dt-bindings/clock/st,stm32mp25-rcc.h>
#include <dt-bindings/reset/st,stm32mp25-rcc.h>

#include "stm32mp25_rcc.h"

static const char * const adc12_src[] = {
	"ck_flexgen_46", "ck_icn_ls_mcu"
};

static const char * const adc3_src[] = {
	"ck_flexgen_47", "ck_icn_ls_mcu", "ck_flexgen_46"
};

static const char * const usb2phy1_src[] = {
	"ck_flexgen_57", "hse_div2_ck"
};

static const char * const usb2phy2_src[] = {
	"ck_flexgen_58", "hse_div2_ck"
};

static const char * const usb3pciphy_src[] = {
	"ck_flexgen_34", "hse_div2_ck"
};

static const char * const dsiblane_src[] = {
	"txbyteclk", "ck_ker_ltdc"
};

static const char * const dsiphy_src[] = {
	"ck_flexgen_28", "hse_ck"
};

static const char * const lvdsphy_src[] = {
	"ck_flexgen_32", "hse_ck"
};

static const char * const dts_src[] = {
	"hsi_ck", "hse_ck", "msi_ck"
};

static const char * const mco1_src[] = {
	"ck_flexgen_61", "ck_obs0"
};

static const char * const mco2_src[] = {
	"ck_flexgen_62", "ck_obs1"
};

enum enum_mux_cfg {
	MUX_ADC12,
	MUX_ADC3,
	MUX_DSIBLANE,
	MUX_DSIPHY,
	MUX_DTS,
	MUX_LVDSPHY,
	MUX_MCO1,
	MUX_MCO2,
	MUX_USB2PHY1,
	MUX_USB2PHY2,
	MUX_USB3PCIEPHY,
	MUX_NB
};

#define MUX_CFG(id, _offset, _shift, _witdh)\
	[id] = {\
		.offset		= (_offset),\
		.shift		= (_shift),\
		.width		= (_witdh),\
	}

static const struct stm32_mux_cfg stm32mp25_muxes[MUX_NB] = {
	MUX_CFG(MUX_ADC12,		RCC_ADC12CFGR,		12,	1),
	MUX_CFG(MUX_ADC3,		RCC_ADC3CFGR,		12,	2),
	MUX_CFG(MUX_DSIBLANE,		RCC_DSICFGR,		12,	1),
	MUX_CFG(MUX_DSIPHY,		RCC_DSICFGR,		15,	1),
	MUX_CFG(MUX_DTS,		RCC_DTSCFGR,		12,	2),
	MUX_CFG(MUX_LVDSPHY,		RCC_LVDSCFGR,		15,	1),
	MUX_CFG(MUX_MCO1,		RCC_MCO1CFGR,		0,	1),
	MUX_CFG(MUX_MCO2,		RCC_MCO2CFGR,		0,	1),
	MUX_CFG(MUX_USB2PHY1,		RCC_USB2PHY1CFGR,	15,	1),
	MUX_CFG(MUX_USB2PHY2,		RCC_USB2PHY2CFGR,	15,	1),
	MUX_CFG(MUX_USB3PCIEPHY,	RCC_USB3PCIEPHYCFGR,	15,	1),
};

enum enum_gate_cfg {
	GATE_ADC12,
	GATE_ADC3,
	GATE_ADF1,
	GATE_CCI,
	GATE_CRC,
	GATE_CRYP1,
	GATE_CRYP2,
	GATE_CSI,
	GATE_DCMIPP,
	GATE_DSI,
	GATE_DTS,
	GATE_ETH1,
	GATE_ETH1MAC,
	GATE_ETH1RX,
	GATE_ETH1STP,
	GATE_ETH1TX,
	GATE_ETH2,
	GATE_ETH2MAC,
	GATE_ETH2RX,
	GATE_ETH2STP,
	GATE_ETH2TX,
	GATE_ETHSW,
	GATE_ETHSWACMCFG,
	GATE_ETHSWACMMSG,
	GATE_ETHSWMAC,
	GATE_ETHSWREF,
	GATE_FDCAN,
	GATE_GPU,
	GATE_HASH,
	GATE_HDP,
	GATE_I2C1,
	GATE_I2C2,
	GATE_I2C3,
	GATE_I2C4,
	GATE_I2C5,
	GATE_I2C6,
	GATE_I2C7,
	GATE_I2C8,
	GATE_I3C1,
	GATE_I3C2,
	GATE_I3C3,
	GATE_I3C4,
	GATE_IS2M,
	GATE_IWDG1,
	GATE_IWDG2,
	GATE_IWDG3,
	GATE_IWDG4,
	GATE_IWDG5,
	GATE_LPTIM1,
	GATE_LPTIM2,
	GATE_LPTIM3,
	GATE_LPTIM4,
	GATE_LPTIM5,
	GATE_LPUART1,
	GATE_LTDC,
	GATE_LVDS,
	GATE_MCO1,
	GATE_MCO2,
	GATE_MDF1,
	GATE_OSPIIOM,
	GATE_PCIE,
	GATE_PKA,
	GATE_RNG,
	GATE_SAES,
	GATE_SAI1,
	GATE_SAI2,
	GATE_SAI3,
	GATE_SAI4,
	GATE_SDMMC1,
	GATE_SDMMC2,
	GATE_SDMMC3,
	GATE_SERC,
	GATE_SPDIFRX,
	GATE_SPI1,
	GATE_SPI2,
	GATE_SPI3,
	GATE_SPI4,
	GATE_SPI5,
	GATE_SPI6,
	GATE_SPI7,
	GATE_SPI8,
	GATE_TIM1,
	GATE_TIM10,
	GATE_TIM11,
	GATE_TIM12,
	GATE_TIM13,
	GATE_TIM14,
	GATE_TIM15,
	GATE_TIM16,
	GATE_TIM17,
	GATE_TIM2,
	GATE_TIM20,
	GATE_TIM3,
	GATE_TIM4,
	GATE_TIM5,
	GATE_TIM6,
	GATE_TIM7,
	GATE_TIM8,
	GATE_UART4,
	GATE_UART5,
	GATE_UART7,
	GATE_UART8,
	GATE_UART9,
	GATE_USART1,
	GATE_USART2,
	GATE_USART3,
	GATE_USART6,
	GATE_USB2,
	GATE_USB2PHY1,
	GATE_USB2PHY2,
	GATE_USB3DR,
	GATE_USB3PCIEPHY,
	GATE_USBTC,
	GATE_VDEC,
	GATE_VENC,
	GATE_VREF,
	GATE_WWDG1,
	GATE_WWDG2,
	GATE_NB
};

#define GATE_CFG(id, _offset, _bit_idx, _offset_clr)\
	[id] = {\
		.offset		= (_offset),\
		.bit_idx	= (_bit_idx),\
		.set_clr	= (_offset_clr),\
	}

static const struct stm32_gate_cfg stm32mp25_gates[GATE_NB] = {
	GATE_CFG(GATE_ADC12,		RCC_ADC12CFGR,		1,	0),
	GATE_CFG(GATE_ADC3,		RCC_ADC3CFGR,		1,	0),
	GATE_CFG(GATE_ADF1,		RCC_ADF1CFGR,		1,	0),
	GATE_CFG(GATE_CCI,		RCC_CCICFGR,		1,	0),
	GATE_CFG(GATE_CRC,		RCC_CRCCFGR,		1,	0),
	GATE_CFG(GATE_CRYP1,		RCC_CRYP1CFGR,		1,	0),
	GATE_CFG(GATE_CRYP2,		RCC_CRYP2CFGR,		1,	0),
	GATE_CFG(GATE_CSI,		RCC_CSICFGR,		1,	0),
	GATE_CFG(GATE_DCMIPP,		RCC_DCMIPPCFGR,		1,	0),
	GATE_CFG(GATE_DSI,		RCC_DSICFGR,		1,	0),
	GATE_CFG(GATE_DTS,		RCC_DTSCFGR,		1,	0),
	GATE_CFG(GATE_ETH1,		RCC_ETH1CFGR,		5,	0),
	GATE_CFG(GATE_ETH1MAC,		RCC_ETH1CFGR,		1,	0),
	GATE_CFG(GATE_ETH1RX,		RCC_ETH1CFGR,		10,	0),
	GATE_CFG(GATE_ETH1STP,		RCC_ETH1CFGR,		4,	0),
	GATE_CFG(GATE_ETH1TX,		RCC_ETH1CFGR,		8,	0),
	GATE_CFG(GATE_ETH2,		RCC_ETH2CFGR,		5,	0),
	GATE_CFG(GATE_ETH2MAC,		RCC_ETH2CFGR,		1,	0),
	GATE_CFG(GATE_ETH2RX,		RCC_ETH2CFGR,		10,	0),
	GATE_CFG(GATE_ETH2STP,		RCC_ETH2CFGR,		4,	0),
	GATE_CFG(GATE_ETH2TX,		RCC_ETH2CFGR,		8,	0),
	GATE_CFG(GATE_ETHSW,		RCC_ETHSWCFGR,		5,	0),
	GATE_CFG(GATE_ETHSWACMCFG,	RCC_ETHSWACMCFGR,	1,	0),
	GATE_CFG(GATE_ETHSWACMMSG,	RCC_ETHSWACMMSGCFGR,	1,	0),
	GATE_CFG(GATE_ETHSWMAC,		RCC_ETHSWCFGR,		1,	0),
	GATE_CFG(GATE_ETHSWREF,		RCC_ETHSWCFGR,		21,	0),
	GATE_CFG(GATE_FDCAN,		RCC_FDCANCFGR,		1,	0),
	GATE_CFG(GATE_GPU,		RCC_GPUCFGR,		1,	0),
	GATE_CFG(GATE_HASH,		RCC_HASHCFGR,		1,	0),
	GATE_CFG(GATE_HDP,		RCC_HDPCFGR,		1,	0),
	GATE_CFG(GATE_I2C1,		RCC_I2C1CFGR,		1,	0),
	GATE_CFG(GATE_I2C2,		RCC_I2C2CFGR,		1,	0),
	GATE_CFG(GATE_I2C3,		RCC_I2C3CFGR,		1,	0),
	GATE_CFG(GATE_I2C4,		RCC_I2C4CFGR,		1,	0),
	GATE_CFG(GATE_I2C5,		RCC_I2C5CFGR,		1,	0),
	GATE_CFG(GATE_I2C6,		RCC_I2C6CFGR,		1,	0),
	GATE_CFG(GATE_I2C7,		RCC_I2C7CFGR,		1,	0),
	GATE_CFG(GATE_I2C8,		RCC_I2C8CFGR,		1,	0),
	GATE_CFG(GATE_I3C1,		RCC_I3C1CFGR,		1,	0),
	GATE_CFG(GATE_I3C2,		RCC_I3C2CFGR,		1,	0),
	GATE_CFG(GATE_I3C3,		RCC_I3C3CFGR,		1,	0),
	GATE_CFG(GATE_I3C4,		RCC_I3C4CFGR,		1,	0),
	GATE_CFG(GATE_IS2M,		RCC_IS2MCFGR,		1,	0),
	GATE_CFG(GATE_IWDG1,		RCC_IWDG1CFGR,		1,	0),
	GATE_CFG(GATE_IWDG2,		RCC_IWDG2CFGR,		1,	0),
	GATE_CFG(GATE_IWDG3,		RCC_IWDG3CFGR,		1,	0),
	GATE_CFG(GATE_IWDG4,		RCC_IWDG4CFGR,		1,	0),
	GATE_CFG(GATE_IWDG5,		RCC_IWDG5CFGR,		1,	0),
	GATE_CFG(GATE_LPTIM1,		RCC_LPTIM1CFGR,		1,	0),
	GATE_CFG(GATE_LPTIM2,		RCC_LPTIM2CFGR,		1,	0),
	GATE_CFG(GATE_LPTIM3,		RCC_LPTIM3CFGR,		1,	0),
	GATE_CFG(GATE_LPTIM4,		RCC_LPTIM4CFGR,		1,	0),
	GATE_CFG(GATE_LPTIM5,		RCC_LPTIM5CFGR,		1,	0),
	GATE_CFG(GATE_LPUART1,		RCC_LPUART1CFGR,	1,	0),
	GATE_CFG(GATE_LTDC,		RCC_LTDCCFGR,		1,	0),
	GATE_CFG(GATE_LVDS,		RCC_LVDSCFGR,		1,	0),
	GATE_CFG(GATE_MCO1,		RCC_MCO1CFGR,		8,	0),
	GATE_CFG(GATE_MCO2,		RCC_MCO2CFGR,		8,	0),
	GATE_CFG(GATE_MDF1,		RCC_MDF1CFGR,		1,	0),
	GATE_CFG(GATE_OSPIIOM,		RCC_OSPIIOMCFGR,	1,	0),
	GATE_CFG(GATE_PCIE,		RCC_PCIECFGR,		1,	0),
	GATE_CFG(GATE_PKA,		RCC_PKACFGR,		1,	0),
	GATE_CFG(GATE_RNG,		RCC_RNGCFGR,		1,	0),
	GATE_CFG(GATE_SAES,		RCC_SAESCFGR,		1,	0),
	GATE_CFG(GATE_SAI1,		RCC_SAI1CFGR,		1,	0),
	GATE_CFG(GATE_SAI2,		RCC_SAI2CFGR,		1,	0),
	GATE_CFG(GATE_SAI3,		RCC_SAI3CFGR,		1,	0),
	GATE_CFG(GATE_SAI4,		RCC_SAI4CFGR,		1,	0),
	GATE_CFG(GATE_SDMMC1,		RCC_SDMMC1CFGR,		1,	0),
	GATE_CFG(GATE_SDMMC2,		RCC_SDMMC2CFGR,		1,	0),
	GATE_CFG(GATE_SDMMC3,		RCC_SDMMC3CFGR,		1,	0),
	GATE_CFG(GATE_SERC,		RCC_SERCCFGR,		1,	0),
	GATE_CFG(GATE_SPDIFRX,		RCC_SPDIFRXCFGR,	1,	0),
	GATE_CFG(GATE_SPI1,		RCC_SPI1CFGR,		1,	0),
	GATE_CFG(GATE_SPI2,		RCC_SPI2CFGR,		1,	0),
	GATE_CFG(GATE_SPI3,		RCC_SPI3CFGR,		1,	0),
	GATE_CFG(GATE_SPI4,		RCC_SPI4CFGR,		1,	0),
	GATE_CFG(GATE_SPI5,		RCC_SPI5CFGR,		1,	0),
	GATE_CFG(GATE_SPI6,		RCC_SPI6CFGR,		1,	0),
	GATE_CFG(GATE_SPI7,		RCC_SPI7CFGR,		1,	0),
	GATE_CFG(GATE_SPI8,		RCC_SPI8CFGR,		1,	0),
	GATE_CFG(GATE_TIM1,		RCC_TIM1CFGR,		1,	0),
	GATE_CFG(GATE_TIM10,		RCC_TIM10CFGR,		1,	0),
	GATE_CFG(GATE_TIM11,		RCC_TIM11CFGR,		1,	0),
	GATE_CFG(GATE_TIM12,		RCC_TIM12CFGR,		1,	0),
	GATE_CFG(GATE_TIM13,		RCC_TIM13CFGR,		1,	0),
	GATE_CFG(GATE_TIM14,		RCC_TIM14CFGR,		1,	0),
	GATE_CFG(GATE_TIM15,		RCC_TIM15CFGR,		1,	0),
	GATE_CFG(GATE_TIM16,		RCC_TIM16CFGR,		1,	0),
	GATE_CFG(GATE_TIM17,		RCC_TIM17CFGR,		1,	0),
	GATE_CFG(GATE_TIM2,		RCC_TIM2CFGR,		1,	0),
	GATE_CFG(GATE_TIM20,		RCC_TIM20CFGR,		1,	0),
	GATE_CFG(GATE_TIM3,		RCC_TIM3CFGR,		1,	0),
	GATE_CFG(GATE_TIM4,		RCC_TIM4CFGR,		1,	0),
	GATE_CFG(GATE_TIM5,		RCC_TIM5CFGR,		1,	0),
	GATE_CFG(GATE_TIM6,		RCC_TIM6CFGR,		1,	0),
	GATE_CFG(GATE_TIM7,		RCC_TIM7CFGR,		1,	0),
	GATE_CFG(GATE_TIM8,		RCC_TIM8CFGR,		1,	0),
	GATE_CFG(GATE_UART4,		RCC_UART4CFGR,		1,	0),
	GATE_CFG(GATE_UART5,		RCC_UART5CFGR,		1,	0),
	GATE_CFG(GATE_UART7,		RCC_UART7CFGR,		1,	0),
	GATE_CFG(GATE_UART8,		RCC_UART8CFGR,		1,	0),
	GATE_CFG(GATE_UART9,		RCC_UART9CFGR,		1,	0),
	GATE_CFG(GATE_USART1,		RCC_USART1CFGR,		1,	0),
	GATE_CFG(GATE_USART2,		RCC_USART2CFGR,		1,	0),
	GATE_CFG(GATE_USART3,		RCC_USART3CFGR,		1,	0),
	GATE_CFG(GATE_USART6,		RCC_USART6CFGR,		1,	0),
	GATE_CFG(GATE_USB2,		RCC_USB2CFGR,		1,	0),
	GATE_CFG(GATE_USB2PHY1,		RCC_USB2PHY1CFGR,	1,	0),
	GATE_CFG(GATE_USB2PHY2,		RCC_USB2PHY2CFGR,	1,	0),
	GATE_CFG(GATE_USB3DR,		RCC_USB3DRCFGR,		1,	0),
	GATE_CFG(GATE_USB3PCIEPHY,	RCC_USB3PCIEPHYCFGR,	1,	0),
	GATE_CFG(GATE_USBTC,		RCC_USBTCCFGR,		1,	0),
	GATE_CFG(GATE_VDEC,		RCC_VDECCFGR,		1,	0),
	GATE_CFG(GATE_VENC,		RCC_VENCCFGR,		1,	0),
	GATE_CFG(GATE_VREF,		RCC_VREFCFGR,		1,	0),
	GATE_CFG(GATE_WWDG1,		RCC_WWDG1CFGR,		1,	0),
	GATE_CFG(GATE_WWDG2,		RCC_WWDG2CFGR,		1,	0),
};

#define CLK_STM32_GATE(_name, _parent, _flags, _gate_id)\
struct clk_stm32_gate _name = {\
	.gate_id = _gate_id,\
	.hw.init = CLK_HW_INIT(#_name, _parent, &clk_stm32_gate_ops, _flags),\
}

#define CLK_STM32_MUX(_name, _parents, _flags, _mux_id)\
struct clk_stm32_mux _name = {\
	.mux_id = _mux_id,\
	.hw.init = CLK_HW_INIT_PARENTS(#_name, _parents, &clk_stm32_mux_ops, _flags),\
}

#define CLK_STM32_DIV(_name, _parent, _flags, _div_id)\
struct clk_stm32_div _name = {\
	.div_id = _div_id,\
	.hw.init = CLK_HW_INIT(#_name, _parent, &clk_stm32_divider_ops, _flags),\
}

#define CLK_STM32_COMPOSITE(_name, _parents, _flags, _gate_id, _mux_id, _div_id)\
struct clk_stm32_composite _name = {\
	.gate_id = _gate_id,\
	.mux_id = _mux_id,\
	.div_id = _div_id,\
	.hw.init = CLK_HW_INIT_PARENTS(#_name, _parents, &clk_stm32_composite_ops, _flags),\
}

/* ADC */
static CLK_STM32_GATE(ck_icn_p_adc12, "ck_icn_ls_mcu", 0, GATE_ADC12);
static CLK_STM32_COMPOSITE(ck_ker_adc12, adc12_src, 0, GATE_ADC12, MUX_ADC12, NO_STM32_DIV);
static CLK_STM32_GATE(ck_icn_p_adc3, "ck_icn_ls_mcu", 0, GATE_ADC3);
static CLK_STM32_COMPOSITE(ck_ker_adc3, adc3_src, 0, GATE_ADC3, MUX_ADC3, NO_STM32_DIV);

/* ADF */
static CLK_STM32_GATE(ck_icn_p_adf1, "ck_icn_ls_mcu", 0, GATE_ADF1);
static CLK_STM32_GATE(ck_ker_adf1, "ck_flexgen_42", 0, GATE_ADF1);

/* DCMI */
static CLK_STM32_GATE(ck_icn_p_cci, "ck_icn_ls_mcu", 0, GATE_CCI);

/* CSI-HOST */
static CLK_STM32_GATE(ck_icn_p_csi, "ck_icn_apb4", 0, GATE_CSI);
static CLK_STM32_GATE(ck_ker_csi, "ck_flexgen_29", 0, GATE_CSI);
static CLK_STM32_GATE(ck_ker_csitxesc, "ck_flexgen_30", 0, GATE_CSI);

/* CSI-PHY */
static CLK_STM32_GATE(ck_ker_csiphy, "ck_flexgen_31", 0, GATE_CSI);

/* DCMIPP */
static CLK_STM32_GATE(ck_icn_p_dcmipp, "ck_icn_apb4", 0, GATE_DCMIPP);

/* CRC */
static CLK_STM32_GATE(ck_icn_p_crc, "ck_icn_ls_mcu", 0, GATE_CRC);

/* CRYP */
static CLK_STM32_GATE(ck_icn_p_cryp1, "ck_icn_ls_mcu", 0, GATE_CRYP1);
static CLK_STM32_GATE(ck_icn_p_cryp2, "ck_icn_ls_mcu", 0, GATE_CRYP2);

/* DBG & TRACE*/
/* Trace and debug clocks are managed by SCMI */

/* LTDC */
static CLK_STM32_GATE(ck_icn_p_ltdc, "ck_icn_apb4", 0, GATE_LTDC);
static CLK_STM32_GATE(ck_ker_ltdc, "ck_flexgen_27", CLK_SET_RATE_PARENT, GATE_LTDC);

/* DSI */
static CLK_STM32_GATE(ck_icn_p_dsi, "ck_icn_apb4", 0, GATE_DSI);
static CLK_STM32_COMPOSITE(clk_lanebyte, dsiblane_src, 0, GATE_DSI, MUX_DSIBLANE, NO_STM32_DIV);

/* LVDS */
static CLK_STM32_GATE(ck_icn_p_lvds, "ck_icn_apb4", 0, GATE_LVDS);

/* DSI PHY */
static CLK_STM32_COMPOSITE(clk_phy_dsi, dsiphy_src, 0, GATE_DSI, MUX_DSIPHY, NO_STM32_DIV);

/* LVDS PHY */
static CLK_STM32_COMPOSITE(ck_ker_lvdsphy, lvdsphy_src, 0, GATE_LVDS, MUX_LVDSPHY, NO_STM32_DIV);

/* DTS */
static CLK_STM32_COMPOSITE(ck_ker_dts, dts_src, 0, GATE_DTS, MUX_DTS, NO_STM32_DIV);

/* ETHERNET */
static CLK_STM32_GATE(ck_icn_p_eth1, "ck_icn_ls_mcu", 0, GATE_ETH1);
static CLK_STM32_GATE(ck_ker_eth1stp, "ck_icn_ls_mcu", 0, GATE_ETH1STP);
static CLK_STM32_GATE(ck_ker_eth1, "ck_flexgen_54", 0, GATE_ETH1);
static CLK_STM32_GATE(ck_ker_eth1ptp, "ck_flexgen_56", 0, GATE_ETH1);
static CLK_STM32_GATE(ck_ker_eth1mac, "ck_icn_ls_mcu", 0, GATE_ETH1MAC);
static CLK_STM32_GATE(ck_ker_eth1tx, "ck_icn_ls_mcu", 0, GATE_ETH1TX);
static CLK_STM32_GATE(ck_ker_eth1rx, "ck_icn_ls_mcu", 0, GATE_ETH1RX);

static CLK_STM32_GATE(ck_icn_p_eth2, "ck_icn_ls_mcu", 0, GATE_ETH2);
static CLK_STM32_GATE(ck_ker_eth2stp, "ck_icn_ls_mcu", 0, GATE_ETH2STP);
static CLK_STM32_GATE(ck_ker_eth2, "ck_flexgen_55", 0, GATE_ETH2);
static CLK_STM32_GATE(ck_ker_eth2ptp, "ck_flexgen_56", 0, GATE_ETH2);
static CLK_STM32_GATE(ck_ker_eth2mac, "ck_icn_ls_mcu", 0, GATE_ETH2MAC);
static CLK_STM32_GATE(ck_ker_eth2tx, "ck_icn_ls_mcu", 0, GATE_ETH2TX);
static CLK_STM32_GATE(ck_ker_eth2rx, "ck_icn_ls_mcu", 0, GATE_ETH2RX);

static CLK_STM32_GATE(ck_icn_p_ethsw, "ck_icn_ls_mcu", 0, GATE_ETHSWMAC);
static CLK_STM32_GATE(ck_ker_ethsw, "ck_flexgen_54", 0, GATE_ETHSW);
static CLK_STM32_GATE(ck_ker_ethswref, "ck_flexgen_60", 0, GATE_ETHSWREF);

static CLK_STM32_GATE(ck_icn_p_ethsw_acm_cfg, "ck_icn_ls_mcu", 0, GATE_ETHSWACMCFG);
static CLK_STM32_GATE(ck_icn_p_ethsw_acm_msg, "ck_icn_ls_mcu", 0, GATE_ETHSWACMMSG);

/* FDCAN */
static CLK_STM32_GATE(ck_icn_p_fdcan, "ck_icn_apb2", 0, GATE_FDCAN);
static CLK_STM32_GATE(ck_ker_fdcan, "ck_flexgen_26", 0, GATE_FDCAN);

/* GPU */
static CLK_STM32_GATE(ck_icn_m_gpu, "ck_flexgen_59", 0, GATE_GPU);
static CLK_STM32_GATE(ck_ker_gpu, "ck_pll3", 0, GATE_GPU);

/* HASH */
static CLK_STM32_GATE(ck_icn_p_hash, "ck_icn_ls_mcu", 0, GATE_HASH);

/* HDP */
static CLK_STM32_GATE(ck_icn_p_hdp, "ck_icn_apb3", 0, GATE_HDP);

/* I2C */
static CLK_STM32_GATE(ck_icn_p_i2c8, "ck_icn_ls_mcu", 0, GATE_I2C8);
static CLK_STM32_GATE(ck_icn_p_i2c1, "ck_icn_apb1", 0, GATE_I2C1);
static CLK_STM32_GATE(ck_icn_p_i2c2, "ck_icn_apb1", 0, GATE_I2C2);
static CLK_STM32_GATE(ck_icn_p_i2c3, "ck_icn_apb1", 0, GATE_I2C3);
static CLK_STM32_GATE(ck_icn_p_i2c4, "ck_icn_apb1", 0, GATE_I2C4);
static CLK_STM32_GATE(ck_icn_p_i2c5, "ck_icn_apb1", 0, GATE_I2C5);
static CLK_STM32_GATE(ck_icn_p_i2c6, "ck_icn_apb1", 0, GATE_I2C6);
static CLK_STM32_GATE(ck_icn_p_i2c7, "ck_icn_apb1", 0, GATE_I2C7);

static CLK_STM32_GATE(ck_ker_i2c1, "ck_flexgen_12", 0, GATE_I2C1);
static CLK_STM32_GATE(ck_ker_i2c2, "ck_flexgen_12", 0, GATE_I2C2);
static CLK_STM32_GATE(ck_ker_i2c3, "ck_flexgen_13", 0, GATE_I2C3);
static CLK_STM32_GATE(ck_ker_i2c5, "ck_flexgen_13", 0, GATE_I2C5);
static CLK_STM32_GATE(ck_ker_i2c4, "ck_flexgen_14", 0, GATE_I2C4);
static CLK_STM32_GATE(ck_ker_i2c6, "ck_flexgen_14", 0, GATE_I2C6);
static CLK_STM32_GATE(ck_ker_i2c7, "ck_flexgen_15", 0, GATE_I2C7);
static CLK_STM32_GATE(ck_ker_i2c8, "ck_flexgen_38", 0, GATE_I2C8);

/* I3C */
static CLK_STM32_GATE(ck_icn_p_i3c1, "ck_icn_apb1", 0, GATE_I3C1);
static CLK_STM32_GATE(ck_icn_p_i3c2, "ck_icn_apb1", 0, GATE_I3C2);
static CLK_STM32_GATE(ck_icn_p_i3c3, "ck_icn_apb1", 0, GATE_I3C3);
static CLK_STM32_GATE(ck_icn_p_i3c4, "ck_icn_ls_mcu", 0, GATE_I3C4);

static CLK_STM32_GATE(ck_ker_i3c1, "ck_flexgen_12", 0, GATE_I3C1);
static CLK_STM32_GATE(ck_ker_i3c2, "ck_flexgen_12", 0, GATE_I3C2);
static CLK_STM32_GATE(ck_ker_i3c3, "ck_flexgen_13", 0, GATE_I3C3);
static CLK_STM32_GATE(ck_ker_i3c4, "ck_flexgen_36", 0, GATE_I3C4);

/* I2S */
static CLK_STM32_GATE(ck_icn_p_is2m, "ck_icn_apb3", 0, GATE_IS2M);

/* IWDG */
static CLK_STM32_GATE(ck_icn_p_iwdg2, "ck_icn_apb3", 0, GATE_IWDG2);
static CLK_STM32_GATE(ck_icn_p_iwdg3, "ck_icn_apb3", 0, GATE_IWDG3);
static CLK_STM32_GATE(ck_icn_p_iwdg4, "ck_icn_apb3", 0, GATE_IWDG4);
static CLK_STM32_GATE(ck_icn_p_iwdg5, "ck_icn_ls_mcu", 0, GATE_IWDG5);

/* LPTIM */
static CLK_STM32_GATE(ck_icn_p_lptim1, "ck_icn_apb1", 0, GATE_LPTIM1);
static CLK_STM32_GATE(ck_icn_p_lptim2, "ck_icn_apb1", 0, GATE_LPTIM2);
static CLK_STM32_GATE(ck_icn_p_lptim3, "ck_icn_ls_mcu", 0, GATE_LPTIM3);
static CLK_STM32_GATE(ck_icn_p_lptim4, "ck_icn_ls_mcu", 0, GATE_LPTIM4);
static CLK_STM32_GATE(ck_icn_p_lptim5, "ck_icn_ls_mcu", 0, GATE_LPTIM5);

static CLK_STM32_GATE(ck_ker_lptim1, "ck_flexgen_07", 0, GATE_LPTIM1);
static CLK_STM32_GATE(ck_ker_lptim2, "ck_flexgen_07", 0, GATE_LPTIM2);
static CLK_STM32_GATE(ck_ker_lptim3, "ck_flexgen_40", 0, GATE_LPTIM3);
static CLK_STM32_GATE(ck_ker_lptim4, "ck_flexgen_41", 0, GATE_LPTIM4);
static CLK_STM32_GATE(ck_ker_lptim5, "ck_flexgen_41", 0, GATE_LPTIM5);

/* LPUART */
static CLK_STM32_GATE(ck_icn_p_lpuart1, "ck_icn_ls_mcu", 0, GATE_LPUART1);
static CLK_STM32_GATE(ck_ker_lpuart1, "ck_flexgen_39", 0, GATE_LPUART1);

/* MCO1 & MCO2 */
static CLK_STM32_COMPOSITE(ck_mco1, mco1_src, 0, GATE_MCO1, MUX_MCO1, NO_STM32_DIV);
static CLK_STM32_COMPOSITE(ck_mco2, mco2_src, 0, GATE_MCO2, MUX_MCO2, NO_STM32_DIV);

/* MDF */
static CLK_STM32_GATE(ck_icn_p_mdf1, "ck_icn_ls_mcu", 0, GATE_MDF1);
static CLK_STM32_GATE(ck_ker_mdf1, "ck_flexgen_23", 0, GATE_MDF1);

/* OSPI */
static CLK_STM32_GATE(ck_icn_p_ospiiom, "ck_icn_ls_mcu", 0, GATE_OSPIIOM);

/* PCIE */
static CLK_STM32_GATE(ck_icn_p_pcie, "ck_icn_ls_mcu", 0, GATE_PCIE);

/* SAI */
static CLK_STM32_GATE(ck_icn_p_sai1, "ck_icn_apb2", 0, GATE_SAI1);
static CLK_STM32_GATE(ck_icn_p_sai2, "ck_icn_apb2", 0, GATE_SAI2);
static CLK_STM32_GATE(ck_icn_p_sai3, "ck_icn_apb2", 0, GATE_SAI3);
static CLK_STM32_GATE(ck_icn_p_sai4, "ck_icn_apb2", 0, GATE_SAI4);
static CLK_STM32_GATE(ck_ker_sai1, "ck_flexgen_23", CLK_SET_RATE_PARENT, GATE_SAI1);
static CLK_STM32_GATE(ck_ker_sai2, "ck_flexgen_24", CLK_SET_RATE_PARENT, GATE_SAI2);
static CLK_STM32_GATE(ck_ker_sai3, "ck_flexgen_25", CLK_SET_RATE_PARENT, GATE_SAI3);
static CLK_STM32_GATE(ck_ker_sai4, "ck_flexgen_25", CLK_SET_RATE_PARENT, GATE_SAI4);

/* SDMMC */
static CLK_STM32_GATE(ck_icn_m_sdmmc1, "ck_icn_sdmmc", 0, GATE_SDMMC1);
static CLK_STM32_GATE(ck_icn_m_sdmmc2, "ck_icn_sdmmc", 0, GATE_SDMMC2);
static CLK_STM32_GATE(ck_icn_m_sdmmc3, "ck_icn_sdmmc", 0, GATE_SDMMC3);
static CLK_STM32_GATE(ck_ker_sdmmc1, "ck_flexgen_51", 0, GATE_SDMMC1);
static CLK_STM32_GATE(ck_ker_sdmmc2, "ck_flexgen_52", 0, GATE_SDMMC2);
static CLK_STM32_GATE(ck_ker_sdmmc3, "ck_flexgen_53", 0, GATE_SDMMC3);

/* SERC */
static CLK_STM32_GATE(ck_icn_p_serc, "ck_icn_apb3", 0, GATE_SERC);

/* SPDIF */
static CLK_STM32_GATE(ck_icn_p_spdifrx, "ck_icn_apb1", 0, GATE_SPDIFRX);
static CLK_STM32_GATE(ck_ker_spdifrx, "ck_flexgen_11", 0, GATE_SPDIFRX);

/* SPI */
static CLK_STM32_GATE(ck_icn_p_spi1, "ck_icn_apb2", 0, GATE_SPI1);
static CLK_STM32_GATE(ck_icn_p_spi2, "ck_icn_apb1", 0, GATE_SPI2);
static CLK_STM32_GATE(ck_icn_p_spi3, "ck_icn_apb1", 0, GATE_SPI3);
static CLK_STM32_GATE(ck_icn_p_spi4, "ck_icn_apb2", 0, GATE_SPI4);
static CLK_STM32_GATE(ck_icn_p_spi5, "ck_icn_apb2", 0, GATE_SPI5);
static CLK_STM32_GATE(ck_icn_p_spi6, "ck_icn_apb2", 0, GATE_SPI6);
static CLK_STM32_GATE(ck_icn_p_spi7, "ck_icn_apb2", 0, GATE_SPI7);
static CLK_STM32_GATE(ck_icn_p_spi8, "ck_icn_ls_mcu", 0, GATE_SPI8);

static CLK_STM32_GATE(ck_ker_spi1, "ck_flexgen_16", CLK_SET_RATE_PARENT, GATE_SPI1);
static CLK_STM32_GATE(ck_ker_spi2, "ck_flexgen_10", CLK_SET_RATE_PARENT, GATE_SPI2);
static CLK_STM32_GATE(ck_ker_spi3, "ck_flexgen_10", CLK_SET_RATE_PARENT, GATE_SPI3);
static CLK_STM32_GATE(ck_ker_spi4, "ck_flexgen_17", 0, GATE_SPI4);
static CLK_STM32_GATE(ck_ker_spi5, "ck_flexgen_17", 0, GATE_SPI5);
static CLK_STM32_GATE(ck_ker_spi6, "ck_flexgen_18", 0, GATE_SPI6);
static CLK_STM32_GATE(ck_ker_spi7, "ck_flexgen_18", 0, GATE_SPI7);
static CLK_STM32_GATE(ck_ker_spi8, "ck_flexgen_37", 0, GATE_SPI8);

/* Timers */
static CLK_STM32_GATE(ck_icn_p_tim2, "ck_icn_apb1", 0, GATE_TIM2);
static CLK_STM32_GATE(ck_icn_p_tim3, "ck_icn_apb1", 0, GATE_TIM3);
static CLK_STM32_GATE(ck_icn_p_tim4, "ck_icn_apb1", 0, GATE_TIM4);
static CLK_STM32_GATE(ck_icn_p_tim5, "ck_icn_apb1", 0, GATE_TIM5);
static CLK_STM32_GATE(ck_icn_p_tim6, "ck_icn_apb1", 0, GATE_TIM6);
static CLK_STM32_GATE(ck_icn_p_tim7, "ck_icn_apb1", 0, GATE_TIM7);
static CLK_STM32_GATE(ck_icn_p_tim10, "ck_icn_apb1", 0, GATE_TIM10);
static CLK_STM32_GATE(ck_icn_p_tim11, "ck_icn_apb1", 0, GATE_TIM11);
static CLK_STM32_GATE(ck_icn_p_tim12, "ck_icn_apb1", 0, GATE_TIM12);
static CLK_STM32_GATE(ck_icn_p_tim13, "ck_icn_apb1", 0, GATE_TIM13);
static CLK_STM32_GATE(ck_icn_p_tim14, "ck_icn_apb1", 0, GATE_TIM14);

static CLK_STM32_GATE(ck_icn_p_tim1, "ck_icn_apb2", 0, GATE_TIM1);
static CLK_STM32_GATE(ck_icn_p_tim8, "ck_icn_apb2", 0, GATE_TIM8);
static CLK_STM32_GATE(ck_icn_p_tim15, "ck_icn_apb2", 0, GATE_TIM15);
static CLK_STM32_GATE(ck_icn_p_tim16, "ck_icn_apb2", 0, GATE_TIM16);
static CLK_STM32_GATE(ck_icn_p_tim17, "ck_icn_apb2", 0, GATE_TIM17);
static CLK_STM32_GATE(ck_icn_p_tim20, "ck_icn_apb2", 0, GATE_TIM20);

static CLK_STM32_GATE(ck_ker_tim2, "timg1_ck", 0, GATE_TIM2);
static CLK_STM32_GATE(ck_ker_tim3, "timg1_ck", 0, GATE_TIM3);
static CLK_STM32_GATE(ck_ker_tim4, "timg1_ck", 0, GATE_TIM4);
static CLK_STM32_GATE(ck_ker_tim5, "timg1_ck", 0, GATE_TIM5);
static CLK_STM32_GATE(ck_ker_tim6, "timg1_ck", 0, GATE_TIM6);
static CLK_STM32_GATE(ck_ker_tim7, "timg1_ck", 0, GATE_TIM7);
static CLK_STM32_GATE(ck_ker_tim10, "timg1_ck", 0, GATE_TIM10);
static CLK_STM32_GATE(ck_ker_tim11, "timg1_ck", 0, GATE_TIM11);
static CLK_STM32_GATE(ck_ker_tim12, "timg1_ck", 0, GATE_TIM12);
static CLK_STM32_GATE(ck_ker_tim13, "timg1_ck", 0, GATE_TIM13);
static CLK_STM32_GATE(ck_ker_tim14, "timg1_ck", 0, GATE_TIM14);

static CLK_STM32_GATE(ck_ker_tim1, "timg2_ck", 0, GATE_TIM1);
static CLK_STM32_GATE(ck_ker_tim8, "timg2_ck", 0, GATE_TIM8);
static CLK_STM32_GATE(ck_ker_tim15, "timg2_ck", 0, GATE_TIM15);
static CLK_STM32_GATE(ck_ker_tim16, "timg2_ck", 0, GATE_TIM16);
static CLK_STM32_GATE(ck_ker_tim17, "timg2_ck", 0, GATE_TIM17);
static CLK_STM32_GATE(ck_ker_tim20, "timg2_ck", 0, GATE_TIM20);

/* UART/USART */
static CLK_STM32_GATE(ck_icn_p_usart2, "ck_icn_apb1", 0, GATE_USART2);
static CLK_STM32_GATE(ck_icn_p_usart3, "ck_icn_apb1", 0, GATE_USART3);
static CLK_STM32_GATE(ck_icn_p_uart4, "ck_icn_apb1", 0, GATE_UART4);
static CLK_STM32_GATE(ck_icn_p_uart5, "ck_icn_apb1", 0, GATE_UART5);
static CLK_STM32_GATE(ck_icn_p_usart1, "ck_icn_apb2", 0, GATE_USART1);
static CLK_STM32_GATE(ck_icn_p_usart6, "ck_icn_apb2", 0, GATE_USART6);
static CLK_STM32_GATE(ck_icn_p_uart7, "ck_icn_apb2", 0, GATE_UART7);
static CLK_STM32_GATE(ck_icn_p_uart8, "ck_icn_apb2", 0, GATE_UART8);
static CLK_STM32_GATE(ck_icn_p_uart9, "ck_icn_apb2", 0, GATE_UART9);

static CLK_STM32_GATE(ck_ker_usart2, "ck_flexgen_08", 0, GATE_USART2);
static CLK_STM32_GATE(ck_ker_uart4, "ck_flexgen_08", 0, GATE_UART4);
static CLK_STM32_GATE(ck_ker_usart3, "ck_flexgen_09", 0, GATE_USART3);
static CLK_STM32_GATE(ck_ker_uart5, "ck_flexgen_09", 0, GATE_UART5);
static CLK_STM32_GATE(ck_ker_usart1, "ck_flexgen_19", 0, GATE_USART1);
static CLK_STM32_GATE(ck_ker_usart6, "ck_flexgen_20", 0, GATE_USART6);
static CLK_STM32_GATE(ck_ker_uart7, "ck_flexgen_21", 0, GATE_UART7);
static CLK_STM32_GATE(ck_ker_uart8, "ck_flexgen_21", 0, GATE_UART8);
static CLK_STM32_GATE(ck_ker_uart9, "ck_flexgen_22", 0, GATE_UART9);

/* USB2PHY1 */
static CLK_STM32_COMPOSITE(ck_ker_usb2phy1, usb2phy1_src, 0,
			   GATE_USB2PHY1, MUX_USB2PHY1, NO_STM32_DIV);

/* USB2H */
static CLK_STM32_GATE(ck_icn_m_usb2ehci, "ck_icn_hsl", 0, GATE_USB2);
static CLK_STM32_GATE(ck_icn_m_usb2ohci, "ck_icn_hsl", 0, GATE_USB2);

/* USB2PHY2 */
static CLK_STM32_COMPOSITE(ck_ker_usb2phy2_en, usb2phy2_src, 0,
			   GATE_USB2PHY2, MUX_USB2PHY2, NO_STM32_DIV);

/* USB3 PCIe COMBOPHY */
static CLK_STM32_GATE(ck_icn_p_usb3pciephy, "ck_icn_apb4", 0, GATE_USB3PCIEPHY);

static CLK_STM32_COMPOSITE(ck_ker_usb3pciephy, usb3pciphy_src, 0,
			   GATE_USB3PCIEPHY, MUX_USB3PCIEPHY, NO_STM32_DIV);

/* USB3 DRD */
static CLK_STM32_GATE(ck_icn_m_usb3dr, "ck_icn_hsl", 0, GATE_USB3DR);
static CLK_STM32_GATE(ck_ker_usb2phy2, "ck_flexgen_58", 0, GATE_USB3DR);

/* USBTC */
static CLK_STM32_GATE(ck_icn_p_usbtc, "ck_icn_apb4", 0, GATE_USBTC);
static CLK_STM32_GATE(ck_ker_usbtc, "ck_flexgen_35", 0, GATE_USBTC);

/* VDEC / VENC */
static CLK_STM32_GATE(ck_icn_p_vdec, "ck_icn_apb4", 0, GATE_VDEC);
static CLK_STM32_GATE(ck_icn_p_venc, "ck_icn_apb4", 0, GATE_VENC);

/* VREF */
static CLK_STM32_GATE(ck_icn_p_vref, "ck_icn_apb3", 0, GATE_VREF);

/* WWDG */
static CLK_STM32_GATE(ck_icn_p_wwdg1, "ck_icn_apb3", 0, GATE_WWDG1);
static CLK_STM32_GATE(ck_icn_p_wwdg2, "ck_icn_ls_mcu", 0, GATE_WWDG2);

enum security_clk {
	SECF_NONE,
};

static const struct clock_config stm32mp25_clock_cfg[] = {
	STM32_GATE_CFG(CK_BUS_ETH1,		ck_icn_p_eth1,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_ETH2,		ck_icn_p_eth2,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_PCIE,		ck_icn_p_pcie,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_ETHSW,		ck_icn_p_ethsw,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_ADC12,		ck_icn_p_adc12,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_ADC3,		ck_icn_p_adc3,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_CCI,		ck_icn_p_cci,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_CRC,		ck_icn_p_crc,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_MDF1,		ck_icn_p_mdf1,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_OSPIIOM,		ck_icn_p_ospiiom,	SECF_NONE),
	STM32_GATE_CFG(CK_BUS_HASH,		ck_icn_p_hash,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_CRYP1,		ck_icn_p_cryp1,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_CRYP2,		ck_icn_p_cryp2,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_ADF1,		ck_icn_p_adf1,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_SPI8,		ck_icn_p_spi8,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_LPUART1,		ck_icn_p_lpuart1,	SECF_NONE),
	STM32_GATE_CFG(CK_BUS_I2C8,		ck_icn_p_i2c8,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_LPTIM3,		ck_icn_p_lptim3,	SECF_NONE),
	STM32_GATE_CFG(CK_BUS_LPTIM4,		ck_icn_p_lptim4,	SECF_NONE),
	STM32_GATE_CFG(CK_BUS_LPTIM5,		ck_icn_p_lptim5,	SECF_NONE),
	STM32_GATE_CFG(CK_BUS_IWDG5,		ck_icn_p_iwdg5,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_WWDG2,		ck_icn_p_wwdg2,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_I3C4,		ck_icn_p_i3c4,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_SDMMC1,		ck_icn_m_sdmmc1,	SECF_NONE),
	STM32_GATE_CFG(CK_BUS_SDMMC2,		ck_icn_m_sdmmc2,	SECF_NONE),
	STM32_GATE_CFG(CK_BUS_SDMMC3,		ck_icn_m_sdmmc3,	SECF_NONE),
	STM32_GATE_CFG(CK_BUS_USB2OHCI,		ck_icn_m_usb2ohci,	SECF_NONE),
	STM32_GATE_CFG(CK_BUS_USB2EHCI,		ck_icn_m_usb2ehci,	SECF_NONE),
	STM32_GATE_CFG(CK_BUS_USB3DR,		ck_icn_m_usb3dr,	SECF_NONE),
	STM32_GATE_CFG(CK_BUS_TIM2,		ck_icn_p_tim2,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_TIM3,		ck_icn_p_tim3,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_TIM4,		ck_icn_p_tim4,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_TIM5,		ck_icn_p_tim5,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_TIM6,		ck_icn_p_tim6,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_TIM7,		ck_icn_p_tim7,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_TIM10,		ck_icn_p_tim10,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_TIM11,		ck_icn_p_tim11,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_TIM12,		ck_icn_p_tim12,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_TIM13,		ck_icn_p_tim13,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_TIM14,		ck_icn_p_tim14,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_LPTIM1,		ck_icn_p_lptim1,	SECF_NONE),
	STM32_GATE_CFG(CK_BUS_LPTIM2,		ck_icn_p_lptim2,	SECF_NONE),
	STM32_GATE_CFG(CK_BUS_SPI2,		ck_icn_p_spi2,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_SPI3,		ck_icn_p_spi3,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_SPDIFRX,		ck_icn_p_spdifrx,	SECF_NONE),
	STM32_GATE_CFG(CK_BUS_USART2,		ck_icn_p_usart2,	SECF_NONE),
	STM32_GATE_CFG(CK_BUS_USART3,		ck_icn_p_usart3,	SECF_NONE),
	STM32_GATE_CFG(CK_BUS_UART4,		ck_icn_p_uart4,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_UART5,		ck_icn_p_uart5,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_I2C1,		ck_icn_p_i2c1,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_I2C2,		ck_icn_p_i2c2,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_I2C3,		ck_icn_p_i2c3,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_I2C4,		ck_icn_p_i2c4,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_I2C5,		ck_icn_p_i2c5,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_I2C6,		ck_icn_p_i2c6,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_I2C7,		ck_icn_p_i2c7,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_I3C1,		ck_icn_p_i3c1,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_I3C2,		ck_icn_p_i3c2,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_I3C3,		ck_icn_p_i3c3,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_TIM1,		ck_icn_p_tim1,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_TIM8,		ck_icn_p_tim8,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_TIM15,		ck_icn_p_tim15,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_TIM16,		ck_icn_p_tim16,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_TIM17,		ck_icn_p_tim17,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_TIM20,		ck_icn_p_tim20,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_SAI1,		ck_icn_p_sai1,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_SAI2,		ck_icn_p_sai2,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_SAI3,		ck_icn_p_sai3,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_SAI4,		ck_icn_p_sai4,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_USART1,		ck_icn_p_usart1,	SECF_NONE),
	STM32_GATE_CFG(CK_BUS_USART6,		ck_icn_p_usart6,	SECF_NONE),
	STM32_GATE_CFG(CK_BUS_UART7,		ck_icn_p_uart7,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_UART8,		ck_icn_p_uart8,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_UART9,		ck_icn_p_uart9,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_FDCAN,		ck_icn_p_fdcan,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_SPI1,		ck_icn_p_spi1,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_SPI4,		ck_icn_p_spi4,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_SPI5,		ck_icn_p_spi5,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_SPI6,		ck_icn_p_spi6,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_SPI7,		ck_icn_p_spi7,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_IWDG2,		ck_icn_p_iwdg2,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_IWDG3,		ck_icn_p_iwdg3,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_IWDG4,		ck_icn_p_iwdg4,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_WWDG1,		ck_icn_p_wwdg1,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_VREF,		ck_icn_p_vref,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_SERC,		ck_icn_p_serc,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_HDP,		ck_icn_p_hdp,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_IS2M,		ck_icn_p_is2m,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_DSI,		ck_icn_p_dsi,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_LTDC,		ck_icn_p_ltdc,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_CSI,		ck_icn_p_csi,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_DCMIPP,		ck_icn_p_dcmipp,	SECF_NONE),
	STM32_GATE_CFG(CK_BUS_LVDS,		ck_icn_p_lvds,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_USBTC,		ck_icn_p_usbtc,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_USB3PCIEPHY,	ck_icn_p_usb3pciephy,	SECF_NONE),
	STM32_GATE_CFG(CK_BUS_VDEC,		ck_icn_p_vdec,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_VENC,		ck_icn_p_venc,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_TIM2,		ck_ker_tim2,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_TIM3,		ck_ker_tim3,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_TIM4,		ck_ker_tim4,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_TIM5,		ck_ker_tim5,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_TIM6,		ck_ker_tim6,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_TIM7,		ck_ker_tim7,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_TIM10,		ck_ker_tim10,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_TIM11,		ck_ker_tim11,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_TIM12,		ck_ker_tim12,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_TIM13,		ck_ker_tim13,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_TIM14,		ck_ker_tim14,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_TIM1,		ck_ker_tim1,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_TIM8,		ck_ker_tim8,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_TIM15,		ck_ker_tim15,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_TIM16,		ck_ker_tim16,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_TIM17,		ck_ker_tim17,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_TIM20,		ck_ker_tim20,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_LPTIM1,		ck_ker_lptim1,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_LPTIM2,		ck_ker_lptim2,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_USART2,		ck_ker_usart2,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_UART4,		ck_ker_uart4,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_USART3,		ck_ker_usart3,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_UART5,		ck_ker_uart5,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_SPI2,		ck_ker_spi2,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_SPI3,		ck_ker_spi3,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_SPDIFRX,		ck_ker_spdifrx,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_I2C1,		ck_ker_i2c1,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_I2C2,		ck_ker_i2c2,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_I3C1,		ck_ker_i3c1,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_I3C2,		ck_ker_i3c2,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_I2C3,		ck_ker_i2c3,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_I2C5,		ck_ker_i2c5,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_I3C3,		ck_ker_i3c3,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_I2C4,		ck_ker_i2c4,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_I2C6,		ck_ker_i2c6,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_I2C7,		ck_ker_i2c7,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_SPI1,		ck_ker_spi1,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_SPI4,		ck_ker_spi4,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_SPI5,		ck_ker_spi5,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_SPI6,		ck_ker_spi6,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_SPI7,		ck_ker_spi7,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_USART1,		ck_ker_usart1,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_USART6,		ck_ker_usart6,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_UART7,		ck_ker_uart7,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_UART8,		ck_ker_uart8,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_UART9,		ck_ker_uart9,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_MDF1,		ck_ker_mdf1,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_SAI1,		ck_ker_sai1,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_SAI2,		ck_ker_sai2,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_SAI3,		ck_ker_sai3,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_SAI4,		ck_ker_sai4,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_FDCAN,		ck_ker_fdcan,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_CSI,		ck_ker_csi,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_CSITXESC,		ck_ker_csitxesc,	SECF_NONE),
	STM32_GATE_CFG(CK_KER_CSIPHY,		ck_ker_csiphy,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_USBTC,		ck_ker_usbtc,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_I3C4,		ck_ker_i3c4,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_SPI8,		ck_ker_spi8,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_I2C8,		ck_ker_i2c8,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_LPUART1,		ck_ker_lpuart1,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_LPTIM3,		ck_ker_lptim3,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_LPTIM4,		ck_ker_lptim4,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_LPTIM5,		ck_ker_lptim5,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_ADF1,		ck_ker_adf1,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_SDMMC1,		ck_ker_sdmmc1,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_SDMMC2,		ck_ker_sdmmc2,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_SDMMC3,		ck_ker_sdmmc3,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_ETH1,		ck_ker_eth1,		SECF_NONE),
	STM32_GATE_CFG(CK_ETH1_STP,		ck_ker_eth1stp,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_ETHSW,		ck_ker_ethsw,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_ETH2,		ck_ker_eth2,		SECF_NONE),
	STM32_GATE_CFG(CK_ETH2_STP,		ck_ker_eth2stp,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_ETH1PTP,		ck_ker_eth1ptp,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_ETH2PTP,		ck_ker_eth2ptp,		SECF_NONE),
	STM32_GATE_CFG(CK_BUS_GPU,		ck_icn_m_gpu,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_GPU,		ck_ker_gpu,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_ETHSWREF,		ck_ker_ethswref,	SECF_NONE),
	STM32_GATE_CFG(CK_BUS_ETHSWACMCFG,	ck_icn_p_ethsw_acm_cfg,	SECF_NONE),
	STM32_GATE_CFG(CK_BUS_ETHSWACMMSG, ck_icn_p_ethsw_acm_msg,	SECF_NONE),
	STM32_GATE_CFG(CK_ETH1_MAC,		ck_ker_eth1mac,		SECF_NONE),
	STM32_GATE_CFG(CK_ETH1_TX,		ck_ker_eth1tx,		SECF_NONE),
	STM32_GATE_CFG(CK_ETH1_RX,		ck_ker_eth1rx,		SECF_NONE),
	STM32_GATE_CFG(CK_ETH2_MAC,		ck_ker_eth2mac,		SECF_NONE),
	STM32_GATE_CFG(CK_ETH2_TX,		ck_ker_eth2tx,		SECF_NONE),
	STM32_GATE_CFG(CK_ETH2_RX,		ck_ker_eth2rx,		SECF_NONE),
	STM32_COMPOSITE_CFG(CK_MCO1,		ck_mco1,		SECF_NONE),
	STM32_COMPOSITE_CFG(CK_MCO2,		ck_mco2,		SECF_NONE),
	STM32_COMPOSITE_CFG(CK_KER_ADC12,	ck_ker_adc12,		SECF_NONE),
	STM32_COMPOSITE_CFG(CK_KER_ADC3,	ck_ker_adc3,		SECF_NONE),
	STM32_COMPOSITE_CFG(CK_KER_USB2PHY1,	ck_ker_usb2phy1,	SECF_NONE),
	STM32_GATE_CFG(CK_KER_USB2PHY2,		ck_ker_usb2phy2,	SECF_NONE),
	STM32_COMPOSITE_CFG(CK_KER_USB2PHY2EN,	ck_ker_usb2phy2_en,	SECF_NONE),
	STM32_COMPOSITE_CFG(CK_KER_USB3PCIEPHY,	ck_ker_usb3pciephy,	SECF_NONE),
	STM32_COMPOSITE_CFG(CK_KER_DSIBLANE,	clk_lanebyte,		SECF_NONE),
	STM32_COMPOSITE_CFG(CK_KER_DSIPHY,	clk_phy_dsi,		SECF_NONE),
	STM32_COMPOSITE_CFG(CK_KER_LVDSPHY,	ck_ker_lvdsphy,		SECF_NONE),
	STM32_COMPOSITE_CFG(CK_KER_DTS,		ck_ker_dts,		SECF_NONE),
	STM32_GATE_CFG(CK_KER_LTDC,		ck_ker_ltdc,		SECF_NONE),
};

#define RESET_MP25(id, _offset, _bit_idx, _set_clr)\
	[id] = &(struct stm32_reset_cfg){\
		.offset		= (_offset),\
		.bit_idx	= (_bit_idx),\
		.set_clr	= (_set_clr),\
	}

static const struct stm32_reset_cfg *stm32mp25_reset_cfg[STM32MP25_LAST_RESET] = {
	RESET_MP25(TIM1_R,		RCC_TIM1CFGR,		0,	0),
	RESET_MP25(TIM2_R,		RCC_TIM2CFGR,		0,	0),
	RESET_MP25(TIM3_R,		RCC_TIM3CFGR,		0,	0),
	RESET_MP25(TIM4_R,		RCC_TIM4CFGR,		0,	0),
	RESET_MP25(TIM5_R,		RCC_TIM5CFGR,		0,	0),
	RESET_MP25(TIM6_R,		RCC_TIM6CFGR,		0,	0),
	RESET_MP25(TIM7_R,		RCC_TIM7CFGR,		0,	0),
	RESET_MP25(TIM8_R,		RCC_TIM8CFGR,		0,	0),
	RESET_MP25(TIM10_R,		RCC_TIM10CFGR,		0,	0),
	RESET_MP25(TIM11_R,		RCC_TIM11CFGR,		0,	0),
	RESET_MP25(TIM12_R,		RCC_TIM12CFGR,		0,	0),
	RESET_MP25(TIM13_R,		RCC_TIM13CFGR,		0,	0),
	RESET_MP25(TIM14_R,		RCC_TIM14CFGR,		0,	0),
	RESET_MP25(TIM15_R,		RCC_TIM15CFGR,		0,	0),
	RESET_MP25(TIM16_R,		RCC_TIM16CFGR,		0,	0),
	RESET_MP25(TIM17_R,		RCC_TIM17CFGR,		0,	0),
	RESET_MP25(TIM20_R,		RCC_TIM20CFGR,		0,	0),
	RESET_MP25(LPTIM1_R,		RCC_LPTIM1CFGR,		0,	0),
	RESET_MP25(LPTIM2_R,		RCC_LPTIM2CFGR,		0,	0),
	RESET_MP25(LPTIM3_R,		RCC_LPTIM3CFGR,		0,	0),
	RESET_MP25(LPTIM4_R,		RCC_LPTIM4CFGR,		0,	0),
	RESET_MP25(LPTIM5_R,		RCC_LPTIM5CFGR,		0,	0),
	RESET_MP25(SPI1_R,		RCC_SPI1CFGR,		0,	0),
	RESET_MP25(SPI2_R,		RCC_SPI2CFGR,		0,	0),
	RESET_MP25(SPI3_R,		RCC_SPI3CFGR,		0,	0),
	RESET_MP25(SPI4_R,		RCC_SPI4CFGR,		0,	0),
	RESET_MP25(SPI5_R,		RCC_SPI5CFGR,		0,	0),
	RESET_MP25(SPI6_R,		RCC_SPI6CFGR,		0,	0),
	RESET_MP25(SPI7_R,		RCC_SPI7CFGR,		0,	0),
	RESET_MP25(SPI8_R,		RCC_SPI8CFGR,		0,	0),
	RESET_MP25(SPDIFRX_R,		RCC_SPDIFRXCFGR,	0,	0),
	RESET_MP25(USART1_R,		RCC_USART1CFGR,		0,	0),
	RESET_MP25(USART2_R,		RCC_USART2CFGR,		0,	0),
	RESET_MP25(USART3_R,		RCC_USART3CFGR,		0,	0),
	RESET_MP25(UART4_R,		RCC_UART4CFGR,		0,	0),
	RESET_MP25(UART5_R,		RCC_UART5CFGR,		0,	0),
	RESET_MP25(USART6_R,		RCC_USART6CFGR,		0,	0),
	RESET_MP25(UART7_R,		RCC_UART7CFGR,		0,	0),
	RESET_MP25(UART8_R,		RCC_UART8CFGR,		0,	0),
	RESET_MP25(UART9_R,		RCC_UART9CFGR,		0,	0),
	RESET_MP25(LPUART1_R,		RCC_LPUART1CFGR,	0,	0),
	RESET_MP25(IS2M_R,		RCC_IS2MCFGR,		0,	0),
	RESET_MP25(I2C1_R,		RCC_I2C1CFGR,		0,	0),
	RESET_MP25(I2C2_R,		RCC_I2C2CFGR,		0,	0),
	RESET_MP25(I2C3_R,		RCC_I2C3CFGR,		0,	0),
	RESET_MP25(I2C4_R,		RCC_I2C4CFGR,		0,	0),
	RESET_MP25(I2C5_R,		RCC_I2C5CFGR,		0,	0),
	RESET_MP25(I2C6_R,		RCC_I2C6CFGR,		0,	0),
	RESET_MP25(I2C7_R,		RCC_I2C7CFGR,		0,	0),
	RESET_MP25(I2C8_R,		RCC_I2C8CFGR,		0,	0),
	RESET_MP25(SAI1_R,		RCC_SAI1CFGR,		0,	0),
	RESET_MP25(SAI2_R,		RCC_SAI2CFGR,		0,	0),
	RESET_MP25(SAI3_R,		RCC_SAI3CFGR,		0,	0),
	RESET_MP25(SAI4_R,		RCC_SAI4CFGR,		0,	0),
	RESET_MP25(MDF1_R,		RCC_MDF1CFGR,		0,	0),
	RESET_MP25(MDF2_R,		RCC_ADF1CFGR,		0,	0),
	RESET_MP25(FDCAN_R,		RCC_FDCANCFGR,		0,	0),
	RESET_MP25(HDP_R,		RCC_HDPCFGR,		0,	0),
	RESET_MP25(ADC12_R,		RCC_ADC12CFGR,		0,	0),
	RESET_MP25(ADC3_R,		RCC_ADC3CFGR,		0,	0),
	RESET_MP25(ETH1_R,		RCC_ETH1CFGR,		0,	0),
	RESET_MP25(ETH2_R,		RCC_ETH2CFGR,		0,	0),
	RESET_MP25(USB2_R,		RCC_USB2CFGR,		0,	0),
	RESET_MP25(USB2PHY1_R,		RCC_USB2PHY1CFGR,	0,	0),
	RESET_MP25(USB2PHY2_R,		RCC_USB2PHY2CFGR,	0,	0),
	RESET_MP25(USB3DR_R,		RCC_USB3DRCFGR,		0,	0),
	RESET_MP25(USB3PCIEPHY_R,	RCC_USB3PCIEPHYCFGR,	0,	0),
	RESET_MP25(USBTC_R,		RCC_USBTCCFGR,		0,	0),
	RESET_MP25(ETHSW_R,		RCC_ETHSWCFGR,		0,	0),
	RESET_MP25(SDMMC1_R,		RCC_SDMMC1CFGR,		0,	0),
	RESET_MP25(SDMMC1DLL_R,		RCC_SDMMC1CFGR,		16,	0),
	RESET_MP25(SDMMC2_R,		RCC_SDMMC2CFGR,		0,	0),
	RESET_MP25(SDMMC2DLL_R,		RCC_SDMMC2CFGR,		16,	0),
	RESET_MP25(SDMMC3_R,		RCC_SDMMC3CFGR,		0,	0),
	RESET_MP25(SDMMC3DLL_R,		RCC_SDMMC3CFGR,		16,	0),
	RESET_MP25(GPU_R,		RCC_GPUCFGR,		0,	0),
	RESET_MP25(LTDC_R,		RCC_LTDCCFGR,		0,	0),
	RESET_MP25(DSI_R,		RCC_DSICFGR,		0,	0),
	RESET_MP25(LVDS_R,		RCC_LVDSCFGR,		0,	0),
	RESET_MP25(CSI_R,		RCC_CSICFGR,		0,	0),
	RESET_MP25(DCMIPP_R,		RCC_DCMIPPCFGR,		0,	0),
	RESET_MP25(CCI_R,		RCC_CCICFGR,		0,	0),
	RESET_MP25(VDEC_R,		RCC_VDECCFGR,		0,	0),
	RESET_MP25(VENC_R,		RCC_VENCCFGR,		0,	0),
	RESET_MP25(WWDG1_R,		RCC_WWDG1CFGR,		0,	0),
	RESET_MP25(WWDG2_R,		RCC_WWDG2CFGR,		0,	0),
	RESET_MP25(VREF_R,		RCC_VREFCFGR,		0,	0),
	RESET_MP25(DTS_R,		RCC_DTSCFGR,		0,	0),
	RESET_MP25(CRC_R,		RCC_CRCCFGR,		0,	0),
	RESET_MP25(SERC_R,		RCC_SERCCFGR,		0,	0),
	RESET_MP25(OSPIIOM_R,		RCC_OSPIIOMCFGR,	0,	0),
	RESET_MP25(I3C1_R,		RCC_I3C1CFGR,		0,	0),
	RESET_MP25(I3C2_R,		RCC_I3C2CFGR,		0,	0),
	RESET_MP25(I3C3_R,		RCC_I3C3CFGR,		0,	0),
	RESET_MP25(I3C4_R,		RCC_I3C4CFGR,		0,	0),
	RESET_MP25(IWDG2_KER_R,		RCC_IWDGC1CFGSETR,	18,	1),
	RESET_MP25(IWDG4_KER_R,		RCC_IWDGC2CFGSETR,	18,	1),
	RESET_MP25(RNG_R,		RCC_RNGCFGR,		0,	0),
	RESET_MP25(PKA_R,		RCC_PKACFGR,		0,	0),
	RESET_MP25(SAES_R,		RCC_SAESCFGR,		0,	0),
	RESET_MP25(HASH_R,		RCC_HASHCFGR,		0,	0),
	RESET_MP25(CRYP1_R,		RCC_CRYP1CFGR,		0,	0),
	RESET_MP25(CRYP2_R,		RCC_CRYP2CFGR,		0,	0),
	RESET_MP25(PCIE_R,		RCC_PCIECFGR,		0,	0),
};

static u16 stm32mp25_cpt_gate[GATE_NB];

static struct clk_stm32_clock_data stm32mp25_clock_data = {
	.gate_cpt	= stm32mp25_cpt_gate,
	.gates		= stm32mp25_gates,
	.muxes		= stm32mp25_muxes,
};

static struct clk_stm32_reset_data stm32mp25_reset_data = {
	.reset_lines	= stm32mp25_reset_cfg,
	.nr_lines	= ARRAY_SIZE(stm32mp25_reset_cfg),
};

static struct stm32_rcc_match_data stm32mp25_data = {
	.tab_clocks	= stm32mp25_clock_cfg,
	.num_clocks	= ARRAY_SIZE(stm32mp25_clock_cfg),
	.maxbinding	= STM32MP25_LAST_CLK,
	.clock_data	= &stm32mp25_clock_data,
	.reset_data	= &stm32mp25_reset_data,
};

static const struct of_device_id stm32mp25_match_data[] = {
	{
		.compatible = "st,stm32mp25-rcc",
		.data = &stm32mp25_data,
	},
	{ }
};
MODULE_DEVICE_TABLE(of, stm32mp25_match_data);

static int stm32mp25_rcc_init(struct device *dev)
{
	void __iomem *base;
	int ret;

	base = of_iomap(dev_of_node(dev), 0);
	if (!base) {
		dev_err(dev, "%pOFn: unable to map resource", dev_of_node(dev));
		ret = -ENOMEM;
		goto out;
	}

	ret = stm32_rcc_init(dev, stm32mp25_match_data, base);

out:
	if (ret) {
		if (base)
			iounmap(base);

		of_node_put(dev_of_node(dev));
	}

	return ret;
}

static int get_clock_deps(struct device *dev)
{
	static const char * const clock_deps_name[] = {
		"hsi", "hse", "msi", "lsi", "lse",
	};
	size_t deps_size = sizeof(struct clk *) * ARRAY_SIZE(clock_deps_name);
	struct clk **clk_deps;
	int i;

	clk_deps = devm_kzalloc(dev, deps_size, GFP_KERNEL);
	if (!clk_deps)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(clock_deps_name); i++) {
		struct clk *clk;

		clk = of_clk_get_by_name(dev_of_node(dev), clock_deps_name[i]);

		if (IS_ERR(clk)) {
			if (PTR_ERR(clk) != -EINVAL && PTR_ERR(clk) != -ENOENT)
				return PTR_ERR(clk);
		} else {
			/* Device gets a reference count on the clock */
			clk_deps[i] = devm_clk_get(dev, __clk_get_name(clk));
			clk_put(clk);
		}
	}

	return 0;
}

static int stm32mp25_rcc_clocks_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int ret = get_clock_deps(dev);

	if (!ret)
		ret = stm32mp25_rcc_init(dev);

	return ret;
}

static int stm32mp25_rcc_clocks_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *child, *np = dev_of_node(dev);

	for_each_available_child_of_node(np, child)
		of_clk_del_provider(child);

	return 0;
}

static struct platform_driver stm32mp25_rcc_clocks_driver = {
	.driver	= {
		.name = "stm32mp25_rcc",
		.of_match_table = stm32mp25_match_data,
	},
	.probe = stm32mp25_rcc_clocks_probe,
	.remove = stm32mp25_rcc_clocks_remove,
};

static int __init stm32mp25_clocks_init(void)
{
	return platform_driver_register(&stm32mp25_rcc_clocks_driver);
}

core_initcall(stm32mp25_clocks_init);
