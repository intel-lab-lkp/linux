/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2025 Nuvoton Technology Corp.
 *
 * Nuvoton NCT6694 core definitions shared by all transport drivers
 * and sub-device drivers.
 */

#ifndef __MFD_NCT6694_H
#define __MFD_NCT6694_H

#include <linux/bitfield.h>
#include <linux/idr.h>
#include <linux/regmap.h>
#include <linux/spinlock.h>
#include <linux/types.h>

struct device;
struct irq_domain;
struct mfd_cell;

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

/* Maximum payload length the firmware accepts in a single command */
#define NCT6694_MAX_PACKET_SIZE	0x3F0

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
	struct regmap *regmap;
	struct ida gpio_ida;
	struct ida i2c_ida;
	struct ida canfd_ida;
	struct ida wdt_ida;
	struct irq_domain *domain;
	spinlock_t irq_lock;
	unsigned int irq_enable;
	void *priv;
};

/*
 * Firmware messages are addressed by a module id and a 16-bit offset (a
 * command/selector pair). Pack them together with the host control byte into a
 * single 32-bit regmap register, so that sub-device drivers can issue commands
 * through the regmap bulk accessors while each transport driver only has to
 * implement a regmap bus.
 *
 *   bits [31:24]  host control (NCT6694_HCTRL_GET / NCT6694_HCTRL_SET)
 *   bits [23:16]  module id
 *   bits [15:0]   offset (low byte = command, high byte = selector)
 */
#define NCT6694_REG_HCTRL	GENMASK(31, 24)
#define NCT6694_REG_MOD		GENMASK(23, 16)
#define NCT6694_REG_OFFSET	GENMASK(15, 0)

static inline u32 nct6694_cmd_to_reg(const struct nct6694_cmd_header *cmd_hd,
				     u8 hctrl)
{
	return FIELD_PREP(NCT6694_REG_HCTRL, hctrl) |
	       FIELD_PREP(NCT6694_REG_MOD, cmd_hd->mod) |
	       FIELD_PREP(NCT6694_REG_OFFSET, le16_to_cpu(cmd_hd->offset));
}

static inline int nct6694_read_msg(struct nct6694 *nct6694,
				   const struct nct6694_cmd_header *cmd_hd,
				   void *buf)
{
	return regmap_bulk_read(nct6694->regmap,
				nct6694_cmd_to_reg(cmd_hd, NCT6694_HCTRL_GET),
				buf, le16_to_cpu(cmd_hd->len));
}

static inline int nct6694_write_msg(struct nct6694 *nct6694,
				    const struct nct6694_cmd_header *cmd_hd,
				    void *buf)
{
	return regmap_bulk_write(nct6694->regmap,
				 nct6694_cmd_to_reg(cmd_hd, NCT6694_HCTRL_SET),
				 buf, le16_to_cpu(cmd_hd->len));
}

/*
 * A few commands, such as the I2C deliver, transmit a request and read the
 * reply back within the same firmware message. regmap has no accessor for such
 * an exchange, so express it as a read of a SET register: @buf carries the
 * request on entry and holds the reply on return. This relies on the transport
 * bus being handed @buf directly, which holds as long as the regmap is left
 * uncached and byte sized.
 */
static inline int nct6694_write_read_msg(struct nct6694 *nct6694,
					 const struct nct6694_cmd_header *cmd_hd,
					 void *buf)
{
	return regmap_bulk_read(nct6694->regmap,
				nct6694_cmd_to_reg(cmd_hd, NCT6694_HCTRL_SET),
				buf, le16_to_cpu(cmd_hd->len));
}

int nct6694_core_probe(struct device *dev, struct nct6694 *nct6694,
		       const struct mfd_cell *cells, int n_cells);
void nct6694_core_remove(struct nct6694 *nct6694);

#endif
