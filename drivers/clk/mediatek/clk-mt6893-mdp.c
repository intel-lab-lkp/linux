// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020 MediaTek Inc.
 * Copyright (c) 2025 Collabora Ltd
 *                    AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>
 */

#include <linux/clk-provider.h>
#include <linux/platform_device.h>
#include <dt-bindings/clock/mediatek,mt6893-clk.h>

#include "clk-gate.h"
#include "clk-mtk.h"

static const struct mtk_gate_regs mdp0_cg_regs = {
	.set_ofs = 0x104,
	.clr_ofs = 0x108,
	.sta_ofs = 0x100,
};

static const struct mtk_gate_regs mdp1_cg_regs = {
	.set_ofs = 0x114,
	.clr_ofs = 0x118,
	.sta_ofs = 0x110,
};

static const struct mtk_gate_regs mdp2_cg_regs = {
	.set_ofs = 0x124,
	.clr_ofs = 0x128,
	.sta_ofs = 0x120,
};

#define GATE_MDP0(_id, _name, _parent, _shift)			\
	GATE_MTK(_id, _name, _parent, &mdp0_cg_regs, _shift, &mtk_clk_gate_ops_setclr)

#define GATE_MDP1(_id, _name, _parent, _shift)			\
	GATE_MTK(_id, _name, _parent, &mdp1_cg_regs, _shift, &mtk_clk_gate_ops_setclr)

#define GATE_MDP2(_id, _name, _parent, _shift)			\
	GATE_MTK(_id, _name, _parent, &mdp2_cg_regs, _shift, &mtk_clk_gate_ops_setclr)

static const struct mtk_gate mdp_clks[] = {
	/* MDP0 */
	GATE_MDP0(CLK_MDP_RDMA0, "mdp_rdma0", "mdp_sel", 0),
	GATE_MDP0(CLK_MDP_FG0, "mdp_fg0", "mdp_sel", 1),
	GATE_MDP0(CLK_MDP_HDR0, "mdp_hdr0", "mdp_sel", 2),
	GATE_MDP0(CLK_MDP_AAL0, "mdp_aal0", "mdp_sel", 3),
	GATE_MDP0(CLK_MDP_RSZ0, "mdp_rsz0", "mdp_sel", 4),
	GATE_MDP0(CLK_MDP_TDSHP0, "mdp_tdshp0", "mdp_sel", 5),
	GATE_MDP0(CLK_MDP_TCC0, "mdp_tcc0", "mdp_sel", 6),
	GATE_MDP0(CLK_MDP_WROT0, "mdp_wrot0", "mdp_sel", 7),
	GATE_MDP0(CLK_MDP_RDMA2, "mdp_rdma2", "mdp_sel", 8),
	GATE_MDP0(CLK_MDP_AAL2, "mdp_aal2", "mdp_sel", 9),
	GATE_MDP0(CLK_MDP_RSZ2, "mdp_rsz2", "mdp_sel", 10),
	GATE_MDP0(CLK_MDP_COLOR0, "mdp_color0", "mdp_sel", 11),
	GATE_MDP0(CLK_MDP_TDSHP2, "mdp_tdshp2", "mdp_sel", 12),
	GATE_MDP0(CLK_MDP_TCC2, "mdp_tcc2", "mdp_sel", 13),
	GATE_MDP0(CLK_MDP_WROT2, "mdp_wrot2", "mdp_sel", 14),
	GATE_MDP0(CLK_MDP_MUTEX0, "mdp_mutex0", "mdp_sel", 15),
	GATE_MDP0(CLK_MDP_RDMA1, "mdp_rdma1", "mdp_sel", 16),
	GATE_MDP0(CLK_MDP_FG1, "mdp_fg1", "mdp_sel", 17),
	GATE_MDP0(CLK_MDP_HDR1, "mdp_hdr1", "mdp_sel", 18),
	GATE_MDP0(CLK_MDP_AAL1, "mdp_aal1", "mdp_sel", 19),
	GATE_MDP0(CLK_MDP_RSZ1, "mdp_rsz1", "mdp_sel", 20),
	GATE_MDP0(CLK_MDP_TDSHP1, "mdp_tdshp1", "mdp_sel", 21),
	GATE_MDP0(CLK_MDP_TCC1, "mdp_tcc1", "mdp_sel", 22),
	GATE_MDP0(CLK_MDP_WROT1, "mdp_wrot1", "mdp_sel", 23),
	GATE_MDP0(CLK_MDP_RDMA3, "mdp_rdma3", "mdp_sel", 24),
	GATE_MDP0(CLK_MDP_AAL3, "mdp_aal3", "mdp_sel", 25),
	GATE_MDP0(CLK_MDP_RSZ3, "mdp_rsz3", "mdp_sel", 26),
	GATE_MDP0(CLK_MDP_COLOR1, "mdp_color1", "mdp_sel", 27),
	GATE_MDP0(CLK_MDP_TDSHP3, "mdp_tdshp3", "mdp_sel", 28),
	GATE_MDP0(CLK_MDP_TCC3, "mdp_tcc3", "mdp_sel", 29),
	GATE_MDP0(CLK_MDP_WROT3, "mdp_wrot3", "mdp_sel", 30),
	GATE_MDP0(CLK_MDP_APB_BUS, "mdp_apb_bus", "mdp_sel", 31),
	/* MDP1 */
	GATE_MDP1(CLK_MDP_MMSYSRAM, "mdp_mmsysram", "mdp_sel", 0),
	GATE_MDP1(CLK_MDP_APMCU_GALS, "mdp_apmcu_gals", "mdp_sel", 1),
	GATE_MDP1(CLK_MDP_SMI0, "mdp_smi0", "mdp_sel", 4),
	GATE_MDP1(CLK_MDP_IMG_DL_ASYNC0, "mdp_img_dl_async0", "mdp_sel", 5),
	GATE_MDP1(CLK_MDP_IMG_DL_ASYNC1, "mdp_img_dl_async1", "mdp_sel", 6),
	GATE_MDP1(CLK_MDP_IMG_DL_ASYNC2, "mdp_img_dl_async2", "mdp_sel", 7),
	GATE_MDP1(CLK_MDP_SMI1, "mdp_smi1", "mdp_sel", 8),
	GATE_MDP1(CLK_MDP_IMG_DL_ASYNC3, "mdp_img_dl_async3", "mdp_sel", 9),
	GATE_MDP1(CLK_MDP_SMI2, "mdp_smi2", "mdp_sel", 12),
	/* MDP2 */
	GATE_MDP2(CLK_MDP_IMG0_IMG_DL_ASYNC0, "mdp_img0_dl_as0", "img1_sel", 0),
	GATE_MDP2(CLK_MDP_IMG0_IMG_DL_ASYNC1, "mdp_img0_dl_as1", "img1_sel", 1),
	GATE_MDP2(CLK_MDP_IMG1_IMG_DL_ASYNC2, "mdp_img1_dl_as2", "img2_sel", 8),
	GATE_MDP2(CLK_MDP_IMG1_IMG_DL_ASYNC3, "mdp_img1_dl_as3", "img2_sel", 9),
};

static const struct mtk_clk_desc mdp_desc = {
	.clks = mdp_clks,
	.num_clks = ARRAY_SIZE(mdp_clks),
};

static const struct of_device_id of_match_clk_mt6893_mdp[] = {
	{ .compatible = "mediatek,mt6893-mdpsys", .data = &mdp_desc },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6893_mdp);

static struct platform_driver clk_mt6893_mdp_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt6893-mdp",
		.of_match_table = of_match_clk_mt6893_mdp,
	},
};
module_platform_driver(clk_mt6893_mdp_drv);

MODULE_AUTHOR("AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>");
MODULE_DESCRIPTION("MediaTek MT6893 Multimedia Data Path clocks driver");
MODULE_LICENSE("GPL");
