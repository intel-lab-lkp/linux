/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_PWM_RENESAS_RZG2L_GPT_H__
#define __LINUX_PWM_RENESAS_RZG2L_GPT_H__

#if IS_ENABLED(CONFIG_PWM_RENESAS_RZG2L_GPT)
u32 rzg2l_gpt_poeg_disable_req_irq_status(void *dev, u8 grp);
int rzg2l_gpt_poeg_disable_req_clr(void *gpt_device, u8 grp);
int rzg2l_gpt_pin_reenable(void *gpt_device, u8 grp);
int rzg2l_gpt_poeg_disable_req_both_high(void *gpt_device, u8 grp, bool on);
#else
static inline u32 rzg2l_gpt_poeg_disable_req_irq_status(void *dev, u8 grp)
{
	return -ENODEV;
}

static inline int rzg2l_gpt_poeg_disable_req_clr(void *gpt_device, u8 grp)
{
	return -ENODEV;
}

static inline int rzg2l_gpt_pin_reenable(void *gpt_device, u8 grp)
{
	return -ENODEV;
}

static inline int rzg2l_gpt_poeg_disable_req_both_high(void *gpt_device, u8 grp, bool on)
{
	return -ENODEV;
}

#endif

#endif /* __LINUX_PWM_RENESAS_RZG2L_GPT_H__ */
