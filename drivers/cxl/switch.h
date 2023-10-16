/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CXL_SWITCH_H__
#define __CXL_SWITCH_H__

#include <linux/device.h>
#include <linux/cdev.h>
#include "cxlmbox.h"

struct cxl_swdev {
	struct device dev;
	struct cdev cdev;
	struct cxl_mbox mbox;
	int id;
	bool dying;
};

struct cxl_swdev *cxl_swdev_alloc(struct device *parent);
void cxl_swdev_shutdown(struct cxl_swdev *cxlswd);
#endif /* __CXL_SWITCH_H__ */
