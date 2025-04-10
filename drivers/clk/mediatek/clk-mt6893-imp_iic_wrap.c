// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021 MediaTek Inc.
 * Copyright (c) 2025 Collabora Ltd
 *                    AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>
 */

#include <linux/clk-provider.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>

#include <dt-bindings/clock/mediatek,mt6893-clk.h>

#include "clk-gate.h"
#include "clk-mtk.h"

static const struct mtk_gate_regs imp_iic_wrap_cg_regs = {
	.set_ofs = 0xe08,
	.clr_ofs = 0xe04,
	.sta_ofs = 0xe00,
};

#define GATE_IMP_IIC_WRAP(_id, _name, _parent, _shift)				\
	GATE_MTK_FLAGS(_id, _name, _parent, &imp_iic_wrap_cg_regs, _shift,	\
		&mtk_clk_gate_ops_setclr, CLK_OPS_PARENT_ENABLE)

static const struct mtk_gate imp_iic_wrap_c_clks[] = {
	GATE_IMP_IIC_WRAP(CLK_IMP_IIC_WRAP_C_AP_I2C0, "imp_iic_wrap_c_ap_i2c0", "i2c_sel", 0),
	GATE_IMP_IIC_WRAP(CLK_IMP_IIC_WRAP_C_AP_I2C10, "imp_iic_wrap_c_ap_i2c10", "i2c_sel", 1),
	GATE_IMP_IIC_WRAP(CLK_IMP_IIC_WRAP_C_AP_I2C11, "imp_iic_wrap_c_ap_i2c11", "i2c_sel", 2),
	GATE_IMP_IIC_WRAP(CLK_IMP_IIC_WRAP_C_AP_I2C12, "imp_iic_wrap_c_ap_i2c12", "i2c_sel", 3),
	GATE_IMP_IIC_WRAP(CLK_IMP_IIC_WRAP_C_AP_I2C13, "imp_iic_wrap_c_ap_i2c13", "i2c_sel", 4),
};

static const struct mtk_gate imp_iic_wrap_e_clks[] = {
	GATE_IMP_IIC_WRAP(CLK_IMP_IIC_WRAP_E_AP_I2C3, "imp_iic_wrap_e_ap_i2c3", "i2c_sel", 0),
	GATE_IMP_IIC_WRAP(CLK_IMP_IIC_WRAP_E_AP_I2C9, "imp_iic_wrap_e_ap_i2c9", "i2c_sel", 1),
};

static const struct mtk_gate imp_iic_wrap_n_clks[] = {
	GATE_IMP_IIC_WRAP(CLK_IMP_IIC_WRAP_N_AP_I2C5, "imp_iic_wrap_n_ap_i2c5", "i2c_sel", 0),
	GATE_IMP_IIC_WRAP(CLK_IMP_IIC_WRAP_N_AP_I2C6, "imp_iic_wrap_n_ap_i2c6", "i2c_sel", 1),
};

static const struct mtk_gate imp_iic_wrap_s_clks[] = {
	GATE_IMP_IIC_WRAP(CLK_IMP_IIC_WRAP_S_AP_I2C1, "imp_iic_wrap_s_ap_i2c1", "i2c_sel", 0),
	GATE_IMP_IIC_WRAP(CLK_IMP_IIC_WRAP_S_AP_I2C2, "imp_iic_wrap_s_ap_i2c2", "i2c_sel", 1),
	GATE_IMP_IIC_WRAP(CLK_IMP_IIC_WRAP_S_AP_I2C4, "imp_iic_wrap_s_ap_i2c4", "i2c_sel", 2),
	GATE_IMP_IIC_WRAP(CLK_IMP_IIC_WRAP_S_AP_I2C7, "imp_iic_wrap_s_ap_i2c7", "i2c_sel", 3),
	GATE_IMP_IIC_WRAP(CLK_IMP_IIC_WRAP_S_AP_I2C8, "imp_iic_wrap_s_ap_i2c8", "i2c_sel", 4),
};

static const struct mtk_clk_desc imp_iic_wrap_c_desc = {
	.clks = imp_iic_wrap_c_clks,
	.num_clks = ARRAY_SIZE(imp_iic_wrap_c_clks),
};

static const struct mtk_clk_desc imp_iic_wrap_e_desc = {
	.clks = imp_iic_wrap_e_clks,
	.num_clks = ARRAY_SIZE(imp_iic_wrap_e_clks),
};

static const struct mtk_clk_desc imp_iic_wrap_n_desc = {
	.clks = imp_iic_wrap_n_clks,
	.num_clks = ARRAY_SIZE(imp_iic_wrap_n_clks),
};

static const struct mtk_clk_desc imp_iic_wrap_s_desc = {
	.clks = imp_iic_wrap_s_clks,
	.num_clks = ARRAY_SIZE(imp_iic_wrap_s_clks),
};

static const struct of_device_id of_match_clk_mt6893_imp_iic_wrap[] = {
	{ .compatible = "mediatek,mt6893-imp-iic-wrap-c", .data = &imp_iic_wrap_c_desc },
	{ .compatible = "mediatek,mt6893-imp-iic-wrap-e", .data = &imp_iic_wrap_e_desc },
	{ .compatible = "mediatek,mt6893-imp-iic-wrap-n", .data = &imp_iic_wrap_n_desc },
	{ .compatible = "mediatek,mt6893-imp-iic-wrap-s", .data = &imp_iic_wrap_s_desc },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6893_imp_iic_wrap);

static struct platform_driver clk_mt6893_imp_iic_wrap_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt6893-imp_iic_wrap",
		.of_match_table = of_match_clk_mt6893_imp_iic_wrap,
	},
};
module_platform_driver(clk_mt6893_imp_iic_wrap_drv);

MODULE_AUTHOR("AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>");
MODULE_DESCRIPTION("MediaTek MT6893 I2C Wrapper clocks driver");
MODULE_LICENSE("GPL");
