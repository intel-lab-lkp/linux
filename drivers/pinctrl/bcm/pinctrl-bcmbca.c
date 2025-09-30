// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2021 Rafał Miłecki <rafal@milecki.pl> */

#include <linux/cleanup.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/string_helpers.h>

#include "../core.h"
#include "../pinmux.h"

#define BCMBCA_TEST_PORT_BLOCK_EN_LSB			0x00
#define BCMBCA_TEST_PORT_BLOCK_DATA_MSB			0x04
#define BCMBCA_TEST_PORT_BLOCK_DATA_LSB			0x08
#define  BCMBCA_TEST_PORT_LSB_PINMUX_DATA_SHIFT		12
#define BCMBCA_TEST_PORT_COMMAND			0x0c
#define  BCMBCA_TEST_PORT_CMD_LOAD_MUX_REG		0x00000021

struct bcmbca_pinctrl_grp {
	const char *name;
	const struct bcmbca_pinctrl_pin_setup *pins;
	const unsigned int num_pins;
};

struct bcmbca_pinctrl_function {
	const char *name;
	const char * const *groups;
	const unsigned int num_groups;
};

struct bcmbca_soc_info {
	unsigned int num_pins;
	const struct bcmbca_pinctrl_grp *groups;
	unsigned int num_groups;
	const struct bcmbca_pinctrl_function *functions;
	unsigned int num_functions;
};

struct bcmbca_pinctrl {
	struct device *dev;
	void __iomem *base;
	struct mutex mutex;
	struct pinctrl_dev *pctldev;
	struct pinctrl_desc pctldesc;
};

struct bcmbca_pinctrl_pin_setup {
	unsigned int number;
	unsigned int function;
};

/* BCM6846 groups and functions */

#define BCM6846_NUM_PINS 79

static const struct bcmbca_pinctrl_pin_setup bcm6846_led_0_pins_a[] = {
	{ 18, 1 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_led_0_pins_b[] = {
	{ 52, 2 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_led_1_pins_a[] = {
	{ 19, 1 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_led_1_pins_b[] = {
	{ 53, 2 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_led_2_pins_a[] = {
	{ 22, 1 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_led_2_pins_b[] = {
	{ 49, 2 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_led_3_pins_a[] = {
	{ 24, 1 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_led_3_pins_b[] = {
	{ 44, 1 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_led_4_pins_a[] = {
	{ 9, 0 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_led_4_pins_b[] = {
	{ 44, 2 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_led_5_pins_a[] = {
	{ 10, 0 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_led_5_pins_b[] = {
	{ 43, 2 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_led_6_pins_a[] = {
	{ 11, 0 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_led_6_pins_b[] = {
	{ 47, 2 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_led_7_pins_a[] = {
	{ 12, 0 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_led_7_pins_b[] = {
	{ 45, 2 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_led_8_pins_a[] = {
	{ 13, 0 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_led_8_pins_b[] = {
	{ 5, 2 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_led_9_pins_a[] = {
	{ 16, 0 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_led_9_pins_b[] = {
	{ 18, 2 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_led_10_pins_a[] = {
	{ 17, 0 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_led_10_pins_b[] = {
	{ 50, 2 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_led_11_pins_a[] = {
	{ 20, 0 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_led_11_pins_b[] = {
	{ 51, 2 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_led_12_pins_a[] = {
	{ 21, 0 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_led_12_pins_b[] = {
	{ 56, 2 },
};

/* LEDs 13 thru 17 can apparently only be muxed in one place */
static const struct bcmbca_pinctrl_pin_setup bcm6846_led_13_pins[] = {
	{ 74, 0 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_led_14_pins[] = {
	{ 56, 0 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_led_15_pins[] = {
	{ 57, 0 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_led_16_pins[] = {
	{ 58, 0 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_led_17_pins[] = {
	{ 59, 0 },
};

/* Pins used for serial LED control can come out in two places */
static const struct bcmbca_pinctrl_pin_setup bcm6846_ser_led_pins_a[] = {
	{ 18, 3 }, /* CLK */
	{ 23, 3 }, /* DATA */
	{ 25, 3 }, /* MASK */
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_ser_led_pins_b[] = {
	{ 43, 3 }, /* CLK */
	{ 45, 3 }, /* DATA */
	{ 44, 3 }, /* MASK */
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_nand_ctrl_pins[] = {
	{ 26, 1 },
	{ 27, 1 },
	{ 28, 1 },
	{ 29, 1 },
	{ 30, 1 },
	{ 31, 1 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_nand_data_pins[] = {
	{ 32, 1 },
	{ 33, 1 },
	{ 34, 1 },
	{ 35, 1 },
	{ 36, 1 },
	{ 37, 1 },
	{ 38, 1 },
	{ 39, 1 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_nand_wp_pins[] = {
	{ 8, 1 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_emmc_ctrl_pins[] = {
	{ 20, 1 },
	{ 21, 1 },
};

/* SPI port "M" can be muxed in three different places */
static const struct bcmbca_pinctrl_pin_setup bcm6846_spim_pins_a[] = {
	{ 62, 1 }, /* CLK */
	{ 63, 1 }, /* MOSI */
	{ 64, 1 }, /* MISO */
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_spim_ss0_pins_a[] = {
	{ 65, 1 }, /* SPI Select 0, Chip select 0 */
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_spim_ss1_pins_a[] = {
	{ 66, 1 }, /* SPI Select 1, Chip select 1 */
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_spim_ss2_pins_a[] = {
	{ 9, 1 }, /* SPI Select 2, Chip select 2 */
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_spim_ss3_pins_a[] = {
	{ 12, 1 }, /* SPI Select 3, Chip select 3 */
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_spim_pins_b[] = {
	{ 49, 0 }, /* CLK */
	{ 50, 0 }, /* MOSI */
	{ 51, 0 }, /* MISO */
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_spim_ss0_pins_b[] = {
	{ 52, 0 }, /* SPI Select 0, Chip select 0 */
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_spim_ss1_pins_b[] = {
	{ 53, 0 }, /* SPI Select 1, Chip select 1 */
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_spim_ss2_pins_b[] = {
	{ 5, 0 }, /* SPI Select 2, Chip select 2 */
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_spim_ss3_pins_b[] = {
	{ 7, 1 }, /* SPI Select 3, Chip select 3 */
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_spim_pins_c[] = {
	{ 24, 0 }, /* CLK */
	{ 6, 0 }, /* MOSI */
	{ 7, 0 }, /* MISO */
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_spim_ss0_pins_c[] = {
	{ 8, 0 }, /* SPI Select 0, Chip select 0 */
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_spim_ss1_pins_c[] = {
	{ 73, 1 }, /* SPI Select 1, Chip select 1 */
};

/* SPI port "S", this has just one optional select pin but can be in two places */
static const struct bcmbca_pinctrl_pin_setup bcm6846_spis_pins_a[] = {
	{ 56, 1 }, /* CLK */
	{ 57, 1 }, /* MOSI */
	{ 58, 1 }, /* MISO */
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_spis_ss_pins_a[] = {
	{ 59, 1 }, /* SPI S Select, Chip select */
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_spis_pins_b[] = {
	{ 0, 2 }, /* CLK */
	{ 1, 2 }, /* MOSI */
	{ 2, 2 }, /* MISO */
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_spis_ss_pins_b[] = {
	{ 3, 2 }, /* SPI S Select, Chip select */
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_usb0_pwr_pins[] = {
	{ 74, 1 },
	{ 75, 1 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_usb1_pwr_pins[] = {
	{ 76, 1 },
	{ 77, 1 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_i2c_pins[] = {
	{ 68, 0 },	/* SCL */
	{ 69, 0 },	/* SDA */
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_rgmii_pins[] = {
	{ 42, 1 },
	{ 43, 1 },
	{ 44, 1 },
	{ 45, 1 },
	{ 46, 1 },
	{ 47, 1 },
	{ 48, 1 },
	{ 49, 1 },
	{ 50, 1 },
	{ 51, 1 },
	{ 52, 1 },
	{ 53, 1 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_rgmii_rx_ok_pins[] = {
	{ 7, 3 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_rgmii_start_stop_pins[] = {
	{ 6, 3 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_mii_pins[] = {
	{ 8, 3 }, /* RX ER */
	{ 5, 3 }, /* TX ER */
};

/* "signal detect" pin can be in two positions */
static const struct bcmbca_pinctrl_pin_setup bcm6846_signal_detect_pins_a[] = {
	{ 58, 3 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_signal_detect_pins_b[] = {
	{ 15, 3 },
};

/* "one sec pls" pin can be in two positions */
static const struct bcmbca_pinctrl_pin_setup bcm6846_one_sec_pls_pins_a[] = {
	{ 40, 0 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_one_sec_pls_pins_b[] = {
	{ 6, 1 },
};

/* "Rogue onu" pin, presumably a misspelling of "rogue one" */
static const struct bcmbca_pinctrl_pin_setup bcm6846_rogue_onu_pins_a[] = {
	{ 56, 3 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_rogue_onu_pins_b[] = {
	{ 14, 3 },
};

/* WAN extra pins for SynchE and PTP */
static const struct bcmbca_pinctrl_pin_setup bcm6846_wan_mdio_pins[] = {
	{ 54, 1 }, /* MDC */
	{ 55, 1 }, /* MDIO */
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_wan_nco_pins[] = {
	{ 16, 3 }, /* 10 MHz NCO clock output */
	{ 5, 1 }, /* 8 kHz NCO clock output */
	{ 20, 3 }, /* NCO programmable clock output */
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_wan_early_txen_pins_a[] = {
	{ 10, 3 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_wan_early_txen_pins_b[] = {
	{ 4, 1 },
};

/* 1PPS pin for IEEE 1588 PTP */
static const struct bcmbca_pinctrl_pin_setup bcm6846_wan_nco_1pps_sig_pins_a[] = {
	{ 40, 1 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_wan_nco_1pps_sig_pins_b[] = {
	{ 6, 2 },
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_uart0_pins[] = {
	{ 60, 1 }, /* SDIN (RX) */
	{ 61, 1 }, /* SDOUT (TX) */
};

static const struct bcmbca_pinctrl_pin_setup bcm6846_uart2_pins[] = {
	{ 13, 1 }, /* CTS */
	{ 16, 1 }, /* RTS */
	{ 14, 1 }, /* SIN (RX) */
	{ 15, 1 }, /* SOUT (TX) */
};

static const struct bcmbca_pinctrl_grp bcm6846_pinctrl_grps[] = {
	{ "led_0_grp_a", bcm6846_led_0_pins_a, ARRAY_SIZE(bcm6846_led_0_pins_a) },
	{ "led_0_grp_b", bcm6846_led_0_pins_b, ARRAY_SIZE(bcm6846_led_0_pins_b) },
	{ "led_1_grp_a", bcm6846_led_1_pins_a, ARRAY_SIZE(bcm6846_led_1_pins_a) },
	{ "led_1_grp_b", bcm6846_led_1_pins_b, ARRAY_SIZE(bcm6846_led_1_pins_b) },
	{ "led_2_grp_a", bcm6846_led_2_pins_a, ARRAY_SIZE(bcm6846_led_2_pins_a) },
	{ "led_2_grp_b", bcm6846_led_2_pins_b, ARRAY_SIZE(bcm6846_led_2_pins_b) },
	{ "led_3_grp_a", bcm6846_led_3_pins_a, ARRAY_SIZE(bcm6846_led_3_pins_a) },
	{ "led_3_grp_b", bcm6846_led_3_pins_b, ARRAY_SIZE(bcm6846_led_3_pins_b) },
	{ "led_4_grp_a", bcm6846_led_4_pins_a, ARRAY_SIZE(bcm6846_led_4_pins_a) },
	{ "led_4_grp_b", bcm6846_led_4_pins_b, ARRAY_SIZE(bcm6846_led_4_pins_b) },
	{ "led_5_grp_a", bcm6846_led_5_pins_a, ARRAY_SIZE(bcm6846_led_5_pins_a) },
	{ "led_5_grp_b", bcm6846_led_5_pins_b, ARRAY_SIZE(bcm6846_led_5_pins_b) },
	{ "led_6_grp_a", bcm6846_led_6_pins_a, ARRAY_SIZE(bcm6846_led_6_pins_a) },
	{ "led_6_grp_b", bcm6846_led_6_pins_b, ARRAY_SIZE(bcm6846_led_6_pins_b) },
	{ "led_7_grp_a", bcm6846_led_7_pins_a, ARRAY_SIZE(bcm6846_led_7_pins_a) },
	{ "led_7_grp_b", bcm6846_led_7_pins_b, ARRAY_SIZE(bcm6846_led_7_pins_b) },
	{ "led_8_grp_a", bcm6846_led_8_pins_a, ARRAY_SIZE(bcm6846_led_8_pins_a) },
	{ "led_8_grp_b", bcm6846_led_8_pins_b, ARRAY_SIZE(bcm6846_led_8_pins_b) },
	{ "led_9_grp_a", bcm6846_led_9_pins_a, ARRAY_SIZE(bcm6846_led_9_pins_a) },
	{ "led_9_grp_b", bcm6846_led_9_pins_b, ARRAY_SIZE(bcm6846_led_9_pins_b) },
	{ "led_10_grp_a", bcm6846_led_10_pins_a, ARRAY_SIZE(bcm6846_led_10_pins_a) },
	{ "led_10_grp_b", bcm6846_led_10_pins_b, ARRAY_SIZE(bcm6846_led_10_pins_b) },
	{ "led_11_grp_a", bcm6846_led_11_pins_a, ARRAY_SIZE(bcm6846_led_11_pins_a) },
	{ "led_11_grp_b", bcm6846_led_11_pins_b, ARRAY_SIZE(bcm6846_led_11_pins_b) },
	{ "led_12_grp_a", bcm6846_led_12_pins_a, ARRAY_SIZE(bcm6846_led_12_pins_a) },
	{ "led_12_grp_b", bcm6846_led_12_pins_b, ARRAY_SIZE(bcm6846_led_12_pins_b) },
	{ "led_13_grp", bcm6846_led_13_pins, ARRAY_SIZE(bcm6846_led_13_pins) },
	{ "led_14_grp", bcm6846_led_14_pins, ARRAY_SIZE(bcm6846_led_14_pins) },
	{ "led_15_grp", bcm6846_led_15_pins, ARRAY_SIZE(bcm6846_led_15_pins) },
	{ "led_16_grp", bcm6846_led_16_pins, ARRAY_SIZE(bcm6846_led_16_pins) },
	{ "led_17_grp", bcm6846_led_17_pins, ARRAY_SIZE(bcm6846_led_17_pins) },
	{ "ser_led_grp_a", bcm6846_ser_led_pins_a, ARRAY_SIZE(bcm6846_ser_led_pins_a) },
	{ "ser_led_grp_b", bcm6846_ser_led_pins_b, ARRAY_SIZE(bcm6846_ser_led_pins_b) },
	{ "nand_ctrl_grp", bcm6846_nand_ctrl_pins, ARRAY_SIZE(bcm6846_nand_ctrl_pins) },
	{ "nand_data_grp", bcm6846_nand_data_pins, ARRAY_SIZE(bcm6846_nand_data_pins) },
	{ "nand_wp_grp", bcm6846_nand_wp_pins, ARRAY_SIZE(bcm6846_nand_wp_pins) },
	{ "emmc_ctrl_grp", bcm6846_emmc_ctrl_pins, ARRAY_SIZE(bcm6846_emmc_ctrl_pins) },
	{ "spim_grp_a", bcm6846_spim_pins_a, ARRAY_SIZE(bcm6846_spim_pins_a) },
	{ "spim_ss0_grp_a", bcm6846_spim_ss0_pins_a, ARRAY_SIZE(bcm6846_spim_ss0_pins_a) },
	{ "spim_ss1_grp_a", bcm6846_spim_ss1_pins_a, ARRAY_SIZE(bcm6846_spim_ss1_pins_a) },
	{ "spim_ss2_grp_a", bcm6846_spim_ss2_pins_a, ARRAY_SIZE(bcm6846_spim_ss2_pins_a) },
	{ "spim_ss3_grp_a", bcm6846_spim_ss3_pins_a, ARRAY_SIZE(bcm6846_spim_ss3_pins_a) },
	{ "spim_grp_b", bcm6846_spim_pins_b, ARRAY_SIZE(bcm6846_spim_pins_b) },
	{ "spim_ss0_grp_b", bcm6846_spim_ss0_pins_b, ARRAY_SIZE(bcm6846_spim_ss0_pins_b) },
	{ "spim_ss1_grp_b", bcm6846_spim_ss1_pins_b, ARRAY_SIZE(bcm6846_spim_ss1_pins_b) },
	{ "spim_ss2_grp_b", bcm6846_spim_ss2_pins_b, ARRAY_SIZE(bcm6846_spim_ss2_pins_b) },
	{ "spim_ss3_grp_b", bcm6846_spim_ss3_pins_b, ARRAY_SIZE(bcm6846_spim_ss3_pins_b) },
	{ "spim_grp_c", bcm6846_spim_pins_c, ARRAY_SIZE(bcm6846_spim_pins_c) },
	{ "spim_ss0_grp_c", bcm6846_spim_ss0_pins_c, ARRAY_SIZE(bcm6846_spim_ss0_pins_c) },
	{ "spim_ss1_grp_c", bcm6846_spim_ss1_pins_c, ARRAY_SIZE(bcm6846_spim_ss1_pins_c) },
	{ "spis_grp_a", bcm6846_spis_pins_a, ARRAY_SIZE(bcm6846_spis_pins_a) },
	{ "spis_ss_grp_a", bcm6846_spis_ss_pins_a, ARRAY_SIZE(bcm6846_spis_ss_pins_a) },
	{ "spis_grp_b", bcm6846_spis_pins_b, ARRAY_SIZE(bcm6846_spis_pins_b) },
	{ "spis_ss_grp_b", bcm6846_spis_ss_pins_b, ARRAY_SIZE(bcm6846_spis_ss_pins_b) },
	{ "usb0_pwr_grp", bcm6846_usb0_pwr_pins, ARRAY_SIZE(bcm6846_usb0_pwr_pins) },
	{ "usb1_pwr_grp", bcm6846_usb1_pwr_pins, ARRAY_SIZE(bcm6846_usb1_pwr_pins) },
	{ "i2c_grp", bcm6846_i2c_pins, ARRAY_SIZE(bcm6846_i2c_pins) },
	{ "rgmii_grp", bcm6846_rgmii_pins, ARRAY_SIZE(bcm6846_rgmii_pins) },
	{ "rgmii_rx_ok_grp", bcm6846_rgmii_rx_ok_pins, ARRAY_SIZE(bcm6846_rgmii_rx_ok_pins) },
	{ "rgmii_start_stop_grp", bcm6846_rgmii_start_stop_pins, ARRAY_SIZE(bcm6846_rgmii_start_stop_pins) },
	{ "mii_grp", bcm6846_mii_pins, ARRAY_SIZE(bcm6846_mii_pins) },
	{ "signal_detect_grp_a", bcm6846_signal_detect_pins_a, ARRAY_SIZE(bcm6846_signal_detect_pins_a) },
	{ "signal_detect_grp_b", bcm6846_signal_detect_pins_b, ARRAY_SIZE(bcm6846_signal_detect_pins_b) },
	{ "one_sec_pls_grp_a", bcm6846_one_sec_pls_pins_a, ARRAY_SIZE(bcm6846_one_sec_pls_pins_a) },
	{ "one_sec_pls_grp_b", bcm6846_one_sec_pls_pins_b, ARRAY_SIZE(bcm6846_one_sec_pls_pins_b) },
	{ "rogue_onu_grp_a", bcm6846_rogue_onu_pins_a, ARRAY_SIZE(bcm6846_rogue_onu_pins_a) },
	{ "rogue_onu_grp_b", bcm6846_rogue_onu_pins_b, ARRAY_SIZE(bcm6846_rogue_onu_pins_b) },
	{ "wan_mdio_grp", bcm6846_wan_mdio_pins, ARRAY_SIZE(bcm6846_wan_mdio_pins) },
	{ "wan_nco_grp", bcm6846_wan_nco_pins, ARRAY_SIZE(bcm6846_wan_nco_pins) },
	{ "wan_early_txen_grp_a", bcm6846_wan_early_txen_pins_a, ARRAY_SIZE(bcm6846_wan_early_txen_pins_a) },
	{ "wan_early_txen_grp_b", bcm6846_wan_early_txen_pins_b, ARRAY_SIZE(bcm6846_wan_early_txen_pins_b) },
	{ "wan_nco_1pps_sig_grp_a", bcm6846_wan_nco_1pps_sig_pins_a, ARRAY_SIZE(bcm6846_wan_nco_1pps_sig_pins_a) },
	{ "wan_nco_1pps_sig_grp_b", bcm6846_wan_nco_1pps_sig_pins_b, ARRAY_SIZE(bcm6846_wan_nco_1pps_sig_pins_b) },
	{ "uart0_grp", bcm6846_uart0_pins, ARRAY_SIZE(bcm6846_uart0_pins) },
	{ "uart2_grp", bcm6846_uart2_pins, ARRAY_SIZE(bcm6846_uart2_pins) },
};

static const char * const bcm6846_led_0_groups[] = { "led_0_grp_a", "led_0_grp_b" };
static const char * const bcm6846_led_1_groups[] = { "led_1_grp_a", "led_1_grp_b" };
static const char * const bcm6846_led_2_groups[] = { "led_2_grp_a", "led_2_grp_b" };
static const char * const bcm6846_led_3_groups[] = { "led_3_grp_a", "led_3_grp_b" };
static const char * const bcm6846_led_4_groups[] = { "led_4_grp_a", "led_4_grp_b" };
static const char * const bcm6846_led_5_groups[] = { "led_5_grp_a", "led_5_grp_b" };
static const char * const bcm6846_led_6_groups[] = { "led_6_grp_a", "led_6_grp_b" };
static const char * const bcm6846_led_7_groups[] = { "led_7_grp_a", "led_7_grp_b" };
static const char * const bcm6846_led_8_groups[] = { "led_8_grp_a", "led_8_grp_b" };
static const char * const bcm6846_led_9_groups[] = { "led_9_grp_a", "led_9_grp_b" };
static const char * const bcm6846_led_10_groups[] = { "led_10_grp_a", "led_10_grp_b" };
static const char * const bcm6846_led_11_groups[] = { "led_11_grp_a", "led_11_grp_b" };
static const char * const bcm6846_led_12_groups[] = { "led_12_grp_a", "led_12_grp_b" };
static const char * const bcm6846_led_13_groups[] = { "led_13_grp" };
static const char * const bcm6846_led_14_groups[] = { "led_14_grp" };
static const char * const bcm6846_led_15_groups[] = { "led_15_grp" };
static const char * const bcm6846_led_16_groups[] = { "led_16_grp" };
static const char * const bcm6846_led_17_groups[] = { "led_17_grp" };
static const char * const bcm6846_ser_led_groups[] = { "ser_led_grp_a", "ser_led_grp_b" };
/* We use these three groups with the NAND function to get as many pins as we want */
static const char * const bcm6846_nand_groups[] = { "nand_ctrl_grp", "nand_data_grp", "nand_wp_grp" };
static const char * const bcm6846_emmc_groups[] = { "emmc_ctrl_grp" };
/* Activate SPIM with "spim_grp" and then as many selects as used with "spim_ssN_grp" groups */
static const char * const bcm6846_spim_groups[] = {
	"spim_grp_a", "spim_ss0_grp_a",	"spim_ss1_grp_a", "spim_ss2_grp_a", "spim_ss3_grp_a",
	"spim_grp_b", "spim_ss0_grp_b",	"spim_ss1_grp_b", "spim_ss2_grp_b", "spim_ss3_grp_b",
	"spim_grp_c", "spim_ss0_grp_c",	"spim_ss1_grp_c" };
static const char * const bcm6846_spis_groups[] = {
	"spis_grp_a", "spim_ss_grp_a",
	"spis_grp_b", "spis_ss_grp_b" };
static const char * const bcm6846_usb0_pwr_groups[] = { "usb0_pwr_grp" };
static const char * const bcm6846_usb1_pwr_groups[] = { "usb1_pwr_grp" };
static const char * const bcm6846_i2c_groups[] = { "i2c_grp" };
static const char * const bcm6846_rgmii_groups[] = { "rgmii_grp", "rgmii_rx_ok_grp", "rgmii_start_stop_grp" };
static const char * const bcm6846_mii_groups[] = { "mii_grp" };
static const char * const bcm6846_signal_detect_groups[] = { "signal_detect_grp_a", "signal_detect_grp_b" };
static const char * const bcm6846_one_sec_pls_groups[] = { "one_sec_pls_grp_a", "one_sec_pls_grp_b" };
static const char * const bcm6846_rogue_onu_groups[] = { "rogue_onu_grp_a", "rogue_onu_grp_b" };
static const char * const bcm6846_wan_groups[] = { "wan_mdio_grp", "wan_nco_grp",
	"wan_early_txen_grp_a", "wan_early_txen_grp_b", "wan_nco_1pps_sig_grp_a", "wan_nco_1pps_sig_grp_b" };
static const char * const bcm6846_uart0_groups[] = { "uart0_grp" };
static const char * const bcm6846_uart2_groups[] = { "uart2_grp" };

static const struct bcmbca_pinctrl_function bcm6846_pinctrl_functions[] = {
	{ "led_0", bcm6846_led_0_groups, ARRAY_SIZE(bcm6846_led_0_groups) },
	{ "led_1", bcm6846_led_1_groups, ARRAY_SIZE(bcm6846_led_1_groups) },
	{ "led_2", bcm6846_led_2_groups, ARRAY_SIZE(bcm6846_led_2_groups) },
	{ "led_3", bcm6846_led_3_groups, ARRAY_SIZE(bcm6846_led_3_groups) },
	{ "led_4", bcm6846_led_4_groups, ARRAY_SIZE(bcm6846_led_4_groups) },
	{ "led_5", bcm6846_led_5_groups, ARRAY_SIZE(bcm6846_led_5_groups) },
	{ "led_6", bcm6846_led_6_groups, ARRAY_SIZE(bcm6846_led_6_groups) },
	{ "led_7", bcm6846_led_7_groups, ARRAY_SIZE(bcm6846_led_7_groups) },
	{ "led_8", bcm6846_led_8_groups, ARRAY_SIZE(bcm6846_led_8_groups) },
	{ "led_9", bcm6846_led_9_groups, ARRAY_SIZE(bcm6846_led_9_groups) },
	{ "led_10", bcm6846_led_10_groups, ARRAY_SIZE(bcm6846_led_10_groups) },
	{ "led_11", bcm6846_led_11_groups, ARRAY_SIZE(bcm6846_led_11_groups) },
	{ "led_12", bcm6846_led_12_groups, ARRAY_SIZE(bcm6846_led_12_groups) },
	{ "led_13", bcm6846_led_13_groups, ARRAY_SIZE(bcm6846_led_13_groups) },
	{ "led_14", bcm6846_led_14_groups, ARRAY_SIZE(bcm6846_led_14_groups) },
	{ "led_15", bcm6846_led_15_groups, ARRAY_SIZE(bcm6846_led_15_groups) },
	{ "led_16", bcm6846_led_16_groups, ARRAY_SIZE(bcm6846_led_16_groups) },
	{ "led_17", bcm6846_led_17_groups, ARRAY_SIZE(bcm6846_led_17_groups) },
	{ "ser_led", bcm6846_ser_led_groups, ARRAY_SIZE(bcm6846_ser_led_groups) },
	{ "nand", bcm6846_nand_groups, ARRAY_SIZE(bcm6846_nand_groups) },
	{ "emmc", bcm6846_emmc_groups, ARRAY_SIZE(bcm6846_emmc_groups) },
	{ "spim", bcm6846_spim_groups, ARRAY_SIZE(bcm6846_spim_groups) },
	{ "usb0_pwr", bcm6846_usb0_pwr_groups, ARRAY_SIZE(bcm6846_usb0_pwr_groups) },
	{ "usb1_pwr", bcm6846_usb1_pwr_groups, ARRAY_SIZE(bcm6846_usb1_pwr_groups) },
	{ "i2c", bcm6846_i2c_groups, ARRAY_SIZE(bcm6846_i2c_groups) },
	{ "rgmii", bcm6846_rgmii_groups, ARRAY_SIZE(bcm6846_rgmii_groups) },
	{ "mii", bcm6846_mii_groups, ARRAY_SIZE(bcm6846_mii_groups) },
	{ "signal_detect", bcm6846_signal_detect_groups, ARRAY_SIZE(bcm6846_signal_detect_groups) },
	{ "one_sec_pls", bcm6846_one_sec_pls_groups, ARRAY_SIZE(bcm6846_one_sec_pls_groups) },
	{ "rogue_onu", bcm6846_rogue_onu_groups, ARRAY_SIZE(bcm6846_rogue_onu_groups) },
	{ "wan", bcm6846_wan_groups, ARRAY_SIZE(bcm6846_wan_groups) },
	{ "uart0", bcm6846_uart0_groups, ARRAY_SIZE(bcm6846_uart0_groups) },
	{ "uart2", bcm6846_uart2_groups, ARRAY_SIZE(bcm6846_uart2_groups) },
};

static const struct bcmbca_soc_info bcm6846_pinctrl_soc_info = {
	.num_pins = BCM6846_NUM_PINS,
	.groups = bcm6846_pinctrl_grps,
	.num_groups = ARRAY_SIZE(bcm6846_pinctrl_grps),
	.functions = bcm6846_pinctrl_functions,
	.num_functions = ARRAY_SIZE(bcm6846_pinctrl_functions),
};

/* BCM4908 groups and functions */

#define BCM4908_NUM_PINS 86

static const struct bcmbca_pinctrl_pin_setup bcm4908_led_0_pins_a[] = {
	{ 0, 3 },
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_led_1_pins_a[] = {
	{ 1, 3 },
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_led_2_pins_a[] = {
	{ 2, 3 },
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_led_3_pins_a[] = {
	{ 3, 3 },
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_led_4_pins_a[] = {
	{ 4, 3 },
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_led_5_pins_a[] = {
	{ 5, 3 },
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_led_6_pins_a[] = {
	{ 6, 3 },
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_led_7_pins_a[] = {
	{ 7, 3 },
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_led_8_pins_a[] = {
	{ 8, 3 },
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_led_9_pins_a[] = {
	{ 9, 3 },
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_led_10_pins_a[] = {
	{ 10, 3 },
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_led_11_pins_a[] = {
	{ 11, 3 },
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_led_12_pins_a[] = {
	{ 12, 3 },
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_led_13_pins_a[] = {
	{ 13, 3 },
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_led_14_pins_a[] = {
	{ 14, 3 },
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_led_15_pins_a[] = {
	{ 15, 3 },
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_led_16_pins_a[] = {
	{ 16, 3 },
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_led_17_pins_a[] = {
	{ 17, 3 },
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_led_18_pins_a[] = {
	{ 18, 3 },
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_led_19_pins_a[] = {
	{ 19, 3 },
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_led_20_pins_a[] = {
	{ 20, 3 },
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_led_21_pins_a[] = {
	{ 21, 3 },
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_led_22_pins_a[] = {
	{ 22, 3 },
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_led_23_pins_a[] = {
	{ 23, 3 },
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_led_24_pins_a[] = {
	{ 24, 3 },
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_led_25_pins_a[] = {
	{ 25, 3 },
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_led_26_pins_a[] = {
	{ 26, 3 },
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_led_27_pins_a[] = {
	{ 27, 3 },
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_led_28_pins_a[] = {
	{ 28, 3 },
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_led_29_pins_a[] = {
	{ 29, 3 },
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_led_30_pins_a[] = {
	{ 30, 3 },
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_led_31_pins_a[] = {
	{ 31, 3 },
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_led_10_pins_b[] = {
	{ 8, 2 },
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_led_11_pins_b[] = {
	{ 9, 2 },
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_led_12_pins_b[] = {
	{ 0, 2 },
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_led_13_pins_b[] = {
	{ 1, 2 },
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_led_31_pins_b[] = {
	{ 30, 2 },
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_hs_uart_pins[] = {
	{ 10, 0 },	/* CTS */
	{ 11, 0 },	/* RTS */
	{ 12, 0 },	/* RXD */
	{ 13, 0 },	/* TXD */
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_i2c_pins_a[] = {
	{ 18, 0 },	/* SDA */
	{ 19, 0 },	/* SCL */
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_i2c_pins_b[] = {
	{ 22, 0 },	/* SDA */
	{ 23, 0 },	/* SCL */
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_i2s_pins[] = {
	{ 27, 0 },	/* MCLK */
	{ 28, 0 },	/* LRCK */
	{ 29, 0 },	/* SDATA */
	{ 30, 0 },	/* SCLK */
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_nand_ctrl_pins[] = {
	{ 32, 0 },
	{ 33, 0 },
	{ 34, 0 },
	{ 43, 0 },
	{ 44, 0 },
	{ 45, 0 },
	{ 56, 1 },
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_nand_data_pins[] = {
	{ 35, 0 },
	{ 36, 0 },
	{ 37, 0 },
	{ 38, 0 },
	{ 39, 0 },
	{ 40, 0 },
	{ 41, 0 },
	{ 42, 0 },
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_emmc_ctrl_pins[] = {
	{ 46, 0 },
	{ 47, 0 },
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_usb0_pwr_pins[] = {
	{ 63, 0 },
	{ 64, 0 },
};

static const struct bcmbca_pinctrl_pin_setup bcm4908_usb1_pwr_pins[] = {
	{ 66, 0 },
	{ 67, 0 },
};

static const struct bcmbca_pinctrl_grp bcm4908_pinctrl_grps[] = {
	{ "led_0_grp_a", bcm4908_led_0_pins_a, ARRAY_SIZE(bcm4908_led_0_pins_a) },
	{ "led_1_grp_a", bcm4908_led_1_pins_a, ARRAY_SIZE(bcm4908_led_1_pins_a) },
	{ "led_2_grp_a", bcm4908_led_2_pins_a, ARRAY_SIZE(bcm4908_led_2_pins_a) },
	{ "led_3_grp_a", bcm4908_led_3_pins_a, ARRAY_SIZE(bcm4908_led_3_pins_a) },
	{ "led_4_grp_a", bcm4908_led_4_pins_a, ARRAY_SIZE(bcm4908_led_4_pins_a) },
	{ "led_5_grp_a", bcm4908_led_5_pins_a, ARRAY_SIZE(bcm4908_led_5_pins_a) },
	{ "led_6_grp_a", bcm4908_led_6_pins_a, ARRAY_SIZE(bcm4908_led_6_pins_a) },
	{ "led_7_grp_a", bcm4908_led_7_pins_a, ARRAY_SIZE(bcm4908_led_7_pins_a) },
	{ "led_8_grp_a", bcm4908_led_8_pins_a, ARRAY_SIZE(bcm4908_led_8_pins_a) },
	{ "led_9_grp_a", bcm4908_led_9_pins_a, ARRAY_SIZE(bcm4908_led_9_pins_a) },
	{ "led_10_grp_a", bcm4908_led_10_pins_a, ARRAY_SIZE(bcm4908_led_10_pins_a) },
	{ "led_11_grp_a", bcm4908_led_11_pins_a, ARRAY_SIZE(bcm4908_led_11_pins_a) },
	{ "led_12_grp_a", bcm4908_led_12_pins_a, ARRAY_SIZE(bcm4908_led_12_pins_a) },
	{ "led_13_grp_a", bcm4908_led_13_pins_a, ARRAY_SIZE(bcm4908_led_13_pins_a) },
	{ "led_14_grp_a", bcm4908_led_14_pins_a, ARRAY_SIZE(bcm4908_led_14_pins_a) },
	{ "led_15_grp_a", bcm4908_led_15_pins_a, ARRAY_SIZE(bcm4908_led_15_pins_a) },
	{ "led_16_grp_a", bcm4908_led_16_pins_a, ARRAY_SIZE(bcm4908_led_16_pins_a) },
	{ "led_17_grp_a", bcm4908_led_17_pins_a, ARRAY_SIZE(bcm4908_led_17_pins_a) },
	{ "led_18_grp_a", bcm4908_led_18_pins_a, ARRAY_SIZE(bcm4908_led_18_pins_a) },
	{ "led_19_grp_a", bcm4908_led_19_pins_a, ARRAY_SIZE(bcm4908_led_19_pins_a) },
	{ "led_20_grp_a", bcm4908_led_20_pins_a, ARRAY_SIZE(bcm4908_led_20_pins_a) },
	{ "led_21_grp_a", bcm4908_led_21_pins_a, ARRAY_SIZE(bcm4908_led_21_pins_a) },
	{ "led_22_grp_a", bcm4908_led_22_pins_a, ARRAY_SIZE(bcm4908_led_22_pins_a) },
	{ "led_23_grp_a", bcm4908_led_23_pins_a, ARRAY_SIZE(bcm4908_led_23_pins_a) },
	{ "led_24_grp_a", bcm4908_led_24_pins_a, ARRAY_SIZE(bcm4908_led_24_pins_a) },
	{ "led_25_grp_a", bcm4908_led_25_pins_a, ARRAY_SIZE(bcm4908_led_25_pins_a) },
	{ "led_26_grp_a", bcm4908_led_26_pins_a, ARRAY_SIZE(bcm4908_led_26_pins_a) },
	{ "led_27_grp_a", bcm4908_led_27_pins_a, ARRAY_SIZE(bcm4908_led_27_pins_a) },
	{ "led_28_grp_a", bcm4908_led_28_pins_a, ARRAY_SIZE(bcm4908_led_28_pins_a) },
	{ "led_29_grp_a", bcm4908_led_29_pins_a, ARRAY_SIZE(bcm4908_led_29_pins_a) },
	{ "led_30_grp_a", bcm4908_led_30_pins_a, ARRAY_SIZE(bcm4908_led_30_pins_a) },
	{ "led_31_grp_a", bcm4908_led_31_pins_a, ARRAY_SIZE(bcm4908_led_31_pins_a) },
	{ "led_10_grp_b", bcm4908_led_10_pins_b, ARRAY_SIZE(bcm4908_led_10_pins_b) },
	{ "led_11_grp_b", bcm4908_led_11_pins_b, ARRAY_SIZE(bcm4908_led_11_pins_b) },
	{ "led_12_grp_b", bcm4908_led_12_pins_b, ARRAY_SIZE(bcm4908_led_12_pins_b) },
	{ "led_13_grp_b", bcm4908_led_13_pins_b, ARRAY_SIZE(bcm4908_led_13_pins_b) },
	{ "led_31_grp_b", bcm4908_led_31_pins_b, ARRAY_SIZE(bcm4908_led_31_pins_b) },
	{ "hs_uart_grp", bcm4908_hs_uart_pins, ARRAY_SIZE(bcm4908_hs_uart_pins) },
	{ "i2c_grp_a", bcm4908_i2c_pins_a, ARRAY_SIZE(bcm4908_i2c_pins_a) },
	{ "i2c_grp_b", bcm4908_i2c_pins_b, ARRAY_SIZE(bcm4908_i2c_pins_b) },
	{ "i2s_grp", bcm4908_i2s_pins, ARRAY_SIZE(bcm4908_i2s_pins) },
	{ "nand_ctrl_grp", bcm4908_nand_ctrl_pins, ARRAY_SIZE(bcm4908_nand_ctrl_pins) },
	{ "nand_data_grp", bcm4908_nand_data_pins, ARRAY_SIZE(bcm4908_nand_data_pins) },
	{ "emmc_ctrl_grp", bcm4908_emmc_ctrl_pins, ARRAY_SIZE(bcm4908_emmc_ctrl_pins) },
	{ "usb0_pwr_grp", bcm4908_usb0_pwr_pins, ARRAY_SIZE(bcm4908_usb0_pwr_pins) },
	{ "usb1_pwr_grp", bcm4908_usb1_pwr_pins, ARRAY_SIZE(bcm4908_usb1_pwr_pins) },
};

static const char * const bcm4908_led_0_groups[] = { "led_0_grp_a" };
static const char * const bcm4908_led_1_groups[] = { "led_1_grp_a" };
static const char * const bcm4908_led_2_groups[] = { "led_2_grp_a" };
static const char * const bcm4908_led_3_groups[] = { "led_3_grp_a" };
static const char * const bcm4908_led_4_groups[] = { "led_4_grp_a" };
static const char * const bcm4908_led_5_groups[] = { "led_5_grp_a" };
static const char * const bcm4908_led_6_groups[] = { "led_6_grp_a" };
static const char * const bcm4908_led_7_groups[] = { "led_7_grp_a" };
static const char * const bcm4908_led_8_groups[] = { "led_8_grp_a" };
static const char * const bcm4908_led_9_groups[] = { "led_9_grp_a" };
static const char * const bcm4908_led_10_groups[] = { "led_10_grp_a", "led_10_grp_b" };
static const char * const bcm4908_led_11_groups[] = { "led_11_grp_a", "led_11_grp_b" };
static const char * const bcm4908_led_12_groups[] = { "led_12_grp_a", "led_12_grp_b" };
static const char * const bcm4908_led_13_groups[] = { "led_13_grp_a", "led_13_grp_b" };
static const char * const bcm4908_led_14_groups[] = { "led_14_grp_a" };
static const char * const bcm4908_led_15_groups[] = { "led_15_grp_a" };
static const char * const bcm4908_led_16_groups[] = { "led_16_grp_a" };
static const char * const bcm4908_led_17_groups[] = { "led_17_grp_a" };
static const char * const bcm4908_led_18_groups[] = { "led_18_grp_a" };
static const char * const bcm4908_led_19_groups[] = { "led_19_grp_a" };
static const char * const bcm4908_led_20_groups[] = { "led_20_grp_a" };
static const char * const bcm4908_led_21_groups[] = { "led_21_grp_a" };
static const char * const bcm4908_led_22_groups[] = { "led_22_grp_a" };
static const char * const bcm4908_led_23_groups[] = { "led_23_grp_a" };
static const char * const bcm4908_led_24_groups[] = { "led_24_grp_a" };
static const char * const bcm4908_led_25_groups[] = { "led_25_grp_a" };
static const char * const bcm4908_led_26_groups[] = { "led_26_grp_a" };
static const char * const bcm4908_led_27_groups[] = { "led_27_grp_a" };
static const char * const bcm4908_led_28_groups[] = { "led_28_grp_a" };
static const char * const bcm4908_led_29_groups[] = { "led_29_grp_a" };
static const char * const bcm4908_led_30_groups[] = { "led_30_grp_a" };
static const char * const bcm4908_led_31_groups[] = { "led_31_grp_a", "led_31_grp_b" };
static const char * const bcm4908_hs_uart_groups[] = { "hs_uart_grp" };
static const char * const bcm4908_i2c_groups[] = { "i2c_grp_a", "i2c_grp_b" };
static const char * const bcm4908_i2s_groups[] = { "i2s_grp" };
static const char * const bcm4908_nand_ctrl_groups[] = { "nand_ctrl_grp" };
static const char * const bcm4908_nand_data_groups[] = { "nand_data_grp" };
static const char * const bcm4908_emmc_ctrl_groups[] = { "emmc_ctrl_grp" };
static const char * const bcm4908_usb0_pwr_groups[] = { "usb0_pwr_grp" };
static const char * const bcm4908_usb1_pwr_groups[] = { "usb1_pwr_grp" };

static const struct bcmbca_pinctrl_function bcm4908_pinctrl_functions[] = {
	{ "led_0", bcm4908_led_0_groups, ARRAY_SIZE(bcm4908_led_0_groups) },
	{ "led_1", bcm4908_led_1_groups, ARRAY_SIZE(bcm4908_led_1_groups) },
	{ "led_2", bcm4908_led_2_groups, ARRAY_SIZE(bcm4908_led_2_groups) },
	{ "led_3", bcm4908_led_3_groups, ARRAY_SIZE(bcm4908_led_3_groups) },
	{ "led_4", bcm4908_led_4_groups, ARRAY_SIZE(bcm4908_led_4_groups) },
	{ "led_5", bcm4908_led_5_groups, ARRAY_SIZE(bcm4908_led_5_groups) },
	{ "led_6", bcm4908_led_6_groups, ARRAY_SIZE(bcm4908_led_6_groups) },
	{ "led_7", bcm4908_led_7_groups, ARRAY_SIZE(bcm4908_led_7_groups) },
	{ "led_8", bcm4908_led_8_groups, ARRAY_SIZE(bcm4908_led_8_groups) },
	{ "led_9", bcm4908_led_9_groups, ARRAY_SIZE(bcm4908_led_9_groups) },
	{ "led_10", bcm4908_led_10_groups, ARRAY_SIZE(bcm4908_led_10_groups) },
	{ "led_11", bcm4908_led_11_groups, ARRAY_SIZE(bcm4908_led_11_groups) },
	{ "led_12", bcm4908_led_12_groups, ARRAY_SIZE(bcm4908_led_12_groups) },
	{ "led_13", bcm4908_led_13_groups, ARRAY_SIZE(bcm4908_led_13_groups) },
	{ "led_14", bcm4908_led_14_groups, ARRAY_SIZE(bcm4908_led_14_groups) },
	{ "led_15", bcm4908_led_15_groups, ARRAY_SIZE(bcm4908_led_15_groups) },
	{ "led_16", bcm4908_led_16_groups, ARRAY_SIZE(bcm4908_led_16_groups) },
	{ "led_17", bcm4908_led_17_groups, ARRAY_SIZE(bcm4908_led_17_groups) },
	{ "led_18", bcm4908_led_18_groups, ARRAY_SIZE(bcm4908_led_18_groups) },
	{ "led_19", bcm4908_led_19_groups, ARRAY_SIZE(bcm4908_led_19_groups) },
	{ "led_20", bcm4908_led_20_groups, ARRAY_SIZE(bcm4908_led_20_groups) },
	{ "led_21", bcm4908_led_21_groups, ARRAY_SIZE(bcm4908_led_21_groups) },
	{ "led_22", bcm4908_led_22_groups, ARRAY_SIZE(bcm4908_led_22_groups) },
	{ "led_23", bcm4908_led_23_groups, ARRAY_SIZE(bcm4908_led_23_groups) },
	{ "led_24", bcm4908_led_24_groups, ARRAY_SIZE(bcm4908_led_24_groups) },
	{ "led_25", bcm4908_led_25_groups, ARRAY_SIZE(bcm4908_led_25_groups) },
	{ "led_26", bcm4908_led_26_groups, ARRAY_SIZE(bcm4908_led_26_groups) },
	{ "led_27", bcm4908_led_27_groups, ARRAY_SIZE(bcm4908_led_27_groups) },
	{ "led_28", bcm4908_led_28_groups, ARRAY_SIZE(bcm4908_led_28_groups) },
	{ "led_29", bcm4908_led_29_groups, ARRAY_SIZE(bcm4908_led_29_groups) },
	{ "led_30", bcm4908_led_30_groups, ARRAY_SIZE(bcm4908_led_30_groups) },
	{ "led_31", bcm4908_led_31_groups, ARRAY_SIZE(bcm4908_led_31_groups) },
	{ "hs_uart", bcm4908_hs_uart_groups, ARRAY_SIZE(bcm4908_hs_uart_groups) },
	{ "i2c", bcm4908_i2c_groups, ARRAY_SIZE(bcm4908_i2c_groups) },
	{ "i2s", bcm4908_i2s_groups, ARRAY_SIZE(bcm4908_i2s_groups) },
	{ "nand_ctrl", bcm4908_nand_ctrl_groups, ARRAY_SIZE(bcm4908_nand_ctrl_groups) },
	{ "nand_data", bcm4908_nand_data_groups, ARRAY_SIZE(bcm4908_nand_data_groups) },
	{ "emmc_ctrl", bcm4908_emmc_ctrl_groups, ARRAY_SIZE(bcm4908_emmc_ctrl_groups) },
	{ "usb0_pwr", bcm4908_usb0_pwr_groups, ARRAY_SIZE(bcm4908_usb0_pwr_groups) },
	{ "usb1_pwr", bcm4908_usb1_pwr_groups, ARRAY_SIZE(bcm4908_usb1_pwr_groups) },
};

static const struct bcmbca_soc_info bcm4908_pinctrl_soc_info = {
	.num_pins = BCM4908_NUM_PINS,
	.groups = bcm4908_pinctrl_grps,
	.num_groups = ARRAY_SIZE(bcm4908_pinctrl_grps),
	.functions = bcm4908_pinctrl_functions,
	.num_functions = ARRAY_SIZE(bcm4908_pinctrl_functions),
};

/*
 * Groups code
 */

static const struct pinctrl_ops bcmbca_pinctrl_ops = {
	.get_groups_count = pinctrl_generic_get_group_count,
	.get_group_name = pinctrl_generic_get_group_name,
	.get_group_pins = pinctrl_generic_get_group_pins,
	.dt_node_to_map = pinconf_generic_dt_node_to_map_group,
	.dt_free_map = pinconf_generic_dt_free_map,
};

/*
 * Functions code
 */

static int bcmbca_pinctrl_set_mux(struct pinctrl_dev *pctrl_dev,
			      unsigned int func_selector,
			      unsigned int group_selector)
{
	struct bcmbca_pinctrl *bcmbca_pinctrl = pinctrl_dev_get_drvdata(pctrl_dev);
	const struct bcmbca_pinctrl_grp *group;
	struct group_desc *group_desc;
	int i;

	group_desc = pinctrl_generic_get_group(pctrl_dev, group_selector);
	if (!group_desc)
		return -EINVAL;
	group = group_desc->data;

	guard(mutex)(&bcmbca_pinctrl->mutex);
	for (i = 0; i < group->num_pins; i++) {
		u32 lsb = 0;

		lsb |= group->pins[i].number;
		lsb |= group->pins[i].function << BCMBCA_TEST_PORT_LSB_PINMUX_DATA_SHIFT;

		writel(0x0, bcmbca_pinctrl->base + BCMBCA_TEST_PORT_BLOCK_DATA_MSB);
		writel(lsb, bcmbca_pinctrl->base + BCMBCA_TEST_PORT_BLOCK_DATA_LSB);
		writel(BCMBCA_TEST_PORT_CMD_LOAD_MUX_REG,
		       bcmbca_pinctrl->base + BCMBCA_TEST_PORT_COMMAND);
	}

	return 0;
}

static const struct pinmux_ops bcmbca_pinctrl_pmxops = {
	.get_functions_count = pinmux_generic_get_function_count,
	.get_function_name = pinmux_generic_get_function_name,
	.get_function_groups = pinmux_generic_get_function_groups,
	.set_mux = bcmbca_pinctrl_set_mux,
};

/*
 * Controller code
 */

static const struct pinctrl_desc bcmbca_pinctrl_desc = {
	.name = "bcmbca-pinctrl",
	.pctlops = &bcmbca_pinctrl_ops,
	.pmxops = &bcmbca_pinctrl_pmxops,
};

static const struct of_device_id bcmbca_pinctrl_of_match_table[] = {
	{
		.compatible = "brcm,bcm6846-pinctrl",
		.data = &bcm6846_pinctrl_soc_info,
	},
	{
		.compatible = "brcm,bcm4908-pinctrl",
		.data = &bcm4908_pinctrl_soc_info,
	},
	{ }
};

static int bcmbca_pinctrl_probe(struct platform_device *pdev)
{
	const struct bcmbca_soc_info *info;
	struct device *dev = &pdev->dev;
	struct bcmbca_pinctrl *bcmbca_pinctrl;
	struct pinctrl_desc *pctldesc;
	struct pinctrl_pin_desc *pins;
	char **pin_names;
	int i;

	info = device_get_match_data(dev);
	if (!info)
		return dev_err_probe(dev, -EINVAL, "No match data\n");

	bcmbca_pinctrl = devm_kzalloc(dev, sizeof(*bcmbca_pinctrl), GFP_KERNEL);
	if (!bcmbca_pinctrl)
		return -ENOMEM;
	pctldesc = &bcmbca_pinctrl->pctldesc;
	platform_set_drvdata(pdev, bcmbca_pinctrl);

	/* Set basic properties */

	bcmbca_pinctrl->dev = dev;

	bcmbca_pinctrl->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(bcmbca_pinctrl->base))
		return PTR_ERR(bcmbca_pinctrl->base);

	mutex_init(&bcmbca_pinctrl->mutex);

	memcpy(pctldesc, &bcmbca_pinctrl_desc, sizeof(*pctldesc));

	/* Set pinctrl properties */

	pin_names = devm_kasprintf_strarray(dev, "pin", info->num_pins);
	if (IS_ERR(pin_names))
		return PTR_ERR(pin_names);

	pins = devm_kcalloc(dev, info->num_pins, sizeof(*pins), GFP_KERNEL);
	if (!pins)
		return -ENOMEM;
	for (i = 0; i < info->num_pins; i++) {
		pins[i].number = i;
		pins[i].name = pin_names[i];
	}
	pctldesc->pins = pins;
	pctldesc->npins = info->num_pins;

	/* Register */

	bcmbca_pinctrl->pctldev = devm_pinctrl_register(dev, pctldesc, bcmbca_pinctrl);
	if (IS_ERR(bcmbca_pinctrl->pctldev))
		return dev_err_probe(dev, PTR_ERR(bcmbca_pinctrl->pctldev),
				     "Failed to register pinctrl\n");

	/* Groups */

	for (i = 0; i < info->num_groups; i++) {
		const struct bcmbca_pinctrl_grp *group = &info->groups[i];
		int *pins;
		int j;

		pins = devm_kcalloc(dev, group->num_pins, sizeof(*pins), GFP_KERNEL);
		if (!pins)
			return -ENOMEM;
		for (j = 0; j < group->num_pins; j++)
			pins[j] = group->pins[j].number;

		pinctrl_generic_add_group(bcmbca_pinctrl->pctldev, group->name,
					  pins, group->num_pins, (void *)group);
	}

	/* Functions */

	for (i = 0; i < info->num_functions; i++) {
		const struct bcmbca_pinctrl_function *function = &info->functions[i];

		pinmux_generic_add_function(bcmbca_pinctrl->pctldev,
					    function->name,
					    function->groups,
					    function->num_groups, NULL);
	}

	return 0;
}

static struct platform_driver bcmbca_pinctrl_driver = {
	.probe = bcmbca_pinctrl_probe,
	.driver = {
		.name = "bcmbca-pinctrl",
		.of_match_table = bcmbca_pinctrl_of_match_table,
	},
};

module_platform_driver(bcmbca_pinctrl_driver);

MODULE_AUTHOR("Rafał Miłecki");
MODULE_DESCRIPTION("Broadcom BCMBCA pinmux driver");
MODULE_LICENSE("GPL v2");
MODULE_DEVICE_TABLE(of, bcmbca_pinctrl_of_match_table);
