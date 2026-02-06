// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2015, The Linux Foundation. All rights reserved.
 * Copyright (C) 2019-2021 NXP
 */

#ifndef _PLATFORM_H_
#define _PLATFORM_H_

#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/spinlock.h>
#include <linux/miscdevice.h>

/* nfc platform interface type */
enum interface_flags {
	/* I2C physical IF for NFCC */
	PLATFORM_IF_I2C = 0,
	PLATFORM_IF_SPI = 1,
};

/* nfc state flags */
enum nfc_state_flags {
	/* nfc in unknown state */
	NFC_STATE_UNKNOWN = 0,
	/* nfc in download mode */
	NFC_STATE_FW_DWL = 0x1,
	/* nfc booted in NCI mode */
	NFC_STATE_NCI = 0x2,
	/* nfc booted in Fw teared mode */
	NFC_STATE_FW_TEARED = 0x4,
};

/*
 * Power state for IBI handing, mainly needed to defer the IBI handling
 *  for the IBI received in suspend state to do it later in resume call
 */
enum pm_state_flags {
	PM_STATE_NORMAL = 0,
	PM_STATE_SUSPEND,
	PM_STATE_IBI_BEFORE_RESUME,
};

/* Enum for GPIO values */
enum gpio_values {
	GPIO_INPUT = 0x0,
	GPIO_OUTPUT = 0x1,
	GPIO_HIGH = 0x2,
	GPIO_OUTPUT_HIGH = 0x3,
	GPIO_IRQ = 0x4,
};

/* Enum for nfcc_ioctl_request */
enum nfcc_ioctl_request {
	/* NFC disable request with VEN LOW */
	NFC_POWER_OFF = 0,
	/* NFC enable request with VEN Toggle */
	NFC_POWER_ON,
	/* firmware download request with VEN Toggle */
	NFC_FW_DWL_VEN_TOGGLE,
	/* ISO reset request */
	NFC_ISO_RESET,
	/* request for firmware download gpio HIGH */
	NFC_FW_DWL_HIGH,
	/* VEN hard reset request */
	NFC_VEN_FORCED_HARD_RESET,
	/* request for firmware download gpio LOW */
	NFC_FW_DWL_LOW,
};

/* NFC GPIO variables */
struct platform_gpio {
	unsigned int irq;
	unsigned int ven;
	unsigned int dwl_req;
};

/* NFC Struct to get all the required configs from DTS */
struct platform_configs {
	struct platform_gpio gpio;
};

/* Interface specific parameters - 基础结构 */
struct i2c_dev {
	struct i2c_client *client;
	/* IRQ parameters */
	bool irq_enabled;
	spinlock_t irq_enabled_lock;
	/* NFC_IRQ wake-up state */
	bool irq_wake_up;
};

struct spi_dev {
	struct spi_device *client;
	struct miscdevice device;
	/* IRQ parameters */
	bool irq_enabled;
	spinlock_t irq_enabled_lock;
	/* NFC_IRQ wake-up state */
	bool irq_wake_up;
	/* Temporary write kernel buffer */
	uint8_t *tmp_write_kbuf;
	/* Temporary read kernel buffer */
	uint8_t *tmp_read_kbuf;
};

/* Device specific structure */
struct nfc_dev {
	wait_queue_head_t read_wq;
	struct mutex read_mutex;
	struct mutex write_mutex;
	uint8_t *read_kbuf;
	uint8_t *write_kbuf;
	struct mutex dev_ref_mutex;
	unsigned int dev_ref_count;
	struct class *nfc_class;
	struct device *nfc_device;
	struct cdev c_dev;
	dev_t devno;
	/* Interface flag */
	uint8_t interface;
	/* nfc state flags */
	uint8_t nfc_state;
	/* NFC VEN pin state */
	bool nfc_ven_enabled;
	/* Platform specific data */
	void *platform_data;
	struct platform_configs configs;

	/* function pointers */
	int (*nfc_read)(struct nfc_dev *dev, char *buf, size_t count,
			int timeout);
	int (*nfc_write)(struct nfc_dev *dev, const char *buf,
			 const size_t count, int max_retry_cnt);
	int (*nfc_enable_intr)(struct nfc_dev *dev);
	int (*nfc_disable_intr)(struct nfc_dev *dev);
};

/* 从common.h移过来的常量定义 */
#define DEV_COUNT			1
#define CLASS_NAME			"nfc"
#define NFC_CHAR_DEV_NAME		"nxpnfc"
#define NCI_CMD				(0x20)
#define NCI_RSP				(0x40)
#define NCI_HDR_LEN			(3)
#define MAX_NCI_BUFFER_SIZE		(NCI_HDR_LEN + 255)
#define MAX_DL_BUFFER_SIZE		(2 + 2 + 550)
#define NO_RETRY			(1)
#define MAX_RETRY_COUNT			(3)
#define MAX_WRITE_IRQ_COUNT		(5)
#define WAKEUP_SRC_TIMEOUT		(2000)
#define NCI_CMD_RSP_TIMEOUT_MS		(2000)
#define NFC_GPIO_SET_WAIT_TIME_US	(10000)
#define NFC_WRITE_IRQ_WAIT_TIME_US	(3000)
#define WRITE_RETRY_WAIT_TIME_US	(1000)
#define NFC_MAGIC			(0xE9)
#define NFC_SET_PWR			_IOW(NFC_MAGIC, 0x01, long)
#define ESE_SET_PWR			_IOW(NFC_MAGIC, 0x02, long)
#define ESE_GET_PWR			_IOR(NFC_MAGIC, 0x03, long)
#define DTS_IRQ_GPIO_STR		"nxp,nxpnfc-irq"
#define DTS_VEN_GPIO_STR		"nxp,nxpnfc-ven"
#define DTS_FWDN_GPIO_STR		"nxp,nxpnfc-fw-dwnld"

#endif /* _PLATFORM_H_ */
