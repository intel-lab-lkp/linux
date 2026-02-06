// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2015, The Linux Foundation. All rights reserved.
 * Copyright (C) 2019-2021 NXP
 */

#ifndef _I2C_DRV_H_
#define _I2C_DRV_H_

#include "platform.h"

/* kept same as dts */
#define NFC_I2C_DRV_STR		"nxp,nxpnfc"
#define NFC_I2C_DEV_ID		"nxpnfc"

/* Function declarations */
ssize_t nfc_i2c_dev_read(struct file *filp, char __user *buf, size_t count,
			 loff_t *offset);
ssize_t nfc_i2c_dev_write(struct file *filp, const char __user *buf,
			  size_t count, loff_t *offset);
int nfc_i2c_dev_probe(struct i2c_client *client);
void nfc_i2c_dev_remove(struct i2c_client *client);
int nfc_i2c_dev_suspend(struct device *device);
int nfc_i2c_dev_resume(struct device *device);

#endif /* _I2C_DRV_H_ */
