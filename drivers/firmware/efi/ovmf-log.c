// SPDX-License-Identifier: GPL-2.0

#include <linux/efi.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/sysfs.h>

#define MEM_DEBUG_LOG_MAGIC1  0x3167646d666d766f  // "ovmfmdg1"
#define MEM_DEBUG_LOG_MAGIC2  0x3267646d666d766f  // "ovmfmdg2"

struct mem_debug_log_header {
	u64    magic1;
	u64    magic2;
	u64    hdr_size;
	u64    log_size;
	u64    lock; // edk2 spinlock
	u64    head_off;
	u64    tail_off;
	u64    truncated;
	u8     fw_version[128];
};

static struct mem_debug_log_header *hdr;
static u8 *logbuf;
static u64 logbufsize;

static ssize_t ovmf_log_read(struct file *filp, struct kobject *kobj,
			     const struct bin_attribute *attr, char *buf,
			     loff_t offset, size_t count)
{
	u64 start, end;

	start = hdr->head_off + offset;
	if (hdr->head_off > hdr->tail_off && start >= hdr->log_size)
		start -= hdr->log_size;

	end = start + count;
	if (start > hdr->tail_off) {
		if (end > hdr->log_size)
			end = hdr->log_size;
	} else {
		if (end > hdr->tail_off)
			end = hdr->tail_off;
	}

	if (start > logbufsize || end > logbufsize)
		return 0;
	if (start >= end)
		return 0;

	memcpy(buf, logbuf + start, end - start);
	return end - start;
}

static struct bin_attribute ovmf_log_bin_attr = {
	.attr = {
		.name = "ovmf_debug_log",
		.mode = 0444,
	},
	.read = ovmf_log_read,
};

static int ovmf_log_probe(struct platform_device *dev)
{
	u64 size;
	int ret = -EINVAL;

	if (efi.ovmf_debug_log == EFI_INVALID_TABLE_ADDR) {
		dev_err(&dev->dev, "OVMF debug log: not available\n");
		return -EINVAL;
	}

	/* map + verify header */
	hdr = memremap(efi.ovmf_debug_log, sizeof(*hdr), MEMREMAP_WB);
	if (!hdr) {
		dev_err(&dev->dev, "OVMF debug log: header map failed\n");
		return -EINVAL;
	}

	if (hdr->magic1 != MEM_DEBUG_LOG_MAGIC1 ||
	    hdr->magic2 != MEM_DEBUG_LOG_MAGIC2) {
		dev_err(&dev->dev, "OVMF debug log: magic mismatch\n");
		goto err_unmap;
	}

	size = hdr->hdr_size + hdr->log_size;
	dev_info(&dev->dev, "firmware version: \"%s\"\n", hdr->fw_version);
	dev_info(&dev->dev, "log buffer size: %lldk\n", size / 1024);

	/* map complete log buffer */
	iounmap(hdr);
	hdr = memremap(efi.ovmf_debug_log, size, MEMREMAP_WB);
	if (!hdr) {
		dev_err(&dev->dev, "OVMF debug log: buffer map failed\n");
		return -EINVAL;
	}
	logbuf = (void*)hdr + hdr->hdr_size;
	logbufsize = hdr->log_size;

	ovmf_log_bin_attr.size = size;
	ret = sysfs_create_bin_file(efi_kobj, &ovmf_log_bin_attr);
	if (ret != 0) {
		dev_err(&dev->dev, "OVMF debug log: sysfs register failed\n");
		goto err_unmap;
	}

	return 0;

err_unmap:
	iounmap(hdr);
	return ret;
}

static void ovmf_log_remove(struct platform_device *dev)
{
	iounmap(hdr);
}

static struct platform_driver ovmf_log_driver = {
	.probe = ovmf_log_probe,
	.remove = ovmf_log_remove,
	.driver = {
		.name = "ovmf_log",
	},
};

module_platform_driver(ovmf_log_driver);

MODULE_DESCRIPTION("OVMF debug log");
MODULE_AUTHOR("Gerd Hoffmann <kraxel@redhat.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:ovmf_log");
