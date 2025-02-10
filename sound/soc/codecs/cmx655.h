/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef CMX655_H
#define CMX655_H

#include <linux/gpio.h>
#include <linux/regmap.h>
#include <sound/pcm.h>

#define CMX655_ISR      (0x00)
#define     CMX655_ISR_MICR        BIT(0)
#define     CMX655_ISR_MICL        BIT(1)
#define     CMX655_ISR_AMPOC       BIT(2)
#define     CMX655_ISR_AMPCLIP     BIT(3)
#define     CMX655_ISR_CLKRDY      BIT(4)
#define     CMX655_ISR_THERM       BIT(5)
#define     CMX655_ISR_VOL         BIT(6)
#define     CMX655_ISR_CAL         BIT(7)

#define CMX655_ISM      (0x01)
#define     CMX655_ISM_MICR        BIT(0)
#define     CMX655_ISM_MICL        BIT(1)
#define     CMX655_ISM_AMPOC       BIT(2)
#define     CMX655_ISM_AMPCLIP     BIT(3)
#define     CMX655_ISM_CLKRDY      BIT(4)
#define     CMX655_ISM_THERM       BIT(5)
#define     CMX655_ISM_VOL         BIT(6)
#define     CMX655_ISM_CAL         BIT(7)
#define CMX655_ISE      (0x02)
#define CMX655_CLKCTRL  (0x03)
#define     CMX655_CLKCTRL_PREDIV_SHIFT    (0)
#define     CMX655_CLKCTRL_PREDIV_VALUE    (0x3)
#define     CMX655_CLKCTRL_PREDIV_MASK     (CMX655_CLKCTRL_PREDIV_VALUE << \
					CMX655_CLKCTRL_PREDIV_SHIFT)
#define     CMX655_CLKCTRL_CLRSRC_SHIFT    (2)
#define     CMX655_CLKCTRL_CLRSRC_VALUE    (0x7)
#define     CMX655_CLKCTRL_CLRSRC_MASK     (CMX655_CLKCTRL_CLRSRC_VALUE << \
					CMX655_CLKCTRL_CLRSRC_SHIFT)
#define     CMX655_CLKCTRL_CLRSRC_RCLK     (0x0 << CMX655_CLKCTRL_CLRSRC_SHIFT)
#define     CMX655_CLKCTRL_CLRSRC_LPO      (0x1 << CMX655_CLKCTRL_CLRSRC_SHIFT)
#define     CMX655_CLKCTRL_CLRSRC_LRCLK    (0x7 << CMX655_CLKCTRL_CLRSRC_SHIFT)
#define     CMX655_CLKCTRL_SR_SHIFT        (5)
#define     CMX655_CLKCTRL_SR_VALUE        (0x3)
#define     CMX655_CLKCTRL_SR_MASK         (CMX655_CLKCTRL_SR_VALUE << \
					CMX655_CLKCTRL_SR_SHIFT)
#define     CMX655_CLKCTRL_SR_8K            (0x0 << CMX655_CLKCTRL_SR_SHIFT)
#define     CMX655_CLKCTRL_SR_16K           (0x1 << CMX655_CLKCTRL_SR_SHIFT)
#define     CMX655_CLKCTRL_SR_32K           (0x2 << CMX655_CLKCTRL_SR_SHIFT)
#define     CMX655_CLKCTRL_SR_48K           (0x3 << CMX655_CLKCTRL_SR_SHIFT)

#define CMX655_RDIVHI   (0x04)
#define CMX655_RDIVLO   (0x05)
#define CMX655_NDIVHI   (0x06)
#define CMX655_NDIVLO   (0x07)
#define CMX655_PLLCTRL  (0x08)
#define     CMX655_PLLCTRL_CPI_SHIFT       (0)
#define     CMX655_PLLCTRL_LFILT_SHIFT     (4)
#define CMX655_SAICTRL  (0x09)
#define     CMX655_SAI_PCM         BIT(0)
#define     CMX655_SAI_BINV        BIT(2)
#define     CMX655_SAI_POL         BIT(3)
#define     CMX655_SAI_DLY         BIT(4)
#define     CMX655_SAI_MONO        BIT(5)
#define     CMX655_SAI_WL          BIT(6)
#define     CMX655_SAI_MSTR        BIT(7)

#define CMX655_SAIMUX   (0x0a)
#define CMX655_RVF      (0x0c)
#define     CMX655_VF_DCBLOCK_SHIFT     (2)
#define     CMX655_VF_DCBLOCK           (0x1 << CMX655_VF_DCBLOCK_SHIFT)
#define CMX655_LDCTRL   (0x0d)
#define CMX655_RDCTRL   (0x0e)
#define CMX655_LEVEL    (0x0f)
#define CMX655_NGCTRL   (0x1c)
#define CMX655_NGTIME   (0x1d)
#define CMX655_NGLSTAT  (0x1e)
#define CMX655_NGRSTAT  (0x1f)
#define CMX655_PVF      (0x28)
#define CMX655_PREAMP   (0x29)
#define CMX655_VOLUME   (0x2a)
#define CMX655_ALCCTRL  (0x2b)
#define CMX655_ALCTIME  (0x2c)
#define CMX655_ALCGAIN  (0x2d)
#define CMX655_ALCSTAT  (0x2e)
#define CMX655_DST      (0x2f)
#define CMX655_CPR      (0x30)
#define CMX655_SYSCTRL  (0x32)
#define     CMX655_SYSCTRL_MICR    BIT(0)
#define     CMX655_SYSCTRL_MICL    BIT(1)
#define     CMX655_SYSCTRL_PAMP    BIT(3)
#define     CMX655_SYSCTRL_LOUT    BIT(4)
#define     CMX655_SYSCTRL_SAI     BIT(5)

#define CMX655_COMMAND  (0x33)
#define     CMX655_CMD_CLOCK_STOP  (0x00)
#define     CMX655_CMD_CLOCK_START (0x01)
#define     CMX655_CMD_SOFT_RESET  (0xff)

/*  GPIO connection for reset and irq */
#define CMX655_RESETN   (24)
#define CMX655_IRQN     (25)
#define CMX655_CS       (8)

#define CMX655_RATES (SNDRV_PCM_RATE_8000 | \
		SNDRV_PCM_RATE_16000 | \
		SNDRV_PCM_RATE_32000 | \
		SNDRV_PCM_RATE_48000)

#define CMX655_FMTS (SNDRV_PCM_FMTBIT_U8 | SNDRV_PCM_FMTBIT_S16_LE)

/*
 * clock id's when calling set sysclk
 * Auto = Use RCLK when in DAI primary mode. Use LRCLK in secondary mode.
 * DO NOT use CMX655_SYSCLK_LRCLK when in DAI primary mode
 */
#define CMX655_SYSCLK_AUTO  (0)
#define CMX655_SYSCLK_RCLK  (1)
#define CMX655_SYSCLK_LRCLK (2)
#define CMX655_SYSCLK_LPO   (3)
#define CMX655_SYSCLK_MIN   (CMX655_SYSCLK_AUTO)
#define CMX655_SYSCLK_MAX   (CMX655_SYSCLK_LPO)

/* CMX655 microphone widget helper macro */
#define CMX655_DAPM_MIC(wname, wshift) \
  (struct snd_soc_dapm_widget) { \
    .id = snd_soc_dapm_mic, .name = wname, .kcontrol_news = NULL, \
    .num_kcontrols = 0, .reg = CMX655_SYSCTRL, .shift = wshift, \
    .mask = 1, .on_val = 1, .off_val = 0, .event = cmx655_mic_dapm_event, \
    .event_flags = SND_SOC_DAPM_POST_PMU }

struct cmx655_dai_data {
	int sys_clk;
	unsigned int enabled_streams;
	bool best_clk_running;
	int clk_src;
};

struct cmx655_data {
	struct regmap *regmap;
	struct cmx655_dai_data dai_data;
	struct gpio_desc *reset_gpio;
	int irq;
	/* Number of times the class-D overcurrent has been reset */
	unsigned int oc_cnt;
	/* Max times the class-D overcurrent should be reset */
	unsigned int oc_cnt_max;
};

extern const struct regmap_config cmx655_regmap;

int cmx655_common_register_component(struct device *dev, struct regmap *regmap, int irq);
void cmx655_common_unregister_component(struct device *dev);

#endif
