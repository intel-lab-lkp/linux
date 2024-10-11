// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (C) 2024 Google LLC.
 */

#include <linux/fs.h>
#include <linux/cdev.h>

struct file *rust_helper_get_file(struct file *f)
{
	return get_file(f);
}

unsigned int rust_helper_MAJOR(dev_t dev)
{
	return MAJOR(dev);
}

unsigned int rust_helper_MINOR(dev_t dev)
{
	return MINOR(dev);
}

dev_t rust_helper_MKDEV(unsigned int major, unsigned int minor)
{
	return MKDEV(major, minor);
}
