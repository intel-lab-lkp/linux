/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#ifndef __MTK_DRAMC_H__
#define __MTK_DRAMC_H__

struct reg_ctrl_t {
	unsigned int offset;
	unsigned int mask;
	unsigned int shift;
};

struct fmeter_dev_t {
	unsigned int version;
	unsigned int crystal_freq;
	unsigned int shu_of;
	struct reg_ctrl_t shu_lv;
	struct reg_ctrl_t pll_id;
	struct reg_ctrl_t sdmpcw[2];
	struct reg_ctrl_t posdiv[2];
	struct reg_ctrl_t fbksel[2];
	struct reg_ctrl_t dqsopen[2];
	struct reg_ctrl_t async_ca[2];
	struct reg_ctrl_t dq_ser_mode[2];
};

struct dramc_dev_t {
	unsigned int support_ch_cnt;
	void __iomem **dramc_chn_base_ao;
	void __iomem **dramc_chn_base_nao;
	void __iomem **ddrphy_chn_base_ao;
	void __iomem **ddrphy_chn_base_nao;
	void __iomem *sleep_base;
	void *fmeter_dev_ptr;
};

unsigned int mtk_dramc_get_data_rate(void);

#endif /* __MTK_DRAMC_H__ */
