// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Min.Guo <min.guo@mediatek.com>
 * Author: Luca Leonardo Scorcia <l.scorcia@gmail.com>
 */

#include "pinctrl-mtk-mt8167.h"
#include "pinctrl-mtk-mt8516.h"
#include "pinctrl-paris.h"

#define PIN_FIELD15(_s_pin, _e_pin, _s_addr, _x_addrs, _s_bit, _x_bits)	\
	PIN_FIELD_CALC(_s_pin, _e_pin, 0, _s_addr, _x_addrs, _s_bit,	\
		       _x_bits, 15, 0)

#define PIN_FIELD16(_s_pin, _e_pin, _s_addr, _x_addrs, _s_bit, _x_bits)	\
	PIN_FIELD_CALC(_s_pin, _e_pin, 0, _s_addr, _x_addrs, _s_bit,	\
		       _x_bits, 16, 0)

#define PINS_FIELD16(_s_pin, _e_pin, _s_addr, _x_addrs, _s_bit, _x_bits)\
	PIN_FIELD_CALC(_s_pin, _e_pin, 0, _s_addr, _x_addrs, _s_bit,	\
		       _x_bits, 16, 1)

static const struct mtk_pin_field_calc mt8516_pin_dir_range[] = {
	PIN_FIELD16(0, 124, 0x000, 0x10, 0, 1),
};

static const struct mtk_pin_field_calc mt8516_pin_do_range[] = {
	PIN_FIELD16(0, 124, 0x100, 0x10, 0, 1),
};

static const struct mtk_pin_field_calc mt8516_pin_di_range[] = {
	PIN_FIELD16(0, 124, 0x200, 0x10, 0, 1),
};

static const struct mtk_pin_field_calc mt8516_pin_mode_range[] = {
	PIN_FIELD15(0, 124, 0x300, 0x10, 0, 3),
};

static const struct mtk_pin_field_calc mt8516_pin_pullen_range[] = {
	PIN_FIELD16(0, 124, 0x500, 0x10, 0, 1),
};

static const struct mtk_pin_field_calc mt8516_pin_pullsel_range[] = {
	PIN_FIELD16(0, 124, 0x600, 0x10, 0, 1),
};

static const struct mtk_pin_field_calc mt8516_pin_ies_range[] = {
	PINS_FIELD16(0, 6, 0x900, 0x10, 2, 1),
	PINS_FIELD16(7, 10, 0x900, 0x10, 3, 1),
	PINS_FIELD16(11, 13, 0x900, 0x10, 12, 1),
	PINS_FIELD16(14, 17, 0x900, 0x10, 13, 1),
	PINS_FIELD16(18, 20, 0x910, 0x10, 10, 1),
	PINS_FIELD16(21, 23, 0x900, 0x10, 13, 1),
	PINS_FIELD16(24, 25, 0x900, 0x10, 12, 1),
	PINS_FIELD16(26, 30, 0x900, 0x10, 0, 1),
	PINS_FIELD16(31, 33, 0x900, 0x10, 1, 1),
	PINS_FIELD16(34, 39, 0x900, 0x10, 2, 1),
	PIN_FIELD16(40, 40, 0x910, 0x10, 11, 1),
	PINS_FIELD16(41, 43, 0x900, 0x10, 10, 1),
	PINS_FIELD16(44, 47, 0x900, 0x10, 11, 1),
	PINS_FIELD16(48, 51, 0x900, 0x10, 14, 1),
	PINS_FIELD16(52, 53, 0x910, 0x10, 0, 1),
	PIN_FIELD16(54, 54, 0x910, 0x10, 2, 1),
	PINS_FIELD16(55, 57, 0x910, 0x10, 4, 1),
	PINS_FIELD16(58, 59, 0x900, 0x10, 15, 1),
	PINS_FIELD16(60, 61, 0x910, 0x10, 1, 1),
	PINS_FIELD16(62, 65, 0x910, 0x10, 5, 1),
	PINS_FIELD16(66, 67, 0x910, 0x10, 6, 1),
	PIN_FIELD16(68, 68, 0x930, 0x10, 2, 1),
	PIN_FIELD16(69, 69, 0x930, 0x10, 1, 1),
	PIN_FIELD16(70, 70, 0x930, 0x10, 6, 1),
	PIN_FIELD16(71, 71, 0x930, 0x10, 5, 1),
	PIN_FIELD16(72, 72, 0x930, 0x10, 4, 1),
	PIN_FIELD16(73, 73, 0x930, 0x10, 3, 1),
	PINS_FIELD16(100, 103, 0x910, 0x10, 7, 1),
	PIN_FIELD16(104, 104, 0x920, 0x10, 12, 1),
	PIN_FIELD16(105, 105, 0x920, 0x10, 11, 1),
	PIN_FIELD16(106, 106, 0x930, 0x10, 0, 1),
	PIN_FIELD16(107, 107, 0x920, 0x10, 15, 1),
	PIN_FIELD16(108, 108, 0x920, 0x10, 14, 1),
	PIN_FIELD16(109, 109, 0x920, 0x10, 13, 1),
	PIN_FIELD16(110, 110, 0x920, 0x10, 9, 1),
	PIN_FIELD16(111, 111, 0x920, 0x10, 8, 1),
	PIN_FIELD16(112, 112, 0x920, 0x10, 7, 1),
	PIN_FIELD16(113, 113, 0x920, 0x10, 6, 1),
	PIN_FIELD16(114, 114, 0x920, 0x10, 10, 1),
	PIN_FIELD16(115, 115, 0x920, 0x10, 1, 1),
	PIN_FIELD16(116, 116, 0x920, 0x10, 0, 1),
	PIN_FIELD16(117, 117, 0x920, 0x10, 5, 1),
	PIN_FIELD16(118, 118, 0x920, 0x10, 4, 1),
	PIN_FIELD16(119, 119, 0x920, 0x10, 3, 1),
	PIN_FIELD16(120, 120, 0x920, 0x10, 2, 1),
	PINS_FIELD16(121, 124, 0x910, 0x10, 9, 1),
};

static const struct mtk_pin_field_calc mt8516_pin_smt_range[] = {
	PINS_FIELD16(0, 6, 0xa00, 0x10, 2, 1),
	PINS_FIELD16(7, 10, 0xa00, 0x10, 3, 1),
	PINS_FIELD16(11, 13, 0xa00, 0x10, 12, 1),
	PINS_FIELD16(14, 17, 0xa00, 0x10, 13, 1),
	PINS_FIELD16(18, 20, 0xa10, 0x10, 10, 1),
	PINS_FIELD16(21, 23, 0xa00, 0x10, 13, 1),
	PINS_FIELD16(24, 25, 0xa00, 0x10, 12, 1),
	PINS_FIELD16(26, 30, 0xa00, 0x10, 0, 1),
	PINS_FIELD16(31, 33, 0xa00, 0x10, 1, 1),
	PINS_FIELD16(34, 39, 0xa00, 0x10, 2, 1),
	PIN_FIELD16(40, 40, 0xa10, 0x10, 11, 1),
	PINS_FIELD16(41, 43, 0xa00, 0x10, 10, 1),
	PINS_FIELD16(44, 47, 0xa00, 0x10, 11, 1),
	PINS_FIELD16(48, 51, 0xa00, 0x10, 14, 1),
	PINS_FIELD16(52, 53, 0xa10, 0x10, 0, 1),
	PIN_FIELD16(54, 54, 0xa10, 0x10, 2, 1),
	PINS_FIELD16(55, 57, 0xa10, 0x10, 4, 1),
	PINS_FIELD16(58, 59, 0xa00, 0x10, 15, 1),
	PINS_FIELD16(60, 61, 0xa10, 0x10, 1, 1),
	PINS_FIELD16(62, 65, 0xa10, 0x10, 5, 1),
	PINS_FIELD16(66, 67, 0xa10, 0x10, 6, 1),
	PIN_FIELD16(68, 68, 0xa30, 0x10, 2, 1),
	PIN_FIELD16(69, 69, 0xa30, 0x10, 1, 1),
	PIN_FIELD16(70, 70, 0xa30, 0x10, 3, 1),
	PIN_FIELD16(71, 71, 0xa30, 0x10, 4, 1),
	PIN_FIELD16(72, 72, 0xa30, 0x10, 5, 1),
	PIN_FIELD16(73, 73, 0xa30, 0x10, 6, 1),
	PINS_FIELD16(100, 103, 0xa10, 0x10, 7, 1),
	PIN_FIELD16(104, 104, 0xa20, 0x10, 12, 1),
	PIN_FIELD16(105, 105, 0xa20, 0x10, 11, 1),
	PIN_FIELD16(106, 106, 0xa20, 0x10, 13, 1),
	PIN_FIELD16(107, 107, 0xa20, 0x10, 14, 1),
	PIN_FIELD16(108, 108, 0xa20, 0x10, 15, 1),
	PIN_FIELD16(109, 109, 0xa30, 0x10, 0, 1),
	PIN_FIELD16(110, 110, 0xa20, 0x10, 9, 1),
	PIN_FIELD16(111, 111, 0xa20, 0x10, 8, 1),
	PIN_FIELD16(112, 112, 0xa20, 0x10, 7, 1),
	PIN_FIELD16(113, 113, 0xa20, 0x10, 6, 1),
	PIN_FIELD16(114, 114, 0xa20, 0x10, 10, 1),
	PIN_FIELD16(115, 115, 0xa20, 0x10, 1, 1),
	PIN_FIELD16(116, 116, 0xa20, 0x10, 0, 1),
	PIN_FIELD16(117, 117, 0xa20, 0x10, 5, 1),
	PIN_FIELD16(118, 118, 0xa20, 0x10, 4, 1),
	PIN_FIELD16(119, 119, 0xa20, 0x10, 3, 1),
	PIN_FIELD16(120, 120, 0xa20, 0x10, 2, 1),
	PINS_FIELD16(121, 124, 0xa10, 0x10, 9, 1),
};

static const struct mtk_pin_field_calc mt8516_pin_pupd_range[] = {
	/* EINT */
	PIN_FIELD16(14, 14, 0xe50, 0x10, 14, 1),	/* EINT14 */
	PIN_FIELD16(15, 15, 0xe60, 0x10, 2, 1),		/* EINT15 */
	PIN_FIELD16(16, 16, 0xe60, 0x10, 6, 1),		/* EINT16 */
	PIN_FIELD16(17, 17, 0xe60, 0x10, 10, 1),	/* EINT17 */
	PIN_FIELD16(21, 21, 0xe60, 0x10, 14, 1),	/* EINT21 */
	PIN_FIELD16(22, 22, 0xe70, 0x10, 2, 1),		/* EINT22 */
	PIN_FIELD16(23, 23, 0xe70, 0x10, 6, 1),		/* EINT23 */

	/* KPROW */
	PIN_FIELD16(40, 40, 0xe80, 0x10, 2, 1),		/* KPROW0 */
	PIN_FIELD16(41, 41, 0xe80, 0x10, 6, 1),		/* KPROW1 */

	PIN_FIELD16(42, 42, 0xe90, 0x10, 2, 1),		/* KPCOL0 */
	PIN_FIELD16(43, 43, 0xe90, 0x10, 6, 1),		/* KPCOL1 */

	/* MSDC2 */
	PIN_FIELD16(68, 68, 0xe50, 0x10, 10, 1),	/* MSDC2_CMD */
	PIN_FIELD16(69, 69, 0xe50, 0x10, 6, 1),		/* MSDC2_CLK */
	PIN_FIELD16(70, 70, 0xe40, 0x10, 6, 1),		/* MSDC2_DAT0 */
	PIN_FIELD16(71, 71, 0xe40, 0x10, 10, 1),	/* MSDC2_DAT1 */
	PIN_FIELD16(72, 72, 0xe40, 0x10, 14, 1),	/* MSDC2_DAT2 */
	PIN_FIELD16(73, 73, 0xe50, 0x10, 2, 1),		/* MSDC2_DAT3 */

	/* MSDC1 */
	PIN_FIELD16(104, 104, 0xe40, 0x10, 2, 1),	/* MSDC1_CMD */
	PIN_FIELD16(105, 105, 0xe30, 0x10, 14, 1),	/* MSDC1_CLK */
	PIN_FIELD16(106, 106, 0xe20, 0x10, 14, 1),	/* MSDC1_DAT0 */
	PIN_FIELD16(107, 107, 0xe30, 0x10, 2, 1),	/* MSDC1_DAT1 */
	PIN_FIELD16(108, 108, 0xe30, 0x10, 6, 1),	/* MSDC1_DAT2 */
	PIN_FIELD16(109, 109, 0xe30, 0x10, 10, 1),	/* MSDC1_DAT3 */

	/* MSDC0 */
	PIN_FIELD16(110, 110, 0xe10, 0x10, 14, 1),	/* MSDC0_DAT7 */
	PIN_FIELD16(111, 111, 0xe10, 0x10, 10, 1),	/* MSDC0_DAT6 */
	PIN_FIELD16(112, 112, 0xe10, 0x10, 6, 1),	/* MSDC0_DAT5 */
	PIN_FIELD16(113, 113, 0xe10, 0x10, 2, 1),	/* MSDC0_DAT4 */
	PIN_FIELD16(114, 114, 0xe20, 0x10, 10, 1),	/* MSDC0_RSTB */
	PIN_FIELD16(115, 115, 0xe20, 0x10, 2, 1),	/* MSDC0_CMD */
	PIN_FIELD16(116, 116, 0xe20, 0x10, 6, 1),	/* MSDC0_CLK */
	PIN_FIELD16(117, 117, 0xe00, 0x10, 14, 1),	/* MSDC0_DAT3 */
	PIN_FIELD16(118, 118, 0xe00, 0x10, 10, 1),	/* MSDC0_DAT2 */
	PIN_FIELD16(119, 119, 0xe00, 0x10, 6, 1),	/* MSDC0_DAT1 */
	PIN_FIELD16(120, 120, 0xe00, 0x10, 2, 1),	/* MSDC0_DAT0 */
};

static const struct mtk_pin_field_calc mt8516_pin_r0_range[] = {
	/* EINT */
	PIN_FIELD16(14, 14, 0xe50, 0x10, 12, 1),	/* EINT14 */
	PIN_FIELD16(15, 15, 0xe60, 0x10, 0, 1),		/* EINT15 */
	PIN_FIELD16(16, 16, 0xe60, 0x10, 4, 1),		/* EINT16 */
	PIN_FIELD16(17, 17, 0xe60, 0x10, 8, 1),		/* EINT17 */
	PIN_FIELD16(21, 21, 0xe60, 0x10, 12, 1),	/* EINT21 */
	PIN_FIELD16(22, 22, 0xe70, 0x10, 0, 1),		/* EINT22 */
	PIN_FIELD16(23, 23, 0xe70, 0x10, 4, 1),		/* EINT23 */

	/* KPROW */
	PIN_FIELD16(40, 40, 0xe80, 0x10, 0, 1),		/* KPROW0 */
	PIN_FIELD16(41, 41, 0xe80, 0x10, 4, 1),		/* KPROW1 */
	PIN_FIELD16(42, 42, 0xe90, 0x10, 0, 1),		/* KPCOL0 */
	PIN_FIELD16(43, 43, 0xe90, 0x10, 4, 1),		/* KPCOL1 */

	/* MSDC2 */
	PIN_FIELD16(68, 68, 0xe50, 0x10, 8, 1),		/* MSDC2_CMD */
	PIN_FIELD16(69, 69, 0xe50, 0x10, 4, 1),		/* MSDC2_CLK */
	PIN_FIELD16(70, 70, 0xe40, 0x10, 4, 1),		/* MSDC2_DAT0 */
	PIN_FIELD16(71, 71, 0xe40, 0x10, 8, 1),		/* MSDC2_DAT1 */
	PIN_FIELD16(72, 72, 0xe40, 0x10, 12, 1),	/* MSDC2_DAT2 */
	PIN_FIELD16(73, 73, 0xe50, 0x10, 0, 1),		/* MSDC2_DAT3 */

	/* MSDC1 */
	PIN_FIELD16(104, 104, 0xe40, 0x10, 0, 1),	/* MSDC1_CMD */
	PIN_FIELD16(105, 105, 0xe30, 0x10, 12, 1),	/* MSDC1_CLK */
	PIN_FIELD16(106, 106, 0xe20, 0x10, 12, 1),	/* MSDC1_DAT0 */
	PIN_FIELD16(107, 107, 0xe30, 0x10, 0, 1),	/* MSDC1_DAT1 */
	PIN_FIELD16(108, 108, 0xe30, 0x10, 4, 1),	/* MSDC1_DAT2 */
	PIN_FIELD16(109, 109, 0xe30, 0x10, 8, 1),	/* MSDC1_DAT3 */

	/* MSDC0 */
	PIN_FIELD16(110, 110, 0xe10, 0x10, 12, 1),	/* MSDC0_DAT7 */
	PIN_FIELD16(111, 111, 0xe10, 0x10, 8, 1),	/* MSDC0_DAT6 */
	PIN_FIELD16(112, 112, 0xe10, 0x10, 4, 1),	/* MSDC0_DAT5 */
	PIN_FIELD16(113, 113, 0xe10, 0x10, 0, 1),	/* MSDC0_DAT4 */
	PIN_FIELD16(114, 114, 0xe20, 0x10, 8, 1),	/* MSDC0_RSTB */
	PIN_FIELD16(115, 115, 0xe20, 0x10, 0, 1),	/* MSDC0_CMD */
	PIN_FIELD16(116, 116, 0xe20, 0x10, 4, 1),	/* MSDC0_CLK */
	PIN_FIELD16(117, 117, 0xe00, 0x10, 12, 1),	/* MSDC0_DAT3 */
	PIN_FIELD16(118, 118, 0xe00, 0x10, 8, 1),	/* MSDC0_DAT2 */
	PIN_FIELD16(119, 119, 0xe00, 0x10, 4, 1),	/* MSDC0_DAT1 */
	PIN_FIELD16(120, 120, 0xe00, 0x10, 0, 1),	/* MSDC0_DAT0 */
};

static const struct mtk_pin_field_calc mt8516_pin_r1_range[] = {
	/* EINT */
	PIN_FIELD16(14, 14, 0xe50, 0x10, 13, 1),	/* EINT14 */
	PIN_FIELD16(15, 15, 0xe60, 0x10, 1, 1),		/* EINT15 */
	PIN_FIELD16(16, 16, 0xe60, 0x10, 5, 1),		/* EINT16 */
	PIN_FIELD16(17, 17, 0xe60, 0x10, 9, 1),		/* EINT17 */
	PIN_FIELD16(21, 21, 0xe60, 0x10, 13, 1),	/* EINT21 */
	PIN_FIELD16(22, 22, 0xe70, 0x10, 1, 1),		/* EINT22 */
	PIN_FIELD16(23, 23, 0xe70, 0x10, 5, 1),		/* EINT23 */

	/* KPROW */
	PIN_FIELD16(40, 40, 0xe80, 0x10, 1, 1),		/* KPROW0 */
	PIN_FIELD16(41, 41, 0xe80, 0x10, 5, 1),		/* KPROW1 */
	PIN_FIELD16(42, 42, 0xe90, 0x10, 1, 1),		/* KPCOL0 */
	PIN_FIELD16(43, 43, 0xe90, 0x10, 5, 1),		/* KPCOL1 */

	/* MSDC2 */
	PIN_FIELD16(68, 68, 0xe50, 0x10, 9, 1),		/* MSDC2_CMD */
	PIN_FIELD16(69, 69, 0xe50, 0x10, 5, 1),		/* MSDC2_CLK */
	PIN_FIELD16(70, 70, 0xe40, 0x10, 5, 1),		/* MSDC2_DAT0 */
	PIN_FIELD16(71, 71, 0xe40, 0x10, 9, 1),		/* MSDC2_DAT1 */
	PIN_FIELD16(72, 72, 0xe40, 0x10, 13, 1),	/* MSDC2_DAT2 */
	PIN_FIELD16(73, 73, 0xe50, 0x10, 1, 1),		/* MSDC2_DAT3 */

	/* MSDC1 */
	PIN_FIELD16(104, 104, 0xe40, 0x10, 1, 1),	/* MSDC1_CMD */
	PIN_FIELD16(105, 105, 0xe30, 0x10, 13, 1),	/* MSDC1_CLK */
	PIN_FIELD16(106, 106, 0xe20, 0x10, 13, 1),	/* MSDC1_DAT0 */
	PIN_FIELD16(107, 107, 0xe30, 0x10, 1, 1),	/* MSDC1_DAT1 */
	PIN_FIELD16(108, 108, 0xe30, 0x10, 5, 1),	/* MSDC1_DAT2 */
	PIN_FIELD16(109, 109, 0xe30, 0x10, 9, 1),	/* MSDC1_DAT3 */

	/* MSDC0 */
	PIN_FIELD16(110, 110, 0xe10, 0x10, 13, 1),	/* MSDC0_DAT7 */
	PIN_FIELD16(111, 111, 0xe10, 0x10, 9, 1),	/* MSDC0_DAT6 */
	PIN_FIELD16(112, 112, 0xe10, 0x10, 5, 1),	/* MSDC0_DAT5 */
	PIN_FIELD16(113, 113, 0xe10, 0x10, 1, 1),	/* MSDC0_DAT4 */
	PIN_FIELD16(114, 114, 0xe20, 0x10, 9, 1),	/* MSDC0_RSTB */
	PIN_FIELD16(115, 115, 0xe20, 0x10, 1, 1),	/* MSDC0_CMD */
	PIN_FIELD16(116, 116, 0xe20, 0x10, 5, 1),	/* MSDC0_CLK */
	PIN_FIELD16(117, 117, 0xe00, 0x10, 13, 1),	/* MSDC0_DAT3 */
	PIN_FIELD16(118, 118, 0xe00, 0x10, 9, 1),	/* MSDC0_DAT2 */
	PIN_FIELD16(119, 119, 0xe00, 0x10, 5, 1),	/* MSDC0_DAT1 */
	PIN_FIELD16(120, 120, 0xe00, 0x10, 1, 1),	/* MSDC0_DAT0 */
};

static const struct mtk_pin_field_calc mt8516_pin_drv_range[] = {
	PINS_FIELD16(0, 4, 0xd00, 0x10, 0, 2),
	PINS_FIELD16(5, 10, 0xd00, 0x10, 4, 2),
	PINS_FIELD16(11, 13, 0xd00, 0x10, 8, 2),
	PINS_FIELD16(14, 17, 0xd00, 0x10, 12, 2),
	PINS_FIELD16(18, 20, 0xd10, 0x10, 0, 2),
	PINS_FIELD16(21, 23, 0xd00, 0x10, 12, 2),
	PINS_FIELD16(24, 25, 0xd00, 0x10, 8, 2),
	PINS_FIELD16(26, 30, 0xd10, 0x10, 4, 2),
	PINS_FIELD16(31, 33, 0xd10, 0x10, 8, 2),
	PINS_FIELD16(34, 35, 0xd10, 0x10, 12, 2),
	PINS_FIELD16(36, 39, 0xd20, 0x10, 0, 2),
	PIN_FIELD16(40, 40, 0xd20, 0x10, 4, 2),
	PINS_FIELD16(41, 43, 0xd20, 0x10, 8, 2),
	PINS_FIELD16(44, 47, 0xd20, 0x10, 12, 2),
	PINS_FIELD16(48, 51, 0xd30, 0x10, 12, 2),

	PIN_FIELD16(54, 54, 0xd30, 0x10, 8, 2),
	PINS_FIELD16(55, 57, 0xd30, 0x10, 0, 2),

	PINS_FIELD16(62, 67, 0xd40, 0x10, 8, 2),
	PIN_FIELD16(68, 68, 0xd40, 0x10, 12, 2),
	PIN_FIELD16(69, 69, 0xd50, 0x10, 0, 2),
	PINS_FIELD16(70, 73, 0xd50, 0x10, 4, 2),

	PINS_FIELD16(100, 103, 0xd50, 0x10, 8, 2),
	PIN_FIELD16(104, 104, 0xd50, 0x10, 12, 2),
	PIN_FIELD16(105, 105, 0xd60, 0x10, 0, 2),
	PINS_FIELD16(106, 109, 0xd60, 0x10, 4, 2),
	PINS_FIELD16(110, 113, 0xd70, 0x10, 0, 2),
	PIN_FIELD16(114, 114, 0xd70, 0x10, 4, 2),
	PIN_FIELD16(115, 115, 0xd60, 0x10, 12, 2),
	PIN_FIELD16(116, 116, 0xd60, 0x10, 8, 2),
	PINS_FIELD16(117, 120, 0xd70, 0x10, 0, 2),
};

static const struct mtk_pin_field_calc mt8516_pin_sr_range[] = {
	PINS_FIELD16(0, 4, 0xd00, 0x10, 3, 1),
	PINS_FIELD16(5, 10, 0xd00, 0x10, 7, 1),
	PINS_FIELD16(11, 13, 0xd00, 0x10, 11, 1),
	PINS_FIELD16(14, 17, 0xd00, 0x10, 15, 1),
	PINS_FIELD16(18, 20, 0xd10, 0x10, 3, 1),
	PINS_FIELD16(21, 23, 0xd00, 0x10, 15, 1),
	PINS_FIELD16(24, 25, 0xd00, 0x10, 11, 1),
	PINS_FIELD16(26, 30, 0xd10, 0x10, 7, 1),
	PINS_FIELD16(31, 33, 0xd10, 0x10, 11, 1),
	PINS_FIELD16(34, 35, 0xd10, 0x10, 15, 1),
	PINS_FIELD16(36, 39, 0xd20, 0x10, 3, 1),
	PIN_FIELD16(40, 40, 0xd20, 0x10, 7, 1),
	PINS_FIELD16(41, 43, 0xd20, 0x10, 11, 1),
	PINS_FIELD16(44, 47, 0xd20, 0x10, 15, 1),
	PINS_FIELD16(48, 51, 0xd30, 0x10, 15, 1),

	PIN_FIELD16(54, 54, 0xd30, 0x10, 11, 1),
	PINS_FIELD16(55, 57, 0xd30, 0x10, 3, 1),

	PINS_FIELD16(62, 67, 0xd40, 0x10, 11, 1),
	PIN_FIELD16(68, 68, 0xd40, 0x10, 15, 1),
	PIN_FIELD16(69, 69, 0xd50, 0x10, 3, 1),
	PINS_FIELD16(70, 73, 0xd50, 0x10, 7, 1),

	PINS_FIELD16(100, 103, 0xd50, 0x10, 11, 1),
	PIN_FIELD16(104, 104, 0xd50, 0x10, 15, 1),
	PIN_FIELD16(105, 105, 0xd60, 0x10, 3, 1),
	PINS_FIELD16(106, 109, 0xd60, 0x10, 7, 1),
	PINS_FIELD16(110, 113, 0xd70, 0x10, 3, 1),
	PIN_FIELD16(114, 114, 0xd70, 0x10, 7, 1),
	PIN_FIELD16(115, 115, 0xd60, 0x10, 15, 1),
	PIN_FIELD16(116, 116, 0xd60, 0x10, 11, 1),
	PINS_FIELD16(117, 120, 0xd70, 0x10, 3, 1),
};

static const struct mtk_pin_reg_calc mt8516_reg_cals[PINCTRL_PIN_REG_MAX] = {
	[PINCTRL_PIN_REG_MODE] = MTK_RANGE(mt8516_pin_mode_range),
	[PINCTRL_PIN_REG_DIR] = MTK_RANGE(mt8516_pin_dir_range),
	[PINCTRL_PIN_REG_DI] = MTK_RANGE(mt8516_pin_di_range),
	[PINCTRL_PIN_REG_DO] = MTK_RANGE(mt8516_pin_do_range),
	[PINCTRL_PIN_REG_SR] = MTK_RANGE(mt8516_pin_sr_range),
	[PINCTRL_PIN_REG_SMT] = MTK_RANGE(mt8516_pin_smt_range),
	[PINCTRL_PIN_REG_DRV] = MTK_RANGE(mt8516_pin_drv_range),
	[PINCTRL_PIN_REG_PUPD] = MTK_RANGE(mt8516_pin_pupd_range),
	[PINCTRL_PIN_REG_R0] = MTK_RANGE(mt8516_pin_r0_range),
	[PINCTRL_PIN_REG_R1] = MTK_RANGE(mt8516_pin_r1_range),
	[PINCTRL_PIN_REG_IES] = MTK_RANGE(mt8516_pin_ies_range),
	[PINCTRL_PIN_REG_PULLEN] = MTK_RANGE(mt8516_pin_pullen_range),
	[PINCTRL_PIN_REG_PULLSEL] = MTK_RANGE(mt8516_pin_pullsel_range),
};

static const struct mtk_eint_hw mt8516_eint_hw = {
	.port_mask = 7,
	.ports     = 6,
	.ap_num    = 169,
	.db_cnt    = 64,
	.db_time   = debounce_time_mt6795,
};

static const unsigned int mt8516_pull_type[] = {
	MTK_PULL_PULLSEL_TYPE,/*0*/		MTK_PULL_PULLSEL_TYPE,/*1*/
	MTK_PULL_PULLSEL_TYPE,/*2*/		MTK_PULL_PULLSEL_TYPE,/*3*/
	MTK_PULL_PULLSEL_TYPE,/*4*/		MTK_PULL_PULLSEL_TYPE,/*5*/
	MTK_PULL_PULLSEL_TYPE,/*6*/		MTK_PULL_PULLSEL_TYPE,/*7*/
	MTK_PULL_PULLSEL_TYPE,/*8*/		MTK_PULL_PULLSEL_TYPE,/*9*/
	MTK_PULL_PULLSEL_TYPE,/*10*/		MTK_PULL_PULLSEL_TYPE,/*11*/
	MTK_PULL_PULLSEL_TYPE,/*12*/		MTK_PULL_PULLSEL_TYPE,/*13*/
	MTK_PULL_PUPD_R1R0_TYPE,/*14*/		MTK_PULL_PUPD_R1R0_TYPE,/*15*/
	MTK_PULL_PUPD_R1R0_TYPE,/*16*/		MTK_PULL_PUPD_R1R0_TYPE,/*17*/
	MTK_PULL_PULLSEL_TYPE,/*18*/		MTK_PULL_PULLSEL_TYPE,/*19*/
	MTK_PULL_PULLSEL_TYPE,/*20*/		MTK_PULL_PUPD_R1R0_TYPE,/*21*/
	MTK_PULL_PUPD_R1R0_TYPE,/*22*/		MTK_PULL_PUPD_R1R0_TYPE,/*23*/
	MTK_PULL_PULLSEL_TYPE,/*24*/		MTK_PULL_PULLSEL_TYPE,/*25*/
	MTK_PULL_PULLSEL_TYPE,/*26*/		MTK_PULL_PULLSEL_TYPE,/*27*/
	MTK_PULL_PULLSEL_TYPE,/*28*/		MTK_PULL_PULLSEL_TYPE,/*29*/
	MTK_PULL_PULLSEL_TYPE,/*30*/		MTK_PULL_PULLSEL_TYPE,/*31*/
	MTK_PULL_PULLSEL_TYPE,/*32*/		MTK_PULL_PULLSEL_TYPE,/*33*/
	MTK_PULL_PULLSEL_TYPE,/*34*/		MTK_PULL_PULLSEL_TYPE,/*35*/
	MTK_PULL_PULLSEL_TYPE,/*36*/		MTK_PULL_PULLSEL_TYPE,/*37*/
	MTK_PULL_PULLSEL_TYPE,/*38*/		MTK_PULL_PULLSEL_TYPE,/*39*/
	MTK_PULL_PUPD_R1R0_TYPE,/*40*/		MTK_PULL_PUPD_R1R0_TYPE,/*41*/
	MTK_PULL_PUPD_R1R0_TYPE,/*42*/		MTK_PULL_PUPD_R1R0_TYPE,/*43*/
	MTK_PULL_PULLSEL_TYPE,/*44*/		MTK_PULL_PULLSEL_TYPE,/*45*/
	MTK_PULL_PULLSEL_TYPE,/*46*/		MTK_PULL_PULLSEL_TYPE,/*47*/
	MTK_PULL_PULLSEL_TYPE,/*48*/		MTK_PULL_PULLSEL_TYPE,/*49*/
	MTK_PULL_PULLSEL_TYPE,/*50*/		MTK_PULL_PULLSEL_TYPE,/*51*/
	MTK_PULL_PULLSEL_TYPE,/*52*/		MTK_PULL_PULLSEL_TYPE,/*53*/
	MTK_PULL_PULLSEL_TYPE,/*54*/		MTK_PULL_PULLSEL_TYPE,/*55*/
	MTK_PULL_PULLSEL_TYPE,/*56*/		MTK_PULL_PULLSEL_TYPE,/*57*/
	MTK_PULL_PULLSEL_TYPE,/*58*/		MTK_PULL_PULLSEL_TYPE,/*59*/
	MTK_PULL_PULLSEL_TYPE,/*60*/		MTK_PULL_PULLSEL_TYPE,/*61*/
	MTK_PULL_PULLSEL_TYPE,/*62*/		MTK_PULL_PULLSEL_TYPE,/*63*/
	MTK_PULL_PULLSEL_TYPE,/*64*/		MTK_PULL_PULLSEL_TYPE,/*65*/
	MTK_PULL_PULLSEL_TYPE,/*66*/		MTK_PULL_PULLSEL_TYPE,/*67*/
	MTK_PULL_PUPD_R1R0_TYPE,/*68*/		MTK_PULL_PUPD_R1R0_TYPE,/*69*/
	MTK_PULL_PUPD_R1R0_TYPE,/*70*/		MTK_PULL_PUPD_R1R0_TYPE,/*71*/
	MTK_PULL_PUPD_R1R0_TYPE,/*72*/		MTK_PULL_PUPD_R1R0_TYPE,/*73*/
	MTK_PULL_PULLSEL_TYPE,/*74*/		MTK_PULL_PULLSEL_TYPE,/*75*/
	MTK_PULL_PULLSEL_TYPE,/*76*/		MTK_PULL_PULLSEL_TYPE,/*77*/
	MTK_PULL_PULLSEL_TYPE,/*78*/		MTK_PULL_PULLSEL_TYPE,/*79*/
	MTK_PULL_PULLSEL_TYPE,/*80*/		MTK_PULL_PULLSEL_TYPE,/*81*/
	MTK_PULL_PULLSEL_TYPE,/*82*/		MTK_PULL_PULLSEL_TYPE,/*83*/
	MTK_PULL_PULLSEL_TYPE,/*84*/		MTK_PULL_PULLSEL_TYPE,/*85*/
	MTK_PULL_PULLSEL_TYPE,/*86*/		MTK_PULL_PULLSEL_TYPE,/*87*/
	MTK_PULL_PULLSEL_TYPE,/*88*/		MTK_PULL_PULLSEL_TYPE,/*89*/
	MTK_PULL_PULLSEL_TYPE,/*90*/		MTK_PULL_PULLSEL_TYPE,/*91*/
	MTK_PULL_PULLSEL_TYPE,/*92*/		MTK_PULL_PULLSEL_TYPE,/*93*/
	MTK_PULL_PULLSEL_TYPE,/*94*/		MTK_PULL_PULLSEL_TYPE,/*95*/
	MTK_PULL_PULLSEL_TYPE,/*96*/		MTK_PULL_PULLSEL_TYPE,/*97*/
	MTK_PULL_PULLSEL_TYPE,/*98*/		MTK_PULL_PULLSEL_TYPE,/*99*/
	MTK_PULL_PULLSEL_TYPE,/*100*/		MTK_PULL_PULLSEL_TYPE,/*101*/
	MTK_PULL_PULLSEL_TYPE,/*102*/		MTK_PULL_PULLSEL_TYPE,/*103*/
	MTK_PULL_PUPD_R1R0_TYPE,/*104*/		MTK_PULL_PUPD_R1R0_TYPE,/*105*/
	MTK_PULL_PUPD_R1R0_TYPE,/*106*/		MTK_PULL_PUPD_R1R0_TYPE,/*107*/
	MTK_PULL_PUPD_R1R0_TYPE,/*108*/		MTK_PULL_PUPD_R1R0_TYPE,/*109*/
	MTK_PULL_PUPD_R1R0_TYPE,/*110*/		MTK_PULL_PUPD_R1R0_TYPE,/*111*/
	MTK_PULL_PUPD_R1R0_TYPE,/*112*/		MTK_PULL_PUPD_R1R0_TYPE,/*113*/
	MTK_PULL_PUPD_R1R0_TYPE,/*114*/		MTK_PULL_PUPD_R1R0_TYPE,/*115*/
	MTK_PULL_PUPD_R1R0_TYPE,/*116*/		MTK_PULL_PUPD_R1R0_TYPE,/*117*/
	MTK_PULL_PUPD_R1R0_TYPE,/*118*/		MTK_PULL_PUPD_R1R0_TYPE,/*119*/
	MTK_PULL_PUPD_R1R0_TYPE,/*120*/		MTK_PULL_PULLSEL_TYPE,/*121*/
	MTK_PULL_PULLSEL_TYPE,/*122*/		MTK_PULL_PULLSEL_TYPE,/*123*/
	MTK_PULL_PULLSEL_TYPE,/*124*/
};

static const struct mtk_pin_soc mt8167_pinctrl_data = {
	.reg_cal = mt8516_reg_cals,
	.pins = mtk_pins_mt8167,
	.npins = ARRAY_SIZE(mtk_pins_mt8167),
	.ngrps = ARRAY_SIZE(mtk_pins_mt8167),
	.nfuncs = 8,
	.eint_hw = &mt8516_eint_hw,
	.gpio_m = 0,
	.ies_present = true,
	.base_names = mtk_default_register_base_names,
	.nbase_names = ARRAY_SIZE(mtk_default_register_base_names),
	.pull_type = mt8516_pull_type,
	.bias_disable_set = mtk_pinconf_bias_disable_set_rev1,
	.bias_disable_get = mtk_pinconf_bias_disable_get_rev1,
	.bias_set = mtk_pinconf_bias_set_rev1,
	.bias_get = mtk_pinconf_bias_get_rev1,
	.bias_set_combo = mtk_pinconf_bias_set_combo,
	.bias_get_combo = mtk_pinconf_bias_get_combo,
	.drive_set = mtk_pinconf_drive_set_rev1,
	.drive_get = mtk_pinconf_drive_get_rev1,
	.adv_pull_get = mtk_pinconf_adv_pull_get,
	.adv_pull_set = mtk_pinconf_adv_pull_set,
};

static const struct mtk_pin_soc mt8516_pinctrl_data = {
	.reg_cal = mt8516_reg_cals,
	.pins = mtk_pins_mt8516,
	.npins = ARRAY_SIZE(mtk_pins_mt8516),
	.ngrps = ARRAY_SIZE(mtk_pins_mt8516),
	.nfuncs = 8,
	.eint_hw = &mt8516_eint_hw,
	.gpio_m = 0,
	.ies_present = true,
	.base_names = mtk_default_register_base_names,
	.nbase_names = ARRAY_SIZE(mtk_default_register_base_names),
	.pull_type = mt8516_pull_type,
	.bias_disable_set = mtk_pinconf_bias_disable_set_rev1,
	.bias_disable_get = mtk_pinconf_bias_disable_get_rev1,
	.bias_set = mtk_pinconf_bias_set_rev1,
	.bias_get = mtk_pinconf_bias_get_rev1,
	.bias_set_combo = mtk_pinconf_bias_set_combo,
	.bias_get_combo = mtk_pinconf_bias_get_combo,
	.drive_set = mtk_pinconf_drive_set_rev1,
	.drive_get = mtk_pinconf_drive_get_rev1,
	.adv_pull_get = mtk_pinconf_adv_pull_get,
	.adv_pull_set = mtk_pinconf_adv_pull_set,
};

static const struct of_device_id mt8516_pinctrl_of_match[] = {
	{ .compatible = "mediatek,mt8167-pinctrl", .data = &mt8167_pinctrl_data },
	{ .compatible = "mediatek,mt8516-pinctrl", .data = &mt8516_pinctrl_data },
	{}
};
MODULE_DEVICE_TABLE(of, mt8516_pinctrl_of_match);

static struct platform_driver mt8516_pinctrl_driver = {
	.driver = {
		.name = "mediatek-mt8516-pinctrl",
		.of_match_table = mt8516_pinctrl_of_match,
		.pm = pm_sleep_ptr(&mtk_paris_pinctrl_pm_ops),
	},
	.probe = mtk_paris_pinctrl_probe,
};

static int __init mt8516_pinctrl_init(void)
{
	return platform_driver_register(&mt8516_pinctrl_driver);
}
arch_initcall(mt8516_pinctrl_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MediaTek MT8516/MT8167 Pinctrl Driver");
