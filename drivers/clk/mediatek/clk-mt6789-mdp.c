// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022 MediaTek Inc.
 * Copyright (c) 2025 Arseniy Velikanov <me@adomerle.pw>
 */

#include <linux/clk-provider.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>

#include "clk-mtk.h"
#include "clk-gate.h"

#include <dt-bindings/clock/mediatek,mt6789-clk.h>

static const struct mtk_gate_regs mdp0_cg_regs = {
	.set_ofs = 0x104,
	.clr_ofs = 0x108,
	.sta_ofs = 0x100,
};

static const struct mtk_gate_regs mdp1_cg_regs = {
	.set_ofs = 0x124,
	.clr_ofs = 0x128,
	.sta_ofs = 0x120,
};

#define GATE_MDP0(_id, _name, _parent, _shift)	\
	GATE_MTK(_id, _name, _parent, &mdp0_cg_regs, _shift, &mtk_clk_gate_ops_setclr)

#define GATE_MDP1(_id, _name, _parent, _shift)	\
	GATE_MTK(_id, _name, _parent, &mdp1_cg_regs, _shift, &mtk_clk_gate_ops_setclr)

static const struct mtk_gate mdp_clks[] = {
	GATE_MDP0(CLK_MDP_RDMA0, "mdp_rdma0", "mdp_ck", 0),
	GATE_MDP0(CLK_MDP_TDSHP0, "mdp_tdshp0", "mdp_ck", 1),
	GATE_MDP0(CLK_MDP_IMG_DL_ASYNC0, "mdp_img_dl_async0", "mdp_ck", 2),
	GATE_MDP0(CLK_MDP_IMG_DL_ASYNC1, "mdp_img_dl_async1", "mdp_ck", 3),
	GATE_MDP0(CLK_MDP_SMI0, "mdp_smi0", "mdp_ck", 6),
	GATE_MDP0(CLK_MDP_APB_BUS, "mdp_apb_bus", "mdp_ck", 7),
	GATE_MDP0(CLK_MDP_WROT0, "mdp_wrot0", "mdp_ck", 8),
	GATE_MDP0(CLK_MDP_RSZ0, "mdp_rsz0", "mdp_ck", 9),
	GATE_MDP0(CLK_MDP_HDR0, "mdp_hdr0", "mdp_ck", 10),
	GATE_MDP0(CLK_MDP_MUTEX0, "mdp_mutex0", "mdp_ck", 11),
	GATE_MDP0(CLK_MDP_WROT1, "mdp_wrot1", "mdp_ck", 12),
	GATE_MDP0(CLK_MDP_RSZ1, "mdp_rsz1", "mdp_ck", 13),
	GATE_MDP0(CLK_MDP_FAKE_ENG0, "mdp_fake_eng0", "mdp_ck", 14),
	GATE_MDP0(CLK_MDP_AAL0, "mdp_aal0", "mdp_ck", 15),

	GATE_MDP1(CLK_MDP_IMG_DL_RELAY0_ASYNC0, "mdp_img_dl_rel0_as0", "mdp_ck", 0),
	GATE_MDP1(CLK_MDP_IMG_DL_RELAY1_ASYNC1, "mdp_img_dl_rel1_as1", "mdp_ck", 8),
};

static const struct mtk_clk_desc mdp_desc = {
	.clks = mdp_clks,
	.num_clks = ARRAY_SIZE(mdp_clks),
};

static const struct of_device_id of_match_clk_mt6789_mdp[] = {
	{
		.compatible = "mediatek,mt6789-mdpsys",
		.data = &mdp_desc,
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6789_mdp);

static struct platform_driver clk_mt6789_mdp_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt6789-mdp",
		.of_match_table = of_match_clk_mt6789_mdp,
	},
};

module_platform_driver(clk_mt6789_mdp_drv);

MODULE_DESCRIPTION("MediaTek MT6789 Multimedia Data Path clocks driver");
MODULE_LICENSE("GPL");
