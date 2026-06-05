// SPDX-License-Identifier: GPL-2.0
/*
 * KHO (Kexec Handover) backend for pstore.
 *
 * KHO-based pstore provides a mechanism to hand over pstore data (specifically
 * dmesg logs) from one kernel to another across a kexec reboot using the
 * Kexec Handover (KHO) framework.
 *
 * Key advantages of KHO-based pstore include:
 * - No hardcoded memmap: Unlike ramoops, it does not require reserving a static
 *   memory region in the bootloader or device tree. Memory is allocated
 *   dynamically and handed over to the next kernel.
 * - Firmware independence: It does not rely on platform firmware support (like
 *   ACPI ERST or UEFI variable storage) to preserve logs across reboots.
 * - High throughput: It avoids the performance bottlenecks of serial consoles,
 *   not being limited by console baud rates.
 * - Complete log preservation: It preserves all dmesg logs, including those
 *   generated late in the reboot cycle after filesystems have been unmounted,
 *   up to the point of the kexec jump.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kho/abi/pstore.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kexec_handover.h>
#include <linux/module.h>
#include <linux/pstore.h>
#include <linux/slab.h>
#include <linux/string.h>

/*
 * The in and out buffers are separate and they need not be the same size.
 * Therefore, this is not part of ABI.
 */
#define RECORD_MAX_SIZE		(1 << CONFIG_LOG_BUF_SHIFT)

struct pstore_kho_context {
	struct pstore_info pstore;
	bool read_done;
};

static struct pstore_ser *kho_ser_in;
static struct pstore_ser *kho_ser_out;

static int pstore_kho_open(struct pstore_info *psi)
{
	struct pstore_kho_context *cxt = psi->data;

	cxt->read_done = false;
	return 0;
}

static ssize_t pstore_kho_read(struct pstore_record *record)
{
	struct pstore_kho_context *cxt = record->psi->data;
	struct pstore_kho_record *kho_data_in;

	if (cxt->read_done || !kho_ser_in)
		return 0;

	cxt->read_done = true;
	kho_data_in = &kho_ser_in->record;

	record->buf = kmemdup(kho_data_in->buf, kho_data_in->size, GFP_KERNEL);
	if (!record->buf)
		return -ENOMEM;

	record->type = PSTORE_TYPE_DMESG;
	record->id = 0;
	record->size = kho_data_in->size;
	record->time.tv_sec = kho_data_in->time_sec;
	record->time.tv_nsec = kho_data_in->time_nsec;
	record->count = kho_data_in->count;
	record->reason = kho_data_in->reason;
	record->part = kho_data_in->part;
	record->compressed = kho_data_in->compressed;

	return record->size;
}

static int pstore_kho_write(struct pstore_record *record)
{
	struct pstore_kho_record *kho_data_out = &kho_ser_out->record;

	if (record->type != PSTORE_TYPE_DMESG)
		return -EINVAL;

	if (kho_data_out->size != 0) {
		pr_err("pstore kho already contains a record\n");
		return -ENOSPC;
	}

	if (record->size > RECORD_MAX_SIZE) {
		pr_err("dmesg record too big, record size: %lu, available space: %d\n",
		       record->size, RECORD_MAX_SIZE);
		return -ENOSPC;
	}

	memcpy(kho_data_out->buf, record->buf, record->size);
	kho_data_out->size = record->size;
	kho_data_out->time_sec = record->time.tv_sec;
	kho_data_out->time_nsec = record->time.tv_nsec;
	kho_data_out->count = record->count;
	kho_data_out->reason = record->reason;
	kho_data_out->part = record->part;
	kho_data_out->compressed = record->compressed;

	return 0;
}

static struct pstore_kho_context pstore_kho_cxt = {
	.pstore = {
		.owner		= THIS_MODULE,
		.name		= "kho",
		.bufsize	= RECORD_MAX_SIZE,
		.flags		= PSTORE_FLAGS_DMESG,
		.max_reason	= KMSG_DUMP_SHUTDOWN,
		.open		= pstore_kho_open,
		.read		= pstore_kho_read,
		.write		= pstore_kho_write,
	},
};

static void __init kho_setup_incoming(void)
{
	phys_addr_t kho_ser_phys;
	int err;

	err = kho_retrieve_subtree(KHO_PSTORE_FDT_NAME, &kho_ser_phys);
	if (err) {
		if (err != -ENOENT)
			pr_err("failed to retrieve KHO data %s: %d\n",
			       KHO_PSTORE_FDT_NAME, err);
		return;
	}

	kho_ser_in = phys_to_virt(kho_ser_phys);

	if (kho_ser_in->version != KHO_PSTORE_VERSION) {
		pr_err("unsupported KHO pstore version: %d\n", kho_ser_in->version);
		kho_ser_in = NULL;
		return;
	}

	pr_info("successfully restored preserved data\n");
}

static int __init kho_setup_outgoing(void)
{
	int err;
	size_t total_size = sizeof(struct pstore_ser) + RECORD_MAX_SIZE;

	kho_ser_out = kho_alloc_preserve(total_size);
	if (IS_ERR(kho_ser_out)) {
		pr_err("failed to allocate pstore kho ser anchor\n");
		return PTR_ERR(kho_ser_out);
	}
	memset(kho_ser_out, 0, total_size);
	kho_ser_out->version = KHO_PSTORE_VERSION;

	err = kho_add_subtree(KHO_PSTORE_FDT_NAME, kho_ser_out);
	if (err) {
		pr_err("failed to add KHO data\n");
		goto err_free_ser;
	}

	return 0;

err_free_ser:
	kho_unpreserve_free(kho_ser_out);
	return err;
}

static int __init pstore_kho_init(void)
{
	int err;
	struct pstore_kho_context *cxt = &pstore_kho_cxt;

	if (!kho_is_enabled()) {
		pr_info("KHO is disabled, pstore_kho cannot start\n");
		return -ENODEV;
	}

	kho_setup_incoming();
	err = kho_setup_outgoing();
	if (err) {
		pr_err("failed to setup outgoing KHO\n");
		return err;
	}

	cxt->pstore.data = cxt;
	cxt->pstore.buf = kmalloc(cxt->pstore.bufsize, GFP_KERNEL);
	if (!cxt->pstore.buf) {
		err = -ENOMEM;
		goto err_free_outgoing;
	}

	err = pstore_register(&cxt->pstore);
	if (err) {
		pr_err("failed to register with pstore\n");
		goto err_free_pstore_buf;
	}

	return 0;

err_free_pstore_buf:
	kfree(cxt->pstore.buf);

err_free_outgoing:
	kho_remove_subtree(kho_ser_out);
	kho_unpreserve_free(kho_ser_out);

	return err;
}
module_init(pstore_kho_init);

static void __exit pstore_kho_exit(void)
{
	pstore_unregister(&pstore_kho_cxt.pstore);
	kfree(pstore_kho_cxt.pstore.buf);

	kho_remove_subtree(kho_ser_out);
	kho_unpreserve_free(kho_ser_out);
}
module_exit(pstore_kho_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Pstore backend for dmesg preservation over kexec");
