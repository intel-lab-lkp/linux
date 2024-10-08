// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * PPS dummy generator
 *
 * Copyright (C) 2024   Rodolfo Giometti <giometti@enneenne.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/time.h>
#include <linux/pps_gen_kernel.h>

/*
 * Global variables
 */

static struct pps_gen_device *pps_gen;

/*
 * PPS Generator methods
 */

static int pps_gen_dummy_get_time(struct pps_gen_device *pps_gen,
                                        struct timespec64 *time)
{
	struct system_time_snapshot snap;

	ktime_get_snapshot(&snap);
	*time = ktime_to_timespec64(snap.real);

	return 0;
}

static int pps_gen_dummy_enable(struct pps_gen_device *pps_gen, bool enable)
{
	/* always enabled */
	return 0;
}

/*
 * The PPS info struct
 */

static struct pps_gen_source_info pps_gen_dummy_info = {
        .name			= "dummy",
	.use_system_clock	= true,
	.get_time		= pps_gen_dummy_get_time,
	.enable			= pps_gen_dummy_enable,
};

/*
 * Module staff
 */

static void __exit pps_gen_dummy_exit(void)
{
        dev_info(pps_gen->dev, "dummy PPS generator unregistered\n");

        pps_gen_unregister_source(pps_gen);
}

static int __init pps_gen_dummy_init(void)
{
        pps_gen = pps_gen_register_source(&pps_gen_dummy_info);
        if (IS_ERR(pps_gen)) {
                pr_err("cannot register PPS generator\n");
                return PTR_ERR(pps_gen);
        }

        dev_info(pps_gen->dev, "dummy PPS generator registered\n");

        return 0;
}

module_init(pps_gen_dummy_init);
module_exit(pps_gen_dummy_exit);

MODULE_AUTHOR("Rodolfo Giometti <giometti@enneenne.com>");
MODULE_DESCRIPTION("LinuxPPS dummy generator");
MODULE_LICENSE("GPL");
