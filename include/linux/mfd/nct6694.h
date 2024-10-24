/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Nuvoton NCT6694 USB transaction and data structure.
 *
 * Copyright (C) 2024 Nuvoton Technology Corp.
 */

#ifndef __MFD_NCT6694_H
#define __MFD_NCT6694_H

#define NCT6694_DEV_GPIO		"nct6694-gpio"
#define NCT6694_DEV_I2C			"nct6694-i2c"
#define NCT6694_DEV_CAN			"nct6694-can"
#define NCT6694_DEV_WDT			"nct6694-wdt"
#define NCT6694_DEV_IIO			"nct6694-iio"
#define NCT6694_DEV_HWMON		"nct6694-hwmon"
#define NCT6694_DEV_PWM			"nct6694-pwm"
#define NCT6694_DEV_RTC			"nct6694-rtc"

#define NCT6694_VENDOR_ID		0x0416
#define NCT6694_PRODUCT_ID		0x200B
#define INT_IN_ENDPOINT			0x81
#define BULK_IN_ENDPOINT		0x82
#define BULK_OUT_ENDPOINT		0x03
#define MAX_PACKET_SZ			0x100

#define CMD_PACKET_SZ			0x8
#define HCTRL_SET			0x40
#define HCTRL_GET			0x80

#define REQUEST_MOD_IDX			0x01
#define REQUEST_CMD_IDX			0x02
#define REQUEST_SEL_IDX			0x03
#define REQUEST_HCTRL_IDX		0x04
#define REQUEST_LEN_L_IDX		0x06
#define REQUEST_LEN_H_IDX		0x07

#define RESPONSE_STS_IDX		0x01

#define INT_IN_IRQ_IDX			0x00
#define GPIO_IRQ_STATUS			BIT(0)
#define CAN_IRQ_STATUS			BIT(2)
#define RTC_IRQ_STATUS			BIT(3)

#define URB_TIMEOUT			1000

/*
 * struct nct6694 - Nuvoton NCT6694 structure
 *
 * @udev: Pointer to the USB device
 * @int_in_urb: Interrupt pipe urb
 * @access_lock: USB transaction lock
 * @handler_list: List of registered handlers
 * @async_workqueue: Workqueue of processing asynchronous work
 * @tx_buffer: USB write message buffer
 * @rx_buffer: USB read message buffer
 * @cmd_buffer: USB send command message buffer
 * @int_buffer: USB receive interrupt message buffer
 * @lock: Handlers lock
 * @timeout: URB timeout
 * @maxp: Maximum packet of bulk pipe
 */
struct nct6694 {
	struct usb_device *udev;
	struct urb *int_in_urb;
	struct list_head handler_list;
	struct workqueue_struct *async_workqueue;

	/* Make sure that every USB transaction is not interrupted */
	struct mutex access_lock;

	unsigned char *tx_buffer;
	unsigned char *rx_buffer;
	unsigned char *cmd_buffer;
	unsigned char *int_buffer;

	/* Prevent races within handlers */
	spinlock_t lock;

	/* time in msec to wait for the urb to the complete */
	long timeout;

	/* Bulk pipe maximum packet for each transaction */
	int maxp;
};

/*
 * struct nct6694_handler_entry - Stores the interrupt handling information
 * for each registered peripheral
 *
 * @irq_bit: The bit in irq_status[INT_IN_IRQ_IDX] representing interrupt
 * @handler: Function pointer to the interrupt handler of the peripheral
 * @private_data: Private data specific to the peripheral driver
 * @list: Node used to link to the handler_list
 */
struct nct6694_handler_entry {
	int irq_bit;
	void (*handler)(void *private_data);
	void *private_data;
	struct list_head list;
};

/*
 * nct6694_register_handler - Register a handler with private data for
 * interrupt pipe irq event
 *
 * @nct6694 - Nuvoton NCT6694 structure
 * @irq_bit - The irq for which to register a handler
 * @handler - The handler function
 * @private_data - Private data for which to register a handler
 *
 * This function is called when peripherals need to register a handler
 * for receiving interrupt pipe.
 *
 * Don't use the wait_for_completion function in handler function, as
 * it is in interrupt context.
 */
int nct6694_register_handler(struct nct6694 *nct6694, int irq_bit,
			     void (*handler)(void *),
			     void *private_data);

/*
 * nct6694_read_msg - Receive data from NCT6694 USB device
 *
 * @nct6694 - Nuvoton NCT6694 structure
 * @mod - Module byte
 * @offset - Offset byte or (Select byte | Command byte)
 * @length - Length byte
 * @rd_idx - Read data from rx buffer at index
 * @rd_len - Read length from rx buffer
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
		     u16 length, u8 rd_idx, u8 rd_len,
		     unsigned char *buf);

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
		      u16 length, unsigned char *buf);

#endif
