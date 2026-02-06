// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2015, The Linux Foundation. All rights reserved.
 * Copyright (C) 2019-2021 NXP
 */

#ifndef _COMMON_H_
#define _COMMON_H_

#include "platform.h"

/* 函数声明 */
int nfc_dev_open(struct inode *inode, struct file *filp);
int nfc_dev_close(struct inode *inode, struct file *filp);
long nfc_dev_ioctl(struct file *pfile, unsigned int cmd, unsigned long arg);
int nfc_parse_dt(struct device *dev, struct platform_configs *nfc_configs,
		 uint8_t interface);
int nfc_misc_register(struct nfc_dev *nfc_dev,
		      const struct file_operations *nfc_fops, int count,
		      char *devname, char *classname);
void nfc_misc_unregister(struct nfc_dev *nfc_dev, int count);
int configure_gpio(unsigned int gpio, int flag);
void gpio_set_ven(struct nfc_dev *nfc_dev, int value);
void gpio_free_all(struct nfc_dev *nfc_dev);
int validate_nfc_state_nci(struct nfc_dev *nfc_dev);
void set_valid_gpio(int gpio, int value);
int get_valid_gpio(int gpio);

/* I2C specific functions */
int i2c_disable_irq(struct nfc_dev *dev);
int i2c_enable_irq(struct nfc_dev *dev);
int i2c_read(struct nfc_dev *nfc_dev, char *buf, size_t count, int timeout);
int i2c_write(struct nfc_dev *nfc_dev, const char *buf, size_t count,
	      int max_retry_cnt);

#endif /* _COMMON_H_ */
