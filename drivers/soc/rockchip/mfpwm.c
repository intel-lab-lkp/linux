// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2025 Collabora Ltd.
 *
 * A driver to manage all the different functionalities exposed by Rockchip's
 * PWMv4 hardware.
 *
 * This driver is chiefly focused on guaranteeing non-concurrent operation
 * between the different device functions, as well as setting the clocks.
 * It registers the device function platform devices, e.g. PWM output or
 * PWM capture.
 *
 * Authors:
 *     Nicolas Frattaroli <nicolas.frattaroli@collabora.com>
 */

#include <linux/clk.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/overflow.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <soc/rockchip/mfpwm.h>

/**
 * struct rockchip_mfpwm - private mfpwm driver instance state struct
 * @pdev: pointer to this instance's &struct platform_device
 * @base: pointer to the memory mapped registers of this device
 * @pwm_clk: pointer to the PLL clock the PWM signal may be derived from
 * @osc_clk: pointer to the fixed crystal the PWM signal may be derived from
 * @chosen_clk: is one of either @pwm_clk or @osc_clk, depending on choice.
 *              May only be swapped out while holding @state_lock.
 * @pclk: pointer to the APB bus clock needed for mmio register access
 * @pwm_dev: pointer to the &struct platform_device of the pwm output driver
 * @counter_dev: pointer to the &struct platform_device of the counter driver
 * @active_func: pointer to the currently active device function, or %NULL if no
 *              device function is currently actively using any of the shared
 *              resources. May only be checked/modified with @state_lock held.
 * @acquire_cnt: number of times @active_func has currently mfpwm_acquire()'d
 *               it. Must only be checked or modified while holding @state_lock.
 * @pwmclk_enable_cnt: number of times @active_func has enabled the pwmclk sans
 *                     disabling it. Must only be checked or modified while
 *                     holding @state_lock. Only exists to fix a splat on mfpwm
 *                     driver remove.
 * @state_lock: this lock is held while either the active device function, the
 *              enable register, or the chosen clock is being changed.
 * @irq: the IRQ number of this device
 */
struct rockchip_mfpwm {
	struct platform_device *pdev;
	void __iomem *base;
	struct clk *pwm_clk;
	struct clk *osc_clk;
	struct clk *chosen_clk;
	struct clk *pclk;
	struct platform_device *pwm_dev;
	struct platform_device *counter_dev;
	struct rockchip_mfpwm_func *active_func;
	unsigned int acquire_cnt;
	unsigned int pwmclk_enable_cnt;
	spinlock_t state_lock;
	int irq;
};

static atomic_t subdev_id = ATOMIC_INIT(0);

static inline struct rockchip_mfpwm *to_rockchip_mfpwm(struct platform_device *pdev)
{
	return platform_get_drvdata(pdev);
}

unsigned long mfpwm_clk_get_rate(struct rockchip_mfpwm *mfpwm)
{
	if (!mfpwm || !mfpwm->chosen_clk)
		return 0;

	return clk_get_rate(mfpwm->chosen_clk);
}
EXPORT_SYMBOL_NS_GPL(mfpwm_clk_get_rate, "ROCKCHIP_MFPWM");

static int mfpwm_check_pwmf(const struct rockchip_mfpwm_func *pwmf,
			    const char *fname)
{
	if (IS_ERR_OR_NULL(pwmf)) {
		WARN(1, "called %s with an erroneous handle, no effect\n",
		     fname);
		return -EINVAL;
	}

	if (IS_ERR_OR_NULL(pwmf->parent)) {
		WARN(1, "called %s with an erroneous mfpwm_func parent, no effect\n",
		     fname);
		return -EINVAL;
	}

	return 0;
}

__attribute__((nonnull))
static bool mfpwm_pwmf_is_active_pwmf(const struct rockchip_mfpwm_func *pwmf)
{
	if (pwmf->parent->active_func) {
		if (pwmf->parent->active_func->id == pwmf->id)
			return true;
	}

	return false;
}

int mfpwm_pwmclk_enable(struct rockchip_mfpwm_func *pwmf)
{
	unsigned long flags;
	int ret;

	ret = mfpwm_check_pwmf(pwmf, "mfpwm_pwmclk_enable");
	if (ret)
		return ret;

	spin_lock_irqsave(&pwmf->parent->state_lock, flags);
	if (mfpwm_pwmf_is_active_pwmf(pwmf)) {
		ret = clk_enable(pwmf->parent->chosen_clk);
		pwmf->parent->pwmclk_enable_cnt++;
	} else {
		ret = -EBUSY;
	}

	spin_unlock_irqrestore(&pwmf->parent->state_lock, flags);

	return ret;
}
EXPORT_SYMBOL_NS_GPL(mfpwm_pwmclk_enable, "ROCKCHIP_MFPWM");

void mfpwm_pwmclk_disable(struct rockchip_mfpwm_func *pwmf)
{
	unsigned long flags;

	if (mfpwm_check_pwmf(pwmf, "mfpwm_pwmclk_enable"))
		return;

	spin_lock_irqsave(&pwmf->parent->state_lock, flags);
	if (mfpwm_pwmf_is_active_pwmf(pwmf)) {
		clk_disable(pwmf->parent->chosen_clk);
		pwmf->parent->pwmclk_enable_cnt--;
	}
	spin_unlock_irqrestore(&pwmf->parent->state_lock, flags);
}
EXPORT_SYMBOL_NS_GPL(mfpwm_pwmclk_disable, "ROCKCHIP_MFPWM");

__attribute__((nonnull))
static int mfpwm_do_acquire(struct rockchip_mfpwm_func *pwmf)
{
	struct rockchip_mfpwm *mfpwm = pwmf->parent;
	unsigned int cnt;

	if (mfpwm->active_func && pwmf->id != mfpwm->active_func->id)
		return -EBUSY;

	if (!mfpwm->active_func)
		mfpwm->active_func = pwmf;

	if (!check_add_overflow(mfpwm->acquire_cnt, 1, &cnt)) {
		mfpwm->acquire_cnt = cnt;
	} else {
		WARN(1, "prevented acquire counter overflow in %s\n", __func__);
		return -EOVERFLOW;
	}

	dev_dbg(&mfpwm->pdev->dev, "%d acquired mfpwm, acquires now at %u\n",
		pwmf->id, mfpwm->acquire_cnt);

	return clk_enable(mfpwm->pclk);
}

int mfpwm_acquire(struct rockchip_mfpwm_func *pwmf)
{
	struct rockchip_mfpwm *mfpwm;
	unsigned long flags;
	int ret = 0;

	ret = mfpwm_check_pwmf(pwmf, "mfpwm_acquire");
	if (ret)
		return ret;

	mfpwm = pwmf->parent;
	dev_dbg(&mfpwm->pdev->dev, "%d is attempting to acquire\n", pwmf->id);

	if (!spin_trylock_irqsave(&mfpwm->state_lock, flags))
		return -EBUSY;

	ret = mfpwm_do_acquire(pwmf);

	spin_unlock_irqrestore(&mfpwm->state_lock, flags);

	return ret;
}
EXPORT_SYMBOL_NS_GPL(mfpwm_acquire, "ROCKCHIP_MFPWM");

__attribute__((nonnull))
static void mfpwm_do_release(const struct rockchip_mfpwm_func *pwmf)
{
	struct rockchip_mfpwm *mfpwm = pwmf->parent;

	if (!mfpwm->active_func)
		return;

	if (mfpwm->active_func->id != pwmf->id)
		return;

	/*
	 * No need to check_sub_overflow here, !mfpwm->active_func above catches
	 * this type of problem already.
	 */
	mfpwm->acquire_cnt--;

	if (!mfpwm->acquire_cnt)
		mfpwm->active_func = NULL;

	clk_disable(mfpwm->pclk);
}

void mfpwm_release(const struct rockchip_mfpwm_func *pwmf)
{
	struct rockchip_mfpwm *mfpwm;
	unsigned long flags;

	if (mfpwm_check_pwmf(pwmf, "mfpwm_release"))
		return;

	mfpwm = pwmf->parent;

	spin_lock_irqsave(&mfpwm->state_lock, flags);
	mfpwm_do_release(pwmf);
	dev_dbg(&mfpwm->pdev->dev, "%d released mfpwm, acquires now at %u\n",
		pwmf->id, mfpwm->acquire_cnt);
	spin_unlock_irqrestore(&mfpwm->state_lock, flags);
}
EXPORT_SYMBOL_NS_GPL(mfpwm_release, "ROCKCHIP_MFPWM");

void mfpwm_remove_func(struct rockchip_mfpwm_func *pwmf)
{
	struct rockchip_mfpwm *mfpwm;
	unsigned long flags;

	if (mfpwm_check_pwmf(pwmf, "mfpwm_remove_func"))
		return;

	mfpwm = pwmf->parent;
	spin_lock_irqsave(&mfpwm->state_lock, flags);

	if (mfpwm_pwmf_is_active_pwmf(pwmf)) {
		dev_dbg(&mfpwm->pdev->dev, "removing active function %d\n",
			pwmf->id);

		while (mfpwm->acquire_cnt > 0)
			mfpwm_do_release(pwmf);
		for (; mfpwm->pwmclk_enable_cnt > 0; mfpwm->pwmclk_enable_cnt--)
			clk_disable(mfpwm->chosen_clk);

		mfpwm_reg_write(mfpwm->base, PWMV4_REG_ENABLE,
				PWMV4_EN(false) | PWMV4_CLK_EN(false));
	}

	if (mfpwm->pwm_dev && mfpwm->pwm_dev->id == pwmf->id) {
		dev_dbg(&mfpwm->pdev->dev, "clearing pwm_dev pointer\n");
		mfpwm->pwm_dev = NULL;
	} else if (mfpwm->counter_dev && mfpwm->counter_dev->id == pwmf->id) {
		dev_dbg(&mfpwm->pdev->dev, "clearing counter_dev pointer\n");
		mfpwm->counter_dev = NULL;
	} else {
		WARN(1, "trying to remove an unknown mfpwm device function");
	}

	spin_unlock_irqrestore(&mfpwm->state_lock, flags);
}
EXPORT_SYMBOL_NS_GPL(mfpwm_remove_func, "ROCKCHIP_MFPWM");

/**
 * mfpwm_register_subdev - register a single mfpwm_func
 * @mfpwm: pointer to the parent &struct rockchip_mfpwm
 * @target: pointer to where the &struct platform_device pointer should be
 *          stored, usually a member of @mfpwm
 * @name: sub-device name string
 *
 * Allocate a single &struct mfpwm_func, fill its members with appropriate data,
 * and register a new platform device, saving its pointer to @target. The
 * allocation is devres tracked, so will be automatically freed on mfpwm remove.
 *
 * Returns: 0 on success, negative errno on error
 */
static int mfpwm_register_subdev(struct rockchip_mfpwm *mfpwm,
				 struct platform_device **target,
				 const char *name)
{
	struct rockchip_mfpwm_func *func;
	struct platform_device *child;

	func = devm_kzalloc(&mfpwm->pdev->dev, sizeof(*func), GFP_KERNEL);
	if (IS_ERR(func))
		return PTR_ERR(func);
	func->irq = mfpwm->irq;
	func->parent = mfpwm;
	func->id = atomic_inc_return(&subdev_id);
	func->base = mfpwm->base;
	child = platform_device_register_data(&mfpwm->pdev->dev, name, func->id,
					      func, sizeof(*func));

	if (IS_ERR(child))
		return PTR_ERR(child);

	*target = child;

	return 0;
}

static int mfpwm_register_subdevs(struct rockchip_mfpwm *mfpwm)
{
	int ret;

	ret = mfpwm_register_subdev(mfpwm, &mfpwm->pwm_dev, "pwm-rockchip-v4");
	if (ret)
		return ret;

	ret = mfpwm_register_subdev(mfpwm, &mfpwm->counter_dev,
				    "rockchip-pwm-capture");
	if (ret)
		goto err_unreg_pwm_dev;

	return 0;

err_unreg_pwm_dev:
	platform_device_unregister(mfpwm->pwm_dev);

	return ret;
}

/**
 * mfpwm_get_clk_src - read the currently selected clock source
 * @mfpwm: pointer to the driver's private &struct rockchip_mfpwm instance
 *
 * Read the device register to extract the currently selected clock source,
 * and return it.
 *
 * Returns:
 * * the numeric clock source ID on success, 0 <= id <= 2
 * * negative errno on error
 */
static int mfpwm_get_clk_src(struct rockchip_mfpwm *mfpwm)
{
	u32 val;

	clk_enable(mfpwm->pclk);
	val = mfpwm_reg_read(mfpwm->base, PWMV4_REG_CLK_CTRL);
	clk_disable(mfpwm->pclk);

	return (val & PWMV4_CLK_SRC_MASK) >> PWMV4_CLK_SRC_SHIFT;
}

static int mfpwm_choose_clk(struct rockchip_mfpwm *mfpwm)
{
	int ret;

	ret = mfpwm_get_clk_src(mfpwm);
	if (ret < 0) {
		dev_err(&mfpwm->pdev->dev, "couldn't get current clock source: %pe\n",
			ERR_PTR(ret));
		return ret;
	}
	if (ret == PWMV4_CLK_SRC_CRYSTAL) {
		if (mfpwm->osc_clk) {
			mfpwm->chosen_clk = mfpwm->osc_clk;
		} else {
			dev_warn(&mfpwm->pdev->dev, "initial state wanted 'osc' as clock source, but it's unavailable. Defaulting to 'pwm'.\n");
			mfpwm->chosen_clk = mfpwm->pwm_clk;
		}
	} else {
		mfpwm->chosen_clk = mfpwm->pwm_clk;
	}

	return clk_rate_exclusive_get(mfpwm->chosen_clk);
}

/**
 * mfpwm_switch_clk_src - switch between PWM clock sources
 * @mfpwm: pointer to &struct rockchip_mfpwm driver data
 * @clk_src: one of either %PWMV4_CLK_SRC_CRYSTAL or %PWMV4_CLK_SRC_PLL
 *
 * Switch between clock sources, ``_exclusive_put``ing the old rate,
 * ``clk_rate_exclusive_get``ing the new one, writing the registers and
 * swapping out the &struct_rockchip_mfpwm->chosen_clk.
 *
 * Returns:
 * * %0        - Success
 * * %-EINVAL  - A wrong @clk_src was given or it is unavailable
 * * %-EBUSY   - Device is currently in use, try again later
 */
__attribute__((nonnull))
static int mfpwm_switch_clk_src(struct rockchip_mfpwm *mfpwm,
					  unsigned int clk_src)
{
	struct clk *prev;
	int ret = 0;

	scoped_cond_guard(spinlock_try, return -EBUSY, &mfpwm->state_lock) {
		/* Don't fiddle with any of this stuff if the PWM is on */
		if (mfpwm->active_func)
			return -EBUSY;

		prev = mfpwm->chosen_clk;
		ret = mfpwm_get_clk_src(mfpwm);
		if (ret < 0)
			return ret;
		if (ret == clk_src)
			return 0;

		switch (clk_src) {
		case PWMV4_CLK_SRC_PLL:
			mfpwm->chosen_clk = mfpwm->pwm_clk;
			break;
		case PWMV4_CLK_SRC_CRYSTAL:
			if (!mfpwm->osc_clk)
				return -EINVAL;
			mfpwm->chosen_clk = mfpwm->osc_clk;
			break;
		default:
			return -EINVAL;
		}

		clk_enable(mfpwm->pclk);

		mfpwm_reg_write(mfpwm->base, PWMV4_REG_CLK_CTRL,
				PWMV4_CLK_SRC(clk_src));
		clk_rate_exclusive_get(mfpwm->chosen_clk);
		if (prev)
			clk_rate_exclusive_put(prev);

		clk_disable(mfpwm->pclk);
	}

	return ret;
}

static ssize_t chosen_clock_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct rockchip_mfpwm *mfpwm = dev_get_drvdata(dev);
	unsigned long clk_src = 0;

	/*
	 * Why the weird indirection here? I have the suspicion that if we
	 * emitted to sysfs with the lock still held, then a nefarious program
	 * could hog the lock by somehow forcing a full buffer condition and
	 * then refusing to read from it. Don't know whether that's feasible
	 * to achieve in reality, but I don't want to find out the hard way
	 * either.
	 */
	scoped_guard(spinlock, &mfpwm->state_lock) {
		if (mfpwm->chosen_clk == mfpwm->pwm_clk)
			clk_src = PWMV4_CLK_SRC_PLL;
		else if (mfpwm->osc_clk && mfpwm->chosen_clk == mfpwm->osc_clk)
			clk_src = PWMV4_CLK_SRC_CRYSTAL;
		else
			return -ENODEV;
	}

	if (clk_src == PWMV4_CLK_SRC_PLL)
		return sysfs_emit(buf, "pll\n");
	else if (clk_src == PWMV4_CLK_SRC_CRYSTAL)
		return sysfs_emit(buf, "crystal\n");

	return -ENODEV;
}

static ssize_t chosen_clock_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct rockchip_mfpwm *mfpwm = dev_get_drvdata(dev);
	int ret;

	if (sysfs_streq(buf, "pll")) {
		ret = mfpwm_switch_clk_src(mfpwm, PWMV4_CLK_SRC_PLL);
		if (ret)
			return ret;
		return count;
	} else if (sysfs_streq(buf, "crystal")) {
		ret = mfpwm_switch_clk_src(mfpwm, PWMV4_CLK_SRC_CRYSTAL);
		if (ret)
			return ret;
		return count;
	} else {
		return -EINVAL;
	}
}

static DEVICE_ATTR_RW(chosen_clock);

static ssize_t available_clocks_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct rockchip_mfpwm *mfpwm = dev_get_drvdata(dev);
	ssize_t size = 0;

	size += sysfs_emit_at(buf, size, "pll\n");
	if (mfpwm->osc_clk)
		size += sysfs_emit_at(buf, size, "crystal\n");

	return size;
}

static DEVICE_ATTR_RO(available_clocks);

static struct attribute *mfpwm_attrs[] = {
	&dev_attr_available_clocks.attr,
	&dev_attr_chosen_clock.attr,
	NULL,
};

ATTRIBUTE_GROUPS(mfpwm);

static int rockchip_mfpwm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rockchip_mfpwm *mfpwm;
	int ret = 0;

	mfpwm = devm_kzalloc(&pdev->dev, sizeof(*mfpwm), GFP_KERNEL);
	if (IS_ERR(mfpwm))
		return PTR_ERR(mfpwm);

	mfpwm->pdev = pdev;

	spin_lock_init(&mfpwm->state_lock);

	mfpwm->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(mfpwm->base))
		return dev_err_probe(dev, PTR_ERR(mfpwm->base),
				     "failed to ioremap address\n");

	mfpwm->pclk = devm_clk_get_prepared(dev, "pclk");
	if (IS_ERR(mfpwm->pclk))
		return dev_err_probe(dev, PTR_ERR(mfpwm->pclk),
				     "couldn't get and prepare 'pclk' clock\n");

	mfpwm->irq = platform_get_irq(pdev, 0);
	if (mfpwm->irq < 0)
		return dev_err_probe(dev, mfpwm->irq, "couldn't get irq 0\n");

	mfpwm->pwm_clk = devm_clk_get_prepared(dev, "pwm");
	if (IS_ERR(mfpwm->pwm_clk))
		return dev_err_probe(dev, PTR_ERR(mfpwm->pwm_clk),
				     "couldn't get and prepare 'pwm' clock\n");

	mfpwm->osc_clk = devm_clk_get_optional_prepared(dev, "osc");
	if (IS_ERR(mfpwm->osc_clk))
		return dev_err_probe(dev, PTR_ERR(mfpwm->osc_clk),
				     "couldn't get and prepare 'osc' clock\n");

	ret = mfpwm_choose_clk(mfpwm);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, mfpwm);

	ret = mfpwm_register_subdevs(mfpwm);
	if (ret) {
		dev_err(dev, "failed to register sub-devices: %pe\n",
			ERR_PTR(ret));
		return ret;
	}

	return ret;
}

static void rockchip_mfpwm_remove(struct platform_device *pdev)
{
	struct rockchip_mfpwm *mfpwm = to_rockchip_mfpwm(pdev);
	unsigned long flags;

	spin_lock_irqsave(&mfpwm->state_lock, flags);

	if (mfpwm->chosen_clk)
		clk_rate_exclusive_put(mfpwm->chosen_clk);

	spin_unlock_irqrestore(&mfpwm->state_lock, flags);
}

static const struct of_device_id rockchip_mfpwm_of_match[] = {
	{
		.compatible = "rockchip,rk3576-pwm",
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, rockchip_mfpwm_of_match);

static struct platform_driver rockchip_mfpwm_driver = {
	.driver = {
		.name = KBUILD_MODNAME,
		.of_match_table = rockchip_mfpwm_of_match,
		.dev_groups = mfpwm_groups,
	},
	.probe = rockchip_mfpwm_probe,
	.remove = rockchip_mfpwm_remove,
};
module_platform_driver(rockchip_mfpwm_driver);

MODULE_AUTHOR("Nicolas Frattaroli <nicolas.frattaroli@collabora.com>");
MODULE_DESCRIPTION("Rockchip MFPWM Driver");
MODULE_LICENSE("GPL");
