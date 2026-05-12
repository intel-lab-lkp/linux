// SPDX-License-Identifier: GPL-2.0-only
/*
 * PCI Crash Buffer - Capture PCI config space for crash analysis
 *
 * Copyright (c) 2026 Amazon.com, Inc. or its affiliates.
 *
 * Captures PCI configuration space at crash time so AER error
 * registers reflect the crash-time state for off-site analysis.
 *
 * Design:
 * - Init (late_initcall): enumerate devices, allocate buffer.
 * - Hotplug: bus notifier queues deferred rebuild of device list
 *   and buffer via workqueue -- no PCI reads.
 * - Crash: crash_save_vmcoreinfo() calls pci_crash_save() which
 *   reads config space into buffer, flushes dcache to RAM so
 *   data survives kexec into crash kernel.
 *
 * Records are variable-length: each device's record is exactly
 * 8 + pdev->cfg_size bytes (264 for legacy PCI, 4104 for PCIe).
 * The parser walks records sequentially using per-record config_size.
 *
 * Buffer pages may be physically scattered (kvmalloc falls back to
 * vmalloc for buffers exceeding ~4 MB).  A small kmalloc'd pagemap
 * records each page's physical address so the crash parser can
 * reconstruct the buffer without page-table walking.
 *
 * pci_read_config_dword() is direct MMIO (no locks) -- safe in crash.
 * for_each_pci_dev() needs pci_bus_sem -- only used at init/hotplug.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/crash_dump.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/pci_crash.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>
#include <linux/workqueue.h>

#include <linux/cacheflush.h>
#include <linux/unaligned.h>

/**
 * pci_crash_flush_dcache() - Flush a memory region from CPU cache to RAM
 * @addr: virtual address of region to flush
 * @size: size in bytes
 *
 * Used at crash time to ensure the crash kernel sees our buffer/pagemap
 * writes after kexec.
 */
static inline void pci_crash_flush_dcache(void *addr, size_t size)
{
#ifdef CONFIG_ARM64
	unsigned long start = (unsigned long)addr;
	unsigned long end = start + size;

	dcache_clean_inval_poc(start, end);
#elif defined(CONFIG_X86)
	clflush_cache_range(addr, size);
#endif
}

/**
 * pci_crash_buffer - Virtual address of PCI crash capture buffer
 *
 * Contains PCI configuration space data captured at panic time.
 * Written by pci_crash_save(), read by crash kernel via VMCOREINFO.
 * May be vmalloc'd -- use pci_crash_pagemap for physical addresses.
 */
void *pci_crash_buffer;
EXPORT_SYMBOL_GPL(pci_crash_buffer);

/**
 * pci_crash_buffer_size - Size of pci_crash_buffer in bytes
 */
size_t pci_crash_buffer_size;
EXPORT_SYMBOL_GPL(pci_crash_buffer_size);

/**
 * pci_crash_pagemap_phys - Physical address of page directory
 *
 * Points to a struct pci_crash_pagemap (always kmalloc'd, direct-mapped).
 * Exported via VMCOREINFO so the crash kernel can locate buffer pages
 * without walking page tables.
 */
phys_addr_t pci_crash_pagemap_phys;
EXPORT_SYMBOL_GPL(pci_crash_pagemap_phys);

static struct pci_crash_pagemap *pci_crash_pagemap;
static size_t pci_crash_pagemap_size;
static struct pci_dev **pci_crash_devs;
static unsigned int pci_crash_num_devs;
static DEFINE_MUTEX(pci_crash_lock);

/*
 * Set in pci_crash_init() after delayed_work, PCI bus and notifier are
 * ready.  Guards parse + rebuild in param setters: at boot (level -1)
 * the setter just stores the string; pci_crash_init() parses and does
 * the initial rebuild once PCI is up.
 */
static bool pci_crash_ready;

/*
 * capture -- when to capture PCI config space.
 * Comma-separated tokens:
 *   aer    -- root port ROOT_STATUS has uncorrectable errors (default)
 *   always -- every panic regardless of PCI error state
 *
 * Writable at runtime (0644) so operators and tests can toggle without
 * reboot.  Writes re-parse capture_flags immediately.
 */
static char capture[32] = "aer";

#define PCI_CRASH_CAPTURE_AER		BIT(0)
#define PCI_CRASH_CAPTURE_ALWAYS	BIT(1)
static unsigned long capture_flags = PCI_CRASH_CAPTURE_AER;

static void pci_crash_parse_capture(void);

static int capture_param_set(const char *val, const struct kernel_param *kp)
{
	if (strlen(val) >= sizeof(capture))
		return -EINVAL;
	strscpy(capture, val, sizeof(capture));
	strim(capture);
	if (READ_ONCE(pci_crash_ready))
		pci_crash_parse_capture();
	return 0;
}

static int capture_param_get(char *buf, const struct kernel_param *kp)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n", capture);
}

static const struct kernel_param_ops capture_param_ops = {
	.set = capture_param_set,
	.get = capture_param_get,
};
module_param_cb(capture, &capture_param_ops, NULL, 0644);
MODULE_PARM_DESC(capture, "When to capture: aer, always (default: aer)");

/*
 * devices -- which devices to capture.
 * Comma-separated tokens:
 *   all        -- every PCI device (default)
 *   bridges    -- PCI bridges (class 0604, 0607)
 *   root_ports -- PCIe root ports only
 *   XXYY       -- hex PCI class code (class + subclass)
 *
 * Bridges are always implicitly included regardless of filter value
 * because they hold the AER registers needed for root cause analysis.
 * Applies at rebuild time only -- zero cost at crash time.  Writable
 * at runtime (0644); writes re-parse and trigger async rebuild.
 */
static char devices[256] = "all";

static void pci_crash_parse_devices(void);
static struct delayed_work pci_crash_rebuild_dwork;

/* Debounce period for bus notifications (ms).
 * TRN2 liveupdate enumerates ~3000 VFs in ~1.5s -- this coalesces
 * the storm into a single rebuild after the last event.
 */
#define PCI_CRASH_REBUILD_DELAY_MS	200

static int devices_param_set(const char *val, const struct kernel_param *kp)
{
	if (strlen(val) >= sizeof(devices))
		return -EINVAL;

	mutex_lock(&pci_crash_lock);
	strscpy(devices, val, sizeof(devices));
	strim(devices);
	if (READ_ONCE(pci_crash_ready)) {
		pci_crash_parse_devices();
		mod_delayed_work(system_wq, &pci_crash_rebuild_dwork,
				 msecs_to_jiffies(PCI_CRASH_REBUILD_DELAY_MS));
	}
	mutex_unlock(&pci_crash_lock);
	return 0;
}

static int devices_param_get(char *buf, const struct kernel_param *kp)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n", devices);
}

static const struct kernel_param_ops devices_param_ops = {
	.set = devices_param_set,
	.get = devices_param_get,
};
module_param_cb(devices, &devices_param_ops, NULL, 0644);
MODULE_PARM_DESC(devices,
	"Which devices: all, bridges, root_ports, XXYY hex class (default: all)");

#define PCI_CRASH_DEVICES_ALL		BIT(0)
#define PCI_CRASH_DEVICES_BRIDGES	BIT(1)
#define PCI_CRASH_DEVICES_ROOT_PORTS	BIT(2)
#define PCI_CRASH_MAX_DEVICE_CLASSES	8
static unsigned long devices_flags = PCI_CRASH_DEVICES_ALL;
static u16 device_classes[PCI_CRASH_MAX_DEVICE_CLASSES];
static unsigned int device_class_count;

static void pci_crash_parse_capture(void)
{
	char *buf, *token, *rest;
	unsigned long flags = 0;

	if (!*capture) {
		WRITE_ONCE(capture_flags, PCI_CRASH_CAPTURE_AER);
		return;
	}

	buf = kstrdup(capture, GFP_KERNEL);
	if (!buf) {
		WRITE_ONCE(capture_flags, PCI_CRASH_CAPTURE_AER);
		return;
	}

	rest = buf;
	while ((token = strsep(&rest, ",")) != NULL) {
		if (strcmp(token, "aer") == 0)
			flags |= PCI_CRASH_CAPTURE_AER;
		else if (strcmp(token, "always") == 0)
			flags |= PCI_CRASH_CAPTURE_ALWAYS;
		else
			pr_warn("unknown capture token: %s\n",
				token);
	}
	kfree(buf);

	if (!flags) {
		pr_warn("no valid capture tokens, defaulting to aer\n");
		flags = PCI_CRASH_CAPTURE_AER;
	}
	WRITE_ONCE(capture_flags, flags);
}

static void pci_crash_parse_devices(void)
{
	char *buf, *token, *rest;
	unsigned long val;

	devices_flags = 0;
	device_class_count = 0;

	if (!*devices) {
		devices_flags = PCI_CRASH_DEVICES_ALL;
		return;
	}

	buf = kstrdup(devices, GFP_KERNEL);
	if (!buf) {
		devices_flags = PCI_CRASH_DEVICES_ALL;
		return;
	}

	rest = buf;
	while ((token = strsep(&rest, ",")) != NULL) {
		if (strcmp(token, "all") == 0) {
			devices_flags |= PCI_CRASH_DEVICES_ALL;
		} else if (strcmp(token, "bridges") == 0) {
			devices_flags |= PCI_CRASH_DEVICES_BRIDGES;
		} else if (strcmp(token, "root_ports") == 0) {
			devices_flags |= PCI_CRASH_DEVICES_ROOT_PORTS;
		} else if (kstrtoul(token, 16, &val) == 0 && val <= 0xFFFF) {
			if (device_class_count < PCI_CRASH_MAX_DEVICE_CLASSES)
				device_classes[device_class_count++] = (u16)val;
			else
				pr_warn("too many device classes (max %d)\n",
					PCI_CRASH_MAX_DEVICE_CLASSES);
		} else {
			pr_warn("unknown devices token: %s\n",
				token);
		}
	}
	kfree(buf);

	if (!devices_flags && device_class_count == 0) {
		pr_warn("no valid devices tokens, defaulting to all\n");
		devices_flags = PCI_CRASH_DEVICES_ALL;
	}
}

static bool pci_crash_device_matches(struct pci_dev *pdev)
{
	unsigned int i;
	u16 dev_class = pdev->class >> 8;

	if (devices_flags & PCI_CRASH_DEVICES_ALL)
		return true;

	/* Bridges always included -- they hold AER registers */
	if (dev_class == PCI_CLASS_BRIDGE_PCI ||
	    dev_class == PCI_CLASS_BRIDGE_CARDBUS)
		return true;

	if ((devices_flags & PCI_CRASH_DEVICES_ROOT_PORTS) &&
	    pci_pcie_type(pdev) == PCI_EXP_TYPE_ROOT_PORT)
		return true;

	for (i = 0; i < device_class_count; i++) {
		if (dev_class == device_classes[i])
			return true;
	}

	return false;
}

/* Sanity limit -- prevents multi-GB allocations on systems with many VFs */
#define PCI_CRASH_MAX_BUFFER_SIZE	(24 * 1024 * 1024)

/**
 * pci_crash_build_pagemap() - Build physical page directory for buffer
 * @buf: buffer allocated via kvmalloc (may be vmalloc'd)
 * @buf_size: buffer size in bytes
 *
 * Allocates a kmalloc'd directory containing the physical address of
 * each page backing @buf.  The pagemap is always direct-mapped, so
 * virt_to_phys() works on it at crash time.
 *
 * Returns the new pagemap, or NULL on allocation failure.
 */
static struct pci_crash_pagemap *pci_crash_build_pagemap(void *buf,
							 size_t buf_size)
{
	unsigned int num_pages = DIV_ROUND_UP(buf_size, PAGE_SIZE);
	struct pci_crash_pagemap *pm;
	unsigned int i;

	pm = kmalloc(struct_size(pm, addrs, num_pages), GFP_KERNEL);
	if (!pm)
		return NULL;

	pm->magic = cpu_to_le32(PCI_CRASH_PAGEMAP_MAGIC);
	pm->num_pages = cpu_to_le32(num_pages);
	pm->buf_size = cpu_to_le64(buf_size);

	for (i = 0; i < num_pages; i++) {
		struct page *page;
		phys_addr_t pa;

		if (is_vmalloc_addr(buf + i * PAGE_SIZE))
			page = vmalloc_to_page(buf + i * PAGE_SIZE);
		else
			page = virt_to_page(buf + i * PAGE_SIZE);

		if (!page) {
			kfree(pm);
			return NULL;
		}
		pa = page_to_phys(page);
		pm->addrs[i] = cpu_to_le64(pa);
	}

	return pm;
}

/**
 * pci_crash_read_config_space() - Read config space for one device
 * @pdev: PCI device to read
 * @ptr: destination pointer within the crash buffer
 *
 * Reads pdev->cfg_size bytes of config space (256 for legacy PCI,
 * 4096 for PCIe). Each 4-byte dword is read individually via MMIO.
 * Failed reads write 0xFFFFFFFF -- standard PCI convention for
 * absent/unreachable devices.
 */
static void pci_crash_read_config_space(struct pci_dev *pdev, u8 *ptr)
{
	struct pci_crash_device_record *record =
		(struct pci_crash_device_record *)ptr;
	u8 *cfg_data = ptr + PCI_CRASH_RECORD_META;
	int offset;
	u32 val;

	record->domain = cpu_to_le16(pci_domain_nr(pdev->bus));
	record->bus = pdev->bus->number;
	record->devfn = pdev->devfn;
	record->config_size = cpu_to_le32(pdev->cfg_size);

	for (offset = 0; offset < pdev->cfg_size; offset += 4) {
		if (pci_read_config_dword(pdev, offset, &val)) {
			put_unaligned_le32(0xFFFFFFFF, &cfg_data[offset]);
			continue;
		}
		put_unaligned_le32(val, &cfg_data[offset]);
	}
}

/**
 * pci_crash_fill_buffer() - Populate buffer with config space
 * @buffer: destination buffer (header + variable-length records)
 * @num_devs: number of devices to read
 *
 * Records are variable-length: each is PCI_CRASH_RECORD_META +
 * pdev->cfg_size bytes. The header's config_size is 0 to indicate
 * variable-length; parsers walk records using per-record config_size.
 *
 * Uses ktime_get_real_fast_ns() for the timestamp -- safe in NMI/panic
 * context (lockless, reads the NMI-safe timekeeper snapshot).
 */
static void pci_crash_fill_buffer(void *buffer, unsigned int num_devs)
{
	struct pci_crash_buffer_header *header = buffer;
	struct pci_dev **devs = READ_ONCE(pci_crash_devs);
	u8 *ptr;
	unsigned int i;

	header->magic = cpu_to_le32(PCI_CRASH_MAGIC);
	header->version = cpu_to_le32(PCI_CRASH_VERSION);
	header->device_count = cpu_to_le32(num_devs);
	header->config_size = 0;
	header->timestamp = cpu_to_le64(ktime_get_real_fast_ns());
	header->flags = 0;
	header->reserved = 0;

	ptr = (u8 *)buffer + PCI_CRASH_HEADER_SIZE;
	for (i = 0; i < num_devs; i++) {
		struct pci_dev *pdev = devs[i];

		if (unlikely(!pdev))
			break;
		pci_crash_read_config_space(pdev, ptr);
		ptr += PCI_CRASH_RECORD_META + pdev->cfg_size;
	}

	header->device_count = cpu_to_le32(i);
}

/**
 * pci_crash_rebuild_snapshot() - Rebuild device list and allocate buffer
 *
 * Two-pass approach:
 *   Pass 1: count PCI devices
 *   Pass 2: populate device array (filtered) and compute exact buffer
 *            size from actual pdev->cfg_size per device (no padding)
 *
 * The devices param controls which devices are included.  Bridges are
 * always included regardless of devices setting (they hold AER registers).
 * devices=all (default) includes everything.
 *
 * Does NOT read PCI config space -- reads happen only at crash time.
 * This keeps rebuild fast during VF enumeration storms (~6000 ADD
 * events on TRN2 during liveupdate).
 *
 * After allocation, builds the pagemap so the crash parser can
 * locate the buffer's physical pages in the vmcore.
 *
 * Caller must hold pci_crash_lock.
 */
static void pci_crash_rebuild_snapshot(void)
{
	struct pci_dev *pdev = NULL;
	unsigned int count = 0, i;
	void *old_buf;
	unsigned int old_num_devs = pci_crash_num_devs;
	struct pci_dev **old_devs = pci_crash_devs;
	struct pci_crash_pagemap *old_pm = pci_crash_pagemap;
	struct pci_crash_pagemap *new_pm;
	struct pci_dev **new_devs;
	void *new_buf;
	size_t total_size;

	/* Pass 1: count devices */
	for_each_pci_dev(pdev)
		count++;

	if (count == 0) {
		pr_info("no PCI devices found\n");
		WRITE_ONCE(pci_crash_num_devs, 0);
		if (old_devs) {
			for (i = 0; i < old_num_devs; i++)
				pci_dev_put(old_devs[i]);
			kvfree(old_devs);
			WRITE_ONCE(pci_crash_devs, NULL);
		}
		kfree(old_pm);
		WRITE_ONCE(pci_crash_pagemap, NULL);
		WRITE_ONCE(pci_crash_pagemap_phys, 0);
		WRITE_ONCE(pci_crash_pagemap_size, 0);
		old_buf = pci_crash_buffer;
		WRITE_ONCE(pci_crash_buffer, NULL);
		WRITE_ONCE(pci_crash_buffer_size, 0);
		kvfree(old_buf);
		return;
	}

	/*
	 * Disable capture during rebuild to prevent pci_crash_save()
	 * from accessing stale device references or partially populated
	 * arrays.
	 */
	WRITE_ONCE(pci_crash_num_devs, 0);

	new_devs = kvmalloc_array(count, sizeof(struct pci_dev *),
				  GFP_KERNEL | __GFP_ZERO);
	if (!new_devs) {
		pr_err("dev array alloc failed (%u)\n", count);
		goto err_restore;
	}

	/*
	 * Pass 2: populate device array (filtered) and compute exact
	 * buffer size from actual pdev->cfg_size per device.
	 * count from pass 1 is an upper bound; actual may be smaller.
	 */
	total_size = PCI_CRASH_HEADER_SIZE;
	pdev = NULL;
	i = 0;
	for_each_pci_dev(pdev) {
		if (i >= count) {
			pci_dev_put(pdev);
			break;
		}
		if (!pci_crash_device_matches(pdev))
			continue;
		new_devs[i] = pci_dev_get(pdev);
		total_size += PCI_CRASH_RECORD_META + pdev->cfg_size;
		i++;
	}
	count = i;

	if (count == 0) {
		kvfree(new_devs);
		pr_info("no devices match devices=%s\n", devices);
		goto err_restore;
	}

	if (total_size > PCI_CRASH_MAX_BUFFER_SIZE) {
		for (i = 0; i < count; i++)
			pci_dev_put(new_devs[i]);
		kvfree(new_devs);
		pr_warn("buffer too large (%zu > %d bytes)\n",
			total_size, PCI_CRASH_MAX_BUFFER_SIZE);
		goto err_restore;
	}

	new_buf = kvmalloc(total_size, GFP_KERNEL | __GFP_ZERO);
	if (!new_buf) {
		for (i = 0; i < count; i++)
			pci_dev_put(new_devs[i]);
		kvfree(new_devs);
		pr_err("buffer alloc failed (%zu bytes)\n",
		       total_size);
		goto err_restore;
	}

	new_pm = pci_crash_build_pagemap(new_buf, total_size);
	if (!new_pm) {
		kvfree(new_buf);
		for (i = 0; i < count; i++)
			pci_dev_put(new_devs[i]);
		kvfree(new_devs);
		pr_err("pagemap alloc failed\n");
		goto err_restore;
	}

	/* Release old device references */
	for (i = 0; i < old_num_devs; i++)
		if (old_devs && old_devs[i])
			pci_dev_put(old_devs[i]);

	WRITE_ONCE(pci_crash_devs, new_devs);
	kvfree(old_devs);

	/*
	 * Swap pagemap first (with pre-computed phys addr), then buffer,
	 * then size and count.  If crash fires mid-swap, num_devs is
	 * still 0 from above, so pci_crash_save() bails out safely.
	 */
	WRITE_ONCE(pci_crash_pagemap, new_pm);
	WRITE_ONCE(pci_crash_pagemap_phys, virt_to_phys(new_pm));
	WRITE_ONCE(pci_crash_pagemap_size, struct_size(new_pm, addrs,
			le32_to_cpu(new_pm->num_pages)));
	kfree(old_pm);

	old_buf = pci_crash_buffer;
	WRITE_ONCE(pci_crash_buffer, new_buf);
	WRITE_ONCE(pci_crash_buffer_size, total_size);
	/* Ensure buffer/pagemap/size are visible before num_devs enables capture */
	smp_wmb();
	WRITE_ONCE(pci_crash_num_devs, count);
	kvfree(old_buf);

	pr_info("rebuild: %u devices (%zu bytes, %u pages)\n",
		count, total_size, le32_to_cpu(new_pm->num_pages));
	return;

err_restore:
	WRITE_ONCE(pci_crash_num_devs, old_num_devs);
}

/**
 * pci_crash_save() - Capture PCI config space at crash time
 *
 * Called from crash_save_vmcoreinfo() inside __crash_kexec(), which
 * runs before machine_kexec() boots the crash kernel.  This is the
 * only reliable capture point -- panic notifiers run AFTER kexec by
 * default (crash_kexec_post_notifiers=0).
 *
 * Capture check (capture param):
 *   always  -- capture unconditionally
 *   aer     -- quick-scan root port AER ROOT_STATUS for uncorrectable
 *             errors; skip if none found
 *
 * When capture=always, captures on every panic.
 * This is useful for cascading failures: a PCI link-down can cause
 * an MCE or NMI watchdog timeout before DPC/AER fires, so the crash
 * reason is UNKNOWN but AER registers may still hold error state.
 *
 * Reads config space fresh -- successful reads get current register
 * state, failed reads (offline devices) write 0xFFFFFFFF.
 *
 * Flushes both buffer and pagemap from CPU cache to RAM so data
 * survives kexec into crash kernel.
 */
void pci_crash_save(void)
{
	struct pci_crash_pagemap *pm;
	unsigned long cflags;
	unsigned int num_devs;
	size_t pm_size;
	size_t buf_size;
	void *buffer;

	num_devs = READ_ONCE(pci_crash_num_devs);
	if (num_devs == 0)
		return;

	/* Pairs with smp_wmb() in rebuild -- ensures buffer/pagemap visible */
	smp_rmb();

	buffer = READ_ONCE(pci_crash_buffer);
	buf_size = READ_ONCE(pci_crash_buffer_size);
	pm = READ_ONCE(pci_crash_pagemap);
	pm_size = READ_ONCE(pci_crash_pagemap_size);
	if (!buffer || buf_size == 0)
		return;

	cflags = READ_ONCE(capture_flags);
	if (!(cflags & PCI_CRASH_CAPTURE_ALWAYS)) {
		if (cflags & PCI_CRASH_CAPTURE_AER) {
			struct pci_dev **devs = READ_ONCE(pci_crash_devs);
			unsigned int i;
			bool pci_error_found = false;

			if (!devs)
				return;

			for (i = 0; i < num_devs; i++) {
				struct pci_dev *pdev = devs[i];
				u32 status;

				if (!pdev || !pdev->aer_cap)
					continue;

				if (pci_pcie_type(pdev) == PCI_EXP_TYPE_ROOT_PORT) {
					pci_read_config_dword(pdev,
							     pdev->aer_cap + PCI_ERR_ROOT_STATUS,
							     &status);
					if (status & PCI_ERR_ROOT_UNCOR_RCV) {
						pci_error_found = true;
						break;
					}
				}
			}

			if (!pci_error_found) {
				pr_emerg("no PCI errors detected, skipping capture\n");
				return;
			}
		} else {
			return;
		}
	}

	pci_crash_fill_buffer(buffer, num_devs);

	/*
	 * Flush buffer and pagemap from CPU cache to RAM so the
	 * crash kernel sees our writes after kexec.
	 */
	pci_crash_flush_dcache(buffer, buf_size);

	if (pm && pm_size > 0)
		pci_crash_flush_dcache(pm, pm_size);

	pr_emerg("CAPTURE: %u devices, %zu bytes\n",
		 num_devs, buf_size);
}
EXPORT_SYMBOL_GPL(pci_crash_save);

static void pci_crash_rebuild_worker(struct work_struct *work)
{
	mutex_lock(&pci_crash_lock);
	pci_crash_rebuild_snapshot();
	mutex_unlock(&pci_crash_lock);
}

static int pci_crash_bus_notifier(struct notifier_block *nb,
				  unsigned long action, void *data)
{
	if (action == BUS_NOTIFY_ADD_DEVICE ||
	    action == BUS_NOTIFY_DEL_DEVICE)
		mod_delayed_work(system_wq, &pci_crash_rebuild_dwork,
				 msecs_to_jiffies(PCI_CRASH_REBUILD_DELAY_MS));

	return NOTIFY_OK;
}

static struct notifier_block pci_crash_bus_nb = {
	.notifier_call = pci_crash_bus_notifier,
};

static int __init pci_crash_init(void)
{
	/* Nothing to do in crash kernel -- the buffer from the first kernel
	 * is already in RAM (flushed before kexec) and the parser finds it
	 * via the pagemap in VMCOREINFO.
	 */
	if (is_kdump_kernel())
		return 0;

	INIT_DELAYED_WORK(&pci_crash_rebuild_dwork, pci_crash_rebuild_worker);

	pci_crash_parse_capture();
	pci_crash_parse_devices();

	mutex_lock(&pci_crash_lock);
	pci_crash_rebuild_snapshot();
	mutex_unlock(&pci_crash_lock);

	bus_register_notifier(&pci_bus_type, &pci_crash_bus_nb);

	WRITE_ONCE(pci_crash_ready, true);

	pr_info("ready: %u devices (%zu bytes), capture=%s devices=%s\n",
		pci_crash_num_devs, pci_crash_buffer_size,
		capture, devices);

	return 0;
}
late_initcall(pci_crash_init);

/* Built-in only: crash infrastructure must outlive all drivers. */
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Capture PCI config space at panic time for crash analysis");
MODULE_AUTHOR("Amazon.com, Inc.");
