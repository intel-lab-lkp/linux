/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _GOODIX_EC_H_
#define _GOODIX_EC_H_

#include <linux/gpio/consumer.h>
#include <linux/ioport.h>
#include <linux/kfifo.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/stddef.h>
#include <linux/types.h>

#define GOODIX_EC_DRIVER_NAME		"goodix-ec-mailbox"

/* Shared-memory mailbox layout. */
#define GOODIX_EC_MMIO_SIZE		0x1000
#define GOODIX_EC_TX_OFFSET		0x000
#define GOODIX_EC_TX_SIZE		0x0200
#define GOODIX_EC_RX_OFFSET		0x0200
#define GOODIX_EC_RX_SIZE		0x0e00
#define GOODIX_EC_PACKET_ALIGNMENT	8

/* EC mailbox framing. */
#define GOODIX_EC_PACKET_TYPE		0xf0
#define GOODIX_EC_SEQUENCE_SEED		0x8881

/* MP framing. The payload is opaque to the kernel. */
#define GOODIX_MP_HEADER_SIZE		4

/* Mailbox handshake timing. */
#define GOODIX_WRITE_DONE_PRE_US	50
#define GOODIX_WRITE_DONE_HIGH_US	4000
#define GOODIX_WRITE_DONE_POST_US	200
#define GOODIX_READ_DONE_HIGH_US	50
#define GOODIX_READ_DONE_POST_US	200
#define GOODIX_SYNC_RX_DELAY_US		1000

struct device;

struct goodix_ec_header {
	u8 type;
	__le16 payload_len;
	u8 checksum;
	__le16 sequence;
	u8 reserved[2];
} __packed;

struct goodix_mp_header {
	u8 type;
	__le16 payload_len;
	u8 checksum;
} __packed;

struct goodix_device;

struct goodix_model_data {
	const char *name;
};

struct goodix_device {
	struct device *dev;
	struct kref refcount;
	bool disconnected;
	bool suspended;
	bool irq_enabled;

	void __iomem *mailbox;
	resource_size_t mailbox_phys;
	resource_size_t mailbox_size;

	struct gpio_desc *write_done_gpio;
	struct gpio_desc *read_done_gpio;
	struct gpio_desc *irq_gpio;
	int irq;

	u8 *tx_buf;
	u8 *rx_buf;

	u8 *rx_reassembly;
	size_t rx_reassembly_len;
	size_t rx_reassembly_received;
	u8 rx_reassembly_mp_type;
	bool rx_reassembly_active;

	u16 tx_sequence;
	/* Serializes mailbox TX with threaded-IRQ RX access. */
	struct mutex transfer_lock;

	struct miscdevice miscdev;
	struct kfifo rx_fifo;
	/* Protects the RX FIFO and reader ownership state. */
	spinlock_t rx_fifo_lock;
	/* Serializes record-oriented read operations. */
	struct mutex rx_read_lock;
	wait_queue_head_t rx_wait;
	bool rx_fifo_ready;
	bool rx_reader_open;
	bool misc_registered;

	const struct goodix_model_data *model;
};

int goodix_ec_sync_send(struct goodix_device *gdev,
			const u8 *tx, size_t tx_len);
bool goodix_ec_device_get(struct goodix_device *gdev);
void goodix_ec_device_put(struct goodix_device *gdev);
int goodix_ec_uapi_register(struct goodix_device *gdev);
void goodix_ec_uapi_unregister(struct goodix_device *gdev);

#endif /* _GOODIX_EC_H_ */
