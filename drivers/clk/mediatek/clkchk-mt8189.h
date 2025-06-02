/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2024 MediaTek Inc.
 * Author: Qiqi Wang <qiqi.wang@mediatek.com>
 */

#ifndef __DRV_CLKCHK_MT8189_H
#define __DRV_CLKCHK_MT8189_H

enum chk_sys_id {
	top = 0,
	ifrao = 1,
	infracfg_ao_reg_bus = 2,
	apmixed = 3,
	emicfg_ao_mem = 4,
	perao = 5,
	afe = 6,
	ufscfg_ao_reg = 7,
	ufscfg_pdn_reg = 8,
	impws = 9,
	impe = 10,
	imps = 11,
	impen = 12,
	mfg = 13,
	mm = 14,
	imgsys1 = 15,
	imgsys2 = 16,
	vdec_core = 17,
	ven1 = 18,
	spm = 19,
	vlpcfg_reg_bus = 20,
	vlp_ck = 21,
	scp_iic = 22,
	scp = 23,
	vad = 24,
	cam_m = 25,
	cam_ra = 26,
	cam_rb = 27,
	ipe = 28,
	vlpcfg_ao_reg = 29,
	dvfsrc_top = 30,
	mminfra_config = 31,
	gce_d = 32,
	gce_m = 33,
	mdp = 34,
	dbgao = 35,
	dem = 36,
	hwv = 37,
	hwv_ext = 38,
	hwv_wrt = 39,
	chk_sys_num = 40,
};

enum chk_pd_id {
	MT8189_CHK_PD_CONN = 0,
	MT8189_CHK_PD_UFS0 = 1,
	MT8189_CHK_PD_UFS0_PHY = 2,
	MT8189_CHK_PD_AUDIO = 3,
	MT8189_CHK_PD_ADSP_TOP_DORMANT = 4,
	MT8189_CHK_PD_ADSP_INFRA = 5,
	MT8189_CHK_PD_ADSP_AO = 6,
	MT8189_CHK_PD_MM_INFRA = 7,
	MT8189_CHK_PD_ISP_IMG1 = 8,
	MT8189_CHK_PD_ISP_IMG2 = 9,
	MT8189_CHK_PD_ISP_IPE = 10,
	MT8189_CHK_PD_VDE0 = 11,
	MT8189_CHK_PD_VEN0 = 12,
	MT8189_CHK_PD_CAM_MAIN = 13,
	MT8189_CHK_PD_CAM_SUBA = 14,
	MT8189_CHK_PD_CAM_SUBB = 15,
	MT8189_CHK_PD_MDP0 = 16,
	MT8189_CHK_PD_DIS0 = 17,
	MT8189_CHK_PD_DP_TX = 18,
	MT8189_CHK_PD_CSI_RX = 19,
	MT8189_CHK_PD_SSUSB = 20,
	MT8189_CHK_PD_MFG0 = 21,
	MT8189_CHK_PD_MFG1 = 22,
	MT8189_CHK_PD_MFG2 = 23,
	MT8189_CHK_PD_MFG3 = 24,
	MT8189_CHK_PD_EDP_TX_DORMANT = 25,
	MT8189_CHK_PD_PCIE = 26,
	MT8189_CHK_PD_PCIE_PHY = 27,
	MT8189_CHK_PD_APU = 28,
	MT8189_CHK_PD_NUM,
};

#ifdef CONFIG_MTK_DVFSRC_HELPER
extern int get_sw_req_vcore_opp(void);
#endif

extern void print_subsys_reg_mt8189(enum chk_sys_id id);
extern void set_subsys_reg_dump_mt8189(enum chk_sys_id id[]);
extern void get_subsys_reg_dump_mt8189(void);
extern u32 get_mt8189_reg_value(u32 id, u32 ofs);
#endif	/* __DRV_CLKCHK_MT8189_H */
