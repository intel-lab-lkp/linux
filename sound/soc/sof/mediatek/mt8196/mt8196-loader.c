// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
/*
 * Copyright(c) 2025 MediaTek Inc. All rights reserved.
 * Author: Hailong Fan <hailong.fan@mediatek.com>
 */

// Hardware interface for mt8196 DSP code loader

#include <sound/sof.h>
#include "mt8196.h"
#include "../../ops.h"

void mt8196_sof_hifixdsp_boot_sequence(struct snd_sof_dev *sdev, u32 boot_addr)
{
	/* step1. clr ADSP_ALTVEC_C0 */
	snd_sof_dsp_write(sdev, DSP_SECREG_BAR, ADSP_ALTVEC_C0, 0);
	snd_sof_dsp_write(sdev, DSP_SECREG_BAR, ADSP_ALTVECSEL, 0);

	/* step2. set core boot address */
	snd_sof_dsp_write(sdev, DSP_SECREG_BAR, ADSP_ALTVEC_C0, boot_addr);
	snd_sof_dsp_write(sdev, DSP_SECREG_BAR, ADSP_ALTVECSEL, ADSP_ALTVECSEL_C0);

	/* step3. set RUNSTALL to stop core */
	snd_sof_dsp_update_bits(sdev, DSP_REG_BAR, ADSP_HIFI_RUNSTALL,
				RUNSTALL, RUNSTALL);

	/* enable mbox 0 & 1 IRQ */
	snd_sof_dsp_update_bits(sdev, DSP_REG_BAR, ADSP_MBOX_IRQ_EN,
				DSP_MBOX0_IRQ_EN | DSP_MBOX1_IRQ_EN,
				DSP_MBOX0_IRQ_EN | DSP_MBOX1_IRQ_EN);

	/* step4. assert core reset */
	snd_sof_dsp_update_bits(sdev, DSP_REG_BAR, ADSP_CFGREG_SW_RSTN,
				SW_RSTN_C0 | SW_DBG_RSTN_C0,
				SW_RSTN_C0 | SW_DBG_RSTN_C0);

	/* hardware requirement */
	udelay(1);

	/* step5. release core reset */
	snd_sof_dsp_update_bits(sdev, DSP_REG_BAR, ADSP_CFGREG_SW_RSTN,
				SW_RSTN_C0 | SW_DBG_RSTN_C0,
				0);

	/* step6. clear RUNSTALL to start core */
	snd_sof_dsp_update_bits(sdev, DSP_REG_BAR, ADSP_HIFI_RUNSTALL,
				RUNSTALL, 0);
}

void mt8196_sof_hifixdsp_shutdown(struct snd_sof_dev *sdev)
{
	/* set RUNSTALL to stop core */
	snd_sof_dsp_update_bits(sdev, DSP_REG_BAR, ADSP_HIFI_RUNSTALL,
				RUNSTALL, RUNSTALL);

	/* assert core reset */
	snd_sof_dsp_update_bits(sdev, DSP_REG_BAR, ADSP_CFGREG_SW_RSTN,
				SW_RSTN_C0 | SW_DBG_RSTN_C0,
				SW_RSTN_C0 | SW_DBG_RSTN_C0);
}
