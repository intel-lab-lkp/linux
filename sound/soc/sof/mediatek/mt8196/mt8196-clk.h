/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause) */
/*
 * Copyright(c) 2025 MediaTek Inc. All rights reserved.
 * Author: Hailong Fan <hailong.fan@mediatek.com>
 */

#ifndef __MT8196_CLK_H
#define __MT8196_CLK_H

struct snd_sof_dev;

/* DSP clock */
enum adsp_clk_id {
	CLK_TOP_ADSP_SEL,
	CLK_TOP_CLK26M,
	CLK_TOP_ADSPPLL,
	ADSP_CLK_MAX
};

int mt8196_adsp_init_clock(struct snd_sof_dev *sdev);
int mt8196_adsp_clock_on(struct snd_sof_dev *sdev);
void mt8196_adsp_clock_off(struct snd_sof_dev *sdev);
#endif
