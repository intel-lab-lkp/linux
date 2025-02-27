// SPDX-License-Identifier: GPL-2.0
/*
 * Motion Control Subsystem - Simple speed proportional-PWM based motor driver
 *
 * Copyright (C) 2024 Protonic Holland
 *      David Jander <david@protonic.nl>
 */

#include <asm-generic/errno-base.h>
#include <linux/err.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/motion.h>
#include <linux/types.h>
#include <linux/pm.h>
#include <linux/pwm.h>
#include <asm/unaligned.h>

#include "motion-core.h"
#include "motion-helpers.h"

#define MOTPWM_PWM_SCALE 100000
#define MOTPWM_TIMER_PERIOD (20 * NSEC_PER_MSEC)

struct motpwm {
	struct pwm_device *pwms[2];
	struct motion_device mdev;
	struct platform_device *pdev;
	bool pwm_inverted;
};

static inline int __effective_dc(struct motpwm *mp, unsigned int dc)
{
	if (mp->pwm_inverted)
		return MOTPWM_PWM_SCALE - dc;
	return dc;
}

static inline int __motpwm_set_speed_locked(struct motpwm *mp, unsigned int dir,
		unsigned int dc, bool enable)
{
	struct pwm_state ps;
	int cidx = !dir;
	struct pwm_device *pwm, *cpwm;

	dir = !!dir;
	pwm = mp->pwms[dir];
	cpwm = mp->pwms[cidx];

	if (cpwm) {
		pwm_init_state(cpwm, &ps);
		ps.duty_cycle = __effective_dc(mp, 0);
		ps.enabled = enable;
		pwm_apply_might_sleep(cpwm, &ps);
	}
	if (!pwm)
		return -EINVAL;

	pwm_init_state(pwm, &ps);
	pwm_set_relative_duty_cycle(&ps, __effective_dc(mp, dc), MOTPWM_PWM_SCALE);
	ps.enabled = enable;
	pwm_apply_might_sleep(pwm, &ps);

	return 0;
}

static void motpwm_startup(struct motion_device *mdev)
{
	dev_info(mdev->dev, "Startup\n");
}

static void motpwm_powerdown(struct motion_device *mdev)
{
	struct motpwm *mp = container_of(mdev, struct motpwm, mdev);

	dev_info(mdev->dev, "Shutdown\n");
	__motpwm_set_speed_locked(mp, 0, 0, false);
}

static int motpwm_check_speed(struct motion_device *mdev, unsigned int dir,
		unsigned int speed)
{
	struct motpwm *mp = container_of(mdev, struct motpwm, mdev);

	if (!mp->pwms[!!dir])
		return -EINVAL;

	if (speed > MOTPWM_PWM_SCALE)
		return -EINVAL;

	return 0;
}

static void motpwm_set_speed(struct motion_device *mdev, unsigned int dir,
		unsigned int speed)
{
	struct motpwm *mp = container_of(mdev, struct motpwm, mdev);

	__motpwm_set_speed_locked(mp, dir, speed, true);
}

static struct motion_timed_speed_ops motpwm_mts_ops = {
	.startup = motpwm_startup,
	.powerdown = motpwm_powerdown,
	.check_speed = motpwm_check_speed,
	.set_speed = motpwm_set_speed
};

static int motpwm_probe(struct platform_device *pdev)
{
	struct motpwm *mp;
	struct device *dev = &pdev->dev;
	struct fwnode_handle *fwnode = dev_fwnode(dev);

	mp = devm_kzalloc(dev, sizeof(struct motpwm), GFP_KERNEL);
	if (!mp)
		return -ENOMEM;

	mp->pwms[0] = devm_fwnode_pwm_get(dev, fwnode, "left");
	if (IS_ERR(mp->pwms[0])) {
		int err = PTR_ERR(mp->pwms[0]);

		if (err == -ENODEV)
			mp->pwms[0] = NULL;
		else
			return err;
	}
	mp->pwms[1] = devm_fwnode_pwm_get(dev, fwnode, "right");
	if (IS_ERR(mp->pwms[1])) {
		int err = PTR_ERR(mp->pwms[1]);

		if (err == -ENODEV)
			mp->pwms[1] = NULL;
		else
			return err;
	}
	if (!mp->pwms[0] && !mp->pwms[1]) {
		dev_err(dev, "Need at least one PWM");
		return -ENODEV;
	}

	mp->pwm_inverted = fwnode_property_read_bool(fwnode, "motion,pwm-inverted");

	mp->pdev = pdev;
	platform_set_drvdata(pdev, mp);

	mp->mdev.parent = &pdev->dev;
	motion_timed_speed_init(&mp->mdev, &motpwm_mts_ops, MOTPWM_PWM_SCALE);
	mp->mdev.capabilities.type = MOT_TYPE_DC_MOTOR;
	mp->mdev.capabilities.subdiv = 1;
	motion_fwnode_get_capabilities(&mp->mdev, fwnode);

	return motion_register_device(&mp->mdev);
}

static void motpwm_shutdown(struct platform_device *pdev)
{
	struct motpwm *mp = platform_get_drvdata(pdev);

	motion_unregister_device(&mp->mdev);
}

static int motpwm_suspend(struct device *dev)
{
	return 0;
}

static int motpwm_resume(struct device *dev)
{
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(motpwm_pm, motpwm_suspend, motpwm_resume);

static const struct of_device_id motpwm_of_match[] = {
	{ .compatible = "motion-simple-pwm" },
	{}
};
MODULE_DEVICE_TABLE(of, motpwm_of_match);

static struct platform_driver motpwm_driver = {
	.probe = motpwm_probe,
	.shutdown = motpwm_shutdown,
	.driver = {
		.name = "motion-simple-pwm",
		.pm = pm_sleep_ptr(&motpwm_pm),
		.of_match_table = motpwm_of_match,
	},
};

module_platform_driver(motpwm_driver);

MODULE_AUTHOR("David Jander <david@protonic.nl>");
MODULE_DESCRIPTION("Simple PWM based DC motor motion control driver");
MODULE_LICENSE("GPL");
