/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Qualcomm MSM8x60 family (MSM8260/MSM8660/APQ8060) MPM wake-source consumer interface.
 *
 * The MPM driver lives at drivers/irqchip/irq-msm8660-mpm.c. It exposes
 * TWO interfaces:
 *
 *  1. Hierarchical irqdomain (preferred). For wake sources that map to
 *     GIC SPIs (USB1_HS, HDMI, ...). Consumers wire their IRQ through
 *     the MPM via `interrupts-extended = <&msm8660_mpm ...>` in DT and
 *     the IRQ subsystem manages enable / mask / set_type via the
 *     irqdomain alloc path. No explicit C API call needed.
 *
 *  2. Raw-pin API (this header). For wake sources that do NOT have a
 *     GIC IRQ mapping: SDC3_DAT1=21, SDC3_DAT3=22, SDC4_DAT1=23,
 *     SDC4_DAT3=24. These are physical wake-signal lines that MPM
 *     monitors directly. mmci (for SDC4 WiFi wake) obtains a handle
 *     via msm8660_mpm_get() and uses the helpers below.
 */

#ifndef __SOC_QCOM_MSM8660_MPM_H__
#define __SOC_QCOM_MSM8660_MPM_H__

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/irq.h>

#define MSM8660_MPM_NR_PINS	64

/* Well-known wake-source pin indices (from the legacy 2.6.35-palm tree). */
#define MSM8660_MPM_PIN_SDC3_DAT1	21
#define MSM8660_MPM_PIN_SDC3_DAT3	22
#define MSM8660_MPM_PIN_SDC4_DAT1	23
#define MSM8660_MPM_PIN_SDC4_DAT3	24

struct device;
struct device_node;
struct msm8660_mpm;

#if IS_ENABLED(CONFIG_QCOM_MSM8660_MPM)

struct msm8660_mpm *msm8660_mpm_get(struct device *consumer,
				    struct device_node *np,
				    const char *propname);

int msm8660_mpm_set_pin_wake(struct msm8660_mpm *mpm, unsigned int pin,
			     bool on);
int msm8660_mpm_enable_pin(struct msm8660_mpm *mpm, unsigned int pin,
			   bool enable);
int msm8660_mpm_set_pin_type(struct msm8660_mpm *mpm, unsigned int pin,
			     unsigned int flow_type);

#else /* !CONFIG_QCOM_MSM8660_MPM */

static inline struct msm8660_mpm *
msm8660_mpm_get(struct device *consumer, struct device_node *np,
		const char *propname)
{
	return ERR_PTR(-ENODEV);
}

static inline int msm8660_mpm_set_pin_wake(struct msm8660_mpm *mpm,
					   unsigned int pin, bool on)
{
	return -ENODEV;
}

static inline int msm8660_mpm_enable_pin(struct msm8660_mpm *mpm,
					 unsigned int pin, bool enable)
{
	return -ENODEV;
}

static inline int msm8660_mpm_set_pin_type(struct msm8660_mpm *mpm,
					   unsigned int pin,
					   unsigned int flow_type)
{
	return -ENODEV;
}

#endif /* CONFIG_QCOM_MSM8660_MPM */

#endif /* __SOC_QCOM_MSM8660_MPM_H__ */
