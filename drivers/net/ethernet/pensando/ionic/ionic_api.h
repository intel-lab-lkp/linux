/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2018-2025, Advanced Micro Devices, Inc. */

#ifndef _IONIC_API_H_
#define _IONIC_API_H_

#include <linux/auxiliary_bus.h>
#include "ionic_if.h"
#include "ionic_regs.h"

/**
 * struct ionic_aux_dev - Auxiliary device information
 * @handle:     Handle for this auxiliary device
 * @idx:        Index identifier
 * @adev:       Auxiliary device
 */
struct ionic_aux_dev {
	void *handle;
	int idx;
	struct auxiliary_device adev;
};

/**
 * struct ionic_devinfo - device information
 * @asic_type:      Device ASIC type code
 * @asic_rev:       Device ASIC revision code
 * @fw_version:     Device firmware version, as a string
 * @serial_num:     Device serial number, as a string
 */
struct ionic_devinfo {
	u8 asic_type;
	u8 asic_rev;
	char fw_version[IONIC_DEVINFO_FWVERS_BUFLEN + 1];
	char serial_num[IONIC_DEVINFO_SERIAL_BUFLEN + 1];
};

/**
 * ionic_api_get_identity - Get result of device identification
 * @handle:     Handle to lif
 * @lif_index:  This lif index
 *
 * Return: pointer to result of identification
 */
const union ionic_lif_identity *ionic_api_get_identity(void *handle,
						       int *lif_index);

/**
 * ionic_api_get_netdev_from_handle - Get a network device associated with the
 *                                    handle
 * @handle:     Handle to lif
 *
 * This returns a network device associated with the lif handle.
 * If network device is available it holds the reference to device. Caller must
 * ensure that it releases the device using dev_put() after its usage.
 *
 * Return: Network device on success or ERR_PTR(error)
 */
struct net_device *ionic_api_get_netdev_from_handle(void *handle);

/**
 * ionic_api_get_devinfo - Get device information
 * @handle:     Handle to lif
 *
 * Return: pointer to device information
 */
const struct ionic_devinfo *ionic_api_get_devinfo(void *handle);

/**
 * ionic_api_request_reset - request reset or disable the device or lif
 * @handle:     Handle to lif
 *
 * The reset is triggered asynchronously. It will wait until reset request
 * completes or times out.
 */
void ionic_api_request_reset(void *handle);

#define IONIC_EXPDB_64B_WQE	BIT(0)
#define IONIC_EXPDB_128B_WQE	BIT(1)
#define IONIC_EXPDB_256B_WQE	BIT(2)
#define IONIC_EXPDB_512B_WQE	BIT(3)
struct ionic_qtype_info {
	u64 features;
	u16 desc_sz;
	u16 comp_sz;
	u16 sg_desc_sz;
	u16 max_sg_elems;
	u16 sg_desc_stride;
	u8  version;
	u8  supported;
};

/**
 * ionic_api_get_queue_identity - Get queue identity
 * @handle:     Handle to lif
 * @qtype:      Queue type (enum ionic_logical_qtype)
 *
 * Return: pointer to queue identity
 */
struct ionic_qtype_info *
ionic_api_get_queue_identity(void *handle, enum ionic_logical_qtype qtype);

/**
 * ionic_api_get_expdb - Get express DB capability
 * @handle:     Handle to lif
 *
 * Return: express DB capability flag
 */
u8 ionic_api_get_expdb(void *handle);

/**
 * ionic_api_get_intr - Reserve a device interrupt index
 * @handle:     Handle to lif
 * @irq:        OS interrupt number returned
 *
 * Reserve an interrupt index, and indicate the irq number for that index.
 *
 * Return: interrupt index or negative error status
 */
int ionic_api_get_intr(void *handle, int *irq);

/**
 * ionic_api_put_intr - Release a device interrupt index
 * @handle:     Handle to lif
 * @intr:       Interrupt index
 *
 * Mark the interrupt index unused so that it can be reserved again.
 */
void ionic_api_put_intr(void *handle, int intr);

/**
 * ionic_api_get_cmb - Reserve cmb pages
 * @handle:      Handle to lif
 * @pgid:        First page index
 * @pgaddr:      First page bus addr (contiguous)
 * @order:       Log base two number of pages (PAGE_SIZE)
 * @stride_log2: Size of stride to determine CMB pool
 * @expdb:       Will be set to true if this CMB region has expdb enabled
 *
 * Return: zero or negative error status
 */
int ionic_api_get_cmb(void *handle, u32 *pgid, phys_addr_t *pgaddr, int order,
		      u8 stride_log2, bool *expdb);

/**
 * ionic_api_put_cmb - Release cmb pages
 * @handle:     Handle to lif
 * @pgid:       First page index
 * @order:      Log base two number of pages (PAGE_SIZE)
 */
void ionic_api_put_cmb(void *handle, u32 pgid, int order);

/**
 * ionic_api_kernel_dbpage - Get mapped doorbell page for use in kernel space
 * @handle:     Handle to lif
 * @intr_ctrl:  Interrupt control registers
 * @dbid:       Doorbell id for use in kernel space
 * @dbpage:     One ioremapped doorbell page for use in kernel space
 *
 * This also provides mapped interrupt control registers.
 *
 * The id and page returned here refer to the doorbell page reserved for use in
 * kernel space for this lif.  For user space, use ionic_api_get_dbid to
 * allocate a doorbell id for exclusive use by a process.
 */
void ionic_api_kernel_dbpage(void *handle,
			     struct ionic_intr __iomem **intr_ctrl,
			     u32 *dbid, u64 __iomem **dbpage);

/**
 * ionic_api_get_dbid - Reserve a doorbell id
 * @handle:     Handle to lif
 * @dbid:       Doorbell id
 * @addr:       Phys address of doorbell page
 *
 * Reserve a doorbell id.  This corresponds with exactly one doorbell page at
 * an offset from the doorbell page base address, that can be mapped into a
 * user space process.
 *
 * Return: zero on success or negative error status
 */
int ionic_api_get_dbid(void *handle, u32 *dbid, phys_addr_t *addr);

/**
 * ionic_api_put_dbid - Release a doorbell id
 * @handle:     Handle to lif
 * @dbid:       Doorbell id
 *
 * Mark the doorbell id unused, so that it can be reserved again.
 */
void ionic_api_put_dbid(void *handle, int dbid);

/**
 * struct ionic_admin_ctx - Admin command context
 * @work:       Work completion wait queue element
 * @cmd:        Admin command (64B) to be copied to the queue
 * @comp:       Admin completion (16B) copied from the queue
 */
struct ionic_admin_ctx {
	struct completion work;
	union ionic_adminq_cmd cmd;
	union ionic_adminq_comp comp;
};

/**
 * ionic_api_adminq_post - Post an admin command
 * @handle:     Handle to lif
 * @ctx:        API admin command context
 *
 * Post the command to an admin queue in the ethernet driver.  If this command
 * succeeds, then the command has been posted, but that does not indicate a
 * completion.  If this command returns success, then the completion callback
 * will eventually be called.
 *
 * Return: zero or negative error status
 */
int ionic_api_adminq_post(void *handle, struct ionic_admin_ctx *ctx);

/**
 * ionic_api_adminq_post_wait - Post an admin command and wait for response
 * @handle:     Handle to lif
 * @ctx:        API admin command context
 *
 * Post the command to an admin queue in the ethernet driver.  If this command
 * succeeds, then the command has been posted, but that does not indicate a
 * completion.  If this command returns success, then the completion callback
 * will eventually be called.
 *
 * Return: zero or negative error status
 */
int ionic_api_adminq_post_wait(void *handle, struct ionic_admin_ctx *ctx);

/**
 * ionic_api_error_to_errno - Transform ionic_if errors to os errno
 * @code:       Ionic error number
 *
 * Return:      Negative OS error number or zero
 */
int ionic_api_error_to_errno(enum ionic_status_code code);

#endif /* _IONIC_API_H_ */
