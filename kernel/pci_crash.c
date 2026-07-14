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
 * Config reads at crash time use pci_bus_read_config_dword_trylock(), which
 * trylocks pci_lock and skips the device on contention.  pci_crash_save() runs
 * from crash_save_vmcoreinfo() inside __crash_kexec(); depending on
 * crash_kexec_post_notifiers, peer CPUs may already be halted (possibly while
 * holding pci_lock) and this CPU may itself hold pci_lock (a panic inside a
 * config access).  pci_lock is a raw, non-reentrant spinlock, so a blocking
 * acquire would deadlock the dump in either case; the trylock skips instead.
 * This guards against the lock deadlock only -- a read to an unreachable device
 * is handled separately by pci_crash_endpoint_reachable().
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
#include <linux/rcupdate.h>

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
	/* Only ARM64 and x86 implemented; Kconfig enforces depends on (ARM64 || X86). */
#ifdef CONFIG_ARM64
	unsigned long start = (unsigned long)addr;
	unsigned long end = start + size;

	dcache_clean_inval_poc(start, end);
#elif defined(CONFIG_X86)
	clflush_cache_range(addr, size);
#else
#error "CONFIG_PCI_CRASH requires ARM64 or X86 dcache flush support (Kconfig depends)"
#endif
}

/*
 * Live capture state is published as a single RCU-managed snapshot so the
 * lockless crash-time reader (pci_crash_save) always observes a consistent
 * {devs, num_devs, buffer, pagemap} set and can never race the rebuild
 * worker freeing the old arrays.  The retired snapshot is reclaimed via
 * call_rcu() once no reader can hold it -- see pci_crash_rebuild_snapshot().
 */
struct pci_crash_snapshot {
	struct pci_dev		**devs;
	unsigned int		num_devs;
	void			*buffer;
	size_t			buffer_size;
	struct pci_crash_pagemap *pagemap;
	size_t			pagemap_size;
	phys_addr_t		pagemap_phys;
	struct rcu_head		rcu;
};

static struct pci_crash_snapshot __rcu *pci_crash_snap;

/*
 * Scalars consumed by crash_core.c's crash_save_vmcoreinfo() right AFTER it
 * calls pci_crash_save().  pci_crash_save() publishes them from the snapshot
 * it captured; on a skipped or failed capture they are set to 0 so no stale
 * pagemap is exported into the vmcore.
 */
void *pci_crash_buffer;
EXPORT_SYMBOL_GPL(pci_crash_buffer);

size_t pci_crash_buffer_size;
EXPORT_SYMBOL_GPL(pci_crash_buffer_size);

phys_addr_t pci_crash_pagemap_phys;
EXPORT_SYMBOL_GPL(pci_crash_pagemap_phys);

/*
 * Set by pci_crash_save() to the snapshot it captured, so its buffer/pagemap
 * (whose addresses are exported into the vmcore via vmcore_info.c) cannot be
 * reclaimed out from under the crash kernel.  pci_crash_save() publishes the
 * buffer scalars and returns; vmcore_info.c then reads them and machine_kexec()
 * snapshots RAM -- all AFTER rcu_read_unlock(), so RCU read-side protection has
 * already ended by the time the buffer matters.  A rebuild racing on a live
 * peer CPU (the default panic path runs __crash_kexec() before halting peers)
 * could otherwise call_rcu()-free this snapshot before kexec.  The free
 * callback below honours this pin and leaks the snapshot instead -- harmless,
 * the system is going down.  Written under rcu_read_lock() before the scalars
 * are published, so the grace period ordering guarantees the callback sees it.
 */
static struct pci_crash_snapshot *pci_crash_captured_snap;

/* Reclaim a retired snapshot after a grace period: drop dev refs + free. */
static void pci_crash_snapshot_free_rcu(struct rcu_head *head)
{
	struct pci_crash_snapshot *s =
		container_of(head, struct pci_crash_snapshot, rcu);
	unsigned int i;

	/*
	 * Pinned by an in-progress crash capture: its buffer address is live in
	 * the vmcore export.  Leak it rather than free memory kexec will read.
	 */
	if (READ_ONCE(pci_crash_captured_snap) == s)
		return;

	for (i = 0; i < s->num_devs; i++)
		if (s->devs && s->devs[i])
			pci_dev_put(s->devs[i]);
	kvfree(s->devs);
	kvfree(s->buffer);
	kfree(s->pagemap);
	kfree(s);
}

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
 *   aer    -- root port ROOT_STATUS has uncorrectable errors
 *   always -- every panic regardless of PCI error state (default)
 *
 * Writable at runtime (0644) so operators and tests can toggle without
 * reboot.  Writes re-parse capture_flags immediately.
 */
#define PCI_CRASH_PARAM_CAPTURE_LEN	32
static char capture[PCI_CRASH_PARAM_CAPTURE_LEN] = "always";

#define PCI_CRASH_CAPTURE_AER		BIT(0)
#define PCI_CRASH_CAPTURE_ALWAYS	BIT(1)
static unsigned long capture_flags = PCI_CRASH_CAPTURE_ALWAYS;

static void pci_crash_parse_capture(void);

static int capture_param_set(const char *val, const struct kernel_param *kp)
{
	char *trimmed;

	if (strlen(val) >= sizeof(capture))
		return -EINVAL;

	/* Serialize against concurrent sysfs writers mutating the string. */
	mutex_lock(&pci_crash_lock);
	strscpy(capture, val, sizeof(capture));
	trimmed = strim(capture);
	if (trimmed != capture)
		memmove(capture, trimmed, strlen(trimmed) + 1);
	if (READ_ONCE(pci_crash_ready))
		pci_crash_parse_capture();
	mutex_unlock(&pci_crash_lock);
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
MODULE_PARM_DESC(capture, "When to capture: aer, always (default: always)");

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
#define PCI_CRASH_PARAM_DEVICES_LEN	256
static char devices[PCI_CRASH_PARAM_DEVICES_LEN] = "all";

static void pci_crash_parse_devices(void);
static struct delayed_work pci_crash_rebuild_dwork;

/* Debounce period for bus notifications (ms).
 * SR-IOV liveupdate can enumerate ~3000 VFs in ~1.5s -- this coalesces
 * the storm into a single rebuild after the last event.
 */
#define PCI_CRASH_REBUILD_DELAY_MS	200

static int devices_param_set(const char *val, const struct kernel_param *kp)
{
	if (strlen(val) >= sizeof(devices))
		return -EINVAL;

	mutex_lock(&pci_crash_lock);
	strscpy(devices, val, sizeof(devices));
	{
		char *trimmed = strim(devices);

		if (trimmed != devices)
			memmove(devices, trimmed, strlen(trimmed) + 1);
	}
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
/* Max distinct class-code filters in a devices= list; 8 covers realistic use. */
#define PCI_CRASH_MAX_DEVICE_CLASSES	8
static unsigned long devices_flags = PCI_CRASH_DEVICES_ALL;
static u16 device_classes[PCI_CRASH_MAX_DEVICE_CLASSES];
static unsigned int device_class_count;

static void pci_crash_parse_capture(void)
{
	char *buf, *token, *rest;
	unsigned long flags = 0;

	if (!*capture) {
		WRITE_ONCE(capture_flags, PCI_CRASH_CAPTURE_ALWAYS);
		return;
	}

	buf = kstrdup(capture, GFP_KERNEL);
	if (!buf) {
		WRITE_ONCE(capture_flags, PCI_CRASH_CAPTURE_ALWAYS);
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
		pr_warn("no valid capture tokens, defaulting to always\n");
		flags = PCI_CRASH_CAPTURE_ALWAYS;
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

/*
 * PCIe extended config space size. Per-device reads are clamped to this in
 * case a device's cfg_size is corrupt at crash time.
 */
#define PCI_CRASH_MAX_CFG_SIZE		4096

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
	unsigned int num_pages = DIV_ROUND_UP(offset_in_page(buf) + buf_size, PAGE_SIZE);
	struct pci_crash_pagemap *pm;
	unsigned int i;

	pm = kmalloc(struct_size(pm, addrs, num_pages), GFP_KERNEL);
	if (!pm)
		return NULL;

	pm->magic = cpu_to_le32(PCI_CRASH_PAGEMAP_MAGIC);
	pm->num_pages = cpu_to_le32(num_pages);
	pm->buf_size = cpu_to_le64(buf_size);
	pm->buf_offset = cpu_to_le32(offset_in_page(buf));

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
 * pci_crash_endpoint_reachable() - Decide if @pdev is safe to read at crash time
 * @pdev: PCI device about to be read
 *
 * A config read to a device whose PCIe link is physically down can, on some
 * architectures (notably arm64), raise a synchronous external abort.  In the
 * crash path that abort is unrecoverable -- the arm64 SEA handler (do_sea())
 * has no kernel-mode fixup and calls arm64_notify_die(), double-faulting the
 * panic and hanging the very dump we are trying to produce.  On x86 such a
 * read returns all-ones harmlessly.  So before touching an endpoint we must
 * establish reachability WITHOUT reading the endpoint itself.
 *
 * Two cheap, panic-safe signals are used, in order:
 *
 *  1. Software state -- pci_dev_is_disconnected(), pci_channel_offline() and
 *     PCI_D3cold are pure flag reads (no MMIO).  They catch devices a
 *     subsystem has already marked gone (hotplug remove, failed AER/DPC
 *     recovery, powered-off).  They are necessary but not sufficient: in the
 *     cascading-failure window this feature targets, a link can be down before
 *     any subsystem has updated error_state, so these flags can still read
 *     "live".
 *
 *  2. Parent-bridge link state -- read PCI_EXP_LNKSTA on the immediate upstream
 *     PCIe port and test Data Link Layer Link Active (DLLLA).  The upstream
 *     port is on-die and always responds, so reading *its* config space cannot
 *     raise an abort caused by the endpoint's dead link.  If DLLLA is clear,
 *     the endpoint is unreachable and is skipped without ever being touched.
 *     The bridge read uses the crash-safe trylock accessor so it cannot hang
 *     on pci_lock either.
 *
 * Returns false if @pdev should be skipped (record filled with 0xFFFFFFFF).
 *
 * Residual: a single immediate-parent check; a link that drops between this
 * check and the read (TOCTOU), or a multi-level fabric collapse where the
 * upstream port itself sits behind a dead link, is not covered here.  See
 * Documentation/PCI/pci-crash-capture.rst for the documented arm64 caveat.
 */
static bool pci_crash_endpoint_reachable(struct pci_dev *pdev)
{
	struct pci_dev *bridge;
	u32 lnksta = 0;
	u16 sta;
	int pos;

	/* Software-state flags: pure reads, no MMIO -- always panic-safe. */
	if (pci_dev_is_disconnected(pdev) || pci_channel_offline(pdev) ||
	    pdev->current_state == PCI_D3cold)
		return false;

	/*
	 * Live link check via the immediate upstream PCIe port.  Only meaningful
	 * for a device sitting below a PCIe downstream/root port; for non-PCIe
	 * or root-complex-integrated devices there is no such link to test, so
	 * treat them as reachable and let the crash-safe accessor handle the read.
	 */
	bridge = pci_upstream_bridge(pdev);
	if (bridge && pci_is_pcie(bridge) &&
	    (pci_pcie_type(bridge) == PCI_EXP_TYPE_ROOT_PORT ||
	     pci_pcie_type(bridge) == PCI_EXP_TYPE_DOWNSTREAM) &&
	    bridge->pcie_cap && bridge->bus) {
		pos = bridge->pcie_cap + PCI_EXP_LNKSTA;
		/*
		 * Read the aligned dword containing LNKSTA from the on-die
		 * upstream port (always present), then extract the 16-bit field.
		 *
		 * Fail closed: if the bridge read cannot complete -- lock
		 * contention (PCIBIOS_SET_FAILED) or any PCIBIOS error -- we
		 * cannot prove the link is up, so treat the endpoint as
		 * unreachable rather than issuing an MMIO read that may raise a
		 * fatal external abort.  Contention is exactly a crash-path
		 * condition we assume is likely, so "unknown" must mean "skip".
		 */
		if (pci_bus_read_config_dword_trylock(bridge->bus, bridge->devfn,
						      pos & ~0x3, &lnksta) != 0)
			return false;

		sta = (pos & 0x2) ? (lnksta >> 16) : (lnksta & 0xffff);
		if (!(sta & PCI_EXP_LNKSTA_DLLLA))
			return false;	/* link down -> endpoint gone */
	}

	return true;
}

/**
 * pci_crash_read_config_space() - Read config space for one device
 * @pdev: PCI device to read
 * @ptr: destination pointer within the crash buffer
 * @cfg_size: number of config bytes to read (already clamped by the caller)
 *
 * Reads @cfg_size bytes one dword at a time.  Devices deemed unreachable by
 * pci_crash_endpoint_reachable() (disconnected, in error recovery, powered
 * off, or behind a down PCIe link) are skipped without being touched: on x86
 * such a read returns all-ones harmlessly, but on other architectures (e.g.
 * arm64) it can raise a fatal external abort that double-faults the panic
 * path.  Skipped or failed reads store 0xFFFFFFFF -- the standard PCI
 * convention for absent/unreachable registers.
 */
static void pci_crash_read_config_space(struct pci_dev *pdev, u8 *ptr,
					unsigned int cfg_size)
{
	struct pci_crash_device_record *record =
		(struct pci_crash_device_record *)ptr;
	u8 *cfg_data = ptr + PCI_CRASH_RECORD_META;
	unsigned int offset;
	u32 val;

	/* Defensive: never trust cfg_size, never deref a torn-down bus. */
	if (cfg_size > PCI_CRASH_MAX_CFG_SIZE)
		cfg_size = PCI_CRASH_MAX_CFG_SIZE;

	if (!pdev->bus) {
		record->domain = 0;
		record->bus = 0;
		record->devfn = pdev->devfn;
		record->config_size = cpu_to_le32(cfg_size);
		memset(cfg_data, 0xff, cfg_size);
		return;
	}

	record->domain = cpu_to_le16(pci_domain_nr(pdev->bus));
	record->bus = pdev->bus->number;
	record->devfn = pdev->devfn;
	record->config_size = cpu_to_le32(cfg_size);

	if (!pci_crash_endpoint_reachable(pdev)) {
		memset(cfg_data, 0xff, cfg_size);
		return;
	}

	for (offset = 0; offset < cfg_size; offset += 4) {
		if (pci_bus_read_config_dword_trylock(pdev->bus, pdev->devfn,
						      offset, &val)) {
			put_unaligned_le32(0xFFFFFFFF, &cfg_data[offset]);
			continue;
		}
		put_unaligned_le32(val, &cfg_data[offset]);
	}
}

/**
 * pci_crash_fill_buffer() - Populate buffer with config space
 * @s: snapshot whose buffer is filled from its captured device list
 *
 * Records are variable-length: each is PCI_CRASH_RECORD_META +
 * pdev->cfg_size bytes. The header's config_size is 0 to indicate
 * variable-length; parsers walk records using per-record config_size.
 *
 * Uses ktime_get_real_fast_ns() for the timestamp -- safe in NMI/panic
 * context (lockless, reads the NMI-safe timekeeper snapshot).
 *
 * Caller holds rcu_read_lock() so @s stays valid for the whole fill.
 */
static void pci_crash_fill_buffer(struct pci_crash_snapshot *s)
{
	struct pci_crash_buffer_header *header = s->buffer;
	struct pci_dev **devs = s->devs;
	u8 *ptr, *end;
	unsigned int i;

	header->magic = cpu_to_le32(PCI_CRASH_MAGIC);
	header->version = cpu_to_le32(PCI_CRASH_VERSION);
	header->device_count = cpu_to_le32(s->num_devs);
	header->config_size = 0;
	header->timestamp = cpu_to_le64(ktime_get_real_fast_ns());
	header->flags = 0;
	header->reserved = 0;

	ptr = (u8 *)s->buffer + PCI_CRASH_HEADER_SIZE;
	end = (u8 *)s->buffer + s->buffer_size;
	for (i = 0; i < s->num_devs; i++) {
		struct pci_dev *pdev = devs[i];
		unsigned int cfg_size;
		size_t rec_size;

		if (unlikely(!pdev))
			break;

		cfg_size = pdev->cfg_size;
		if (cfg_size > PCI_CRASH_MAX_CFG_SIZE)
			cfg_size = PCI_CRASH_MAX_CFG_SIZE;
		rec_size = PCI_CRASH_RECORD_META + cfg_size;

		/*
		 * Never write past the buffer if the device set or a device's
		 * cfg_size grew since the buffer was sized at rebuild time.
		 */
		if (unlikely(ptr + rec_size > end))
			break;

		pci_crash_read_config_space(pdev, ptr, cfg_size);
		ptr += rec_size;
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
 * events on large accelerator hosts during liveupdate).
 *
 * After allocation, builds the pagemap so the crash parser can
 * locate the buffer's physical pages in the vmcore.
 *
 * Publishes the new snapshot via rcu_assign_pointer() and retires the
 * previous one via call_rcu(), so the lockless crash-time reader never
 * sees a half-updated state or a freed array.
 *
 * Caller must hold pci_crash_lock.
 */
static void pci_crash_rebuild_snapshot(void)
{
	struct pci_crash_snapshot *old, *new;
	struct pci_dev *pdev = NULL;
	unsigned int count = 0, i;
	size_t total_size;

	old = rcu_dereference_protected(pci_crash_snap,
					lockdep_is_held(&pci_crash_lock));

	/* Pass 1: count devices (upper bound). */
	for_each_pci_dev(pdev)
		count++;

	new = kzalloc(sizeof(*new), GFP_KERNEL);
	if (!new) {
		pr_warn_ratelimited("snapshot alloc failed; keeping previous (capture may be stale)\n");
		return;
	}

	if (count == 0) {
		pr_info("no PCI devices found\n");
		goto publish;	/* publish an empty snapshot */
	}

	new->devs = kvmalloc_array(count, sizeof(*new->devs),
				   GFP_KERNEL | __GFP_ZERO);
	if (!new->devs) {
		kfree(new);
		pr_warn_ratelimited("devs alloc failed; keeping previous (capture may be stale)\n");
		return;
	}

	/*
	 * Pass 2: populate filtered device array and compute the exact
	 * buffer size.  count (pass 1) is an upper bound; actual may be less.
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
		new->devs[i] = pci_dev_get(pdev);
		total_size += PCI_CRASH_RECORD_META + pdev->cfg_size;
		i++;
	}
	new->num_devs = i;

	if (new->num_devs == 0) {
		/* Publish empty: releases the previous (now stale) device set. */
		kvfree(new->devs);
		new->devs = NULL;
		pr_info("no devices match devices=%s\n", devices);
		goto publish;
	}

	if (total_size > PCI_CRASH_MAX_BUFFER_SIZE) {
		pr_warn("buffer too large (%zu > %d bytes)\n",
			total_size, PCI_CRASH_MAX_BUFFER_SIZE);
		goto err_free_devs;
	}

	new->buffer = kvmalloc(total_size, GFP_KERNEL | __GFP_ZERO);
	if (!new->buffer)
		goto err_free_devs;
	new->buffer_size = total_size;

	new->pagemap = pci_crash_build_pagemap(new->buffer, total_size);
	if (!new->pagemap)
		goto err_free_buf;
	new->pagemap_size = struct_size(new->pagemap, addrs,
					le32_to_cpu(new->pagemap->num_pages));
	new->pagemap_phys = virt_to_phys(new->pagemap);

	pr_info("rebuild: %u devices (%zu bytes, %u pages)\n",
		new->num_devs, total_size,
		le32_to_cpu(new->pagemap->num_pages));

publish:
	/*
	 * Publish the new snapshot and retire the old one.  Readers in
	 * pci_crash_save() hold rcu_read_lock(), so call_rcu() defers the old
	 * snapshot's frees (array, buffer, pagemap, dev refs) until no
	 * crash-time reader can still hold it -- closing the UAF/OOB window.
	 */
	rcu_assign_pointer(pci_crash_snap, new);
	if (old)
		call_rcu(&old->rcu, pci_crash_snapshot_free_rcu);
	return;

err_free_buf:
	kvfree(new->buffer);
err_free_devs:
	for (i = 0; i < new->num_devs; i++)
		pci_dev_put(new->devs[i]);
	kvfree(new->devs);
	kfree(new);
	/*
	 * Allocation failed building the new snapshot.  Keep the existing
	 * snapshot live (do not publish) so capture still works with the
	 * prior device set; warn (ratelimited) so persistent failures show.
	 */
	pr_warn_ratelimited("rebuild failed; keeping previous snapshot (capture may be stale)\n");
}

#ifdef CONFIG_PCIEAER
/*
 * Quick-scan root ports for a received uncorrectable AER error -- the signal
 * that this panic is PCI-related and worth capturing.  Returns true on the
 * first root port whose ROOT_STATUS reports an uncorrectable error.
 */
static bool pci_crash_aer_error_present(struct pci_crash_snapshot *s)
{
	unsigned int i;

	for (i = 0; i < s->num_devs; i++) {
		struct pci_dev *pdev = s->devs[i];
		u32 status = 0;

		if (!pdev || !pdev->aer_cap)
			continue;
		if (pci_pcie_type(pdev) != PCI_EXP_TYPE_ROOT_PORT)
			continue;
		/*
		 * Same reachability gate as the capture path.  A root port is
		 * on-die (no upstream bridge), so this reduces to the software-
		 * state flags -- we read the port's own AER registers, never an
		 * endpoint behind a potentially-dead link.
		 */
		if (!pci_crash_endpoint_reachable(pdev))
			continue;

		/*
		 * Fail closed, like the reachability check: a failed read
		 * (lock contention or PCIBIOS error) sets status to ~0, which
		 * would falsely test as "uncorrectable error present" and force
		 * a pointless full capture.  Unknown means "no error seen".
		 */
		if (pci_bus_read_config_dword_trylock(pdev->bus, pdev->devfn,
						      pdev->aer_cap + PCI_ERR_ROOT_STATUS,
						      &status) != 0)
			continue;
		if (status & PCI_ERR_ROOT_UNCOR_RCV)
			return true;
	}
	return false;
}
#else
static inline bool pci_crash_aer_error_present(struct pci_crash_snapshot *s)
{
	return false;
}
#endif

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
 *
 * Runs under rcu_read_lock(): a rebuild worker may still be mid-flight on a
 * peer CPU, so RCU keeps the sampled snapshot alive for the whole capture.
 */
void pci_crash_save(void)
{
	struct pci_crash_snapshot *s;
	unsigned long cflags;

	/* Cleared first; set only on a successful capture below. */
	pci_crash_buffer = NULL;
	pci_crash_buffer_size = 0;
	pci_crash_pagemap_phys = 0;

	rcu_read_lock();
	s = rcu_dereference(pci_crash_snap);
	if (!s || s->num_devs == 0)
		goto out;
	if (!s->buffer || s->buffer_size == 0)
		goto out;

	/*
	 * Pin this snapshot so a rebuild racing on a live peer CPU cannot
	 * call_rcu()-free its buffer/pagemap before machine_kexec() snapshots
	 * RAM.  The scalars below are read by vmcore_info.c AFTER we return and
	 * rcu_read_unlock() -- i.e. after RCU read-side protection has ended --
	 * so RCU alone does not keep the buffer alive that long.  Written here,
	 * under rcu_read_lock() and before the scalars are published; the free
	 * callback honours it (see pci_crash_snapshot_free_rcu).
	 */
	WRITE_ONCE(pci_crash_captured_snap, s);

	/*
	 * Publish the buffer location now -- before the AER quick-scan that may
	 * skip the capture -- so vmcore_info.c always exports a valid (possibly
	 * empty) buffer.  vmcore_info.c reads only these scalars, immediately
	 * after we return and still inside __crash_kexec() before
	 * machine_kexec().
	 */
	pci_crash_buffer = s->buffer;
	pci_crash_buffer_size = s->buffer_size;
	pci_crash_pagemap_phys = s->pagemap_phys;

	cflags = READ_ONCE(capture_flags);
	if (!(cflags & PCI_CRASH_CAPTURE_ALWAYS)) {
		if (!(cflags & PCI_CRASH_CAPTURE_AER)) {
			/* Neither 'always' nor a usable 'aer' mode -- skip. */
			goto out;
		}
		if (!pci_crash_aer_error_present(s)) {
			pr_info("no PCI errors detected, skipping capture\n");
			goto out;
		}
	}

	pci_crash_fill_buffer(s);

	/*
	 * Flush buffer and pagemap from CPU cache to RAM so the
	 * crash kernel sees our writes after kexec.
	 */
	pci_crash_flush_dcache(s->buffer, s->buffer_size);
	if (s->pagemap && s->pagemap_size > 0)
		pci_crash_flush_dcache(s->pagemap, s->pagemap_size);

	pr_info("CAPTURE: %u devices, %zu bytes\n",
		s->num_devs, s->buffer_size);
out:
	rcu_read_unlock();
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
	/*
	 * The on-wire buffer/pagemap layout is shared with userspace vmcore
	 * parsers, which hardcode these sizes.  Catch any struct drift at
	 * build time.
	 */
	BUILD_BUG_ON(sizeof(struct pci_crash_buffer_header) != 32);
	BUILD_BUG_ON(sizeof(struct pci_crash_device_record) != 8);
	BUILD_BUG_ON(offsetof(struct pci_crash_pagemap, addrs) != 20);

	/* Nothing to do in crash kernel -- the buffer from the first kernel
	 * is already in RAM (flushed before kexec) and the parser finds it
	 * via the pagemap in VMCOREINFO.
	 */
	if (is_kdump_kernel())
		return 0;

	INIT_DELAYED_WORK(&pci_crash_rebuild_dwork, pci_crash_rebuild_worker);

	pci_crash_parse_capture();
	pci_crash_parse_devices();

	/*
	 * Register the hotplug notifier BEFORE the initial snapshot so no
	 * ADD/DEL event in the startup window is missed.  The notifier only
	 * schedules the debounced rebuild worker, which serializes on
	 * pci_crash_lock behind this initial rebuild.
	 */
	bus_register_notifier(&pci_bus_type, &pci_crash_bus_nb);

	mutex_lock(&pci_crash_lock);
	pci_crash_rebuild_snapshot();
	mutex_unlock(&pci_crash_lock);

	WRITE_ONCE(pci_crash_ready, true);

#ifndef CONFIG_PCIEAER
	if ((capture_flags & PCI_CRASH_CAPTURE_AER) &&
	    !(capture_flags & PCI_CRASH_CAPTURE_ALWAYS))
		pr_warn("capture=aer but CONFIG_PCIEAER=n; capture will not trigger unless set to 'always'\n");
#endif

	rcu_read_lock();
	{
		struct pci_crash_snapshot *s = rcu_dereference(pci_crash_snap);

		pr_info("ready: %u devices (%zu bytes), capture=%s devices=%s\n",
			s ? s->num_devs : 0, s ? s->buffer_size : 0,
			capture, devices);
	}
	rcu_read_unlock();

	return 0;
}
late_initcall(pci_crash_init);

/* Built-in only: crash infrastructure must outlive all drivers. */
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Capture PCI config space at panic time for crash analysis");
MODULE_AUTHOR("Amazon.com, Inc.");
