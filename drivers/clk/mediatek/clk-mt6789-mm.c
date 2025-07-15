// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022 MediaTek Inc.
 * Copyright (c) 2025 Arseniy Velikanov <me@adomerle.pw>
 */

#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#include "clk-mtk.h"
#include "clk-gate.h"

#include <dt-bindings/clock/mediatek,mt6789-clk.h>

static const struct mtk_gate_regs mm0_cg_regs = {
	.set_ofs = 0x104,
	.clr_ofs = 0x108,
	.sta_ofs = 0x100,
};

static const struct mtk_gate_regs mm1_cg_regs = {
	.set_ofs = 0x1A4,
	.clr_ofs = 0x1A8,
	.sta_ofs = 0x1A0,
};

#define GATE_MM0(_id, _name, _parent, _shift)	\
	GATE_MTK(_id, _name, _parent, &mm0_cg_regs, _shift, &mtk_clk_gate_ops_setclr)

#define GATE_MM1(_id, _name, _parent, _shift)	\
	GATE_MTK(_id, _name, _parent, &mm1_cg_regs, _shift, &mtk_clk_gate_ops_setclr)

static const struct mtk_gate mm_clks[] = {
	GATE_MM0(CLK_MM_DISP_MUTEX0, "mm_disp_mutex0", "disp_ck", 0),
	GATE_MM0(CLK_MM_APB_BUS, "mm_apb_bus", "disp_ck", 1),
	GATE_MM0(CLK_MM_DISP_OVL0, "mm_disp_ovl0", "disp_ck", 2),
	GATE_MM0(CLK_MM_DISP_RDMA0, "mm_disp_rdma0", "disp_ck", 3),
	GATE_MM0(CLK_MM_DISP_OVL0_2L, "mm_disp_ovl0_2l", "disp_ck", 4),
	GATE_MM0(CLK_MM_DISP_WDMA0, "mm_disp_wdma0", "disp_ck", 5),
	GATE_MM0(CLK_MM_DISP_RSZ0, "mm_disp_rsz0", "disp_ck", 7),
	GATE_MM0(CLK_MM_DISP_AAL0, "mm_disp_aal0", "disp_ck", 8),
	GATE_MM0(CLK_MM_DISP_CCORR0, "mm_disp_ccorr0", "disp_ck", 9),
	GATE_MM0(CLK_MM_DISP_COLOR0, "mm_disp_color0", "disp_ck", 10),
	GATE_MM0(CLK_MM_SMI_INFRA, "mm_smi_infra", "disp_ck", 11),
	GATE_MM0(CLK_MM_DISP_DSC_WRAP0, "mm_disp_dsc_wrap0", "disp_ck", 12),
	GATE_MM0(CLK_MM_DISP_GAMMA0, "mm_disp_gamma0", "disp_ck", 13),
	GATE_MM0(CLK_MM_DISP_POSTMASK0, "mm_disp_postmask0", "disp_ck", 14),
	GATE_MM0(CLK_MM_DISP_DITHER0, "mm_disp_dither0", "disp_ck", 16),
	GATE_MM0(CLK_MM_SMI_COMMON, "mm_smi_common", "disp_ck", 17),
	GATE_MM0(CLK_MM_DSI0, "mm_dsi0", "disp_ck", 19),
	GATE_MM0(CLK_MM_DISP_FAKE_ENG0, "mm_disp_fake_eng0", "disp_ck", 20),
	GATE_MM0(CLK_MM_DISP_FAKE_ENG1, "mm_disp_fake_eng1", "disp_ck", 21),
	GATE_MM0(CLK_MM_SMI_GALS, "mm_smi_gals", "disp_ck", 22),
	GATE_MM0(CLK_MM_SMI_IOMMU, "mm_smi_iommu", "disp_ck", 24),

	GATE_MM1(CLK_MM_DSI0_DSI_CK_DOMAIN, "mm_dsi0_dsi_domain", "dsi_occ_ck", 0),
	GATE_MM1(CLK_MM_DISP_26M, "mm_disp_26m_ck", "disp_ck", 10),
};

static const struct mtk_clk_desc mm_desc = {
	.clks = mm_clks,
	.num_clks = ARRAY_SIZE(mm_clks),
};

static const struct platform_device_id clk_mt6789_mm_id_table[] = {
	{ .name = "clk-mt6789-mm", .driver_data = (kernel_ulong_t)&mm_desc },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(platform, clk_mt6789_mm_id_table);

static struct platform_driver clk_mt6789_mm_drv = {
	.probe = mtk_clk_pdev_probe,
	.remove = mtk_clk_pdev_remove,
	.driver = {
		.name = "clk-mt6789-mm",
	},
	.id_table = clk_mt6789_mm_id_table,
};

module_platform_driver(clk_mt6789_mm_drv);

MODULE_DESCRIPTION("MediaTek MT6789 MultiMedia clocks driver");
MODULE_LICENSE("GPL");
