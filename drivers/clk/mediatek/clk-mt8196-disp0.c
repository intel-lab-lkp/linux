// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 MediaTek Inc.
 * Author: Guangjie Song <guangjie.song@mediatek.com>
 */
#include <dt-bindings/clock/mt8196-clk.h>
#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#include "clk-gate.h"
#include "clk-mtk.h"

static const struct mtk_gate_regs mm0_cg_regs = {
	.set_ofs = 0x104,
	.clr_ofs = 0x108,
	.sta_ofs = 0x100,
};

static const struct mtk_gate_regs mm0_vote_regs = {
	.set_ofs = 0x0020,
	.clr_ofs = 0x0024,
	.sta_ofs = 0x2c10,
};

static const struct mtk_gate_regs mm1_cg_regs = {
	.set_ofs = 0x114,
	.clr_ofs = 0x118,
	.sta_ofs = 0x110,
};

static const struct mtk_gate_regs mm1_vote_regs = {
	.set_ofs = 0x0028,
	.clr_ofs = 0x002c,
	.sta_ofs = 0x2c14,
};

#define GATE_MM0(_id, _name, _parent, _shift) {	\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &mm0_cg_regs,		\
		.shift = _shift,		\
		.flags = CLK_OPS_PARENT_ENABLE,	\
		.ops = &mtk_clk_gate_ops_setclr,\
	}

#define GATE_MM0_V(_id, _name, _parent) {	\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &cg_regs_dummy,		\
		.ops = &mtk_clk_dummy_ops,	\
	}

#define GATE_VOTE_MM0(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.vote_comp = "mm-vote-regmap",		\
		.regs = &mm0_cg_regs,			\
		.vote_regs = &mm0_vote_regs,		\
		.shift = _shift,			\
		.ops = &mtk_clk_gate_ops_vote,		\
		.dma_ops = &mtk_clk_gate_ops_setclr,	\
		.flags = CLK_USE_VOTE |			\
			 CLK_OPS_PARENT_ENABLE		\
	}

#define GATE_MM1(_id, _name, _parent, _shift) {	\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &mm1_cg_regs,		\
		.shift = _shift,		\
		.flags = CLK_OPS_PARENT_ENABLE,	\
		.ops = &mtk_clk_gate_ops_setclr,\
	}

#define GATE_MM1_V(_id, _name, _parent) {	\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &cg_regs_dummy,		\
		.ops = &mtk_clk_dummy_ops,	\
	}

#define GATE_VOTE_MM1(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.vote_comp = "mm-vote-regmap",		\
		.regs = &mm1_cg_regs,			\
		.vote_regs = &mm1_vote_regs,		\
		.shift = _shift,			\
		.ops = &mtk_clk_gate_ops_vote,		\
		.dma_ops = &mtk_clk_gate_ops_setclr,	\
		.flags = CLK_USE_VOTE |			\
			 CLK_OPS_PARENT_ENABLE,		\
	}

static const struct mtk_gate mm_clks[] = {
	/* MM0 */
	GATE_VOTE_MM0(CLK_MM_CONFIG, "mm_config", "ck2_disp_ck", 0),
	GATE_MM0_V(CLK_MM_CONFIG_DISP, "mm_config_disp", "mm_config"),
	GATE_VOTE_MM0(CLK_MM_DISP_MUTEX0, "mm_disp_mutex0", "ck2_disp_ck", 1),
	GATE_MM0_V(CLK_MM_DISP_MUTEX0_DISP, "mm_disp_mutex0_disp", "mm_disp_mutex0"),
	GATE_VOTE_MM0(CLK_MM_DISP_AAL0, "mm_disp_aal0", "ck2_disp_ck", 2),
	GATE_MM0_V(CLK_MM_DISP_AAL0_PQ, "mm_disp_aal0_pq", "mm_disp_aal0"),
	GATE_VOTE_MM0(CLK_MM_DISP_AAL1, "mm_disp_aal1", "ck2_disp_ck", 3),
	GATE_MM0_V(CLK_MM_DISP_AAL1_PQ, "mm_disp_aal1_pq", "mm_disp_aal1"),
	GATE_MM0(CLK_MM_DISP_C3D0, "mm_disp_c3d0", "ck2_disp_ck", 4),
	GATE_MM0_V(CLK_MM_DISP_C3D0_PQ, "mm_disp_c3d0_pq", "mm_disp_c3d0"),
	GATE_MM0(CLK_MM_DISP_C3D1, "mm_disp_c3d1", "ck2_disp_ck", 5),
	GATE_MM0_V(CLK_MM_DISP_C3D1_PQ, "mm_disp_c3d1_pq", "mm_disp_c3d1"),
	GATE_MM0(CLK_MM_DISP_C3D2, "mm_disp_c3d2", "ck2_disp_ck", 6),
	GATE_MM0_V(CLK_MM_DISP_C3D2_PQ, "mm_disp_c3d2_pq", "mm_disp_c3d2"),
	GATE_MM0(CLK_MM_DISP_C3D3, "mm_disp_c3d3", "ck2_disp_ck", 7),
	GATE_MM0_V(CLK_MM_DISP_C3D3_PQ, "mm_disp_c3d3_pq", "mm_disp_c3d3"),
	GATE_MM0(CLK_MM_DISP_CCORR0, "mm_disp_ccorr0", "ck2_disp_ck", 8),
	GATE_MM0_V(CLK_MM_DISP_CCORR0_PQ, "mm_disp_ccorr0_pq", "mm_disp_ccorr0"),
	GATE_MM0(CLK_MM_DISP_CCORR1, "mm_disp_ccorr1", "ck2_disp_ck", 9),
	GATE_MM0_V(CLK_MM_DISP_CCORR1_PQ, "mm_disp_ccorr1_pq", "mm_disp_ccorr1"),
	GATE_MM0(CLK_MM_DISP_CCORR2, "mm_disp_ccorr2", "ck2_disp_ck", 10),
	GATE_MM0_V(CLK_MM_DISP_CCORR2_PQ, "mm_disp_ccorr2_pq", "mm_disp_ccorr2"),
	GATE_MM0(CLK_MM_DISP_CCORR3, "mm_disp_ccorr3", "ck2_disp_ck", 11),
	GATE_MM0_V(CLK_MM_DISP_CCORR3_PQ, "mm_disp_ccorr3_pq", "mm_disp_ccorr3"),
	GATE_MM0(CLK_MM_DISP_CHIST0, "mm_disp_chist0", "ck2_disp_ck", 12),
	GATE_MM0_V(CLK_MM_DISP_CHIST0_PQ, "mm_disp_chist0_pq", "mm_disp_chist0"),
	GATE_MM0(CLK_MM_DISP_CHIST1, "mm_disp_chist1", "ck2_disp_ck", 13),
	GATE_MM0_V(CLK_MM_DISP_CHIST1_PQ, "mm_disp_chist1_pq", "mm_disp_chist1"),
	GATE_MM0(CLK_MM_DISP_COLOR0, "mm_disp_color0", "ck2_disp_ck", 14),
	GATE_MM0_V(CLK_MM_DISP_COLOR0_PQ, "mm_disp_color0_pq", "mm_disp_color0"),
	GATE_MM0(CLK_MM_DISP_COLOR1, "mm_disp_color1", "ck2_disp_ck", 15),
	GATE_MM0_V(CLK_MM_DISP_COLOR1_PQ, "mm_disp_color1_pq", "mm_disp_color1"),
	GATE_MM0(CLK_MM_DISP_DITHER0, "mm_disp_dither0", "ck2_disp_ck", 16),
	GATE_MM0_V(CLK_MM_DISP_DITHER0_PQ, "mm_disp_dither0_pq", "mm_disp_dither0"),
	GATE_MM0(CLK_MM_DISP_DITHER1, "mm_disp_dither1", "ck2_disp_ck", 17),
	GATE_MM0_V(CLK_MM_DISP_DITHER1_PQ, "mm_disp_dither1_pq", "mm_disp_dither1"),
	GATE_VOTE_MM0(CLK_MM_DISP_DLI_ASYNC0, "mm_disp_dli_async0", "ck2_disp_ck", 18),
	GATE_MM0_V(CLK_MM_DISP_DLI_ASYNC0_DISP, "mm_disp_dli_async0_disp", "mm_disp_dli_async0"),
	GATE_VOTE_MM0(CLK_MM_DISP_DLI_ASYNC1, "mm_disp_dli_async1", "ck2_disp_ck", 19),
	GATE_MM0_V(CLK_MM_DISP_DLI_ASYNC1_DISP, "mm_disp_dli_async1_disp", "mm_disp_dli_async1"),
	GATE_VOTE_MM0(CLK_MM_DISP_DLI_ASYNC2, "mm_disp_dli_async2", "ck2_disp_ck", 20),
	GATE_MM0_V(CLK_MM_DISP_DLI_ASYNC2_DISP, "mm_disp_dli_async2_disp", "mm_disp_dli_async2"),
	GATE_VOTE_MM0(CLK_MM_DISP_DLI_ASYNC3, "mm_disp_dli_async3", "ck2_disp_ck", 21),
	GATE_MM0_V(CLK_MM_DISP_DLI_ASYNC3_DISP, "mm_disp_dli_async3_disp", "mm_disp_dli_async3"),
	GATE_VOTE_MM0(CLK_MM_DISP_DLI_ASYNC4, "mm_disp_dli_async4", "ck2_disp_ck", 22),
	GATE_MM0_V(CLK_MM_DISP_DLI_ASYNC4_DISP, "mm_disp_dli_async4_disp", "mm_disp_dli_async4"),
	GATE_VOTE_MM0(CLK_MM_DISP_DLI_ASYNC5, "mm_disp_dli_async5", "ck2_disp_ck", 23),
	GATE_MM0_V(CLK_MM_DISP_DLI_ASYNC5_DISP, "mm_disp_dli_async5_disp", "mm_disp_dli_async5"),
	GATE_VOTE_MM0(CLK_MM_DISP_DLI_ASYNC6, "mm_disp_dli_async6", "ck2_disp_ck", 24),
	GATE_MM0_V(CLK_MM_DISP_DLI_ASYNC6_DISP, "mm_disp_dli_async6_disp", "mm_disp_dli_async6"),
	GATE_VOTE_MM0(CLK_MM_DISP_DLI_ASYNC7, "mm_disp_dli_async7", "ck2_disp_ck", 25),
	GATE_MM0_V(CLK_MM_DISP_DLI_ASYNC7_DISP, "mm_disp_dli_async7_disp", "mm_disp_dli_async7"),
	GATE_VOTE_MM0(CLK_MM_DISP_DLI_ASYNC8, "mm_disp_dli_async8", "ck2_disp_ck", 26),
	GATE_MM0_V(CLK_MM_DISP_DLI_ASYNC8_DISP, "mm_disp_dli_async8_disp", "mm_disp_dli_async8"),
	GATE_VOTE_MM0(CLK_MM_DISP_DLI_ASYNC9, "mm_disp_dli_async9", "ck2_disp_ck", 27),
	GATE_MM0_V(CLK_MM_DISP_DLI_ASYNC9_DISP, "mm_disp_dli_async9_disp", "mm_disp_dli_async9"),
	GATE_VOTE_MM0(CLK_MM_DISP_DLI_ASYNC10, "mm_disp_dli_async10", "ck2_disp_ck", 28),
	GATE_MM0_V(CLK_MM_DISP_DLI_ASYNC10_DISP, "mm_disp_dli_async10_disp", "mm_disp_dli_async10"),
	GATE_VOTE_MM0(CLK_MM_DISP_DLI_ASYNC11, "mm_disp_dli_async11", "ck2_disp_ck", 29),
	GATE_MM0_V(CLK_MM_DISP_DLI_ASYNC11_DISP, "mm_disp_dli_async11_disp", "mm_disp_dli_async11"),
	GATE_VOTE_MM0(CLK_MM_DISP_DLI_ASYNC12, "mm_disp_dli_async12", "ck2_disp_ck", 30),
	GATE_MM0_V(CLK_MM_DISP_DLI_ASYNC12_DISP, "mm_disp_dli_async12_disp", "mm_disp_dli_async12"),
	GATE_VOTE_MM0(CLK_MM_DISP_DLI_ASYNC13, "mm_disp_dli_async13", "ck2_disp_ck", 31),
	GATE_MM0_V(CLK_MM_DISP_DLI_ASYNC13_DISP, "mm_disp_dli_async13_disp", "mm_disp_dli_async13"),
	/* MM1 */
	GATE_VOTE_MM1(CLK_MM_DISP_DLI_ASYNC14, "mm_disp_dli_async14", "ck2_disp_ck", 0),
	GATE_MM1_V(CLK_MM_DISP_DLI_ASYNC14_DISP, "mm_disp_dli_async14_disp", "mm_disp_dli_async14"),
	GATE_VOTE_MM1(CLK_MM_DISP_DLI_ASYNC15, "mm_disp_dli_async15", "ck2_disp_ck", 1),
	GATE_MM1_V(CLK_MM_DISP_DLI_ASYNC15_DISP, "mm_disp_dli_async15_disp", "mm_disp_dli_async15"),
	GATE_VOTE_MM1(CLK_MM_DISP_DLO_ASYNC0, "mm_disp_dlo_async0", "ck2_disp_ck", 2),
	GATE_MM1_V(CLK_MM_DISP_DLO_ASYNC0_DISP, "mm_disp_dlo_async0_disp", "mm_disp_dlo_async0"),
	GATE_VOTE_MM1(CLK_MM_DISP_DLO_ASYNC1, "mm_disp_dlo_async1", "ck2_disp_ck", 3),
	GATE_MM1_V(CLK_MM_DISP_DLO_ASYNC1_DISP, "mm_disp_dlo_async1_disp", "mm_disp_dlo_async1"),
	GATE_VOTE_MM1(CLK_MM_DISP_DLO_ASYNC2, "mm_disp_dlo_async2", "ck2_disp_ck", 4),
	GATE_MM1_V(CLK_MM_DISP_DLO_ASYNC2_DISP, "mm_disp_dlo_async2_disp", "mm_disp_dlo_async2"),
	GATE_VOTE_MM1(CLK_MM_DISP_DLO_ASYNC3, "mm_disp_dlo_async3", "ck2_disp_ck", 5),
	GATE_MM1_V(CLK_MM_DISP_DLO_ASYNC3_DISP, "mm_disp_dlo_async3_disp", "mm_disp_dlo_async3"),
	GATE_VOTE_MM1(CLK_MM_DISP_DLO_ASYNC4, "mm_disp_dlo_async4", "ck2_disp_ck", 6),
	GATE_MM1_V(CLK_MM_DISP_DLO_ASYNC4_DISP, "mm_disp_dlo_async4_disp", "mm_disp_dlo_async4"),
	GATE_VOTE_MM1(CLK_MM_DISP_DLO_ASYNC5, "mm_disp_dlo_async5", "ck2_disp_ck", 7),
	GATE_MM1_V(CLK_MM_DISP_DLO_ASYNC5_DISP, "mm_disp_dlo_async5_disp", "mm_disp_dlo_async5"),
	GATE_VOTE_MM1(CLK_MM_DISP_DLO_ASYNC6, "mm_disp_dlo_async6", "ck2_disp_ck", 8),
	GATE_MM1_V(CLK_MM_DISP_DLO_ASYNC6_DISP, "mm_disp_dlo_async6_disp", "mm_disp_dlo_async6"),
	GATE_VOTE_MM1(CLK_MM_DISP_DLO_ASYNC7, "mm_disp_dlo_async7", "ck2_disp_ck", 9),
	GATE_MM1_V(CLK_MM_DISP_DLO_ASYNC7_DISP, "mm_disp_dlo_async7_disp", "mm_disp_dlo_async7"),
	GATE_VOTE_MM1(CLK_MM_DISP_DLO_ASYNC8, "mm_disp_dlo_async8", "ck2_disp_ck", 10),
	GATE_MM1_V(CLK_MM_DISP_DLO_ASYNC8_DISP, "mm_disp_dlo_async8_disp", "mm_disp_dlo_async8"),
	GATE_MM1(CLK_MM_DISP_GAMMA0, "mm_disp_gamma0", "ck2_disp_ck", 11),
	GATE_MM1_V(CLK_MM_DISP_GAMMA0_PQ, "mm_disp_gamma0_pq", "mm_disp_gamma0"),
	GATE_MM1(CLK_MM_DISP_GAMMA1, "mm_disp_gamma1", "ck2_disp_ck", 12),
	GATE_MM1_V(CLK_MM_DISP_GAMMA1_PQ, "mm_disp_gamma1_pq", "mm_disp_gamma1"),
	GATE_MM1(CLK_MM_MDP_AAL0, "mm_mdp_aal0", "ck2_disp_ck", 13),
	GATE_MM1_V(CLK_MM_MDP_AAL0_PQ, "mm_mdp_aal0_pq", "mm_mdp_aal0"),
	GATE_MM1(CLK_MM_MDP_AAL1, "mm_mdp_aal1", "ck2_disp_ck", 14),
	GATE_MM1_V(CLK_MM_MDP_AAL1_PQ, "mm_mdp_aal1_pq", "mm_mdp_aal1"),
	GATE_VOTE_MM1(CLK_MM_MDP_RDMA0, "mm_mdp_rdma0", "ck2_disp_ck", 15),
	GATE_MM1_V(CLK_MM_MDP_RDMA0_DISP, "mm_mdp_rdma0_disp", "mm_mdp_rdma0"),
	GATE_VOTE_MM1(CLK_MM_DISP_POSTMASK0, "mm_disp_postmask0", "ck2_disp_ck", 16),
	GATE_MM1_V(CLK_MM_DISP_POSTMASK0_DISP, "mm_disp_postmask0_disp", "mm_disp_postmask0"),
	GATE_VOTE_MM1(CLK_MM_DISP_POSTMASK1, "mm_disp_postmask1", "ck2_disp_ck", 17),
	GATE_MM1_V(CLK_MM_DISP_POSTMASK1_DISP, "mm_disp_postmask1_disp", "mm_disp_postmask1"),
	GATE_VOTE_MM1(CLK_MM_MDP_RSZ0, "mm_mdp_rsz0", "ck2_disp_ck", 18),
	GATE_MM1_V(CLK_MM_MDP_RSZ0_DISP, "mm_mdp_rsz0_disp", "mm_mdp_rsz0"),
	GATE_VOTE_MM1(CLK_MM_MDP_RSZ1, "mm_mdp_rsz1", "ck2_disp_ck", 19),
	GATE_MM1_V(CLK_MM_MDP_RSZ1_DISP, "mm_mdp_rsz1_disp", "mm_mdp_rsz1"),
	GATE_VOTE_MM1(CLK_MM_DISP_SPR0, "mm_disp_spr0", "ck2_disp_ck", 20),
	GATE_MM1_V(CLK_MM_DISP_SPR0_DISP, "mm_disp_spr0_disp", "mm_disp_spr0"),
	GATE_MM1(CLK_MM_DISP_TDSHP0, "mm_disp_tdshp0", "ck2_disp_ck", 21),
	GATE_MM1_V(CLK_MM_DISP_TDSHP0_PQ, "mm_disp_tdshp0_pq", "mm_disp_tdshp0"),
	GATE_MM1(CLK_MM_DISP_TDSHP1, "mm_disp_tdshp1", "ck2_disp_ck", 22),
	GATE_MM1_V(CLK_MM_DISP_TDSHP1_PQ, "mm_disp_tdshp1_pq", "mm_disp_tdshp1"),
	GATE_VOTE_MM1(CLK_MM_DISP_WDMA0, "mm_disp_wdma0", "ck2_disp_ck", 23),
	GATE_MM1_V(CLK_MM_DISP_WDMA0_DISP, "mm_disp_wdma0_disp", "mm_disp_wdma0"),
	GATE_VOTE_MM1(CLK_MM_DISP_Y2R0, "mm_disp_y2r0", "ck2_disp_ck", 24),
	GATE_MM1_V(CLK_MM_DISP_Y2R0_DISP, "mm_disp_y2r0_disp", "mm_disp_y2r0"),
	GATE_VOTE_MM1(CLK_MM_SMI_SUB_COMM0, "mm_ssc", "ck2_disp_ck", 25),
	GATE_MM1_V(CLK_MM_SMI_SUB_COMM0_SMI, "mm_ssc_smi", "mm_ssc"),
	GATE_VOTE_MM1(CLK_MM_DISP_FAKE_ENG0, "mm_disp_fake_eng0", "ck2_disp_ck", 26),
	GATE_MM1_V(CLK_MM_DISP_FAKE_ENG0_DISP, "mm_disp_fake_eng0_disp", "mm_disp_fake_eng0"),
};

static const struct mtk_clk_desc mm_mcd = {
	.clks = mm_clks,
	.num_clks = ARRAY_SIZE(mm_clks),
};

static const struct platform_device_id clk_mt8196_disp0_id_table[] = {
	{ .name = "clk-mt8196-disp0", .driver_data = (kernel_ulong_t)&mm_mcd },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(platform, clk_mt8196_disp0_id_table);

static struct platform_driver clk_mt8196_disp0_drv = {
	.probe = mtk_clk_pdev_probe,
	.remove = mtk_clk_pdev_remove,
	.driver = {
		.name = "clk-mt8196-disp0",
	},
	.id_table = clk_mt8196_disp0_id_table,
};

module_platform_driver(clk_mt8196_disp0_drv);
MODULE_LICENSE("GPL");
