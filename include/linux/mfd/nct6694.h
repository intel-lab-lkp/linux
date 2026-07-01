/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2025 Nuvoton Technology Corp.
 *
 * Nuvoton NCT6694 core definitions shared by all transport drivers
 * and sub-device drivers.
 */

#ifndef __MFD_NCT6694_H
#define __MFD_NCT6694_H

#include <linux/idr.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#define NCT6694_HWMON_MOD	0x00
#define NCT6694_PWM_MOD		0x01
#define NCT6694_I2C_MOD		0x03
#define NCT6694_CANFD_MOD	0x05
#define NCT6694_WDT_MOD		0x07
#define NCT6694_RTC_MOD		0x08
#define NCT6694_RPT_MOD		0xFF
#define NCT6694_GPIO_MOD	NCT6694_RPT_MOD

#define NCT6694_HCTRL_SET	0x40
#define NCT6694_HCTRL_GET	0x80

enum nct6694_irq_id {
	NCT6694_IRQ_GPIO0 = 0,
	NCT6694_IRQ_GPIO1,
	NCT6694_IRQ_GPIO2,
	NCT6694_IRQ_GPIO3,
	NCT6694_IRQ_GPIO4,
	NCT6694_IRQ_GPIO5,
	NCT6694_IRQ_GPIO6,
	NCT6694_IRQ_GPIO7,
	NCT6694_IRQ_GPIO8,
	NCT6694_IRQ_GPIO9,
	NCT6694_IRQ_GPIOA,
	NCT6694_IRQ_GPIOB,
	NCT6694_IRQ_GPIOC,
	NCT6694_IRQ_GPIOD,
	NCT6694_IRQ_GPIOE,
	NCT6694_IRQ_GPIOF,
	NCT6694_IRQ_CAN0,
	NCT6694_IRQ_CAN1,
	NCT6694_IRQ_RTC,
	NCT6694_NR_IRQS,
};

enum nct6694_response_err_status {
	NCT6694_NO_ERROR = 0,
	NCT6694_FORMAT_ERROR,
	NCT6694_RESERVED1,
	NCT6694_RESERVED2,
	NCT6694_NOT_SUPPORT_ERROR,
	NCT6694_NO_RESPONSE_ERROR,
	NCT6694_TIMEOUT_ERROR,
	NCT6694_PENDING,
};

struct __packed nct6694_cmd_header {
	u8 rsv1;
	u8 mod;
	union __packed {
		__le16 offset;
		struct __packed {
			u8 cmd;
			u8 sel;
		};
	};
	u8 hctrl;
	u8 rsv2;
	__le16 len;
};

struct __packed nct6694_response_header {
	u8 sequence_id;
	u8 sts;
	u8 reserved[4];
	__le16 len;
};

struct nct6694 {
	struct device *dev;
	struct ida gpio_ida;
	struct ida i2c_ida;
	struct ida canfd_ida;
	struct ida wdt_ida;
	struct irq_domain *domain;
	spinlock_t irq_lock;
	unsigned int irq_enable;
	void *priv;
};

int nct6694_read_msg(struct nct6694 *nct6694, const struct nct6694_cmd_header *cmd_hd, void *buf);
int nct6694_write_msg(struct nct6694 *nct6694, const struct nct6694_cmd_header *cmd_hd, void *buf);

#endif
