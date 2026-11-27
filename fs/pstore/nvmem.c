// SPDX-License-Identifier: GPL-2.0
/*
 * pstore backend for NVMEM devices.
 *
 * The NVMEM device is selected by name.  Normal writes use the regular
 * NVMEM write path; panic writes use the non-sleeping write callback so a
 * SPI FRAM or another NVMEM provider can persist the crash log before the
 * next reset.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/device.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/pstore_zone.h>
#include <linux/string.h>

static char pstore_nvmem_dev_name[80] = CONFIG_PSTORE_NVMEM_DEV;
module_param_string(device_name, pstore_nvmem_dev_name, 80, 0400);
MODULE_PARM_DESC(device_name, "NVMEM device name for pstore storage");

static unsigned long offset;
module_param(offset, ulong, 0400);
MODULE_PARM_DESC(offset, "offset in the NVMEM device in bytes");

static unsigned long total_size;
module_param(total_size, ulong, 0400);
MODULE_PARM_DESC(total_size, "pstore area size in bytes (0: rest of device)");

static long kmsg_size = CONFIG_PSTORE_NVMEM_KMSG_SIZE;
module_param(kmsg_size, long, 0400);
MODULE_PARM_DESC(kmsg_size, "kmsg dump zone size in kbytes");

static int max_reason = CONFIG_PSTORE_NVMEM_MAX_REASON;
module_param(max_reason, int, 0400);
MODULE_PARM_DESC(max_reason, "maximum reason for kmsg dump");

#if IS_ENABLED(CONFIG_PSTORE_PMSG)
static long pmsg_size = CONFIG_PSTORE_NVMEM_PMSG_SIZE;
#else
static long pmsg_size = -1;
#endif
module_param(pmsg_size, long, 0400);
MODULE_PARM_DESC(pmsg_size, "pmsg zone size in kbytes");

#if IS_ENABLED(CONFIG_PSTORE_CONSOLE)
static long console_size = CONFIG_PSTORE_NVMEM_CONSOLE_SIZE;
#else
static long console_size = -1;
#endif
module_param(console_size, long, 0400);
MODULE_PARM_DESC(console_size, "console zone size in kbytes");

#if IS_ENABLED(CONFIG_PSTORE_FTRACE)
static long ftrace_size = CONFIG_PSTORE_NVMEM_FTRACE_SIZE;
#else
static long ftrace_size = -1;
#endif
module_param(ftrace_size, long, 0400);
MODULE_PARM_DESC(ftrace_size, "ftrace zone size in kbytes");

static struct nvmem_device *pstore_nvmem;
static struct pstore_zone_info pstore_info;

static int pstore_nvmem_match(struct device *dev, const void *data)
{
	return !strcmp(dev_name(dev), data);
}

static unsigned long size_kb_to_bytes(long kb)
{
	if (kb <= 0)
		return 0;

	return ALIGN((unsigned long)kb * 1024, 4096);
}

static bool pstore_nvmem_range_ok(size_t size, loff_t off)
{
	if (off < 0 || off > UINT_MAX || size > UINT_MAX)
		return false;
	if (offset > UINT_MAX - off)
		return false;

	return size <= UINT_MAX - (offset + off);
}

static ssize_t pstore_nvmem_read(char *buf, size_t size, loff_t off)
{
	int ret;

	if (!pstore_nvmem_range_ok(size, off))
		return -EINVAL;

	ret = nvmem_device_read(pstore_nvmem, offset + off, size, buf);
	if (ret)
		return ret;

	return size;
}

static ssize_t pstore_nvmem_write(const char *buf, size_t size, loff_t off)
{
	int ret;

	if (!pstore_nvmem_range_ok(size, off))
		return -EINVAL;

	ret = nvmem_device_write(pstore_nvmem, offset + off, size, (void *)buf);
	if (ret)
		return ret;

	return size;
}

static ssize_t pstore_nvmem_panic_write(const char *buf, size_t size,
					loff_t off)
{
	int ret;

	if (!pstore_nvmem_range_ok(size, off))
		return -EINVAL;

	ret = nvmem_device_write_panic(pstore_nvmem, offset + off, size,
				       (void *)buf);
	if (ret)
		return ret;

	return size;
}

/* The NVMEM device must already be registered; load providers first. */
static int __init pstore_nvmem_init(void)
{
	unsigned long dev_size, area_size;
	unsigned long kmsg, pmsg, console, ftrace;
	int ret;

	if (!pstore_nvmem_dev_name[0]) {
		pr_err("no NVMEM device name configured\n");
		return -EINVAL;
	}

	pstore_nvmem = nvmem_device_find(pstore_nvmem_dev_name, pstore_nvmem_match);
	if (IS_ERR(pstore_nvmem)) {
		ret = PTR_ERR(pstore_nvmem);
		pr_err("failed to find NVMEM device %s: %d\n", pstore_nvmem_dev_name, ret);
		return ret;
	}

	dev_size = nvmem_dev_size(pstore_nvmem);
	if (!dev_size || offset >= dev_size) {
		pr_err("invalid offset %lu for %lu-byte device\n",
		       offset, dev_size);
		ret = -EINVAL;
		goto err_put;
	}

	area_size = dev_size - offset;
	if (total_size && total_size < area_size)
		area_size = total_size;
	area_size = ALIGN_DOWN(area_size, 4096);
	if (area_size < 4096) {
		pr_err("pstore area must be at least 4096 bytes\n");
		ret = -EINVAL;
		goto err_put;
	}

	kmsg = size_kb_to_bytes(kmsg_size);
	pmsg = size_kb_to_bytes(pmsg_size);
	console = size_kb_to_bytes(console_size);
	ftrace = size_kb_to_bytes(ftrace_size);
	if (!kmsg && !pmsg && !console && !ftrace) {
		pr_err("at least one zone size must be non-zero\n");
		ret = -EINVAL;
		goto err_put;
	}
	if (kmsg + pmsg + console + ftrace > area_size) {
		pr_err("zone sizes exceed the %lu-byte pstore area\n",
		       area_size);
		ret = -EINVAL;
		goto err_put;
	}

	pstore_info.owner = THIS_MODULE;
	pstore_info.name = KBUILD_MODNAME;
	pstore_info.total_size = area_size;
	pstore_info.kmsg_size = kmsg;
	pstore_info.pmsg_size = pmsg;
	pstore_info.console_size = console;
	pstore_info.ftrace_size = ftrace;
	pstore_info.max_reason = max_reason;
	pstore_info.read = pstore_nvmem_read;
	pstore_info.write = pstore_nvmem_write;
	pstore_info.panic_write = pstore_nvmem_panic_write;

	ret = register_pstore_zone(&pstore_info);
	if (ret) {
		pr_err("failed to register pstore zone: %d\n", ret);
		goto err_put;
	}

	pr_info("using NVMEM device %s, offset %lu, size %lu\n",
		pstore_nvmem_dev_name, offset, area_size);
	return 0;

err_put:
	nvmem_device_put(pstore_nvmem);
	pstore_nvmem = NULL;
	return ret;
}
late_initcall(pstore_nvmem_init);

static void __exit pstore_nvmem_exit(void)
{
	if (!pstore_nvmem)
		return;

	unregister_pstore_zone(&pstore_info);
	nvmem_device_put(pstore_nvmem);
	pstore_nvmem = NULL;
}
module_exit(pstore_nvmem_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("pstore backend for NVMEM devices");
