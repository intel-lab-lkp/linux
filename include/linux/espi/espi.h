/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * eSPI (Enhanced Serial Peripheral Interface) framework
 *
 * Copyright (c) 2026, Advanced Micro Devices, Inc.
 * All Rights Reserved.
 */
#ifndef _LINUX_ESPI_ESPI_H
#define _LINUX_ESPI_ESPI_H

#include <linux/bits.h>
#include <linux/device.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/mod_devicetable.h>
#include <linux/notifier.h>

struct espi_controller;
struct espi_device;
struct espi_driver;
struct espi_board_info;

#define ESPI_NAME_SIZE			32

#define ESPI_CHANNEL_PERIPH		0
#define ESPI_CHANNEL_VWIRE		1
#define ESPI_CHANNEL_OOB		2
#define ESPI_CHANNEL_FLASH		3
#define ESPI_CHANNEL_COUNT		4

#define ESPI_CHANNEL_PERIPH_SUPP	BIT(ESPI_CHANNEL_PERIPH)
#define ESPI_CHANNEL_VWIRE_SUPP		BIT(ESPI_CHANNEL_VWIRE)
#define ESPI_CHANNEL_OOB_SUPP		BIT(ESPI_CHANNEL_OOB)
#define ESPI_CHANNEL_FLASH_SUPP		BIT(ESPI_CHANNEL_FLASH)
#define ESPI_CHANNEL_ALL		(ESPI_CHANNEL_PERIPH_SUPP | \
					 ESPI_CHANNEL_VWIRE_SUPP | \
					 ESPI_CHANNEL_OOB_SUPP | \
					 ESPI_CHANNEL_FLASH_SUPP)

#define ESPI_IO_MODE_SINGLE		0
#define ESPI_IO_MODE_DUAL		1
#define ESPI_IO_MODE_QUAD		2

#define ESPI_FREQ_16MHZ			16
#define ESPI_FREQ_33MHZ			33
#define ESPI_FREQ_66MHZ			66

enum espi_cmd_type {
	ESPI_CMD_SET_CONFIGURATION	= 0,
	ESPI_CMD_GET_CONFIGURATION	= 1,
	ESPI_CMD_IN_BAND_RESET		= 2,
	ESPI_CMD_GET_STATUS		= 3,
	ESPI_CMD_PERIPHERAL		= 4,
	ESPI_CMD_VWIRE			= 5,
	ESPI_CMD_OOB			= 6,
	ESPI_CMD_FLASH			= 7,
};

#define ESPI_SLAVE_REG_DEVICE_ID	0x0004
#define ESPI_SLAVE_REG_GENERAL_CFG	0x0008

/* General Configuration register (0x08) field definitions. */
#define ESPI_GENCFG_OP_FREQ		GENMASK(22, 20)
#define ESPI_GENCFG_IO_MODE		GENMASK(27, 26)
#define ESPI_GENCFG_ALERT_MODE		BIT(28)
#define ESPI_GENCFG_CRC_EN		BIT(31)
/* ESPI_GENCFG_OP_FREQ values */
#define ESPI_GENCFG_FREQ_20MHZ		0x0
#define ESPI_GENCFG_FREQ_33MHZ		0x2
#define ESPI_GENCFG_FREQ_66MHZ		0x4

/* Channel-specific capability/configuration register addresses */
#define ESPI_SLAVE_REG_PERIPH_CFG	0x0010
#define ESPI_SLAVE_REG_VWIRE_CFG	0x0020
#define ESPI_SLAVE_REG_OOB_CFG		0x0030
#define ESPI_SLAVE_REG_FLASH_CFG	0x0040

struct espi_slave_status {
	bool pc_free;
	bool np_free;
	bool vwire_free;
	bool oob_free;
	bool flash_free;
	u16 raw;
};

struct espi_device_id {
	char name[ESPI_NAME_SIZE];
	kernel_ulong_t driver_data;
};

enum espi_event_type {
	ESPI_EVENT_CHANNEL_READY	= 0,
	ESPI_EVENT_CHANNEL_RESET	= 1,
	ESPI_EVENT_VWIRE_CHANGED	= 2,
	ESPI_EVENT_OOB_RECEIVED		= 3,
	ESPI_EVENT_PERIPH_POSTED	= 4,
	ESPI_EVENT_ALERT		= 5,
	ESPI_EVENT_RESET		= 6,
};

struct espi_event {
	enum espi_event_type type;
	int channel;
	u32 details;
	struct espi_controller *ctrl;
};

struct espi_board_info {
	char type[ESPI_NAME_SIZE];
	u8 cs;
	void *platform_data;
	struct fwnode_handle *fwnode;
};

struct espi_capabilities {
	u32 supported_channels;
	u32 max_freq_mhz;
	u8 io_mode;
	bool alert_mode;
	bool crc_supported;
	u16 periph_max_payload;
	u8 vwire_max_count;
	u16 oob_max_payload;
	u16 flash_max_payload;
};

/**
 * struct espi_controller_ops - hardware operation callbacks
 *
 * All callbacks are optional and return 0 or a negative errno. The core
 * returns -EOPNOTSUPP for operations a controller does not provide.
 */
struct espi_controller_ops {
	int (*setup)(struct espi_controller *ctrl);
	void (*cleanup)(struct espi_controller *ctrl);

	int (*get_configuration)(struct espi_controller *ctrl,
				 u32 slave_reg_addr, u32 *config);
	int (*set_configuration)(struct espi_controller *ctrl,
				 u32 slave_reg_addr, u32 config);
	int (*inband_reset)(struct espi_controller *ctrl);
	int (*get_status)(struct espi_controller *ctrl,
			  struct espi_slave_status *status);

	int (*enable_channel)(struct espi_controller *ctrl, u8 channel);
	int (*disable_channel)(struct espi_controller *ctrl, u8 channel);

	int (*periph_io_read)(struct espi_controller *ctrl,
			      u16 port, u8 width, u32 *value);
	int (*periph_io_write)(struct espi_controller *ctrl,
			       u16 port, u8 width, u32 value);
	int (*periph_mem_read)(struct espi_controller *ctrl,
			       u32 addr, void *buf, size_t len);
	int (*periph_mem_write)(struct espi_controller *ctrl,
				u32 addr, const void *buf, size_t len);

	int (*vwire_get)(struct espi_controller *ctrl,
			 u8 index, u8 *value, u8 *valid);
	int (*vwire_put)(struct espi_controller *ctrl,
			 u8 index, u8 value, u8 valid);

	int (*oob_send)(struct espi_controller *ctrl,
			const void *buf, size_t len, u8 tag);
	int (*oob_recv)(struct espi_controller *ctrl,
			void *buf, size_t *len, u8 *tag);

	int (*flash_read)(struct espi_controller *ctrl,
			  u32 offset, void *buf, size_t len);
	int (*flash_write)(struct espi_controller *ctrl,
			   u32 offset, const void *buf, size_t len);
	int (*flash_erase)(struct espi_controller *ctrl,
			   u32 offset, size_t len);

	int (*handle_alert)(struct espi_controller *ctrl);
};

struct espi_controller {
	struct device dev;
	int bus_num;
	u8 max_targets;		/* number of Chip Select# pins supported */

	const struct espi_controller_ops *ops;
	struct espi_capabilities caps;

	struct mutex lock;	/* serialises controller ops and @channel_enabled */
	u32 channel_enabled;

	struct list_head device_list;
	struct mutex device_list_lock;	/* protects @device_list */

	struct blocking_notifier_head notifier_list;
};

#define to_espi_controller(d)	container_of(d, struct espi_controller, dev)

struct espi_device {
	struct device dev;
	struct espi_controller *ctrl;
	u8 cs;
	char modalias[ESPI_NAME_SIZE];
	void *platform_data;
	struct list_head list;
};

#define to_espi_device(d)	container_of(d, struct espi_device, dev)

struct espi_driver {
	struct device_driver driver;
	int (*probe)(struct espi_device *edev);
	void (*remove)(struct espi_device *edev);
	const struct espi_device_id *id_table;
};

#define to_espi_driver(d)	container_of_const(d, struct espi_driver, driver)

extern const struct bus_type espi_bus_type;

struct espi_controller *espi_controller_alloc(struct device *parent,
					      unsigned int size);
int espi_controller_register(struct espi_controller *ctrl);
void espi_controller_unregister(struct espi_controller *ctrl);
void espi_controller_put(struct espi_controller *ctrl);
struct espi_controller *espi_controller_get_by_bus_num(int bus_num);

static inline void *espi_controller_get_devdata(struct espi_controller *ctrl)
{
	return dev_get_drvdata(&ctrl->dev);
}

static inline void espi_controller_set_devdata(struct espi_controller *ctrl,
					       void *data)
{
	dev_set_drvdata(&ctrl->dev, data);
}

int espi_get_capabilities(struct espi_controller *ctrl,
			  struct espi_capabilities *caps);
bool espi_channel_is_enabled(struct espi_controller *ctrl, u8 channel);

int espi_get_configuration(struct espi_controller *ctrl,
			   u32 slave_reg_addr, u32 *config);
int espi_set_configuration(struct espi_controller *ctrl,
			   u32 slave_reg_addr, u32 config);
int espi_inband_reset(struct espi_controller *ctrl);
int espi_get_status(struct espi_controller *ctrl,
		    struct espi_slave_status *status);

int espi_enable_channel(struct espi_controller *ctrl, u8 channel);
int espi_disable_channel(struct espi_controller *ctrl, u8 channel);

/**
 * espi_periph_io_read - issue a Peripheral Channel I/O read cycle
 * @ctrl:  controller
 * @port:  16-bit I/O port address
 * @width: access width in bytes (1, 2, or 4)
 * @value: receives the read value
 */
int espi_periph_io_read(struct espi_controller *ctrl,
			u16 port, u8 width, u32 *value);
/**
 * espi_periph_io_write - issue a Peripheral Channel I/O write cycle
 * @ctrl:  controller
 * @port:  16-bit I/O port address
 * @width: access width in bytes (1, 2, or 4)
 * @value: value to write
 */
int espi_periph_io_write(struct espi_controller *ctrl,
			 u16 port, u8 width, u32 value);
int espi_periph_mem_read(struct espi_controller *ctrl,
			 u32 addr, void *buf, size_t len);
int espi_periph_mem_write(struct espi_controller *ctrl,
			  u32 addr, const void *buf, size_t len);

int espi_vwire_get(struct espi_controller *ctrl,
		   u8 index, u8 *value, u8 *valid);
/**
 * espi_vwire_put - send a PUT_VIRTUAL_WIRE command
 * @ctrl:  controller
 * @index: VWire group index
 * @value: wire value byte
 * @valid: valid mask byte
 */
int espi_vwire_put(struct espi_controller *ctrl,
		   u8 index, u8 value, u8 valid);

int espi_oob_send(struct espi_controller *ctrl,
		  const void *buf, size_t len, u8 tag);
int espi_oob_recv(struct espi_controller *ctrl,
		  void *buf, size_t *len, u8 *tag);

int espi_flash_read(struct espi_controller *ctrl,
		    u32 offset, void *buf, size_t len);
int espi_flash_write(struct espi_controller *ctrl,
		     u32 offset, const void *buf, size_t len);
int espi_flash_erase(struct espi_controller *ctrl,
		     u32 offset, size_t len);

int espi_handle_alert(struct espi_controller *ctrl);

struct espi_device *espi_new_device(struct espi_controller *ctrl,
				    const struct espi_board_info *info);
void espi_remove_device(struct espi_device *edev);

int espi_register_notifier(struct espi_controller *ctrl,
			   struct notifier_block *nb);
int espi_unregister_notifier(struct espi_controller *ctrl,
			     struct notifier_block *nb);
int espi_notify_event(struct espi_controller *ctrl,
		      struct espi_event *event);

static inline int espi_dev_get_configuration(struct espi_device *edev,
					     u32 addr, u32 *config)
{
	return espi_get_configuration(edev->ctrl, addr, config);
}

static inline int espi_dev_set_configuration(struct espi_device *edev,
					     u32 addr, u32 config)
{
	return espi_set_configuration(edev->ctrl, addr, config);
}

static inline int espi_dev_inband_reset(struct espi_device *edev)
{
	return espi_inband_reset(edev->ctrl);
}

static inline int espi_dev_get_status(struct espi_device *edev,
				      struct espi_slave_status *status)
{
	return espi_get_status(edev->ctrl, status);
}

int __espi_register_driver(struct module *owner, struct espi_driver *drv);
void espi_unregister_driver(struct espi_driver *drv);

#define espi_register_driver(drv) \
	__espi_register_driver(THIS_MODULE, drv)

#define module_espi_driver(__espi_driver) \
	module_driver(__espi_driver, espi_register_driver, espi_unregister_driver)

#endif /* _LINUX_ESPI_ESPI_H */
