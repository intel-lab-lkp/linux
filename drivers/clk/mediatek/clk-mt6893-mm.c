// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020 MediaTek Inc.
 * Copyright (c) 2025 Collabora Ltd
 *                    AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>
 */

#include <linux/clk-provider.h>
#include <linux/platform_device.h>

#include "clk-mtk.h"
#include "clk-gate.h"

#include <dt-bindings/clock/mediatek,mt6893-clk.h>

static const struct mtk_gate_regs mm0_cg_regs = {
	.set_ofs = 0x104,
	.clr_ofs = 0x108,
	.sta_ofs = 0x100,
};

static const struct mtk_gate_regs mm1_cg_regs = {
	.set_ofs = 0x114,
	.clr_ofs = 0x118,
	.sta_ofs = 0x110,
};

static const struct mtk_gate_regs mm2_cg_regs = {
	.set_ofs = 0x1a4,
	.clr_ofs = 0x1a8,
	.sta_ofs = 0x1a0,
};

#define GATE_MM0(_id, _name, _parent, _shift)	\
	GATE_MTK(_id, _name, _parent, &mm0_cg_regs, _shift, &mtk_clk_gate_ops_setclr)

#define GATE_MM1(_id, _name, _parent, _shift)	\
	GATE_MTK(_id, _name, _parent, &mm1_cg_regs, _shift, &mtk_clk_gate_ops_setclr)

#define GATE_MM2(_id, _name, _parent, _shift)	\
	GATE_MTK(_id, _name, _parent, &mm2_cg_regs, _shift, &mtk_clk_gate_ops_setclr)

static const struct mtk_gate mm_clks[] = {
	/* MM0 */
	GATE_MM0(CLK_MM_DISP_RSZ0, "mm_disp_rsz0", "disp_sel", 0),
	GATE_MM0(CLK_MM_DISP_RSZ1, "mm_disp_rsz1", "disp_sel", 1),
	GATE_MM0(CLK_MM_DISP_OVL0, "mm_disp_ovl0", "disp_sel", 2),
	GATE_MM0(CLK_MM_INLINE, "mm_inline", "disp_sel", 3),
	GATE_MM0(CLK_MM_MDP_TDSHP4, "mm_mdp_tdshp4", "disp_sel", 4),
	GATE_MM0(CLK_MM_MDP_TDSHP5, "mm_mdp_tdshp5", "disp_sel", 5),
	GATE_MM0(CLK_MM_MDP_AAL4, "mm_mdp_aal4", "disp_sel", 6),
	GATE_MM0(CLK_MM_MDP_AAL5, "mm_mdp_aal5", "disp_sel", 7),
	GATE_MM0(CLK_MM_MDP_HDR4, "mm_mdp_hdr4", "disp_sel", 8),
	GATE_MM0(CLK_MM_MDP_HDR5, "mm_mdp_hdr5", "disp_sel", 9),
	GATE_MM0(CLK_MM_MDP_RSZ4, "mm_mdp_rsz4", "disp_sel", 10),
	GATE_MM0(CLK_MM_MDP_RSZ5, "mm_mdp_rsz5", "disp_sel", 11),
	GATE_MM0(CLK_MM_MDP_RDMA4, "mm_mdp_rdma4", "disp_sel", 12),
	GATE_MM0(CLK_MM_MDP_RDMA5, "mm_mdp_rdma5", "disp_sel", 13),
	GATE_MM0(CLK_MM_DISP_FAKE_ENG0, "mm_disp_fake_eng0", "disp_sel", 14),
	GATE_MM0(CLK_MM_DISP_FAKE_ENG1, "mm_disp_fake_eng1", "disp_sel", 15),
	GATE_MM0(CLK_MM_DISP_OVL0_2L, "mm_disp_ovl0_2l", "disp_sel", 16),
	GATE_MM0(CLK_MM_DISP_OVL1_2L, "mm_disp_ovl1_2l", "disp_sel", 17),
	GATE_MM0(CLK_MM_DISP_OVL2_2L, "mm_disp_ovl2_2l", "disp_sel", 18),
	GATE_MM0(CLK_MM_DISP_MUTEX, "mm_disp_mutex", "disp_sel", 19),
	GATE_MM0(CLK_MM_DISP_OVL1, "mm_disp_ovl1", "disp_sel", 20),
	GATE_MM0(CLK_MM_DISP_OVL3_2L, "mm_disp_ovl3_2l", "disp_sel", 21),
	GATE_MM0(CLK_MM_DISP_CCORR0, "mm_disp_ccorr0", "disp_sel", 22),
	GATE_MM0(CLK_MM_DISP_CCORR1, "mm_disp_ccorr1", "disp_sel", 23),
	GATE_MM0(CLK_MM_DISP_COLOR0, "mm_disp_color0", "disp_sel", 24),
	GATE_MM0(CLK_MM_DISP_COLOR1, "mm_disp_color1", "disp_sel", 25),
	GATE_MM0(CLK_MM_DISP_POSTMASK0, "mm_disp_postmask0", "disp_sel", 26),
	GATE_MM0(CLK_MM_DISP_POSTMASK1, "mm_disp_postmask1", "disp_sel", 27),
	GATE_MM0(CLK_MM_DISP_DITHER0, "mm_disp_dither0", "disp_sel", 28),
	GATE_MM0(CLK_MM_DISP_DITHER1, "mm_disp_dither1", "disp_sel", 29),
	GATE_MM0(CLK_MM_DSI0_MM_CLK, "mm_dsi0_mm_clk", "disp_sel", 30),
	GATE_MM0(CLK_MM_DSI1_MM_CLK, "mm_dsi1_mm_clk", "disp_sel", 31),
	/* MM1 */
	GATE_MM1(CLK_MM_DISP_GAMMA0, "mm_disp_gamma0", "disp_sel", 0),
	GATE_MM1(CLK_MM_DISP_GAMMA1, "mm_disp_gamma1", "disp_sel", 1),
	GATE_MM1(CLK_MM_DISP_AAL0, "mm_disp_aal0", "disp_sel", 2),
	GATE_MM1(CLK_MM_DISP_AAL1, "mm_disp_aal1", "disp_sel", 3),
	GATE_MM1(CLK_MM_DISP_WDMA0, "mm_disp_wdma0", "disp_sel", 4),
	GATE_MM1(CLK_MM_DISP_WDMA1, "mm_disp_wdma1", "disp_sel", 5),
	GATE_MM1(CLK_MM_DISP_UFBC_WDMA0, "mm_disp_ufbc_wdma0", "disp_sel", 6),
	GATE_MM1(CLK_MM_DISP_UFBC_WDMA1, "mm_disp_ufbc_wdma1", "disp_sel", 7),
	GATE_MM1(CLK_MM_DISP_RDMA0, "mm_disp_rdma0", "disp_sel", 8),
	GATE_MM1(CLK_MM_DISP_RDMA1, "mm_disp_rdma1", "disp_sel", 9),
	GATE_MM1(CLK_MM_DISP_RDMA4, "mm_disp_rdma4", "disp_sel", 10),
	GATE_MM1(CLK_MM_DISP_RDMA5, "mm_disp_rdma5", "disp_sel", 11),
	GATE_MM1(CLK_MM_DISP_DSC_WRAP, "mm_disp_dsc_wrap", "disp_sel", 12),
	GATE_MM1(CLK_MM_DP_INTF_MM_CLK, "mm_dp_intf_mm_clk", "disp_sel", 13),
	GATE_MM1(CLK_MM_DISP_MERGE0, "mm_disp_merge0", "disp_sel", 14),
	GATE_MM1(CLK_MM_DISP_MERGE1, "mm_disp_merge1", "disp_sel", 15),
	GATE_MM1(CLK_MM_SMI_COMMON, "mm_smi_common", "disp_sel", 19),
	GATE_MM1(CLK_MM_SMI_GALS, "mm_smi_gals", "disp_sel", 23),
	GATE_MM1(CLK_MM_SMI_INFRA, "mm_smi_infra", "disp_sel", 27),
	GATE_MM1(CLK_MM_SMI_IOMMU, "mm_smi_iommu", "disp_sel", 31),
	/* MM2 */
	GATE_MM2(CLK_MM_DSI0_INTF_CLK, "mm_dsi0_intf_clk", "disp_sel", 0),
	GATE_MM2(CLK_MM_DSI1_INTF_CLK, "mm_dsi1_intf_clk", "disp_sel", 8),
	GATE_MM2(CLK_MM_DP_INTF_INTF_CLK, "mm_dp_intf_intf_clk", "dp_sel", 16),
	GATE_MM2(CLK_MM_CK_26_MHZ, "mm_26_mhz", "clk26m", 24),
	GATE_MM2(CLK_MM_CK_32_KHZ, "mm_32_khz", "clk32k", 25),
};

static const struct mtk_clk_desc mm_desc = {
	.clks = mm_clks,
	.num_clks = ARRAY_SIZE(mm_clks),
};

static const struct platform_device_id clk_mt6893_mm_id_table[] = {
	{ .name = "clk-mt6893-mm", .driver_data = (kernel_ulong_t)&mm_desc },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(platform, clk_mt6893_mm_id_table);

static struct platform_driver clk_mt6893_mm_drv = {
	.probe = mtk_clk_pdev_probe,
	.remove = mtk_clk_pdev_remove,
	.driver = {
		.name = "clk-mt6893-mm",
	},
	.id_table = clk_mt6893_mm_id_table,
};
module_platform_driver(clk_mt6893_mm_drv);

MODULE_AUTHOR("AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>");
MODULE_DESCRIPTION("MediaTek MT6893 MultiMedia clocks driver");
MODULE_LICENSE("GPL");
