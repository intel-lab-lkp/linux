// SPDX-License-Identifier: GPL-2.0-only
/*
 * Firmware-first RAS: Generic Error Status Core
 *
 * Copyright (C) 2025 ARM Ltd.
 * Author: Ahmed Tiba <ahmed.tiba@arm.com>
 */

#include <linux/kernel.h>
#include <linux/cper.h>
#include <linux/ratelimit.h>
#include <linux/vmalloc.h>
#include <linux/llist.h>
#include <linux/genalloc.h>
#include <linux/pci.h>
#include <linux/pfn.h>
#include <linux/aer.h>
#include <linux/nmi.h>
#include <linux/sched/clock.h>
#include <linux/sched/signal.h>
#include <linux/uuid.h>
#include <linux/kconfig.h>
#include <linux/ras.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/workqueue.h>
#include <linux/task_work.h>
#include <ras/ras_event.h>

#include <linux/estatus.h>
#include <asm/fixmap.h>

void estatus_pool_region_free(unsigned long addr, u32 size);

static void estatus_log_hw_error(char level, const char *seq_tag,
				 const char *name)
{
	switch (level) {
	case '0':
		pr_emerg("%sHardware error from %s\n", seq_tag, name);
		break;
	case '1':
		pr_alert("%sHardware error from %s\n", seq_tag, name);
		break;
	case '2':
		pr_crit("%sHardware error from %s\n", seq_tag, name);
		break;
	case '3':
		pr_err("%sHardware error from %s\n", seq_tag, name);
		break;
	case '4':
		pr_warn("%sHardware error from %s\n", seq_tag, name);
		break;
	case '5':
		pr_notice("%sHardware error from %s\n", seq_tag, name);
		break;
	case '6':
		pr_info("%sHardware error from %s\n", seq_tag, name);
		break;
	default:
		pr_debug("%sHardware error from %s\n", seq_tag, name);
		break;
	}
}

static inline u32 estatus_len(struct acpi_hest_generic_status *estatus)
{
	if (estatus->raw_data_length)
		return estatus->raw_data_offset + estatus->raw_data_length;

	return sizeof(*estatus) + estatus->data_length;
}

#define ESTATUS_PFX	"ESTATUS: "

#define ESTATUS_ESOURCE_PREALLOC_MAX_SIZE_SIZE	65536

#define ESTATUS_POOL_MIN_ALLOC_ORDER 3

/* This is just an estimation for memory pool allocation */
#define ESTATUS_CACHE_AVG_SIZE	512

#define ESTATUS_CACHES_SIZE	4

#define ESTATUS_IN_CACHE_MAX_NSEC	10000000000ULL
/* Prevent too many caches are allocated because of RCU */
#define ESTATUS_CACHE_ALLOCED_MAX	(ESTATUS_CACHES_SIZE * 3 / 2)

#define ESTATUS_CACHE_LEN(estatus_len)			\
	(sizeof(struct estatus_cache) + (estatus_len))
#define ESTATUS_FROM_CACHE(cache)			\
	((struct acpi_hest_generic_status *)		\
	 ((struct estatus_cache *)(cache) + 1))

#define ESTATUS_NODE_LEN(estatus_len)			\
	(sizeof(struct estatus_node) + (estatus_len))
#define ESTATUS_FROM_NODE(node)				\
	((struct acpi_hest_generic_status *)		\
	 ((struct estatus_node *)(node) + 1))

#define ESTATUS_VENDOR_ENTRY_LEN(gdata_len)		\
	(sizeof(struct estatus_vendor_record_entry) + (gdata_len))
#define ESTATUS_GDATA_FROM_VENDOR_ENTRY(vendor_entry)	\
	((struct acpi_hest_generic_data *)		\
	((struct estatus_vendor_record_entry *)(vendor_entry) + 1))

static ATOMIC_NOTIFIER_HEAD(estatus_report_chain);

struct estatus_vendor_record_entry {
	struct work_struct work;
	int error_severity;
	char vendor_record[];
};

static struct estatus_cache __rcu *estatus_caches[ESTATUS_CACHES_SIZE];
static atomic_t estatus_cache_alloced;

static int estatus_panic_timeout __read_mostly = 30;

static struct gen_pool *estatus_pool;
static DEFINE_MUTEX(estatus_pool_mutex);

static enum fixed_addresses estatus_source_fixmap(struct estatus_source *source)
{
	if (WARN_ON_ONCE(!source->fixmap_idx))
		return FIX_HOLE;

	return source->fixmap_idx;
}

static inline const char *estatus_source_name(struct estatus_source *source)
{
	if (source->ops && source->ops->get_name)
		return source->ops->get_name(source);

	return "unknown";
}

static inline size_t estatus_source_max_len(struct estatus_source *source)
{
	if (source->ops && source->ops->get_max_len)
		return source->ops->get_max_len(source);

	return 0;
}

static inline enum estatus_notify_mode
estatus_source_notify_mode(struct estatus_source *source)
{
	if (source->ops && source->ops->get_notify_mode)
		return source->ops->get_notify_mode(source);

	return ESTATUS_NOTIFY_ASYNC;
}

static inline int estatus_source_get_phys(struct estatus_source *source,
					  phys_addr_t *addr)
{
	if (!source->ops || !source->ops->get_phys)
		return -EOPNOTSUPP;

	return source->ops->get_phys(source, addr);
}

static inline int estatus_source_read(struct estatus_source *source,
				      phys_addr_t addr, void *buf, size_t len,
				      enum fixed_addresses fixmap_idx)
{
	if (!source->ops || !source->ops->read)
		return -EOPNOTSUPP;

	return source->ops->read(source, addr, buf, len, fixmap_idx);
}

static inline int estatus_source_write(struct estatus_source *source,
				       phys_addr_t addr, const void *buf,
				       size_t len,
				       enum fixed_addresses fixmap_idx)
{
	if (!source->ops || !source->ops->write)
		return -EOPNOTSUPP;

	return source->ops->write(source, addr, buf, len, fixmap_idx);
}

static inline void estatus_source_ack(struct estatus_source *source)
{
	if (source->ops && source->ops->ack)
		source->ops->ack(source);
}

int estatus_pool_init(unsigned int num_ghes)
{
	unsigned long addr, len;
	int rc = 0;

	mutex_lock(&estatus_pool_mutex);
	if (estatus_pool)
		goto out_unlock;

	estatus_pool = gen_pool_create(ESTATUS_POOL_MIN_ALLOC_ORDER, -1);
	if (!estatus_pool) {
		rc = -ENOMEM;
		goto out_unlock;
	}

	if (!num_ghes)
		num_ghes = 1;

	len = ESTATUS_CACHE_AVG_SIZE * ESTATUS_CACHE_ALLOCED_MAX;
	len += (num_ghes * ESTATUS_ESOURCE_PREALLOC_MAX_SIZE_SIZE);

	addr = (unsigned long)vmalloc(PAGE_ALIGN(len));
	if (!addr) {
		rc = -ENOMEM;
		goto err_pool_alloc;
	}

	rc = gen_pool_add(estatus_pool, addr, PAGE_ALIGN(len), -1);
	if (rc)
		goto err_pool_add;

out_unlock:
	mutex_unlock(&estatus_pool_mutex);
	return rc;

err_pool_add:
	vfree((void *)addr);
err_pool_alloc:
	gen_pool_destroy(estatus_pool);
	estatus_pool = NULL;
	goto out_unlock;
}

/**
 * estatus_pool_region_free - free previously allocated memory
 *				   from the estatus_pool.
 * @addr: address of memory to free.
 * @size: size of memory to free.
 *
 * Returns none.
 */
void estatus_pool_region_free(unsigned long addr, u32 size)
{
	gen_pool_free(estatus_pool, addr, size);
}
EXPORT_SYMBOL_GPL(estatus_pool_region_free);

/* Check the top-level record header has an appropriate size. */
static int __estatus_check_estatus(struct estatus_source *source,
				   struct acpi_hest_generic_status *estatus)
{
	u32 len = estatus_len(estatus);
	size_t max_len = estatus_source_max_len(source);

	if (len < sizeof(*estatus)) {
		pr_warn_ratelimited(FW_WARN ESTATUS_PFX "Truncated error status block!\n");
		return -EIO;
	}

	if (max_len && len > max_len) {
		pr_warn_ratelimited(FW_WARN ESTATUS_PFX "Invalid error status block length!\n");
		return -EIO;
	}

	if (cper_estatus_check_header(estatus)) {
		pr_warn_ratelimited(FW_WARN ESTATUS_PFX "Invalid CPER header!\n");
		return -EIO;
	}

	return 0;
}

/* Read the CPER block, returning its address, and header in estatus. */
static int __estatus_peek_estatus(struct estatus_source *source,
				  struct acpi_hest_generic_status *estatus,
				  phys_addr_t *buf_paddr,
				  enum fixed_addresses fixmap_idx)
{
	int rc;

	rc = estatus_source_get_phys(source, buf_paddr);
	if (rc) {
		*buf_paddr = 0;
		pr_warn_ratelimited(FW_WARN ESTATUS_PFX
				    "Failed to get error status block address for provider %s: %d\n",
				    estatus_source_name(source), rc);
		return rc;
	}

	if (!*buf_paddr)
		return -ENOENT;

	rc = estatus_source_read(source, *buf_paddr, estatus,
				 sizeof(*estatus), fixmap_idx);
	if (rc)
		return rc;

	if (!estatus->block_status) {
		*buf_paddr = 0;
		return -ENOENT;
	}

	return 0;
}

static int __estatus_read_estatus(struct estatus_source *source,
				  struct acpi_hest_generic_status *estatus,
				  phys_addr_t buf_paddr,
				  enum fixed_addresses fixmap_idx,
				  size_t buf_len)
{
	int rc;

	rc = estatus_source_read(source, buf_paddr, estatus, buf_len,
				 fixmap_idx);
	if (rc)
		return rc;

	if (cper_estatus_check(estatus)) {
		pr_warn_ratelimited(FW_WARN ESTATUS_PFX
				    "Failed to read error status block for provider %s!\n",
				    estatus_source_name(source));
		return -EIO;
	}

	return 0;
}

static int estatus_read_estatus(struct estatus_source *source,
				struct acpi_hest_generic_status *estatus,
				phys_addr_t *buf_paddr,
				enum fixed_addresses fixmap_idx)
{
	int rc;

	rc = __estatus_peek_estatus(source, estatus, buf_paddr, fixmap_idx);
	if (rc)
		return rc;

	rc = __estatus_check_estatus(source, estatus);
	if (rc)
		return rc;

	return __estatus_read_estatus(source, estatus, *buf_paddr,
				      fixmap_idx, estatus_len(estatus));
}

static void estatus_clear_estatus(struct estatus_source *source,
				  struct acpi_hest_generic_status *estatus,
				  phys_addr_t buf_paddr,
				  enum fixed_addresses fixmap_idx)
{
	int rc;

	estatus->block_status = 0;

	if (!buf_paddr)
		return;

	rc = estatus_source_write(source, buf_paddr, estatus,
				  sizeof(estatus->block_status), fixmap_idx);
	if (rc)
		pr_warn_ratelimited(FW_WARN ESTATUS_PFX
				    "Failed to clear error status block for provider %s: %d\n",
				    estatus_source_name(source), rc);

	estatus_source_ack(source);
}

static inline int estatus_severity(int severity)
{
	switch (severity) {
	case CPER_SEV_INFORMATIONAL:
		return ESTATUS_SEV_NO;
	case CPER_SEV_CORRECTED:
		return ESTATUS_SEV_CORRECTED;
	case CPER_SEV_RECOVERABLE:
		return ESTATUS_SEV_RECOVERABLE;
	case CPER_SEV_FATAL:
		return ESTATUS_SEV_PANIC;
	default:
		/* Unknown, go panic */
		return ESTATUS_SEV_PANIC;
	}
}

static void __estatus_print_estatus(const char *pfx,
				    struct estatus_source *source,
				    const struct acpi_hest_generic_status *estatus)
{
	static atomic_t seqno;
	unsigned int curr_seqno;
	char pfx_seq[64];
	char seq_tag[64];
	const char *name = estatus_source_name(source);
	const char *level = pfx;
	char level_char = '4';

	if (!level) {
		if (estatus_severity(estatus->error_severity) <=
		    ESTATUS_SEV_CORRECTED)
			level = KERN_WARNING;
		else
			level = KERN_ERR;
	}

	if (level[0] == KERN_SOH_ASCII && level[1])
		level_char = level[1];
	else if (estatus_severity(estatus->error_severity) > ESTATUS_SEV_CORRECTED)
		level_char = '3';

	curr_seqno = atomic_inc_return(&seqno);
	snprintf(seq_tag, sizeof(seq_tag), "{%u}" HW_ERR, curr_seqno);
	snprintf(pfx_seq, sizeof(pfx_seq), "%s%s", level, seq_tag);
	estatus_log_hw_error(level_char, seq_tag, name);
	cper_estatus_print(pfx_seq, estatus);
}

static int estatus_print_estatus(const char *pfx,
				 struct estatus_source *source,
				 const struct acpi_hest_generic_status *estatus)
{
	/* Not more than 2 messages every 5 seconds */
	static DEFINE_RATELIMIT_STATE(ratelimit_corrected, 5 * HZ, 2);
	static DEFINE_RATELIMIT_STATE(ratelimit_uncorrected, 5 * HZ, 2);
	struct ratelimit_state *ratelimit;

	if (estatus_severity(estatus->error_severity) <= ESTATUS_SEV_CORRECTED)
		ratelimit = &ratelimit_corrected;
	else
		ratelimit = &ratelimit_uncorrected;
	if (__ratelimit(ratelimit)) {
		__estatus_print_estatus(pfx, source, estatus);
		return 1;
	}
	return 0;
}

/*
 * GHES error status reporting throttle, to report more kinds of
 * errors, instead of just most frequently occurred errors.
 */
static int estatus_cached(struct acpi_hest_generic_status *estatus)
{
	u32 len;
	int i, cached = 0;
	unsigned long long now;
	struct estatus_cache *cache;
	struct acpi_hest_generic_status *cache_estatus;

	len = estatus_len(estatus);
	rcu_read_lock();
	for (i = 0; i < ESTATUS_CACHES_SIZE; i++) {
		cache = rcu_dereference(estatus_caches[i]);
		if (!cache)
			continue;
		if (len != cache->estatus_len)
			continue;
		cache_estatus = ESTATUS_FROM_CACHE(cache);
		if (memcmp(estatus, cache_estatus, len))
			continue;
		atomic_inc(&cache->count);
		now = sched_clock();
		if (now - cache->time_in < ESTATUS_IN_CACHE_MAX_NSEC)
			cached = 1;
		break;
	}
	rcu_read_unlock();
	return cached;
}

static struct estatus_cache *estatus_cache_alloc(struct estatus_source *source,
						 struct acpi_hest_generic_status *estatus)
{
	int alloced;
	u32 len, cache_len;
	struct estatus_cache *cache;
	struct acpi_hest_generic_status *cache_estatus;

	alloced = atomic_add_return(1, &estatus_cache_alloced);
	if (alloced > ESTATUS_CACHE_ALLOCED_MAX) {
		atomic_dec(&estatus_cache_alloced);
		return NULL;
	}
	len = estatus_len(estatus);
	cache_len = ESTATUS_CACHE_LEN(len);
	cache = (void *)gen_pool_alloc(estatus_pool, cache_len);
	if (!cache) {
		atomic_dec(&estatus_cache_alloced);
		return NULL;
	}
	cache_estatus = ESTATUS_FROM_CACHE(cache);
	memcpy(cache_estatus, estatus, len);
	cache->estatus_len = len;
	atomic_set(&cache->count, 0);
	cache->source = source;
	cache->time_in = sched_clock();
	return cache;
}

static void estatus_cache_rcu_free(struct rcu_head *head)
{
	struct estatus_cache *cache;
	u32 len;

	cache = container_of(head, struct estatus_cache, rcu);
	len = estatus_len(ESTATUS_FROM_CACHE(cache));
	len = ESTATUS_CACHE_LEN(len);
	gen_pool_free(estatus_pool, (unsigned long)cache, len);
	atomic_dec(&estatus_cache_alloced);
}

static void estatus_cache_add(struct estatus_source *source,
			      struct acpi_hest_generic_status *estatus)
{
	unsigned long long now, duration, period, max_period = 0;
	struct estatus_cache *cache, *new_cache;
	struct estatus_cache __rcu *victim;
	int i, slot = -1, count;

	new_cache = estatus_cache_alloc(source, estatus);
	if (!new_cache)
		return;

	rcu_read_lock();
	now = sched_clock();
	for (i = 0; i < ESTATUS_CACHES_SIZE; i++) {
		cache = rcu_dereference(estatus_caches[i]);
		if (!cache) {
			slot = i;
			break;
		}
		duration = now - cache->time_in;
		if (duration >= ESTATUS_IN_CACHE_MAX_NSEC) {
			slot = i;
			break;
		}
		count = atomic_read(&cache->count);
		period = duration;
		do_div(period, (count + 1));
		if (period > max_period) {
			max_period = period;
			slot = i;
		}
	}
	rcu_read_unlock();

	if (slot != -1) {
		/*
		 * Use release semantics to ensure that estatus_cached()
		 * running on another CPU will see the updated cache fields if
		 * it can see the new value of the pointer.
		 */
		victim = xchg_release(&estatus_caches[slot],
				      RCU_INITIALIZER(new_cache));

		/*
		 * At this point, victim may point to a cached item different
		 * from the one based on which we selected the slot. Instead of
		 * going to the loop again to pick another slot, let's just
		 * drop the other item anyway: this may cause a false cache
		 * miss later on, but that won't cause any problems.
		 */
		if (victim)
			call_rcu(&unrcu_pointer(victim)->rcu,
				 estatus_cache_rcu_free);
	}
}

struct estatus_task_work {
	struct callback_head twork;
	u64 pfn;
	int flags;
};

static void estatus_memory_failure_cb(struct callback_head *twork)
{
	struct estatus_task_work *twcb = container_of(twork, struct estatus_task_work, twork);
	int ret;

	ret = memory_failure(twcb->pfn, twcb->flags);
	gen_pool_free(estatus_pool, (unsigned long)twcb, sizeof(*twcb));

	if (!ret || ret == -EHWPOISON || ret == -EOPNOTSUPP)
		return;

	pr_err(HW_ERR ESTATUS_PFX
	       "%#llx: Sending SIGBUS to %s:%d due to hardware memory corruption\n",
	       twcb->pfn, current->comm, task_pid_nr(current));
	force_sig(SIGBUS);
}

static bool estatus_do_memory_failure(u64 physical_addr, int flags)
{
	struct estatus_task_work *twcb;
	unsigned long pfn;

	if (!IS_ENABLED(CONFIG_ACPI_APEI_MEMORY_FAILURE))
		return false;

	pfn = PHYS_PFN(physical_addr);
	if (!pfn_valid(pfn) && !arch_is_platform_page(physical_addr)) {
		pr_warn_ratelimited(FW_WARN ESTATUS_PFX
		"Invalid address in generic error data: %#llx\n",
		physical_addr);
		return false;
	}

	if (flags == MF_ACTION_REQUIRED && current->mm) {
		twcb = (void *)gen_pool_alloc(estatus_pool, sizeof(*twcb));
		if (!twcb)
			return false;

		twcb->pfn = pfn;
		twcb->flags = flags;
		init_task_work(&twcb->twork, estatus_memory_failure_cb);
		task_work_add(current, &twcb->twork, TWA_RESUME);
		return true;
	}

	memory_failure_queue(pfn, flags);
	return true;
}

static bool estatus_handle_memory_failure(estatus_generic_data *gdata, int sev, bool sync)
{
	int flags = -1;
	int sec_sev = estatus_severity(gdata->error_severity);
	struct cper_sec_mem_err *mem_err = estatus_get_payload(gdata);

	if (!(mem_err->validation_bits & CPER_MEM_VALID_PA))
		return false;

	/* iff following two events can be handled properly by now */
	if (sec_sev == ESTATUS_SEV_CORRECTED &&
	    (gdata->flags & CPER_SEC_ERROR_THRESHOLD_EXCEEDED))
		flags = MF_SOFT_OFFLINE;
	if (sev == ESTATUS_SEV_RECOVERABLE && sec_sev == ESTATUS_SEV_RECOVERABLE)
		flags = sync ? MF_ACTION_REQUIRED : 0;

	if (flags != -1)
		return estatus_do_memory_failure(mem_err->physical_addr, flags);

	return false;
}

static bool estatus_handle_arm_hw_error(estatus_generic_data *gdata, int sev, bool sync)
{
	struct cper_sec_proc_arm *err = estatus_get_payload(gdata);
	int flags = sync ? MF_ACTION_REQUIRED : 0;
	bool queued = false;
	int sec_sev, i;
	char *p;

	log_arm_hw_error(err);

	sec_sev = estatus_severity(gdata->error_severity);
	if (sev != ESTATUS_SEV_RECOVERABLE || sec_sev != ESTATUS_SEV_RECOVERABLE)
		return false;

	p = (char *)(err + 1);
	for (i = 0; i < err->err_info_num; i++) {
		struct cper_arm_err_info *err_info = (struct cper_arm_err_info *)p;
		bool is_cache = (err_info->type == CPER_ARM_CACHE_ERROR);
		bool has_pa = (err_info->validation_bits & CPER_ARM_INFO_VALID_PHYSICAL_ADDR);
		const char *error_type = "unknown error";

		/*
		 * The field (err_info->error_info & BIT(26)) is fixed to set to
		 * 1 in some old firmware of HiSilicon Kunpeng920. We assume that
		 * firmware won't mix corrected errors in an uncorrected section,
		 * and don't filter out 'corrected' error here.
		 */
		if (is_cache && has_pa) {
			queued = estatus_do_memory_failure(err_info->physical_fault_addr, flags);
			p += err_info->length;
			continue;
		}

		if (err_info->type < ARRAY_SIZE(cper_proc_error_type_strs))
			error_type = cper_proc_error_type_strs[err_info->type];

		pr_warn_ratelimited(FW_WARN ESTATUS_PFX
				    "Unhandled processor error type: %s\n",
				    error_type);
		p += err_info->length;
	}

	return queued;
}

/*
 * PCIe AER errors need to be sent to the AER driver for reporting and
 * recovery. The ESTATUS severities map to the following AER severities and
 * require the following handling:
 *
 * ESTATUS_SEV_CORRECTABLE -> AER_CORRECTABLE
 *     These need to be reported by the AER driver but no recovery is
 *     necessary.
 * ESTATUS_SEV_RECOVERABLE -> AER_NONFATAL
 * ESTATUS_SEV_RECOVERABLE && CPER_SEC_RESET -> AER_FATAL
 *     These both need to be reported and recovered from by the AER driver.
 * ESTATUS_SEV_PANIC does not make it to this handling since the kernel must
 *     panic.
 */
static void estatus_handle_aer(estatus_generic_data *gdata)
{
#ifdef CONFIG_ACPI_APEI_PCIEAER
	struct cper_sec_pcie *pcie_err = estatus_get_payload(gdata);

	if (pcie_err->validation_bits & CPER_PCIE_VALID_DEVICE_ID &&
	    pcie_err->validation_bits & CPER_PCIE_VALID_AER_INFO) {
		unsigned int devfn;
		int aer_severity;
		u8 *aer_info;

		devfn = PCI_DEVFN(pcie_err->device_id.device,
				  pcie_err->device_id.function);
		aer_severity = cper_severity_to_aer(gdata->error_severity);

		/*
		 * If firmware reset the component to contain
		 * the error, we must reinitialize it before
		 * use, so treat it as a fatal AER error.
		 */
		if (gdata->flags & CPER_SEC_RESET)
			aer_severity = AER_FATAL;

		aer_info = (void *)gen_pool_alloc(estatus_pool,
						  sizeof(struct aer_capability_regs));
		if (!aer_info)
			return;
		memcpy(aer_info, pcie_err->aer_info, sizeof(struct aer_capability_regs));

		aer_recover_queue(pcie_err->device_id.segment,
				  pcie_err->device_id.bus,
				  devfn, aer_severity,
				  (struct aer_capability_regs *)
				  aer_info);
	}
#endif
}

static BLOCKING_NOTIFIER_HEAD(vendor_record_notify_list);

int estatus_register_vendor_record_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&vendor_record_notify_list, nb);
}
EXPORT_SYMBOL_GPL(estatus_register_vendor_record_notifier);

void estatus_unregister_vendor_record_notifier(struct notifier_block *nb)
{
	blocking_notifier_chain_unregister(&vendor_record_notify_list, nb);
}
EXPORT_SYMBOL_GPL(estatus_unregister_vendor_record_notifier);

static void estatus_vendor_record_work_func(struct work_struct *work)
{
	struct estatus_vendor_record_entry *entry;
	estatus_generic_data *gdata;
	u32 len;

	entry = container_of(work, struct estatus_vendor_record_entry, work);
	gdata = ESTATUS_GDATA_FROM_VENDOR_ENTRY(entry);

	blocking_notifier_call_chain(&vendor_record_notify_list,
				     entry->error_severity, gdata);

	len = ESTATUS_VENDOR_ENTRY_LEN(estatus_get_record_size(gdata));
	gen_pool_free(estatus_pool, (unsigned long)entry, len);
}

static void estatus_defer_non_standard_event(estatus_generic_data *gdata, int sev)
{
	estatus_generic_data *copied_gdata;
	struct estatus_vendor_record_entry *entry;
	u32 len;

	len = ESTATUS_VENDOR_ENTRY_LEN(estatus_get_record_size(gdata));
	entry = (void *)gen_pool_alloc(estatus_pool, len);
	if (!entry)
		return;

	copied_gdata = ESTATUS_GDATA_FROM_VENDOR_ENTRY(entry);
	memcpy(copied_gdata, gdata, estatus_get_record_size(gdata));
	entry->error_severity = sev;

	INIT_WORK(&entry->work, estatus_vendor_record_work_func);
	schedule_work(&entry->work);
}

/*
 * A platform may describe one error source for the handling of synchronous
 * errors (e.g. MCE or SEA), or for handling asynchronous errors (e.g. SCI
 * or External Interrupt). On x86, the HEST notifications are always
 * asynchronous, so only SEA on ARM is delivered as a synchronous
 * notification.
 */
static inline bool estatus_is_sync_notify(struct estatus_source *source)
{
	return estatus_source_notify_mode(source) == ESTATUS_NOTIFY_SEA;
}

static void estatus_do_proc(struct estatus_source *source, const estatus_generic_status *estatus)
{
	int sev, sec_sev;
	estatus_generic_data *gdata;
	guid_t *sec_type;
	const guid_t *fru_id = &guid_null;
	char *fru_text = "";
	bool queued = false;
	bool sync = estatus_is_sync_notify(source);

	sev = estatus_severity(estatus->error_severity);
	estatus_for_each_section(estatus, gdata) {
		sec_type = (guid_t *)gdata->section_type;
		sec_sev = estatus_severity(gdata->error_severity);
		if (gdata->validation_bits & CPER_SEC_VALID_FRU_ID)
			fru_id = (guid_t *)gdata->fru_id;

		if (gdata->validation_bits & CPER_SEC_VALID_FRU_TEXT)
			fru_text = gdata->fru_text;

		if (guid_equal(sec_type, &CPER_SEC_PLATFORM_MEM)) {
			struct cper_sec_mem_err *mem_err = estatus_get_payload(gdata);

			atomic_notifier_call_chain(&estatus_report_chain, sev, mem_err);

			estatus_report_mem_error(sev, mem_err);
			queued = estatus_handle_memory_failure(gdata, sev, sync);
		} else if (guid_equal(sec_type, &CPER_SEC_PCIE)) {
			estatus_handle_aer(gdata);
		} else if (guid_equal(sec_type, &CPER_SEC_PROC_ARM)) {
			queued = estatus_handle_arm_hw_error(gdata, sev, sync);
		} else {
			void *err = estatus_get_payload(gdata);

			estatus_defer_non_standard_event(gdata, sev);
			log_non_standard_event(sec_type, fru_id, fru_text,
					       sec_sev, err,
					       gdata->error_data_length);
		}
	}

	if (sync && !queued) {
		pr_err(HW_ERR ESTATUS_PFX
		       "%s: synchronous unrecoverable error (SIGBUS)\n",
		       estatus_source_name(source));
		force_sig(SIGBUS);
	}
}

static void __estatus_panic(struct estatus_source *source, estatus_generic_status *estatus,
			    phys_addr_t buf_paddr, enum fixed_addresses fixmap_idx)
{
	const char *msg = ESTATUS_PFX "Fatal hardware error";

	__estatus_print_estatus(KERN_EMERG, source, estatus);

	add_taint(TAINT_MACHINE_CHECK, LOCKDEP_STILL_OK);

	estatus_clear_estatus(source, estatus, buf_paddr, fixmap_idx);

	if (!panic_timeout)
		pr_emerg("%s but panic disabled\n", msg);

	panic(msg);
}

int estatus_proc(struct estatus_source *source)
{
	estatus_generic_status *estatus = source->estatus;
	phys_addr_t buf_paddr;
	enum fixed_addresses fixmap_idx = estatus_source_fixmap(source);
	int rc;

	rc = estatus_read_estatus(source, estatus, &buf_paddr, fixmap_idx);
	if (rc)
		goto out;

	if (estatus_severity(estatus->error_severity) >= ESTATUS_SEV_PANIC)
		__estatus_panic(source, estatus, buf_paddr, fixmap_idx);

	if (!estatus_cached(estatus)) {
		if (estatus_print_estatus(NULL, source, estatus))
			estatus_cache_add(source, estatus);
	}
	estatus_do_proc(source, estatus);

out:
	estatus_clear_estatus(source, estatus, buf_paddr, fixmap_idx);

	return rc;
}
EXPORT_SYMBOL_GPL(estatus_proc);

/*
 * Handlers for CPER records may not be NMI safe. For example,
 * memory_failure_queue() takes spinlocks and calls schedule_work_on().
 * In any NMI-like handler, memory from estatus_pool is used to save
 * estatus, and added to the estatus_llist. irq_work_queue() causes
 * estatus_proc_in_irq() to run in IRQ context where each estatus in
 * estatus_llist is processed.
 *
 * Memory from the estatus_pool is also used with the estatus_cache
 * to suppress frequent messages.
 */
static struct llist_head estatus_llist;

void estatus_proc_in_irq(struct irq_work *irq_work)
{
	struct llist_node *llnode, *next;
	struct estatus_node *estatus_node;
	struct estatus_source *source;
	estatus_generic_status *estatus;
	u32 len, node_len;

	llnode = llist_del_all(&estatus_llist);
	/*
	 * Because the time order of estatus in list is reversed,
	 * revert it back to proper order.
	 */
	llnode = llist_reverse_order(llnode);
	while (llnode) {
		next = llnode->next;
		estatus_node = llist_entry(llnode, struct estatus_node,
					   llnode);
		source = estatus_node->source;
		estatus = ESTATUS_FROM_NODE(estatus_node);
		len = estatus_len(estatus);
		node_len = ESTATUS_NODE_LEN(len);
		estatus_do_proc(source, estatus);
		if (!estatus_cached(estatus)) {
			if (estatus_print_estatus(NULL, source, estatus))
				estatus_cache_add(source, estatus);
		}
		gen_pool_free(estatus_pool,
			      (unsigned long)estatus_node, node_len);

		llnode = next;
	}
}
EXPORT_SYMBOL_GPL(estatus_proc_in_irq);

static void estatus_print_queued_estatus(void)
{
	struct llist_node *llnode;
	struct estatus_node *estatus_node;
	struct estatus_source *source;
	estatus_generic_status *estatus;

	llnode = llist_del_all(&estatus_llist);
	/*
	 * Because the time order of estatus in list is reversed,
	 * revert it back to proper order.
	 */
	llnode = llist_reverse_order(llnode);
	while (llnode) {
		estatus_node = llist_entry(llnode, struct estatus_node,
					   llnode);
		estatus = ESTATUS_FROM_NODE(estatus_node);
		source = estatus_node->source;
		estatus_print_estatus(NULL, source, estatus);
		llnode = llnode->next;
	}
}

void estatus_report_mem_error(int sev, struct cper_sec_mem_err *mem_err)
{
#if IS_ENABLED(CONFIG_ACPI_APEI_GHES)
	arch_apei_report_mem_error(sev, mem_err);
#endif
}
