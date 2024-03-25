/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __LINUX_USB_UCSI_H
#define __LINUX_USB_UCSI_H

#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/types.h>

/* -------------------------------------------------------------------------- */

struct ucsi;
struct ucsi_altmode;

/* UCSI offsets (Bytes) */
#define UCSI_VERSION			0
#define UCSI_CCI			4
#define UCSI_CONTROL			8
#define UCSI_MESSAGE_IN			16
#define UCSI_MESSAGE_OUT		32
#define UCSIv2_MESSAGE_OUT		272

/* Command Status and Connector Change Indication (CCI) bits */
#define UCSI_CCI_CONNECTOR(_c_)		(((_c_) & GENMASK(7, 1)) >> 1)
#define UCSI_CCI_LENGTH(_c_)		(((_c_) & GENMASK(15, 8)) >> 8)
#define UCSI_CCI_NOT_SUPPORTED		BIT(25)
#define UCSI_CCI_CANCEL_COMPLETE	BIT(26)
#define UCSI_CCI_RESET_COMPLETE		BIT(27)
#define UCSI_CCI_BUSY			BIT(28)
#define UCSI_CCI_ACK_COMPLETE		BIT(29)
#define UCSI_CCI_ERROR			BIT(30)
#define UCSI_CCI_COMMAND_COMPLETE	BIT(31)

/**
 * struct ucsi_operations - UCSI I/O operations
 * @read: Read operation
 * @sync_write: Blocking write operation
 * @async_write: Non-blocking write operation
 * @update_altmodes: Squashes duplicate DP altmodes
 *
 * Read and write routines for UCSI interface. @sync_write must wait for the
 * Command Completion Event from the PPM before returning, and @async_write must
 * return immediately after sending the data to the PPM.
 */
struct ucsi_operations {
	int (*read)(struct ucsi *ucsi, unsigned int offset,
		    void *val, size_t val_len);
	int (*sync_write)(struct ucsi *ucsi, unsigned int offset,
			  const void *val, size_t val_len);
	int (*async_write)(struct ucsi *ucsi, unsigned int offset,
			   const void *val, size_t val_len);
	bool (*update_altmodes)(struct ucsi *ucsi, struct ucsi_altmode *orig,
				struct ucsi_altmode *updated);
};

struct ucsi *ucsi_create(struct device *dev, const struct ucsi_operations *ops);
void ucsi_destroy(struct ucsi *ucsi);
int ucsi_register(struct ucsi *ucsi);
void ucsi_unregister(struct ucsi *ucsi);
int ucsi_resume(struct ucsi *ucsi);
void *ucsi_get_drvdata(struct ucsi *ucsi);
void ucsi_set_drvdata(struct ucsi *ucsi, void *data);

void ucsi_connector_change(struct ucsi *ucsi, u8 num);

#endif /* __LINUX_USB_UCSI_H */
