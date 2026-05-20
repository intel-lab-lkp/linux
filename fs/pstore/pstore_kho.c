// SPDX-License-Identifier: GPL-2.0-only
/*
 * KHO (Kexec Handover) backend for pstore
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/io.h>
#include <linux/kexec_handover.h>
#include <linux/libfdt.h>
#include <linux/module.h>
#include <linux/pstore.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#define KHO_PSTORE_FDT_NAME	"pstore-kho"
#define KHO_PSTORE_COMPAT	"pstore-kho-v1"
#define KHO_PSTORE_DATA		"pstore_kho_record"

static const size_t record_max_size = 1 << CONFIG_LOG_BUF_SHIFT;

struct pstore_kho_record {
	ssize_t			size;
	struct timespec64	time;
	int			count;
	enum kmsg_dump_reason	reason;
	unsigned int		part;
	bool			compressed;
	char			buf[];
};

struct pstore_kho_context {
	struct pstore_info pstore;
	bool read_done;
};

static struct pstore_kho_record *kho_data_in;
static struct pstore_kho_record *kho_data_out;
static void *fdt_out;

static int pstore_kho_open(struct pstore_info *psi)
{
	struct pstore_kho_context *cxt = psi->data;

	cxt->read_done = false;
	return 0;
}

static ssize_t pstore_kho_read(struct pstore_record *record)
{
	struct pstore_kho_context *cxt = record->psi->data;

	if (cxt->read_done)
		return 0;

	cxt->read_done = true;

	if (!kho_data_in || !kho_data_in->size)
		return 0;

	record->buf = kmemdup(kho_data_in->buf, kho_data_in->size, GFP_KERNEL);
	if (!record->buf)
		return -ENOMEM;

	record->type = PSTORE_TYPE_DMESG;
	record->id = 0;
	record->size = kho_data_in->size;
	record->time = kho_data_in->time;
	record->count = kho_data_in->count;
	record->reason = kho_data_in->reason;
	record->part = kho_data_in->part;
	record->compressed = kho_data_in->compressed;

	return record->size;
}

static int pstore_kho_write(struct pstore_record *record)
{
	if (record->type != PSTORE_TYPE_DMESG)
		return -EINVAL;

	if (kho_data_out->size != 0) {
		pr_err("pstore kho already contains a record\n");
		return -ENOSPC;
	}

	if (record->size > record_max_size) {
		pr_err("dmesg record too big, record size: %lu, available space: %lu\n",
		       record->size, record_max_size);
		return -ENOSPC;
	}

	memcpy(kho_data_out->buf, record->buf, record->size);
	kho_data_out->size = record->size;
	kho_data_out->time = record->time;
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
		.bufsize	= record_max_size,
		.flags		= PSTORE_FLAGS_DMESG,
		.max_reason	= KMSG_DUMP_SHUTDOWN,
		.open		= pstore_kho_open,
		.read		= pstore_kho_read,
		.write		= pstore_kho_write,
	},
};

static void __init kho_setup_incoming(void)
{
	phys_addr_t fdt_phys;
	void *fdt_in;
	const phys_addr_t *kho_data_phys;
	int err, len;

	err = kho_retrieve_subtree(KHO_PSTORE_FDT_NAME, &fdt_phys);
	if (err) {
		if (err != -ENOENT)
			pr_err("failed to retrieve FDT %s from KHO: %d\n",
			       KHO_PSTORE_FDT_NAME, err);
		return;
	}

	fdt_in = phys_to_virt(fdt_phys);
	err = fdt_node_check_compatible(fdt_in, 0, KHO_PSTORE_COMPAT);
	if (err) {
		pr_err("FDT '%s' is incompatible with '%s' [%d]\n",
		       KHO_PSTORE_FDT_NAME, KHO_PSTORE_COMPAT, err);
		return;
	}

	kho_data_phys = fdt_getprop(fdt_in, 0, KHO_PSTORE_DATA, &len);
	if (!kho_data_phys || len != sizeof(*kho_data_phys)) {
		pr_err("failed to find kho property %s\n", KHO_PSTORE_DATA);
		return;
	}

	kho_data_in = phys_to_virt(get_unaligned(kho_data_phys));

	pr_info("successfully restored preserved data\n");
}

static int __init kho_setup_outgoing(void)
{
	phys_addr_t header_ser_phys;
	int err;
	size_t total_size = sizeof(struct pstore_kho_record) + record_max_size;

	kho_data_out = kho_alloc_preserve(total_size);
	if (IS_ERR(kho_data_out)) {
		pr_err("failed to allocate pstore kho data\n");
		return PTR_ERR(kho_data_out);
	}

	fdt_out = kho_alloc_preserve(PAGE_SIZE);
	if (IS_ERR(fdt_out)) {
		pr_err("failed to allocate/preserve FDT memory\n");
		err = PTR_ERR(fdt_out);
		goto err_free_header;
	}

	err = fdt_create(fdt_out, PAGE_SIZE);
	err |= fdt_finish_reservemap(fdt_out);
	err |= fdt_begin_node(fdt_out, "");
	err |= fdt_property_string(fdt_out, "compatible", KHO_PSTORE_COMPAT);

	header_ser_phys = virt_to_phys(kho_data_out);
	err |= fdt_property(fdt_out, KHO_PSTORE_DATA, &header_ser_phys,
			    sizeof(header_ser_phys));
	err |= fdt_end_node(fdt_out);
	err |= fdt_finish(fdt_out);
	if (err) {
		pr_err("failed to configure fdt tree\n");
		goto err_free_fdt;
	}

	err = kho_add_subtree(KHO_PSTORE_FDT_NAME, fdt_out);
	if (err) {
		pr_err("failed to add subtree\n");
		goto err_free_fdt;
	}

	return 0;

err_free_fdt:
	kho_unpreserve_free(fdt_out);

err_free_header:
	kho_unpreserve_free(kho_data_out);
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
		goto err_free_incoming;
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
	kho_remove_subtree(fdt_out);
	kho_unpreserve_free(fdt_out);

	kho_unpreserve_free(kho_data_out);

err_free_incoming:
	if (kho_data_in)
		kho_restore_free(kho_data_in);

	return err;
}
module_init(pstore_kho_init);

static void __exit pstore_kho_exit(void)
{
	pstore_unregister(&pstore_kho_cxt.pstore);
	kfree(pstore_kho_cxt.pstore.buf);

	kho_remove_subtree(fdt_out);
	kho_unpreserve_free(fdt_out);

	kho_unpreserve_free(kho_data_out);

	if (kho_data_in)
		kho_restore_free(kho_data_in);
}
module_exit(pstore_kho_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Pstore backend for dmesg preservation over kexec");
