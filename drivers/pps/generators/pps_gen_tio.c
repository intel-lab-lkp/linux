// SPDX-License-Identifier: GPL-2.0-only
/*
 * Intel PPS signal Generator Driver
 *
 * Copyright (C) 2023 Intel Corporation
 */

#include <linux/bits.h>
#include <linux/bitfield.h>
#include <linux/cleanup.h>
#include <linux/container_of.h>
#include <linux/cpu.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/hrtimer.h>
#include <linux/io-64-nonatomic-hi-lo.h>
#include <linux/kstrtox.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>
#include <linux/timekeeping.h>
#include <linux/types.h>

#include <asm/cpu_device_id.h>

#define TIOCTL			0x00
#define TIOCOMPV		0x10

/* Control Register */
#define TIOCTL_EN			BIT(0)
#define TIOCTL_DIR			BIT(1)
#define TIOCTL_EP			GENMASK(3, 2)
#define TIOCTL_EP_RISING_EDGE		FIELD_PREP(TIOCTL_EP, 0)
#define TIOCTL_EP_FALLING_EDGE		FIELD_PREP(TIOCTL_EP, 1)
#define TIOCTL_EP_TOGGLE_EDGE		FIELD_PREP(TIOCTL_EP, 2)

#define SAFE_TIME_NS			(10 * NSEC_PER_MSEC) /* Safety time to set hrtimer early */
#define MAGIC_CONST			(NSEC_PER_SEC - SAFE_TIME_NS)
#define ART_HW_DELAY_CYCLES		2

struct pps_tio {
	struct hrtimer timer;
	struct device *dev;
	spinlock_t lock;
	struct attribute_group attrs;
	void __iomem *base;
	bool enabled;
};

static inline u32 pps_ctl_read(struct pps_tio *tio)
{
	return readl(tio->base + TIOCTL);
}

static inline void pps_ctl_write(struct pps_tio *tio, u32 value)
{
	writel(value, tio->base + TIOCTL);
}

/* For COMPV register, It's safer to write higher 32-bit followed by lower 32-bit */
static inline void pps_compv_write(struct pps_tio *tio, u64 value)
{
	hi_lo_writeq(value, tio->base + TIOCOMPV);
}

static inline ktime_t first_event(struct pps_tio *tio)
{
	return ktime_set(ktime_get_real_seconds() + 1, MAGIC_CONST);
}

static u32 pps_tio_disable(struct pps_tio *tio)
{
	u32 ctrl;

	ctrl = pps_ctl_read(tio);
	pps_compv_write(tio, 0);

	ctrl &= ~TIOCTL_EN;
	pps_ctl_write(tio, ctrl);

	return ctrl;
}

static void pps_tio_direction_output(struct pps_tio *tio)
{
	u32 ctrl;

	ctrl = pps_tio_disable(tio);

	/* We enable the device, be sure that the 'compare' value is invalid */
	pps_compv_write(tio, 0);

	ctrl &= ~(TIOCTL_DIR | TIOCTL_EP);
	ctrl |= TIOCTL_EP_TOGGLE_EDGE;
	pps_ctl_write(tio, ctrl);

	ctrl |= TIOCTL_EN;
	pps_ctl_write(tio, ctrl);
}

static bool pps_generate_next_pulse(struct pps_tio *tio, ktime_t expires)
{
	u64 art;

	if (!ktime_real_to_base_clock(expires, CSID_X86_ART, &art)) {
		pps_tio_disable(tio);
		return false;
	}

	pps_compv_write(tio, art - ART_HW_DELAY_CYCLES);
	return true;
}

static enum hrtimer_restart hrtimer_callback(struct hrtimer *timer)
{
	struct pps_tio *tio = container_of(timer, struct pps_tio, timer);
	ktime_t expires, now;

	guard(spinlock)(&tio->lock);

	expires = hrtimer_get_expires(timer);
	now = ktime_get_real();

	if (now - expires < SAFE_TIME_NS) {
		if (!pps_generate_next_pulse(tio, expires + SAFE_TIME_NS))
			return HRTIMER_NORESTART;
	}

	hrtimer_forward(timer, now, NSEC_PER_SEC / 2);
	return HRTIMER_RESTART;
}

static ssize_t enable_store(struct device *dev, struct device_attribute *attr, const char *buf,
			    size_t count)
{
	struct pps_tio *tio = dev_get_drvdata(dev);
	bool enable;
	int err;

	err = kstrtobool(buf, &enable);
	if (err)
		return err;

	guard(spinlock_irqsave)(&tio->lock);
	if (enable && !tio->enabled) {
		if (!timekeeping_clocksource_has_base(CSID_X86_ART)) {
			dev_err(tio->dev, "PPS cannot be started as clock is not related to ART");
			return -EPERM;
		}
		pps_tio_direction_output(tio);
		hrtimer_start(&tio->timer, first_event(tio), HRTIMER_MODE_ABS);
		tio->enabled = true;
	} else if (!enable && tio->enabled) {
		hrtimer_cancel(&tio->timer);
		pps_tio_disable(tio);
		tio->enabled = false;
	}
	return count;
}

static ssize_t enable_show(struct device *dev, struct device_attribute *devattr, char *buf)
{
	struct pps_tio *tio = dev_get_drvdata(dev);
	u32 ctrl;

	ctrl = pps_ctl_read(tio);
	ctrl &= TIOCTL_EN;

	return sysfs_emit(buf, "%u\n", ctrl);
}
static DEVICE_ATTR_RW(enable);

static struct attribute *pps_tio_attrs[] = {
	&dev_attr_enable.attr,
	NULL
};
ATTRIBUTE_GROUPS(pps_tio);

static int pps_tio_probe(struct platform_device *pdev)
{
	struct pps_tio *tio;

	if (!(cpu_feature_enabled(X86_FEATURE_TSC_KNOWN_FREQ) &&
	      cpu_feature_enabled(X86_FEATURE_ART))) {
		dev_warn(&pdev->dev, "TSC/ART is not enabled");
		return -ENODEV;
	}

	tio = devm_kzalloc(&pdev->dev, sizeof(*tio), GFP_KERNEL);
	if (!tio)
		return -ENOMEM;

	tio->dev = &pdev->dev;
	tio->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(tio->base))
		return PTR_ERR(tio->base);

	pps_tio_disable(tio);
	hrtimer_init(&tio->timer, CLOCK_REALTIME, HRTIMER_MODE_ABS);
	tio->timer.function = hrtimer_callback;
	spin_lock_init(&tio->lock);
	tio->enabled = false;
	platform_set_drvdata(pdev, tio);

	return 0;
}

static int pps_tio_remove(struct platform_device *pdev)
{
	struct pps_tio *tio = platform_get_drvdata(pdev);

	hrtimer_cancel(&tio->timer);
	pps_tio_disable(tio);

	return 0;
}

static const struct acpi_device_id intel_pmc_tio_acpi_match[] = {
	{ "INTC1021" },
	{ "INTC1022" },
	{ "INTC1023" },
	{ "INTC1024" },
	{}
};
MODULE_DEVICE_TABLE(acpi, intel_pmc_tio_acpi_match);

static struct platform_driver pps_tio_driver = {
	.probe          = pps_tio_probe,
	.remove         = pps_tio_remove,
	.driver         = {
		.name                   = "intel-pps-generator",
		.acpi_match_table       = intel_pmc_tio_acpi_match,
		.dev_groups             = pps_tio_groups,
	},
};
module_platform_driver(pps_tio_driver);

MODULE_AUTHOR("Lakshmi Sowjanya D <lakshmi.sowjanya.d@intel.com>");
MODULE_AUTHOR("Christopher Hall <christopher.s.hall@intel.com>");
MODULE_AUTHOR("Pandith N <pandith.n@intel.com>");
MODULE_AUTHOR("Thejesh Reddy T R <thejesh.reddy.t.r@intel.com>");
MODULE_DESCRIPTION("Intel PMC Time-Aware IO Generator Driver");
MODULE_LICENSE("GPL");
