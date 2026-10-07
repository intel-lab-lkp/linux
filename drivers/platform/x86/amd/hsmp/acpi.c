// SPDX-License-Identifier: GPL-2.0
/*
 * AMD HSMP Platform Driver
 * Copyright (c) 2024, AMD.
 * All Rights Reserved.
 *
 * This file provides an ACPI based driver implementation for HSMP interface.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <asm/amd/hsmp.h>

#include <linux/acpi.h>
#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/dev_printk.h>
#include <linux/ioport.h>
#include <linux/kref.h>
#include <linux/kstrtox.h>
#include <linux/lockdep.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/rwsem.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/topology.h>
#include <linux/uuid.h>

#include <uapi/asm-generic/errno-base.h>

#include "hsmp.h"

#define DRIVER_NAME		"hsmp_acpi"

/* These are the strings specified in ACPI table */
#define MSG_IDOFF_STR		"MsgIdOffset"
#define MSG_ARGOFF_STR		"MsgArgOffset"
#define MSG_RESPOFF_STR		"MsgRspOffset"

static struct hsmp_plat_device *hsmp_pdev;

/*
 * Tracks the ACPI socket platform devices that share the socket array and the
 * /dev/hsmp misc device. The first probe initializes it, each further probe
 * takes a reference and every remove (or probe failure) drops one; the last
 * put frees the shared state via hsmp_acpi_sock_release(). All get/put run
 * under hsmp_sock_rwsem held for write, so the counting is already serialized
 * and the atomic in kref is not strictly needed; kref is used for the clearer
 * get/put interface and its release callback.
 */
static struct kref hsmp_acpi_sock_kref;

static int amd_hsmp_acpi_rdwr(struct hsmp_socket *sock, u32 offset,
			      u32 *value, bool write)
{
	if (write)
		iowrite32(*value, sock->virt_base_addr + offset);
	else
		*value = ioread32(sock->virt_base_addr + offset);

	return 0;
}

/* This is the UUID used for HSMP */
static const guid_t acpi_hsmp_uuid = GUID_INIT(0xb74d619d, 0x5707, 0x48bd,
						0xa6, 0x9f, 0x4e, 0xa2,
						0x87, 0x1f, 0xc2, 0xf6);

static inline bool is_acpi_hsmp_uuid(union acpi_object *obj)
{
	if (obj->type == ACPI_TYPE_BUFFER && obj->buffer.length == UUID_SIZE)
		return guid_equal((guid_t *)obj->buffer.pointer, &acpi_hsmp_uuid);

	return false;
}

static inline int hsmp_get_uid(struct device *dev, u16 *sock_ind)
{
	char *uid;

	/*
	 * UID (ID00, ID01..IDXX) is used for differentiating sockets,
	 * read it and strip the "ID" part of it and convert the remaining
	 * bytes to integer.
	 */
	uid = acpi_device_uid(ACPI_COMPANION(dev));
	if (!uid || strlen(uid) < 3)
		return -EINVAL;

	return kstrtou16(uid + 2, 10, sock_ind);
}

static acpi_status hsmp_resource(struct acpi_resource *res, void *data)
{
	struct hsmp_socket *sock = data;
	struct resource r;

	switch (res->type) {
	case ACPI_RESOURCE_TYPE_FIXED_MEMORY32:
		if (!acpi_dev_resource_memory(res, &r))
			return AE_ERROR;
		if (!r.start || r.end < r.start || !(r.flags & IORESOURCE_MEM_WRITEABLE))
			return AE_ERROR;
		sock->mbinfo.base_addr = r.start;
		sock->mbinfo.size = resource_size(&r);
		break;
	case ACPI_RESOURCE_TYPE_END_TAG:
		break;
	default:
		return AE_ERROR;
	}

	return AE_OK;
}

static int hsmp_read_acpi_dsd(struct device *dev, struct hsmp_socket *sock)
{
	struct acpi_buffer buf = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *guid, *mailbox_package;
	union acpi_object *dsd;
	acpi_status status;
	int ret = 0;
	int j;

	status = acpi_evaluate_object_typed(ACPI_HANDLE(dev), "_DSD", NULL,
					    &buf, ACPI_TYPE_PACKAGE);
	if (ACPI_FAILURE(status)) {
		dev_err(dev, "Failed to read mailbox reg offsets from DSD table, err: %s\n",
			acpi_format_exception(status));
		return -ENODEV;
	}

	dsd = buf.pointer;

	/* HSMP _DSD property should contain 2 objects.
	 * 1. guid which is an acpi object of type ACPI_TYPE_BUFFER
	 * 2. mailbox which is an acpi object of type ACPI_TYPE_PACKAGE
	 *    This mailbox object contains 3 more acpi objects of type
	 *    ACPI_TYPE_PACKAGE for holding msgid, msgresp, msgarg offsets
	 *    these packages inturn contain 2 acpi objects of type
	 *    ACPI_TYPE_STRING and ACPI_TYPE_INTEGER
	 */
	if (!dsd || dsd->type != ACPI_TYPE_PACKAGE || dsd->package.count != 2) {
		ret = -EINVAL;
		goto free_buf;
	}

	guid = &dsd->package.elements[0];
	mailbox_package = &dsd->package.elements[1];
	if (!is_acpi_hsmp_uuid(guid) || mailbox_package->type != ACPI_TYPE_PACKAGE) {
		dev_err(dev, "Invalid hsmp _DSD table data\n");
		ret = -EINVAL;
		goto free_buf;
	}

	for (j = 0; j < mailbox_package->package.count; j++) {
		union acpi_object *msgobj, *msgstr, *msgint;

		msgobj	= &mailbox_package->package.elements[j];

		/* package should have 1 string and 1 integer object */
		if (msgobj->type != ACPI_TYPE_PACKAGE ||
		    msgobj->package.count < 2) {
			ret = -EINVAL;
			goto free_buf;
		}

		msgstr	= &msgobj->package.elements[0];
		msgint	= &msgobj->package.elements[1];

		if (msgstr->type != ACPI_TYPE_STRING ||
		    msgint->type != ACPI_TYPE_INTEGER) {
			ret = -EINVAL;
			goto free_buf;
		}

		if (!strncmp(msgstr->string.pointer, MSG_IDOFF_STR,
			     msgstr->string.length)) {
			sock->mbinfo.msg_id_off = msgint->integer.value;
		} else if (!strncmp(msgstr->string.pointer, MSG_RESPOFF_STR,
				    msgstr->string.length)) {
			sock->mbinfo.msg_resp_off =  msgint->integer.value;
		} else if (!strncmp(msgstr->string.pointer, MSG_ARGOFF_STR,
				    msgstr->string.length)) {
			sock->mbinfo.msg_arg_off = msgint->integer.value;
		} else {
			ret = -ENOENT;
			goto free_buf;
		}
	}

	if (!sock->mbinfo.msg_id_off || !sock->mbinfo.msg_resp_off ||
	    !sock->mbinfo.msg_arg_off)
		ret = -EINVAL;

free_buf:
	ACPI_FREE(buf.pointer);
	return ret;
}

static int hsmp_read_acpi_crs(struct device *dev, struct hsmp_socket *sock)
{
	acpi_status status;

	status = acpi_walk_resources(ACPI_HANDLE(dev), METHOD_NAME__CRS,
				     hsmp_resource, sock);
	if (ACPI_FAILURE(status)) {
		dev_err(dev, "Failed to look up MP1 base address from CRS method, err: %s\n",
			acpi_format_exception(status));
		return -EINVAL;
	}
	if (!sock->mbinfo.base_addr || !sock->mbinfo.size)
		return -EINVAL;

	/* The mapped region should be un-cached */
	sock->virt_base_addr = devm_ioremap_uc(dev, sock->mbinfo.base_addr,
					       sock->mbinfo.size);
	if (!sock->virt_base_addr) {
		dev_err(dev, "Failed to ioremap MP1 base address\n");
		return -ENOMEM;
	}

	return 0;
}

/* Parse the ACPI table to read the data */
static int hsmp_parse_acpi_table(struct device *dev, u16 sock_ind)
{
	struct hsmp_socket *sock = &hsmp_pdev->sock[sock_ind];
	int ret;

	sock->sock_ind		= sock_ind;
	sock->amd_hsmp_rdwr	= amd_hsmp_acpi_rdwr;

	sema_init(&sock->hsmp_sem, 1);

	dev_set_drvdata(dev, sock);

	/* Read MP1 base address from CRS method */
	ret = hsmp_read_acpi_crs(dev, sock);
	if (ret)
		return ret;

	/* Read mailbox offsets from DSD table */
	ret = hsmp_read_acpi_dsd(dev, sock);
	if (ret)
		return ret;

	/*
	 * Publish sock->dev last.  hsmp_send_message() uses it (via
	 * smp_load_acquire()) as the readiness gate for the lock-free data
	 * plane, so it must become visible only after virt_base_addr, the
	 * mailbox offsets and the semaphore are fully initialized.  On a
	 * multi-socket system socket 0 exposes /dev/hsmp before later sockets
	 * finish probing, so without this an ioctl aimed at a socket still in
	 * bring-up could pass the gate and dereference a NULL virt_base_addr.
	 */
	smp_store_release(&sock->dev, dev);

	return 0;
}

/*
 * Bring up one ACPI HSMP socket: parse its ACPI table, run the mailbox
 * handshake and register its sysfs/hwmon interfaces.
 *
 * Called with hsmp_sock_rwsem held for write by hsmp_acpi_probe(), so the
 * per-socket bring-up cannot race a concurrent probe or remove.
 */
static int init_acpi(struct device *dev)
{
	u16 sock_ind;
	int ret;

	lockdep_assert_held_write(&hsmp_sock_rwsem);

	ret = hsmp_get_uid(dev, &sock_ind);
	if (ret)
		return ret;
	if (sock_ind >= hsmp_pdev->num_sockets)
		return -EINVAL;

	ret = hsmp_parse_acpi_table(dev, sock_ind);
	if (ret) {
		dev_err(dev, "Failed to parse ACPI table\n");
		return ret;
	}

	/* Test the hsmp interface */
	ret = hsmp_test(sock_ind, 0xDEADBEEF);
	if (ret) {
		dev_err(dev, "HSMP test message failed on Fam:%x model:%x\n",
			boot_cpu_data.x86, boot_cpu_data.x86_model);
		dev_err(dev, "Is HSMP disabled in BIOS ?\n");
		return ret;
	}

	ret = hsmp_cache_proto_ver(sock_ind);
	if (ret) {
		dev_err(dev, "Failed to read HSMP protocol version\n");
		return ret;
	}

	if (hsmp_pdev->proto_ver >= HSMP_PROTO_VER6) {
		ret = hsmp_get_tbl_dram_base(sock_ind);
		if (ret)
			dev_info(dev, "Failed to init metric table\n");
	}

	ret = hsmp_create_sensor(dev, sock_ind);
	if (ret)
		dev_info(dev, "Failed to register HSMP sensors with hwmon\n");

	dev_set_drvdata(dev, &hsmp_pdev->sock[sock_ind]);

	return 0;
}

static const struct acpi_device_id amd_hsmp_acpi_ids[] = {
	{ACPI_HSMP_DEVICE_HID, 0},
	{}
};
MODULE_DEVICE_TABLE(acpi, amd_hsmp_acpi_ids);

/*
 * kref release: tear down the shared ACPI socket state once the last socket
 * drops its reference. Deregister /dev/hsmp if it was registered, unmap any
 * metric-table DRAM, destroy the per-socket mutexes and free the socket array.
 *
 * Runs from kref_put() with hsmp_sock_rwsem held for write, since the remove
 * and probe-failure paths both drop their reference under that lock. The write
 * lock has drained any in-flight hsmp_send_message(), so unmapping the mailbox
 * and freeing the array cannot race the data plane.
 */
static void hsmp_acpi_sock_release(struct kref *kref)
{
	lockdep_assert_held_write(&hsmp_sock_rwsem);

	if (!IS_ERR_OR_NULL(hsmp_pdev->mdev.this_device))
		hsmp_misc_deregister();
	hsmp_unmap_metric_tbls(hsmp_pdev);
	hsmp_destroy_metric_read_locks(hsmp_pdev);
	kfree(hsmp_pdev->sock);
	hsmp_pdev->sock = NULL;
	hsmp_pdev->num_sockets = 0;
	hsmp_pdev->proto_ver = 0;
}

/**
 * hsmp_acpi_probe_failure_cleanup() - Undo a failed ACPI socket probe.
 * @dev: ACPI companion device whose probe failed.
 *
 * This device already took a reference on entry to hsmp_acpi_probe(), so clear
 * its sock->dev and drop that reference; the shared state is released if it was
 * the last one.
 *
 * Clearing sock->dev matters on multi-socket systems: when a non-first socket
 * fails, the array stays alive (owned by an already-probed socket) and
 * remove() is never called for this device, yet devres unmaps its mailbox once
 * probe() returns. Without clearing dev, a later message to this index would
 * pass every gate in hsmp_send_message() and reach the unmapped mailbox.
 *
 * sock is NULL if probe failed before hsmp_parse_acpi_table() set the drvdata.
 *
 * Called from hsmp_acpi_probe(), which already holds hsmp_sock_rwsem for write.
 */
static void hsmp_acpi_probe_failure_cleanup(struct device *dev)
{
	struct hsmp_socket *sock = dev_get_drvdata(dev);

	lockdep_assert_held_write(&hsmp_sock_rwsem);

	if (sock)
		sock->dev = NULL;

	kref_put(&hsmp_acpi_sock_kref, hsmp_acpi_sock_release);
}

static int hsmp_acpi_probe(struct platform_device *pdev)
{
	int ret;

	hsmp_pdev = get_hsmp_pdev();
	if (!hsmp_pdev)
		return -ENOMEM;

	/*
	 * Multiple ACPI socket devices probe in parallel, but the one-time
	 * socket-array allocation and /dev/hsmp registration below must run
	 * exactly once. Hold the socket rwsem for write across the whole
	 * bring-up so it cannot race a concurrent probe or remove, and so the
	 * probe-failure teardown drains the data plane.
	 */
	guard(rwsem_write)(&hsmp_sock_rwsem);

	if (!hsmp_pdev->sock) {
		hsmp_pdev->num_sockets = topology_max_packages();
		if (!hsmp_pdev->num_sockets) {
			dev_err(&pdev->dev, "No CPU sockets detected\n");
			return -ENODEV;
		}

		hsmp_pdev->sock = kzalloc_objs(*hsmp_pdev->sock,
					       hsmp_pdev->num_sockets);
		if (!hsmp_pdev->sock)
			return -ENOMEM;

		hsmp_init_metric_read_locks(hsmp_pdev);
		kref_init(&hsmp_acpi_sock_kref);
	} else {
		kref_get(&hsmp_acpi_sock_kref);
	}

	/*
	 * This socket now holds a reference (kref_init on the first socket,
	 * kref_get afterwards). Every failure path below drops it via
	 * hsmp_acpi_probe_failure_cleanup(), and a successful probe hands it to
	 * hsmp_acpi_remove().
	 */
	ret = init_acpi(&pdev->dev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to initialize HSMP interface.\n");
		hsmp_acpi_probe_failure_cleanup(&pdev->dev);
		return ret;
	}

	if (IS_ERR_OR_NULL(hsmp_pdev->mdev.this_device)) {
		/*
		 * Register /dev/hsmp unparented. It is a singleton shared by all
		 * ACPI sockets and outlives all but the last of them, so
		 * parenting it to this socket's device would leave a dangling
		 * parent once that socket is unbound.
		 */
		ret = hsmp_misc_register(NULL);
		if (ret) {
			dev_err(&pdev->dev, "Failed to register misc device\n");
			hsmp_acpi_probe_failure_cleanup(&pdev->dev);
			return ret;
		}
		dev_dbg(&pdev->dev, "AMD HSMP ACPI misc device registered\n");
	}

	ret = hsmp_sock_dev_register(dev_get_drvdata(&pdev->dev));
	if (ret)
		dev_warn(&pdev->dev, "Failed to register HSMP socket device: %d\n", ret);

	return 0;
}

static void hsmp_acpi_remove(struct platform_device *pdev)
{
	struct hsmp_socket *sock = dev_get_drvdata(&pdev->dev);

	hsmp_sock_dev_unregister(sock);

	/*
	 * Serialize the kref_put() and any release it triggers against a
	 * concurrent probe, and drain the data plane for the whole
	 * teardown: this covers the per-socket unbind, whose mailbox devres
	 * unmaps once we return, and the last unbind that frees the socket
	 * array in hsmp_acpi_sock_release().
	 */
	guard(rwsem_write)(&hsmp_sock_rwsem);

	/*
	 * Clear this socket's dev so hsmp_send_message() rejects it before
	 * devres unmaps the mailbox. On a non-final unbind the socket array
	 * stays alive, so without this a later message to this index would
	 * reach an unmapped iomem region.
	 */
	sock->dev = NULL;

	kref_put(&hsmp_acpi_sock_kref, hsmp_acpi_sock_release);
}

static struct platform_driver amd_hsmp_driver = {
	.probe		= hsmp_acpi_probe,
	.remove		= hsmp_acpi_remove,
	.driver		= {
		.name	= DRIVER_NAME,
		.acpi_match_table = amd_hsmp_acpi_ids,
		.dev_groups = hsmp_sock_groups,
	},
};

module_platform_driver(amd_hsmp_driver);

MODULE_IMPORT_NS("AMD_HSMP");
MODULE_DESCRIPTION("AMD HSMP Platform Interface Driver");
MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL");
