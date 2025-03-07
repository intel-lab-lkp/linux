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

static const struct mtk_gate_regs ovl10_cg_regs = {
	.set_ofs = 0x104,
	.clr_ofs = 0x108,
	.sta_ofs = 0x100,
};

static const struct mtk_gate_regs ovl10_vote_regs = {
	.set_ofs = 0x0050,
	.clr_ofs = 0x0054,
	.sta_ofs = 0x2c28,
};

static const struct mtk_gate_regs ovl11_cg_regs = {
	.set_ofs = 0x114,
	.clr_ofs = 0x118,
	.sta_ofs = 0x110,
};

static const struct mtk_gate_regs ovl11_vote_regs = {
	.set_ofs = 0x0058,
	.clr_ofs = 0x005c,
	.sta_ofs = 0x2c2c,
};

#define GATE_OVL10(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &ovl10_cg_regs,			\
		.shift = _shift,			\
		.flags = CLK_OPS_PARENT_ENABLE,		\
		.ops = &mtk_clk_gate_ops_setclr,	\
	}

#define GATE_OVL10_V(_id, _name, _parent) {	\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &cg_regs_dummy,		\
		.ops = &mtk_clk_dummy_ops,	\
	}

#define GATE_VOTE_OVL10(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.vote_comp = "mm-vote-regmap",		\
		.regs = &ovl10_cg_regs,			\
		.vote_regs = &ovl10_vote_regs,		\
		.shift = _shift,			\
		.ops = &mtk_clk_gate_ops_vote,		\
		.dma_ops = &mtk_clk_gate_ops_setclr,	\
		.flags = CLK_USE_VOTE |			\
			 CLK_OPS_PARENT_ENABLE,		\
	}

#define GATE_OVL11(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &ovl11_cg_regs,			\
		.shift = _shift,			\
		.flags = CLK_OPS_PARENT_ENABLE,		\
		.ops = &mtk_clk_gate_ops_setclr,	\
	}

#define GATE_OVL11_V(_id, _name, _parent) {	\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &cg_regs_dummy,		\
		.ops = &mtk_clk_dummy_ops,	\
	}

#define GATE_VOTE_OVL11(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.vote_comp = "mm-vote-regmap",		\
		.regs = &ovl11_cg_regs,			\
		.vote_regs = &ovl11_vote_regs,		\
		.shift = _shift,			\
		.ops = &mtk_clk_gate_ops_vote,		\
		.dma_ops = &mtk_clk_gate_ops_setclr,	\
		.flags = CLK_USE_VOTE |			\
			 CLK_OPS_PARENT_ENABLE,		\
	}

static const struct mtk_gate ovl1_clks[] = {
	/* OVL10 */
	GATE_VOTE_OVL10(CLK_OVL1_OVLSYS_CONFIG, "ovl1_ovlsys_config", "ck2_disp_ck", 0),
	GATE_OVL10_V(CLK_OVL1_OVLSYS_CONFIG_DISP, "ovl1_ovlsys_config_disp", "ovl1_ovlsys_config"),
	GATE_VOTE_OVL10(CLK_OVL1_OVL_FAKE_ENG0, "ovl1_ovl_fake_eng0", "ck2_disp_ck", 1),
	GATE_OVL10_V(CLK_OVL1_OVL_FAKE_ENG0_DISP, "ovl1_ovl_fake_eng0_disp", "ovl1_ovl_fake_eng0"),
	GATE_VOTE_OVL10(CLK_OVL1_OVL_FAKE_ENG1, "ovl1_ovl_fake_eng1", "ck2_disp_ck", 2),
	GATE_OVL10_V(CLK_OVL1_OVL_FAKE_ENG1_DISP, "ovl1_ovl_fake_eng1_disp", "ovl1_ovl_fake_eng1"),
	GATE_VOTE_OVL10(CLK_OVL1_OVL_MUTEX0, "ovl1_ovl_mutex0", "ck2_disp_ck", 3),
	GATE_OVL10_V(CLK_OVL1_OVL_MUTEX0_DISP, "ovl1_ovl_mutex0_disp", "ovl1_ovl_mutex0"),
	GATE_VOTE_OVL10(CLK_OVL1_OVL_EXDMA0, "ovl1_ovl_exdma0", "ck2_disp_ck", 4),
	GATE_OVL10_V(CLK_OVL1_OVL_EXDMA0_DISP, "ovl1_ovl_exdma0_disp", "ovl1_ovl_exdma0"),
	GATE_VOTE_OVL10(CLK_OVL1_OVL_EXDMA1, "ovl1_ovl_exdma1", "ck2_disp_ck", 5),
	GATE_OVL10_V(CLK_OVL1_OVL_EXDMA1_DISP, "ovl1_ovl_exdma1_disp", "ovl1_ovl_exdma1"),
	GATE_VOTE_OVL10(CLK_OVL1_OVL_EXDMA2, "ovl1_ovl_exdma2", "ck2_disp_ck", 6),
	GATE_OVL10_V(CLK_OVL1_OVL_EXDMA2_DISP, "ovl1_ovl_exdma2_disp", "ovl1_ovl_exdma2"),
	GATE_VOTE_OVL10(CLK_OVL1_OVL_EXDMA3, "ovl1_ovl_exdma3", "ck2_disp_ck", 7),
	GATE_OVL10_V(CLK_OVL1_OVL_EXDMA3_DISP, "ovl1_ovl_exdma3_disp", "ovl1_ovl_exdma3"),
	GATE_VOTE_OVL10(CLK_OVL1_OVL_EXDMA4, "ovl1_ovl_exdma4", "ck2_disp_ck", 8),
	GATE_OVL10_V(CLK_OVL1_OVL_EXDMA4_DISP, "ovl1_ovl_exdma4_disp", "ovl1_ovl_exdma4"),
	GATE_VOTE_OVL10(CLK_OVL1_OVL_EXDMA5, "ovl1_ovl_exdma5", "ck2_disp_ck", 9),
	GATE_OVL10_V(CLK_OVL1_OVL_EXDMA5_DISP, "ovl1_ovl_exdma5_disp", "ovl1_ovl_exdma5"),
	GATE_VOTE_OVL10(CLK_OVL1_OVL_EXDMA6, "ovl1_ovl_exdma6", "ck2_disp_ck", 10),
	GATE_OVL10_V(CLK_OVL1_OVL_EXDMA6_DISP, "ovl1_ovl_exdma6_disp", "ovl1_ovl_exdma6"),
	GATE_VOTE_OVL10(CLK_OVL1_OVL_EXDMA7, "ovl1_ovl_exdma7", "ck2_disp_ck", 11),
	GATE_OVL10_V(CLK_OVL1_OVL_EXDMA7_DISP, "ovl1_ovl_exdma7_disp", "ovl1_ovl_exdma7"),
	GATE_VOTE_OVL10(CLK_OVL1_OVL_EXDMA8, "ovl1_ovl_exdma8", "ck2_disp_ck", 12),
	GATE_OVL10_V(CLK_OVL1_OVL_EXDMA8_DISP, "ovl1_ovl_exdma8_disp", "ovl1_ovl_exdma8"),
	GATE_VOTE_OVL10(CLK_OVL1_OVL_EXDMA9, "ovl1_ovl_exdma9", "ck2_disp_ck", 13),
	GATE_OVL10_V(CLK_OVL1_OVL_EXDMA9_DISP, "ovl1_ovl_exdma9_disp", "ovl1_ovl_exdma9"),
	GATE_VOTE_OVL10(CLK_OVL1_OVL_BLENDER0, "ovl1_ovl_blender0", "ck2_disp_ck", 14),
	GATE_OVL10_V(CLK_OVL1_OVL_BLENDER0_DISP, "ovl1_ovl_blender0_disp", "ovl1_ovl_blender0"),
	GATE_VOTE_OVL10(CLK_OVL1_OVL_BLENDER1, "ovl1_ovl_blender1", "ck2_disp_ck", 15),
	GATE_OVL10_V(CLK_OVL1_OVL_BLENDER1_DISP, "ovl1_ovl_blender1_disp", "ovl1_ovl_blender1"),
	GATE_VOTE_OVL10(CLK_OVL1_OVL_BLENDER2, "ovl1_ovl_blender2", "ck2_disp_ck", 16),
	GATE_OVL10_V(CLK_OVL1_OVL_BLENDER2_DISP, "ovl1_ovl_blender2_disp", "ovl1_ovl_blender2"),
	GATE_VOTE_OVL10(CLK_OVL1_OVL_BLENDER3, "ovl1_ovl_blender3", "ck2_disp_ck", 17),
	GATE_OVL10_V(CLK_OVL1_OVL_BLENDER3_DISP, "ovl1_ovl_blender3_disp", "ovl1_ovl_blender3"),
	GATE_VOTE_OVL10(CLK_OVL1_OVL_BLENDER4, "ovl1_ovl_blender4", "ck2_disp_ck", 18),
	GATE_OVL10_V(CLK_OVL1_OVL_BLENDER4_DISP, "ovl1_ovl_blender4_disp", "ovl1_ovl_blender4"),
	GATE_VOTE_OVL10(CLK_OVL1_OVL_BLENDER5, "ovl1_ovl_blender5", "ck2_disp_ck", 19),
	GATE_OVL10_V(CLK_OVL1_OVL_BLENDER5_DISP, "ovl1_ovl_blender5_disp", "ovl1_ovl_blender5"),
	GATE_VOTE_OVL10(CLK_OVL1_OVL_BLENDER6, "ovl1_ovl_blender6", "ck2_disp_ck", 20),
	GATE_OVL10_V(CLK_OVL1_OVL_BLENDER6_DISP, "ovl1_ovl_blender6_disp", "ovl1_ovl_blender6"),
	GATE_VOTE_OVL10(CLK_OVL1_OVL_BLENDER7, "ovl1_ovl_blender7", "ck2_disp_ck", 21),
	GATE_OVL10_V(CLK_OVL1_OVL_BLENDER7_DISP, "ovl1_ovl_blender7_disp", "ovl1_ovl_blender7"),
	GATE_VOTE_OVL10(CLK_OVL1_OVL_BLENDER8, "ovl1_ovl_blender8", "ck2_disp_ck", 22),
	GATE_OVL10_V(CLK_OVL1_OVL_BLENDER8_DISP, "ovl1_ovl_blender8_disp", "ovl1_ovl_blender8"),
	GATE_VOTE_OVL10(CLK_OVL1_OVL_BLENDER9, "ovl1_ovl_blender9", "ck2_disp_ck", 23),
	GATE_OVL10_V(CLK_OVL1_OVL_BLENDER9_DISP, "ovl1_ovl_blender9_disp", "ovl1_ovl_blender9"),
	GATE_VOTE_OVL10(CLK_OVL1_OVL_OUTPROC0, "ovl1_ovl_outproc0", "ck2_disp_ck", 24),
	GATE_OVL10_V(CLK_OVL1_OVL_OUTPROC0_DISP, "ovl1_ovl_outproc0_disp", "ovl1_ovl_outproc0"),
	GATE_VOTE_OVL10(CLK_OVL1_OVL_OUTPROC1, "ovl1_ovl_outproc1", "ck2_disp_ck", 25),
	GATE_OVL10_V(CLK_OVL1_OVL_OUTPROC1_DISP, "ovl1_ovl_outproc1_disp", "ovl1_ovl_outproc1"),
	GATE_VOTE_OVL10(CLK_OVL1_OVL_OUTPROC2, "ovl1_ovl_outproc2", "ck2_disp_ck", 26),
	GATE_OVL10_V(CLK_OVL1_OVL_OUTPROC2_DISP, "ovl1_ovl_outproc2_disp", "ovl1_ovl_outproc2"),
	GATE_VOTE_OVL10(CLK_OVL1_OVL_OUTPROC3, "ovl1_ovl_outproc3", "ck2_disp_ck", 27),
	GATE_OVL10_V(CLK_OVL1_OVL_OUTPROC3_DISP, "ovl1_ovl_outproc3_disp", "ovl1_ovl_outproc3"),
	GATE_VOTE_OVL10(CLK_OVL1_OVL_OUTPROC4, "ovl1_ovl_outproc4", "ck2_disp_ck", 28),
	GATE_OVL10_V(CLK_OVL1_OVL_OUTPROC4_DISP, "ovl1_ovl_outproc4_disp", "ovl1_ovl_outproc4"),
	GATE_VOTE_OVL10(CLK_OVL1_OVL_OUTPROC5, "ovl1_ovl_outproc5", "ck2_disp_ck", 29),
	GATE_OVL10_V(CLK_OVL1_OVL_OUTPROC5_DISP, "ovl1_ovl_outproc5_disp", "ovl1_ovl_outproc5"),
	GATE_VOTE_OVL10(CLK_OVL1_OVL_MDP_RSZ0, "ovl1_ovl_mdp_rsz0", "ck2_disp_ck", 30),
	GATE_OVL10_V(CLK_OVL1_OVL_MDP_RSZ0_DISP, "ovl1_ovl_mdp_rsz0_disp", "ovl1_ovl_mdp_rsz0"),
	GATE_VOTE_OVL10(CLK_OVL1_OVL_MDP_RSZ1, "ovl1_ovl_mdp_rsz1", "ck2_disp_ck", 31),
	GATE_OVL10_V(CLK_OVL1_OVL_MDP_RSZ1_DISP, "ovl1_ovl_mdp_rsz1_disp", "ovl1_ovl_mdp_rsz1"),
	/* OVL11 */
	GATE_VOTE_OVL11(CLK_OVL1_OVL_DISP_WDMA0, "ovl1_ovl_disp_wdma0", "ck2_disp_ck", 0),
	GATE_OVL11_V(CLK_OVL1_OVL_DISP_WDMA0_DISP, "ovl1_ovl_disp_wdma0_disp", "ovl1_ovl_disp_wdma0"),
	GATE_VOTE_OVL11(CLK_OVL1_OVL_DISP_WDMA1, "ovl1_ovl_disp_wdma1", "ck2_disp_ck", 1),
	GATE_OVL11_V(CLK_OVL1_OVL_DISP_WDMA1_DISP, "ovl1_ovl_disp_wdma1_disp", "ovl1_ovl_disp_wdma1"),
	GATE_VOTE_OVL11(CLK_OVL1_OVL_UFBC_WDMA0, "ovl1_ovl_ufbc_wdma0", "ck2_disp_ck", 2),
	GATE_OVL11_V(CLK_OVL1_OVL_UFBC_WDMA0_DISP, "ovl1_ovl_ufbc_wdma0_disp", "ovl1_ovl_ufbc_wdma0"),
	GATE_VOTE_OVL11(CLK_OVL1_OVL_MDP_RDMA0, "ovl1_ovl_mdp_rdma0", "ck2_disp_ck", 3),
	GATE_OVL11_V(CLK_OVL1_OVL_MDP_RDMA0_DISP, "ovl1_ovl_mdp_rdma0_disp", "ovl1_ovl_mdp_rdma0"),
	GATE_VOTE_OVL11(CLK_OVL1_OVL_MDP_RDMA1, "ovl1_ovl_mdp_rdma1", "ck2_disp_ck", 4),
	GATE_OVL11_V(CLK_OVL1_OVL_MDP_RDMA1_DISP, "ovl1_ovl_mdp_rdma1_disp", "ovl1_ovl_mdp_rdma1"),
	GATE_VOTE_OVL11(CLK_OVL1_OVL_BWM0, "ovl1_ovl_bwm0", "ck2_disp_ck", 5),
	GATE_OVL11_V(CLK_OVL1_OVL_BWM0_DISP, "ovl1_ovl_bwm0_disp", "ovl1_ovl_bwm0"),
	GATE_VOTE_OVL11(CLK_OVL1_DLI0, "ovl1_dli0", "ck2_disp_ck", 6),
	GATE_OVL11_V(CLK_OVL1_DLI0_DISP, "ovl1_dli0_disp", "ovl1_dli0"),
	GATE_VOTE_OVL11(CLK_OVL1_DLI1, "ovl1_dli1", "ck2_disp_ck", 7),
	GATE_OVL11_V(CLK_OVL1_DLI1_DISP, "ovl1_dli1_disp", "ovl1_dli1"),
	GATE_VOTE_OVL11(CLK_OVL1_DLI2, "ovl1_dli2", "ck2_disp_ck", 8),
	GATE_OVL11_V(CLK_OVL1_DLI2_DISP, "ovl1_dli2_disp", "ovl1_dli2"),
	GATE_VOTE_OVL11(CLK_OVL1_DLI3, "ovl1_dli3", "ck2_disp_ck", 9),
	GATE_OVL11_V(CLK_OVL1_DLI3_DISP, "ovl1_dli3_disp", "ovl1_dli3"),
	GATE_VOTE_OVL11(CLK_OVL1_DLI4, "ovl1_dli4", "ck2_disp_ck", 10),
	GATE_OVL11_V(CLK_OVL1_DLI4_DISP, "ovl1_dli4_disp", "ovl1_dli4"),
	GATE_VOTE_OVL11(CLK_OVL1_DLI5, "ovl1_dli5", "ck2_disp_ck", 11),
	GATE_OVL11_V(CLK_OVL1_DLI5_DISP, "ovl1_dli5_disp", "ovl1_dli5"),
	GATE_VOTE_OVL11(CLK_OVL1_DLI6, "ovl1_dli6", "ck2_disp_ck", 12),
	GATE_OVL11_V(CLK_OVL1_DLI6_DISP, "ovl1_dli6_disp", "ovl1_dli6"),
	GATE_VOTE_OVL11(CLK_OVL1_DLI7, "ovl1_dli7", "ck2_disp_ck", 13),
	GATE_OVL11_V(CLK_OVL1_DLI7_DISP, "ovl1_dli7_disp", "ovl1_dli7"),
	GATE_VOTE_OVL11(CLK_OVL1_DLI8, "ovl1_dli8", "ck2_disp_ck", 14),
	GATE_OVL11_V(CLK_OVL1_DLI8_DISP, "ovl1_dli8_disp", "ovl1_dli8"),
	GATE_VOTE_OVL11(CLK_OVL1_DLO0, "ovl1_dlo0", "ck2_disp_ck", 15),
	GATE_OVL11_V(CLK_OVL1_DLO0_DISP, "ovl1_dlo0_disp", "ovl1_dlo0"),
	GATE_VOTE_OVL11(CLK_OVL1_DLO1, "ovl1_dlo1", "ck2_disp_ck", 16),
	GATE_OVL11_V(CLK_OVL1_DLO1_DISP, "ovl1_dlo1_disp", "ovl1_dlo1"),
	GATE_VOTE_OVL11(CLK_OVL1_DLO2, "ovl1_dlo2", "ck2_disp_ck", 17),
	GATE_OVL11_V(CLK_OVL1_DLO2_DISP, "ovl1_dlo2_disp", "ovl1_dlo2"),
	GATE_VOTE_OVL11(CLK_OVL1_DLO3, "ovl1_dlo3", "ck2_disp_ck", 18),
	GATE_OVL11_V(CLK_OVL1_DLO3_DISP, "ovl1_dlo3_disp", "ovl1_dlo3"),
	GATE_VOTE_OVL11(CLK_OVL1_DLO4, "ovl1_dlo4", "ck2_disp_ck", 19),
	GATE_OVL11_V(CLK_OVL1_DLO4_DISP, "ovl1_dlo4_disp", "ovl1_dlo4"),
	GATE_VOTE_OVL11(CLK_OVL1_DLO5, "ovl1_dlo5", "ck2_disp_ck", 20),
	GATE_OVL11_V(CLK_OVL1_DLO5_DISP, "ovl1_dlo5_disp", "ovl1_dlo5"),
	GATE_VOTE_OVL11(CLK_OVL1_DLO6, "ovl1_dlo6", "ck2_disp_ck", 21),
	GATE_OVL11_V(CLK_OVL1_DLO6_DISP, "ovl1_dlo6_disp", "ovl1_dlo6"),
	GATE_VOTE_OVL11(CLK_OVL1_DLO7, "ovl1_dlo7", "ck2_disp_ck", 22),
	GATE_OVL11_V(CLK_OVL1_DLO7_DISP, "ovl1_dlo7_disp", "ovl1_dlo7"),
	GATE_VOTE_OVL11(CLK_OVL1_DLO8, "ovl1_dlo8", "ck2_disp_ck", 23),
	GATE_OVL11_V(CLK_OVL1_DLO8_DISP, "ovl1_dlo8_disp", "ovl1_dlo8"),
	GATE_VOTE_OVL11(CLK_OVL1_DLO9, "ovl1_dlo9", "ck2_disp_ck", 24),
	GATE_OVL11_V(CLK_OVL1_DLO9_DISP, "ovl1_dlo9_disp", "ovl1_dlo9"),
	GATE_VOTE_OVL11(CLK_OVL1_DLO10, "ovl1_dlo10", "ck2_disp_ck", 25),
	GATE_OVL11_V(CLK_OVL1_DLO10_DISP, "ovl1_dlo10_disp", "ovl1_dlo10"),
	GATE_VOTE_OVL11(CLK_OVL1_DLO11, "ovl1_dlo11", "ck2_disp_ck", 26),
	GATE_OVL11_V(CLK_OVL1_DLO11_DISP, "ovl1_dlo11_disp", "ovl1_dlo11"),
	GATE_VOTE_OVL11(CLK_OVL1_DLO12, "ovl1_dlo12", "ck2_disp_ck", 27),
	GATE_OVL11_V(CLK_OVL1_DLO12_DISP, "ovl1_dlo12_disp", "ovl1_dlo12"),
	GATE_VOTE_OVL11(CLK_OVL1_OVLSYS_RELAY0, "ovl1_ovlsys_relay0", "ck2_disp_ck", 28),
	GATE_OVL11_V(CLK_OVL1_OVLSYS_RELAY0_DISP, "ovl1_ovlsys_relay0_disp", "ovl1_ovlsys_relay0"),
	GATE_VOTE_OVL11(CLK_OVL1_OVL_INLINEROT0, "ovl1_ovl_inlinerot0", "ck2_disp_ck", 29),
	GATE_OVL11_V(CLK_OVL1_OVL_INLINEROT0_DISP, "ovl1_ovl_inlinerot0_disp", "ovl1_ovl_inlinerot0"),
	GATE_VOTE_OVL11(CLK_OVL1_SMI, "ovl1_smi", "ck2_disp_ck", 30),
	GATE_OVL11_V(CLK_OVL1_SMI_SMI, "ovl1_smi_smi", "ovl1_smi"),
};

static const struct mtk_clk_desc ovl1_mcd = {
	.clks = ovl1_clks,
	.num_clks = ARRAY_SIZE(ovl1_clks),
};

static const struct platform_device_id clk_mt8196_ovl1_id_table[] = {
	{ .name = "clk-mt8196-ovl1", .driver_data = (kernel_ulong_t)&ovl1_mcd },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(platform, clk_mt8196_ovl1_id_table);

static struct platform_driver clk_mt8196_ovl1_drv = {
	.probe = mtk_clk_pdev_probe,
	.remove = mtk_clk_pdev_remove,
	.driver = {
		.name = "clk-mt8196-ovl1",
	},
	.id_table = clk_mt8196_ovl1_id_table,
};

module_platform_driver(clk_mt8196_ovl1_drv);
MODULE_LICENSE("GPL");
