// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Ambarella CV75 pinctrl data
 *
 * Copyright (C) 2026, Ambarella, Inc.
 */

#include <linux/kernel.h>

#include "pinctrl-ambarella.h"

#define CV75_PINMUX_GROUP(_name, ...)					\
	static const u32 cv75_##_name##_pinmux[] = { __VA_ARGS__ }

#define CV75_GROUP(_name)						\
	{								\
		.name = #_name,						\
		.pinmux = cv75_##_name##_pinmux,				\
		.num_pins = ARRAY_SIZE(cv75_##_name##_pinmux),		\
	}

#define CV75_FUNCTION(_name)						\
	{								\
		.name = #_name,						\
		.groups = cv75_##_name##_groups,				\
		.num_groups = ARRAY_SIZE(cv75_##_name##_groups),		\
	}

/* UART */
CV75_PINMUX_GROUP(uart0, AMBA_PINMUX(44, 1), AMBA_PINMUX(45, 1));
CV75_PINMUX_GROUP(uart1, AMBA_PINMUX(46, 1), AMBA_PINMUX(47, 1));
CV75_PINMUX_GROUP(uart1_flow, AMBA_PINMUX(48, 1), AMBA_PINMUX(49, 1));
CV75_PINMUX_GROUP(uart2_a, AMBA_PINMUX(46, 2), AMBA_PINMUX(47, 2));
CV75_PINMUX_GROUP(uart2_b, AMBA_PINMUX(50, 2), AMBA_PINMUX(51, 2));
CV75_PINMUX_GROUP(uart2_c, AMBA_PINMUX(66, 2), AMBA_PINMUX(68, 2));
CV75_PINMUX_GROUP(uart2_flow_a, AMBA_PINMUX(48, 2), AMBA_PINMUX(49, 2));
CV75_PINMUX_GROUP(uart2_flow_b, AMBA_PINMUX(65, 3), AMBA_PINMUX(67, 3));
CV75_PINMUX_GROUP(uart3_a, AMBA_PINMUX(70, 3), AMBA_PINMUX(72, 3));
CV75_PINMUX_GROUP(uart3_b, AMBA_PINMUX(80, 3), AMBA_PINMUX(79, 3));
CV75_PINMUX_GROUP(uart3_flow_a, AMBA_PINMUX(71, 3), AMBA_PINMUX(69, 3));
CV75_PINMUX_GROUP(uart3_flow_b, AMBA_PINMUX(81, 3), AMBA_PINMUX(82, 3));
CV75_PINMUX_GROUP(uart4_a, AMBA_PINMUX(29, 3), AMBA_PINMUX(30, 3));
CV75_PINMUX_GROUP(uart4_b, AMBA_PINMUX(76, 3), AMBA_PINMUX(77, 3));
CV75_PINMUX_GROUP(uart4_flow_a, AMBA_PINMUX(28, 3), AMBA_PINMUX(31, 3));
CV75_PINMUX_GROUP(uart4_flow_b, AMBA_PINMUX(74, 3), AMBA_PINMUX(75, 3));

/* Flash */
CV75_PINMUX_GROUP(snand, AMBA_PINMUX(79, 1), AMBA_PINMUX(80, 1),
		  AMBA_PINMUX(81, 1), AMBA_PINMUX(82, 1),
		  AMBA_PINMUX(83, 1), AMBA_PINMUX(84, 1));
CV75_PINMUX_GROUP(spinor, AMBA_PINMUX(79, 2), AMBA_PINMUX(80, 2),
		  AMBA_PINMUX(81, 2), AMBA_PINMUX(82, 2),
		  AMBA_PINMUX(83, 2), AMBA_PINMUX(84, 2),
		  AMBA_PINMUX(85, 2));

/* SD/MMC */
CV75_PINMUX_GROUP(sdmmc0_cd, AMBA_PINMUX(6, 1));
CV75_PINMUX_GROUP(sdmmc0_wp, AMBA_PINMUX(7, 1));
CV75_PINMUX_GROUP(sdmmc0_reset, AMBA_PINMUX(8, 1));
CV75_PINMUX_GROUP(sdmmc0_hs_sel, AMBA_PINMUX(93, 1));
CV75_PINMUX_GROUP(sdmmc0_1bit, AMBA_PINMUX(0, 1), AMBA_PINMUX(4, 1),
		  AMBA_PINMUX(5, 1));
CV75_PINMUX_GROUP(sdmmc0_4bit, AMBA_PINMUX(0, 1), AMBA_PINMUX(1, 1),
		  AMBA_PINMUX(2, 1), AMBA_PINMUX(3, 1),
		  AMBA_PINMUX(4, 1), AMBA_PINMUX(5, 1));
CV75_PINMUX_GROUP(sdmmc1_cd, AMBA_PINMUX(15, 1));
CV75_PINMUX_GROUP(sdmmc1_wp, AMBA_PINMUX(16, 1));
CV75_PINMUX_GROUP(sdmmc1_reset, AMBA_PINMUX(17, 1));
CV75_PINMUX_GROUP(sdmmc1_hs_sel, AMBA_PINMUX(94, 1));
CV75_PINMUX_GROUP(sdmmc1_1bit, AMBA_PINMUX(9, 1), AMBA_PINMUX(13, 1),
		  AMBA_PINMUX(14, 1));
CV75_PINMUX_GROUP(sdmmc1_4bit, AMBA_PINMUX(9, 1), AMBA_PINMUX(10, 1),
		  AMBA_PINMUX(11, 1), AMBA_PINMUX(12, 1),
		  AMBA_PINMUX(13, 1), AMBA_PINMUX(14, 1));

/* Ethernet */
CV75_PINMUX_GROUP(enet_ext_osc_clk, AMBA_PINMUX(77, 1));
CV75_PINMUX_GROUP(enet_2nd_ref_clk_a, AMBA_PINMUX(78, 1));
CV75_PINMUX_GROUP(enet_2nd_ref_clk_b, AMBA_PINMUX(76, 2));
CV75_PINMUX_GROUP(enet0_ptp_pps_o, AMBA_PINMUX(74, 1));
CV75_PINMUX_GROUP(rgmii0, AMBA_PINMUX(62, 1), AMBA_PINMUX(63, 1),
		  AMBA_PINMUX(64, 1), AMBA_PINMUX(65, 1),
		  AMBA_PINMUX(66, 1), AMBA_PINMUX(67, 1),
		  AMBA_PINMUX(68, 1), AMBA_PINMUX(69, 1),
		  AMBA_PINMUX(70, 1), AMBA_PINMUX(71, 1),
		  AMBA_PINMUX(72, 1), AMBA_PINMUX(73, 1),
		  AMBA_PINMUX(75, 1), AMBA_PINMUX(76, 1));
CV75_PINMUX_GROUP(rmii0, AMBA_PINMUX(62, 1), AMBA_PINMUX(63, 1),
		  AMBA_PINMUX(64, 1), AMBA_PINMUX(67, 1),
		  AMBA_PINMUX(68, 1), AMBA_PINMUX(71, 1),
		  AMBA_PINMUX(72, 1), AMBA_PINMUX(73, 1),
		  AMBA_PINMUX(75, 2));

/* I2C */
CV75_PINMUX_GROUP(i2c0_a, AMBA_PINMUX(67, 2), AMBA_PINMUX(68, 2));
CV75_PINMUX_GROUP(i2c0_b, AMBA_PINMUX(86, 1), AMBA_PINMUX(87, 1));
CV75_PINMUX_GROUP(i2c1_a, AMBA_PINMUX(22, 2), AMBA_PINMUX(23, 2));
CV75_PINMUX_GROUP(i2c1_b, AMBA_PINMUX(69, 2), AMBA_PINMUX(70, 2));
CV75_PINMUX_GROUP(i2c2, AMBA_PINMUX(88, 1), AMBA_PINMUX(89, 1));
CV75_PINMUX_GROUP(i2c3_a, AMBA_PINMUX(24, 2), AMBA_PINMUX(25, 2));
CV75_PINMUX_GROUP(i2c3_b, AMBA_PINMUX(48, 3), AMBA_PINMUX(49, 3));
CV75_PINMUX_GROUP(i2c3_c, AMBA_PINMUX(58, 3), AMBA_PINMUX(59, 3));
CV75_PINMUX_GROUP(i2cs_a, AMBA_PINMUX(19, 2), AMBA_PINMUX(21, 2));
CV75_PINMUX_GROUP(i2cs_b, AMBA_PINMUX(60, 3), AMBA_PINMUX(61, 3));
CV75_PINMUX_GROUP(i2cs_c, AMBA_PINMUX(71, 2), AMBA_PINMUX(72, 2));
CV75_PINMUX_GROUP(i2cs_d, AMBA_PINMUX(86, 2), AMBA_PINMUX(87, 2));

/* CAN, IR, WDT */
CV75_PINMUX_GROUP(can0, AMBA_PINMUX(50, 1), AMBA_PINMUX(51, 1));
CV75_PINMUX_GROUP(can1, AMBA_PINMUX(52, 1), AMBA_PINMUX(53, 1));
CV75_PINMUX_GROUP(ir, AMBA_PINMUX(18, 1));
CV75_PINMUX_GROUP(wdt_a, AMBA_PINMUX(20, 2));
CV75_PINMUX_GROUP(wdt_b, AMBA_PINMUX(27, 3));
CV75_PINMUX_GROUP(wdt_c, AMBA_PINMUX(39, 2));
CV75_PINMUX_GROUP(wdt_d, AMBA_PINMUX(83, 3));
CV75_PINMUX_GROUP(wdt_e, AMBA_PINMUX(85, 4));
CV75_PINMUX_GROUP(wdt_f, AMBA_PINMUX(90, 1));

/* I2S */
CV75_PINMUX_GROUP(i2s0, AMBA_PINMUX(54, 1), AMBA_PINMUX(55, 1),
		  AMBA_PINMUX(56, 1), AMBA_PINMUX(57, 1));
CV75_PINMUX_GROUP(i2s1, AMBA_PINMUX(58, 1), AMBA_PINMUX(59, 1),
		  AMBA_PINMUX(60, 1), AMBA_PINMUX(61, 1));
CV75_PINMUX_GROUP(clk_au, AMBA_PINMUX(96, 1));
CV75_PINMUX_GROUP(dmic0, AMBA_PINMUX(54, 2), AMBA_PINMUX(55, 2));

/* PWM */
CV75_PINMUX_GROUP(pwm0, AMBA_PINMUX(40, 1));
CV75_PINMUX_GROUP(pwm1, AMBA_PINMUX(41, 1));
CV75_PINMUX_GROUP(pwm2, AMBA_PINMUX(42, 1));
CV75_PINMUX_GROUP(pwm3, AMBA_PINMUX(43, 1));
CV75_PINMUX_GROUP(pwm4_a, AMBA_PINMUX(19, 3));
CV75_PINMUX_GROUP(pwm4_b, AMBA_PINMUX(32, 4));
CV75_PINMUX_GROUP(pwm5_a, AMBA_PINMUX(20, 3));
CV75_PINMUX_GROUP(pwm5_b, AMBA_PINMUX(33, 4));
CV75_PINMUX_GROUP(pwm6_a, AMBA_PINMUX(21, 3));
CV75_PINMUX_GROUP(pwm6_b, AMBA_PINMUX(34, 4));
CV75_PINMUX_GROUP(pwm7_a, AMBA_PINMUX(22, 3));
CV75_PINMUX_GROUP(pwm7_b, AMBA_PINMUX(35, 4));
CV75_PINMUX_GROUP(pwm8_a, AMBA_PINMUX(23, 3));
CV75_PINMUX_GROUP(pwm8_b, AMBA_PINMUX(36, 4));
CV75_PINMUX_GROUP(pwm9_a, AMBA_PINMUX(24, 3));
CV75_PINMUX_GROUP(pwm9_b, AMBA_PINMUX(37, 4));
CV75_PINMUX_GROUP(pwm10_a, AMBA_PINMUX(25, 3));
CV75_PINMUX_GROUP(pwm10_b, AMBA_PINMUX(38, 4));
CV75_PINMUX_GROUP(pwm11_a, AMBA_PINMUX(26, 3));
CV75_PINMUX_GROUP(pwm11_b, AMBA_PINMUX(39, 4));

/* SPI */
CV75_PINMUX_GROUP(spi0, AMBA_PINMUX(19, 1), AMBA_PINMUX(20, 1),
		  AMBA_PINMUX(21, 1));
CV75_PINMUX_GROUP(spi1, AMBA_PINMUX(24, 1), AMBA_PINMUX(25, 1),
		  AMBA_PINMUX(26, 1));
CV75_PINMUX_GROUP(spi2, AMBA_PINMUX(28, 1), AMBA_PINMUX(29, 1),
		  AMBA_PINMUX(30, 1));
CV75_PINMUX_GROUP(spi3_a, AMBA_PINMUX(32, 5), AMBA_PINMUX(33, 5),
		  AMBA_PINMUX(34, 5));
CV75_PINMUX_GROUP(spi3_b, AMBA_PINMUX(40, 2), AMBA_PINMUX(41, 2),
		  AMBA_PINMUX(43, 2));
CV75_PINMUX_GROUP(spi3_c, AMBA_PINMUX(58, 2), AMBA_PINMUX(59, 2),
		  AMBA_PINMUX(60, 2));
CV75_PINMUX_GROUP(spi_slave_a, AMBA_PINMUX(24, 4), AMBA_PINMUX(25, 4),
		  AMBA_PINMUX(26, 4), AMBA_PINMUX(27, 4));
CV75_PINMUX_GROUP(spi_slave_b, AMBA_PINMUX(28, 2), AMBA_PINMUX(29, 2),
		  AMBA_PINMUX(30, 2), AMBA_PINMUX(31, 2));
CV75_PINMUX_GROUP(spi_slave_c, AMBA_PINMUX(36, 5), AMBA_PINMUX(37, 5),
		  AMBA_PINMUX(38, 5), AMBA_PINMUX(39, 5));
CV75_PINMUX_GROUP(spi_slave_d, AMBA_PINMUX(50, 3), AMBA_PINMUX(51, 3),
		  AMBA_PINMUX(52, 3), AMBA_PINMUX(53, 3));
CV75_PINMUX_GROUP(spi_slave_e, AMBA_PINMUX(81, 4), AMBA_PINMUX(82, 4),
		  AMBA_PINMUX(83, 4), AMBA_PINMUX(84, 4));

/* VIN master sync */
CV75_PINMUX_GROUP(vin_master_sync_a, AMBA_PINMUX(91, 1),
		  AMBA_PINMUX(92, 1));
CV75_PINMUX_GROUP(vin_master_sync_b, AMBA_PINMUX(91, 2),
		  AMBA_PINMUX(92, 2));
CV75_PINMUX_GROUP(vin_master_sync_c, AMBA_PINMUX(40, 3),
		  AMBA_PINMUX(41, 3));
CV75_PINMUX_GROUP(vin_master_sync_d, AMBA_PINMUX(46, 3),
		  AMBA_PINMUX(47, 3));
CV75_PINMUX_GROUP(vin_master_sync_e, AMBA_PINMUX(79, 4),
		  AMBA_PINMUX(80, 4));
CV75_PINMUX_GROUP(vsync0, AMBA_PINMUX(32, 1));
CV75_PINMUX_GROUP(vsync1, AMBA_PINMUX(33, 1));
CV75_PINMUX_GROUP(vsync2, AMBA_PINMUX(34, 1));
CV75_PINMUX_GROUP(vsync3, AMBA_PINMUX(35, 1));
CV75_PINMUX_GROUP(hsync0, AMBA_PINMUX(36, 1));
CV75_PINMUX_GROUP(hsync1, AMBA_PINMUX(37, 1));

static const struct ambpin_group_desc cv75_pin_groups[] = {
	CV75_GROUP(uart0),
	CV75_GROUP(uart1), CV75_GROUP(uart1_flow),
	CV75_GROUP(uart2_a), CV75_GROUP(uart2_b), CV75_GROUP(uart2_c),
	CV75_GROUP(uart2_flow_a), CV75_GROUP(uart2_flow_b),
	CV75_GROUP(uart3_a), CV75_GROUP(uart3_b),
	CV75_GROUP(uart3_flow_a), CV75_GROUP(uart3_flow_b),
	CV75_GROUP(uart4_a), CV75_GROUP(uart4_b),
	CV75_GROUP(uart4_flow_a), CV75_GROUP(uart4_flow_b),
	CV75_GROUP(snand), CV75_GROUP(spinor),
	CV75_GROUP(sdmmc0_cd), CV75_GROUP(sdmmc0_wp),
	CV75_GROUP(sdmmc0_reset), CV75_GROUP(sdmmc0_hs_sel),
	CV75_GROUP(sdmmc0_1bit), CV75_GROUP(sdmmc0_4bit),
	CV75_GROUP(sdmmc1_cd), CV75_GROUP(sdmmc1_wp),
	CV75_GROUP(sdmmc1_reset), CV75_GROUP(sdmmc1_hs_sel),
	CV75_GROUP(sdmmc1_1bit), CV75_GROUP(sdmmc1_4bit),
	CV75_GROUP(enet_ext_osc_clk), CV75_GROUP(enet_2nd_ref_clk_a),
	CV75_GROUP(enet_2nd_ref_clk_b), CV75_GROUP(enet0_ptp_pps_o),
	CV75_GROUP(rgmii0), CV75_GROUP(rmii0),
	CV75_GROUP(i2c0_a), CV75_GROUP(i2c0_b),
	CV75_GROUP(i2c1_a), CV75_GROUP(i2c1_b), CV75_GROUP(i2c2),
	CV75_GROUP(i2c3_a), CV75_GROUP(i2c3_b), CV75_GROUP(i2c3_c),
	CV75_GROUP(i2cs_a), CV75_GROUP(i2cs_b),
	CV75_GROUP(i2cs_c), CV75_GROUP(i2cs_d),
	CV75_GROUP(can0), CV75_GROUP(can1), CV75_GROUP(ir),
	CV75_GROUP(wdt_a), CV75_GROUP(wdt_b), CV75_GROUP(wdt_c),
	CV75_GROUP(wdt_d), CV75_GROUP(wdt_e), CV75_GROUP(wdt_f),
	CV75_GROUP(i2s0), CV75_GROUP(i2s1),
	CV75_GROUP(clk_au), CV75_GROUP(dmic0),
	CV75_GROUP(pwm0), CV75_GROUP(pwm1),
	CV75_GROUP(pwm2), CV75_GROUP(pwm3),
	CV75_GROUP(pwm4_a), CV75_GROUP(pwm4_b),
	CV75_GROUP(pwm5_a), CV75_GROUP(pwm5_b),
	CV75_GROUP(pwm6_a), CV75_GROUP(pwm6_b),
	CV75_GROUP(pwm7_a), CV75_GROUP(pwm7_b),
	CV75_GROUP(pwm8_a), CV75_GROUP(pwm8_b),
	CV75_GROUP(pwm9_a), CV75_GROUP(pwm9_b),
	CV75_GROUP(pwm10_a), CV75_GROUP(pwm10_b),
	CV75_GROUP(pwm11_a), CV75_GROUP(pwm11_b),
	CV75_GROUP(spi0), CV75_GROUP(spi1), CV75_GROUP(spi2),
	CV75_GROUP(spi3_a), CV75_GROUP(spi3_b), CV75_GROUP(spi3_c),
	CV75_GROUP(spi_slave_a), CV75_GROUP(spi_slave_b),
	CV75_GROUP(spi_slave_c), CV75_GROUP(spi_slave_d),
	CV75_GROUP(spi_slave_e),
	CV75_GROUP(vin_master_sync_a), CV75_GROUP(vin_master_sync_b),
	CV75_GROUP(vin_master_sync_c), CV75_GROUP(vin_master_sync_d),
	CV75_GROUP(vin_master_sync_e),
	CV75_GROUP(vsync0), CV75_GROUP(vsync1),
	CV75_GROUP(vsync2), CV75_GROUP(vsync3),
	CV75_GROUP(hsync0), CV75_GROUP(hsync1),
};

static const char * const cv75_uart0_groups[] = {
	"uart0",
};

static const char * const cv75_uart1_groups[] = {
	"uart1",
	"uart1_flow",
};

static const char * const cv75_uart2_groups[] = {
	"uart2_a",
	"uart2_b",
	"uart2_c",
	"uart2_flow_a",
	"uart2_flow_b",
};

static const char * const cv75_uart3_groups[] = {
	"uart3_a",
	"uart3_b",
	"uart3_flow_a",
	"uart3_flow_b",
};

static const char * const cv75_uart4_groups[] = {
	"uart4_a",
	"uart4_b",
	"uart4_flow_a",
	"uart4_flow_b",
};

static const char * const cv75_snand_groups[] = {
	"snand",
};

static const char * const cv75_spinor_groups[] = {
	"spinor",
};

static const char * const cv75_sdmmc0_groups[] = {
	"sdmmc0_cd",
	"sdmmc0_wp",
	"sdmmc0_reset",
	"sdmmc0_hs_sel",
	"sdmmc0_1bit",
	"sdmmc0_4bit",
};

static const char * const cv75_sdmmc1_groups[] = {
	"sdmmc1_cd",
	"sdmmc1_wp",
	"sdmmc1_reset",
	"sdmmc1_hs_sel",
	"sdmmc1_1bit",
	"sdmmc1_4bit",
};

static const char * const cv75_enet0_groups[] = {
	"enet_ext_osc_clk",
	"enet_2nd_ref_clk_a",
	"enet_2nd_ref_clk_b",
	"enet0_ptp_pps_o",
	"rgmii0",
	"rmii0",
};

static const char * const cv75_i2c0_groups[] = {
	"i2c0_a",
	"i2c0_b",
};

static const char * const cv75_i2c1_groups[] = {
	"i2c1_a",
	"i2c1_b",
};

static const char * const cv75_i2c2_groups[] = {
	"i2c2",
};

static const char * const cv75_i2c3_groups[] = {
	"i2c3_a",
	"i2c3_b",
	"i2c3_c",
};

static const char * const cv75_i2cs_groups[] = {
	"i2cs_a",
	"i2cs_b",
	"i2cs_c",
	"i2cs_d",
};

static const char * const cv75_can0_groups[] = {
	"can0",
};

static const char * const cv75_can1_groups[] = {
	"can1",
};

static const char * const cv75_ir_groups[] = {
	"ir",
};

static const char * const cv75_wdt_groups[] = {
	"wdt_a",
	"wdt_b",
	"wdt_c",
	"wdt_d",
	"wdt_e",
	"wdt_f",
};

static const char * const cv75_i2s0_groups[] = {
	"i2s0",
};

static const char * const cv75_i2s1_groups[] = {
	"i2s1",
};

static const char * const cv75_clk_au_groups[] = {
	"clk_au",
};

static const char * const cv75_dmic0_groups[] = {
	"dmic0",
};

static const char * const cv75_pwm0_groups[] = {
	"pwm0",
};

static const char * const cv75_pwm1_groups[] = {
	"pwm1",
};

static const char * const cv75_pwm2_groups[] = {
	"pwm2",
};

static const char * const cv75_pwm3_groups[] = {
	"pwm3",
};

static const char * const cv75_pwm4_groups[] = {
	"pwm4_a",
	"pwm4_b",
};

static const char * const cv75_pwm5_groups[] = {
	"pwm5_a",
	"pwm5_b",
};

static const char * const cv75_pwm6_groups[] = {
	"pwm6_a",
	"pwm6_b",
};

static const char * const cv75_pwm7_groups[] = {
	"pwm7_a",
	"pwm7_b",
};

static const char * const cv75_pwm8_groups[] = {
	"pwm8_a",
	"pwm8_b",
};

static const char * const cv75_pwm9_groups[] = {
	"pwm9_a",
	"pwm9_b",
};

static const char * const cv75_pwm10_groups[] = {
	"pwm10_a",
	"pwm10_b",
};

static const char * const cv75_pwm11_groups[] = {
	"pwm11_a",
	"pwm11_b",
};

static const char * const cv75_spi0_groups[] = {
	"spi0",
};

static const char * const cv75_spi1_groups[] = {
	"spi1",
};

static const char * const cv75_spi2_groups[] = {
	"spi2",
};

static const char * const cv75_spi3_groups[] = {
	"spi3_a",
	"spi3_b",
	"spi3_c",
};

static const char * const cv75_spi_slave_groups[] = {
	"spi_slave_a",
	"spi_slave_b",
	"spi_slave_c",
	"spi_slave_d",
	"spi_slave_e",
};

static const char * const cv75_vin_master_sync_groups[] = {
	"vin_master_sync_a",
	"vin_master_sync_b",
	"vin_master_sync_c",
	"vin_master_sync_d",
	"vin_master_sync_e",
};

static const char * const cv75_vsync0_groups[] = {
	"vsync0",
};

static const char * const cv75_vsync1_groups[] = {
	"vsync1",
};

static const char * const cv75_vsync2_groups[] = {
	"vsync2",
};

static const char * const cv75_vsync3_groups[] = {
	"vsync3",
};

static const char * const cv75_hsync0_groups[] = {
	"hsync0",
};

static const char * const cv75_hsync1_groups[] = {
	"hsync1",
};

static const struct ambpin_function cv75_pin_functions[] = {
	CV75_FUNCTION(uart0),
	CV75_FUNCTION(uart1),
	CV75_FUNCTION(uart2),
	CV75_FUNCTION(uart3),
	CV75_FUNCTION(uart4),
	CV75_FUNCTION(snand),
	CV75_FUNCTION(spinor),
	CV75_FUNCTION(sdmmc0),
	CV75_FUNCTION(sdmmc1),
	CV75_FUNCTION(enet0),
	CV75_FUNCTION(i2c0),
	CV75_FUNCTION(i2c1),
	CV75_FUNCTION(i2c2),
	CV75_FUNCTION(i2c3),
	CV75_FUNCTION(i2cs),
	CV75_FUNCTION(can0),
	CV75_FUNCTION(can1),
	CV75_FUNCTION(ir),
	CV75_FUNCTION(wdt),
	CV75_FUNCTION(i2s0),
	CV75_FUNCTION(i2s1),
	CV75_FUNCTION(clk_au),
	CV75_FUNCTION(dmic0),
	CV75_FUNCTION(pwm0),
	CV75_FUNCTION(pwm1),
	CV75_FUNCTION(pwm2),
	CV75_FUNCTION(pwm3),
	CV75_FUNCTION(pwm4),
	CV75_FUNCTION(pwm5),
	CV75_FUNCTION(pwm6),
	CV75_FUNCTION(pwm7),
	CV75_FUNCTION(pwm8),
	CV75_FUNCTION(pwm9),
	CV75_FUNCTION(pwm10),
	CV75_FUNCTION(pwm11),
	CV75_FUNCTION(spi0),
	CV75_FUNCTION(spi1),
	CV75_FUNCTION(spi2),
	CV75_FUNCTION(spi3),
	CV75_FUNCTION(spi_slave),
	CV75_FUNCTION(vin_master_sync),
	CV75_FUNCTION(vsync0),
	CV75_FUNCTION(vsync1),
	CV75_FUNCTION(vsync2),
	CV75_FUNCTION(vsync3),
	CV75_FUNCTION(hsync0),
	CV75_FUNCTION(hsync1),
};

const struct amb_pinctrl_data ambarella_cv75_pinctrl_data = {
	.ds0 = {
		0x314, 0x320, 0x32c,
	},
	.ds1 = {
		0x318, 0x324, 0x330,
	},
	.ds2 = {
		0x31c, 0x328, 0x334,
	},
	.pull_en = {
		0x60, 0x64, 0x68,
	},
	.pull_dir = {
		0x7c, 0x80, 0x84,
	},
	.have_ds2 = true,
	.clk_au_dedicated_pin = 96,
	.nr_banks = 3,
	.npins = 97,
	.groups = cv75_pin_groups,
	.nr_groups = ARRAY_SIZE(cv75_pin_groups),
	.functions = cv75_pin_functions,
	.nr_functions = ARRAY_SIZE(cv75_pin_functions),
};
