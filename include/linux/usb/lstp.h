/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * LSTP channel interface for the subsystem drivers.
 *
 * Copyright (c) 2026, NVIDIA CORPORATION & AFFILIATES.  All rights reserved.
 */

#ifndef __LINUX_USB_LSTP_H
#define __LINUX_USB_LSTP_H

#include <linux/auxiliary_bus.h>
#include <linux/types.h>

struct fwnode_handle;
struct kvec;
struct module;

#define LSTP_USB_RESPONSE_TIMEOUT_MS 1000
#define LSTP_USB_EP_MIN_SIZE 64
#define LSTP_CH_NAME_LEN 16
#define LSTP_DISPLAY_NAME_LEN (LSTP_CH_NAME_LEN * 2 + 2) /* intf_name + '_' + ch_name + '\0' */

enum lstp_status {
	LSTP_SUCCESS = 0x00,
	LSTP_ERROR = 0x01,
	LSTP_TIMEOUT = 0x02,
	LSTP_BUSY = 0x03,
	LSTP_NACK = 0x04,
	LSTP_ARB_LOST = 0x05,
	LSTP_NOT_SUPP = 0x06,
	LSTP_TOO_LARGE = 0x07,
};

/* lstp_status out-param: no response status applies. Outside 0x00..0x7f. */
#define LSTP_NO_RESPONSE_STATUS (-1)

/*
 * Firmware-controlled (type, flags, name) triple heading both ch0 config
 * packets, split out so callers can stack-allocate it without a flex array.
 */
struct lstp_ch_desc {
	u8 ch_type;
	u8 ch_flags;
	char ch_name[LSTP_CH_NAME_LEN];
} __packed;

struct lstp_ch0_read_resp {
	struct lstp_ch_desc desc;
	u8 ch_config[];
} __packed;

/*
 * Carries no sequence number: a response is matched only to the channel's
 * single outstanding request.
 */
struct lstp_header {
	u8 ch_id;
	union { /* [0:6] cmd/status, [7] 0=request/1=response */
		u8 cmd;
		u8 status;
	};
	__le16 length;
} __packed;

struct lstp_packet {
	struct lstp_header hdr;
	u8 payload[];
} __packed;

/**
 * struct lstp_channel - Per-channel state visible to LSTP subsystems.
 * @ch_id:          Device-assigned channel id; ch0 is management.
 * @display_name:   NUL-terminated framework-built name, valid for the channel's lifetime.
 * @fwnode:         Firmware node for this channel, or NULL if none matched.
 * @dev:            Device for dev_*() / devm_*(): the auxdev, or for ch0 the
 *                  USB interface device.
 * @max_tx_payload: Largest request payload, header excluded, in one bulk-out URB.
 * @max_rx_payload: Largest response payload, header excluded, in one bulk-in URB.
 * @priv:           Subsystem-owned data for the current bind, opaque to the
 *                  framework, which clears it at unbind. State that an open
 *                  handle keeps past the unbind is reached through the handle,
 *                  never through @priv, and freed by the subsystem.
 */
struct lstp_channel {
	u8 ch_id;
	char display_name[LSTP_DISPLAY_NAME_LEN];
	struct fwnode_handle *fwnode;
	struct device *dev;
	size_t max_tx_payload;
	size_t max_rx_payload;
	void *priv;
};

/**
 * struct lstp_channel_init_info - A channel's firmware config, as seen by its subsystem.
 * @config:     Subsystem-specific config blob, unvalidated by the framework.
 * @config_len: On-wire blob length in bytes; may be 0.
 *
 * The auxdev's platform_data, valid only for the call that receives it: copy
 * to retain. A short blob is legal: pad and warn where every absent field has
 * a safe default, reject -EINVAL where one does not.
 */
struct lstp_channel_init_info {
	const void *config;
	size_t config_len;
};

/**
 * struct lstp_ops - Framework-to-driver upcalls for an LSTP aux_driver.
 * @irq_packet: Handle an unsolicited request packet; NULL if the subsystem
 *              never receives one. Runs in atomic context from the USB RX
 *              completion and must not sleep. @pkt and its payload are valid
 *              only for the call, and any answer goes out inline with
 *              lstp_send_irq_resp(), never from a work item. Called only
 *              between a successful .probe() and the start of .remove(); a
 *              call in progress completes before .remove() runs.
 */
struct lstp_ops {
	void (*irq_packet)(struct lstp_channel *ch, struct lstp_packet *pkt);
};

/**
 * struct lstp_auxiliary_driver - LSTP subclass of struct auxiliary_driver.
 * @adrv:         The embedded auxiliary_driver registered on the bus.
 * @ops:          LSTP framework-to-driver upcalls, or NULL.
 * @subsys_probe: Subsystem .probe(), saved when the framework interposes its
 *                own. Framework-owned; a subsystem sets @adrv.probe only.
 *
 * The auxiliary bus matches on a device-name prefix, so a driver not
 * registered through lstp_auxiliary_driver_register() can bind an LSTP
 * channel device; the core never uses the @ops of such a driver.
 */
struct lstp_auxiliary_driver {
	struct auxiliary_driver adrv;
	const struct lstp_ops *ops;
	int (*subsys_probe)(struct auxiliary_device *auxdev, const struct auxiliary_device_id *id);
};

int __lstp_auxiliary_driver_register(struct lstp_auxiliary_driver *drv, struct module *owner,
				     const char *modname);

/* The bus takes the driver's name and module ref from these; expand in the caller. */
#define lstp_auxiliary_driver_register(drv) \
	__lstp_auxiliary_driver_register(drv, THIS_MODULE, KBUILD_MODNAME)
void lstp_auxiliary_driver_unregister(struct lstp_auxiliary_driver *drv);

/**
 * module_lstp_auxiliary_driver() - Register an LSTP aux_driver as its module.
 * @__lstp_auxiliary_driver: The struct lstp_auxiliary_driver to register.
 *
 * For a module whose init and exit do nothing but register and unregister
 * the driver; replaces module_init() and module_exit().
 */
#define module_lstp_auxiliary_driver(__lstp_auxiliary_driver)                  \
	module_driver(__lstp_auxiliary_driver, lstp_auxiliary_driver_register, \
		      lstp_auxiliary_driver_unregister)

struct lstp_channel *lstp_channel_from_auxdev(struct auxiliary_device *auxdev);

bool lstp_channel_disconnected(const struct lstp_channel *ch);

/* Pin a channel past disconnect; see lstp_channel_get(). */
struct lstp_channel *lstp_channel_get(struct lstp_channel *ch);
void lstp_channel_put(struct lstp_channel *ch);

/* USB I/O helpers; lstp_request() documents the fixed and variable response modes. */
int lstp_send(struct lstp_channel *ch, u8 cmd, const void *payload, size_t len);

int lstp_request(struct lstp_channel *ch, u8 cmd, const void *req, size_t req_len, void *resp,
		 size_t resp_cap, size_t *resp_len, int *lstp_status);

int lstp_requestv(struct lstp_channel *ch, u8 cmd, const struct kvec *vecs, size_t nvecs,
		  void *resp, size_t resp_cap, size_t *resp_len, int *lstp_status);

/* Unsolicited-request response (atomic context, called from ops->irq_packet) */
void lstp_send_irq_resp(struct lstp_channel *ch, u8 status);

/* ch0 READ_CONFIG helpers; process context, may sleep. */
int lstp_ch0_read_config(struct lstp_channel *ch, u16 offset, u16 length,
			 struct lstp_ch0_read_resp **resp_out, size_t *len_out);

typedef int (*lstp_config_entry_fn)(struct lstp_channel *ch, unsigned int entry_idx,
				    const void *entry, void *entry_ctx);
int lstp_read_config_table(struct lstp_channel *ch, size_t config_head_size,
			   size_t config_entry_size, unsigned int total_entries,
			   lstp_config_entry_fn entry_fn, void *entry_ctx);

#define LSTP_GPIO_PIN_NAME_LEN 32 /* wire: pin name field, not NUL-terminated */

/**
 * struct lstp_led_pin - One LED pin of a GPIO channel.
 * @pin:  Pin number in the channel's own numbering.
 * @name: Firmware pin name, NUL-terminated.
 */
struct lstp_led_pin {
	u16 pin;
	char name[LSTP_GPIO_PIN_NAME_LEN + 1];
};

/**
 * struct lstp_leds - The LED pins a GPIO channel hands to the LED driver.
 * @auxdev: "lstp.led.<id>", <id> the GPIO channel's auxdev instance, parented
 *          to that auxdev; this struct is freed on its release.
 * @ch:     The GPIO channel the LED requests go out on; valid while a driver is
 *          bound to @auxdev, which the GPIO driver deletes before its unbind.
 * @npins:  Number of entries in @pins.
 * @pins:   The LED pins, in pin order.
 */
struct lstp_leds {
	struct auxiliary_device auxdev;
	struct lstp_channel *ch;
	u16 npins;
	struct lstp_led_pin pins[] __counted_by(npins);
};

/* Not KBUILD_MODNAME: the name must not follow the module the GPIO driver is in. */
#define LSTP_LEDS_AUXDEV_MODNAME "lstp"

#endif /* __LINUX_USB_LSTP_H */
