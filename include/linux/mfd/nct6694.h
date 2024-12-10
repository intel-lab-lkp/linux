/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Nuvoton NCT6694 USB transaction and data structure.
 *
 * Copyright (C) 2024 Nuvoton Technology Corp.
 */

#ifndef __MFD_NCT6694_H
#define __MFD_NCT6694_H

#define NCT6694_DEV_GPIO	"nct6694-gpio"
#define NCT6694_DEV_I2C		"nct6694-i2c"
#define NCT6694_DEV_CAN		"nct6694-can"
#define NCT6694_DEV_WDT		"nct6694-wdt"
#define NCT6694_DEV_HWMON	"nct6694-hwmon"
#define NCT6694_DEV_RTC		"nct6694-rtc"

#define NCT6694_VENDOR_ID	0x0416
#define NCT6694_PRODUCT_ID	0x200B
#define NCT6694_INT_IN_EP	0x81
#define NCT6694_BULK_IN_EP	0x02
#define NCT6694_BULK_OUT_EP	0x03
#define NCT6694_MAX_PACKET_SZ	0x200

#define NCT6694_CMD_PACKET_SZ	0x8
#define NCT6694_HCTRL_SET	0x40
#define NCT6694_HCTRL_GET	0x80

#define NCT6694_URB_TIMEOUT	1000

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
	NCT6694_IRQ_CAN1,
	NCT6694_IRQ_CAN2,
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

struct nct6694 {
	struct usb_device *udev;
	struct urb *int_in_urb;
	struct irq_domain *domain;
	struct nct6694_cmd_header *cmd_header;
	struct nct6694_response_header *response_header;
	struct mutex access_lock;
	struct mutex irq_lock;
	unsigned char *int_buffer;
	unsigned int irq_enable;
	/* time in msec to wait for the urb to the complete */
	long timeout;
};

struct nct6694_cmd_header {
	unsigned char rsv1;
	unsigned char mod;
	unsigned char cmd;
	unsigned char sel;
	unsigned char hctrl;
	unsigned char rsv2;
	unsigned short len;
} __packed;

struct nct6694_response_header {
	unsigned char sequence_id;
	unsigned char sts;
	unsigned int rsv;
	unsigned short len;
} __packed;

/*
 * nct6694_read_msg - Receive data from NCT6694 USB device
 *
 * @nct6694 - Nuvoton NCT6694 structure
 * @mod - Module byte
 * @offset - Offset byte or (Select byte | Command byte)
 * @length - Length byte
 * @buf - Read data from rx buffer
 *
 * USB Transaction format:
 *
 *	OUT	|RSV|MOD|CMD|SEL|HCTL|RSV|LEN_L|LEN_H|
 *	OUT	|SEQ|STS|RSV|RSV|RSV|RSV||LEN_L|LEN_H|
 *	IN	|-------D------A------D------A-------|
 *	IN			......
 *	IN	|-------D------A------D------A-------|
 */
int nct6694_read_msg(struct nct6694 *nct6694, u8 mod, u16 offset,
		     u16 length, void *buf);

/*
 * nct6694_read_msg - Transmit data to NCT6694 USB device
 *
 * @nct6694 - Nuvoton NCT6694 structure
 * @mod - Module byte
 * @offset - Offset byte or (Select byte | Command byte)
 * @length - Length byte
 * @buf - Write data to tx buffer
 *
 * USB Transaction format:
 *
 *	OUT	|RSV|MOD|CMD|SEL|HCTL|RSV|LEN_L|LEN_H|
 *	OUT	|-------D------A------D------A-------|
 *	OUT			......
 *	OUT	|-------D------A------D------A-------|
 *	IN	|SEQ|STS|RSV|RSV|RSV|RSV||LEN_L|LEN_H|
 *	IN	|-------D------A------D------A-------|
 *	IN			......
 *	IN	|-------D------A------D------A-------|
 */
int nct6694_write_msg(struct nct6694 *nct6694, u8 mod, u16 offset,
		      u16 length, void *buf);

#endif
