// SPDX-License-Identifier: GPL-2.0
/*
 * Shared GHES helpers for firmware-first CPER error handling.
 *
 * This file holds the GHES helper code that is shared by the in-tree GHES
 * driver and by other firmware-first error sources that reuse the same CPER
 * handling flow.
 *
 * Derived from the ACPI APEI GHES driver.
 *
 * Copyright 2010,2011 Intel Corp.
 *   Author: Huang Ying <ying.huang@intel.com>
 */

#include <linux/aer.h>
#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/genalloc.h>
#include <linux/io.h>
#include <linux/irq_work.h>
#include <linux/kfifo.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/math64.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/uuid.h>
#include <linux/sched/signal.h>
#include <linux/task_work.h>
#include <linux/notifier.h>
#include <linux/llist.h>
#include <linux/ras.h>
#include <ras/ras_event.h>
#include <linux/ratelimit.h>
#include <linux/rcupdate.h>
#include <linux/rculist.h>
#include <linux/sched/clock.h>
#include <linux/slab.h>
#include <linux/vmcore_info.h>
#include <linux/vmalloc.h>

#include <acpi/apei.h>
#include <acpi/ghes_cper.h>

#include <asm/fixmap.h>
#include <asm/tlbflush.h>

#include "apei-internal.h"

ATOMIC_NOTIFIER_HEAD(ghes_report_chain);

#ifndef CONFIG_ACPI_APEI
void __weak arch_apei_report_mem_error(int sev, struct cper_sec_mem_err *mem_err) { }
#endif

static struct ghes_estatus_cache __rcu *ghes_estatus_caches[GHES_ESTATUS_CACHES_SIZE];
static atomic_t ghes_estatus_cache_alloced;

struct gen_pool *ghes_estatus_pool;

int ghes_estatus_pool_init(unsigned int num_ghes)
{
	unsigned long addr, len;
	int rc;

	ghes_estatus_pool = gen_pool_create(GHES_ESTATUS_POOL_MIN_ALLOC_ORDER, -1);
	if (!ghes_estatus_pool)
		return -ENOMEM;

	len = GHES_ESTATUS_CACHE_AVG_SIZE * GHES_ESTATUS_CACHE_ALLOCED_MAX;
	len += (num_ghes * GHES_ESOURCE_PREALLOC_MAX_SIZE);

	addr = (unsigned long)vmalloc(PAGE_ALIGN(len));
	if (!addr)
		goto err_pool_alloc;

	rc = gen_pool_add(ghes_estatus_pool, addr, PAGE_ALIGN(len), -1);
	if (rc)
		goto err_pool_add;

	return 0;

err_pool_add:
	vfree((void *)addr);

err_pool_alloc:
	gen_pool_destroy(ghes_estatus_pool);

	return -ENOMEM;
}
EXPORT_SYMBOL_GPL(ghes_estatus_pool_init);

/**
 * ghes_estatus_pool_region_free - free previously allocated memory
 *				   from the ghes_estatus_pool.
 * @addr: address of memory to free.
 * @size: size of memory to free.
 *
 * Returns none.
 */
void ghes_estatus_pool_region_free(unsigned long addr, u32 size)
{
	gen_pool_free(ghes_estatus_pool, addr, size);
}
EXPORT_SYMBOL_GPL(ghes_estatus_pool_region_free);

int ghes_severity(int severity)
{
	switch (severity) {
	case CPER_SEV_INFORMATIONAL:
		return GHES_SEV_NO;
	case CPER_SEV_CORRECTED:
		return GHES_SEV_CORRECTED;
	case CPER_SEV_RECOVERABLE:
		return GHES_SEV_RECOVERABLE;
	case CPER_SEV_FATAL:
		return GHES_SEV_PANIC;
	default:
		/* Unknown, go panic */
		return GHES_SEV_PANIC;
	}
}

/**
 * struct ghes_task_work - for synchronous RAS event
 *
 * @twork:                callback_head for task work
 * @pfn:                  page frame number of corrupted page
 * @flags:                work control flags
 *
 * Structure to pass task work to be handled before
 * returning to user-space via task_work_add().
 */
struct ghes_task_work {
	struct callback_head twork;
	u64 pfn;
	int flags;
};

static void memory_failure_cb(struct callback_head *twork)
{
	struct ghes_task_work *twcb = container_of(twork, struct ghes_task_work, twork);
	int ret;

	ret = memory_failure(twcb->pfn, twcb->flags);
	gen_pool_free(ghes_estatus_pool, (unsigned long)twcb, sizeof(*twcb));

	if (!ret || ret == -EHWPOISON || ret == -EOPNOTSUPP)
		return;

	pr_err("%#llx: Sending SIGBUS to %s:%d due to hardware memory corruption\n",
	       twcb->pfn, current->comm, task_pid_nr(current));
	force_sig(SIGBUS);
}

static bool ghes_do_memory_failure(u64 physical_addr, int flags)
{
	struct ghes_task_work *twcb;
	unsigned long pfn;

	if (!IS_ENABLED(CONFIG_ACPI_APEI_MEMORY_FAILURE))
		return false;

	pfn = PHYS_PFN(physical_addr);

	if (flags == MF_ACTION_REQUIRED && current->mm) {
		twcb = (void *)gen_pool_alloc(ghes_estatus_pool, sizeof(*twcb));
		if (!twcb)
			return false;

		twcb->pfn = pfn;
		twcb->flags = flags;
		init_task_work(&twcb->twork, memory_failure_cb);
		task_work_add(current, &twcb->twork, TWA_RESUME);
		return true;
	}

	memory_failure_queue(pfn, flags);
	return true;
}

bool ghes_handle_memory_failure(struct acpi_hest_generic_data *gdata,
				int sev, bool sync)
{
	int flags = -1;
	int sec_sev = ghes_severity(gdata->error_severity);
	struct cper_sec_mem_err *mem_err = acpi_hest_get_payload(gdata);

	if (!(mem_err->validation_bits & CPER_MEM_VALID_PA))
		return false;

	/* iff following two events can be handled properly by now */
	if (sec_sev == GHES_SEV_CORRECTED &&
	    (gdata->flags & CPER_SEC_ERROR_THRESHOLD_EXCEEDED))
		flags = MF_SOFT_OFFLINE;
	if (sev == GHES_SEV_RECOVERABLE && sec_sev == GHES_SEV_RECOVERABLE)
		flags = sync ? MF_ACTION_REQUIRED : 0;

	if (flags != -1)
		return ghes_do_memory_failure(mem_err->physical_addr, flags);

	return false;
}

bool ghes_handle_arm_hw_error(struct acpi_hest_generic_data *gdata,
			      int sev, bool sync)
{
	struct cper_sec_proc_arm *err = acpi_hest_get_payload(gdata);
	int flags = sync ? MF_ACTION_REQUIRED : 0;
	int length = gdata->error_data_length;
	char error_type[120];
	bool queued = false;
	int sec_sev, i;
	char *p;

	sec_sev = ghes_severity(gdata->error_severity);
	if (length >= sizeof(*err)) {
		log_arm_hw_error(err, sec_sev);
	} else {
		pr_warn(FW_BUG "arm error length: %d\n", length);
		pr_warn(FW_BUG "length is too small\n");
		pr_warn(FW_BUG "firmware-generated error record is incorrect\n");
		return false;
	}

	if (sev != GHES_SEV_RECOVERABLE || sec_sev != GHES_SEV_RECOVERABLE)
		return false;

	p = (char *)(err + 1);
	length -= sizeof(err);

	for (i = 0; i < err->err_info_num; i++) {
		struct cper_arm_err_info *err_info;
		bool is_cache, has_pa;

		/* Ensure we have enough data for the error info header */
		if (length < sizeof(*err_info))
			break;

		err_info = (struct cper_arm_err_info *)p;

		/* Validate the claimed length before using it */
		length -= err_info->length;
		if (length < 0)
			break;

		is_cache = err_info->type & CPER_ARM_CACHE_ERROR;
		has_pa = (err_info->validation_bits & CPER_ARM_INFO_VALID_PHYSICAL_ADDR);

		/*
		 * The field (err_info->error_info & BIT(26)) is fixed to set to
		 * 1 in some old firmware of HiSilicon Kunpeng920. We assume that
		 * firmware won't mix corrected errors in an uncorrected section,
		 * and don't filter out 'corrected' error here.
		 */
		if (is_cache && has_pa) {
			queued = ghes_do_memory_failure(err_info->physical_fault_addr, flags);
			p += err_info->length;
			continue;
		}

		cper_bits_to_str(error_type, sizeof(error_type),
				 FIELD_GET(CPER_ARM_ERR_TYPE_MASK, err_info->type),
				 cper_proc_error_type_strs,
				 ARRAY_SIZE(cper_proc_error_type_strs));

			pr_warn_ratelimited(FW_WARN GHES_PFX
					    "Unhandled processor error type 0x%02x: %s%s\n",
					    err_info->type, error_type,
					    err_info->type & ~CPER_ARM_ERR_TYPE_MASK ?
					    " with reserved bit(s)" : "");
		p += err_info->length;
	}

	return queued;
}

/*
 * PCIe AER errors need to be sent to the AER driver for reporting and
 * recovery. The GHES severities map to the following AER severities and
 * require the following handling:
 *
 * GHES_SEV_CORRECTABLE -> AER_CORRECTABLE
 *     These need to be reported by the AER driver but no recovery is
 *     necessary.
 * GHES_SEV_RECOVERABLE -> AER_NONFATAL
 * GHES_SEV_RECOVERABLE && CPER_SEC_RESET -> AER_FATAL
 *     These both need to be reported and recovered from by the AER driver.
 * GHES_SEV_PANIC does not make it to this handling since the kernel must
 *     panic.
 */
void ghes_handle_aer(struct acpi_hest_generic_data *gdata)
{
#ifdef CONFIG_ACPI_APEI_PCIEAER
	struct cper_sec_pcie *pcie_err = acpi_hest_get_payload(gdata);

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

		aer_info = (void *)gen_pool_alloc(ghes_estatus_pool,
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

void ghes_log_hwerr(int sev, guid_t *sec_type)
{
	if (sev != CPER_SEV_RECOVERABLE)
		return;

	if (guid_equal(sec_type, &CPER_SEC_PROC_ARM) ||
	    guid_equal(sec_type, &CPER_SEC_PROC_GENERIC) ||
	    guid_equal(sec_type, &CPER_SEC_PROC_IA)) {
		hwerr_log_error_type(HWERR_RECOV_CPU);
		return;
	}

	if (guid_equal(sec_type, &CPER_SEC_CXL_PROT_ERR) ||
	    guid_equal(sec_type, &CPER_SEC_CXL_GEN_MEDIA_GUID) ||
	    guid_equal(sec_type, &CPER_SEC_CXL_DRAM_GUID) ||
	    guid_equal(sec_type, &CPER_SEC_CXL_MEM_MODULE_GUID)) {
		hwerr_log_error_type(HWERR_RECOV_CXL);
		return;
	}

	if (guid_equal(sec_type, &CPER_SEC_PCIE) ||
	    guid_equal(sec_type, &CPER_SEC_PCI_X_BUS)) {
		hwerr_log_error_type(HWERR_RECOV_PCI);
		return;
	}

	if (guid_equal(sec_type, &CPER_SEC_PLATFORM_MEM)) {
		hwerr_log_error_type(HWERR_RECOV_MEMORY);
		return;
	}

	hwerr_log_error_type(HWERR_RECOV_OTHERS);
}

void __ghes_print_estatus(const char *pfx,
			  const struct acpi_hest_generic *generic,
			  const struct acpi_hest_generic_status *estatus)
{
	static atomic_t seqno;
	unsigned int curr_seqno;
	char pfx_seq[64];

	if (!pfx) {
		if (ghes_severity(estatus->error_severity) <=
		    GHES_SEV_CORRECTED)
			pfx = KERN_WARNING;
		else
			pfx = KERN_ERR;
	}
	curr_seqno = atomic_inc_return(&seqno);
	snprintf(pfx_seq, sizeof(pfx_seq), "%s{%u}" HW_ERR, pfx, curr_seqno);
	printk("%sHardware error from APEI Generic Hardware Error Source: %d\n",
	       pfx_seq, generic->header.source_id);
	cper_estatus_print(pfx_seq, estatus);
}

int ghes_print_estatus(const char *pfx,
		       const struct acpi_hest_generic *generic,
		       const struct acpi_hest_generic_status *estatus)
{
	/* Not more than 2 messages every 5 seconds */
	static DEFINE_RATELIMIT_STATE(ratelimit_corrected, 5 * HZ, 2);
	static DEFINE_RATELIMIT_STATE(ratelimit_uncorrected, 5 * HZ, 2);
	struct ratelimit_state *ratelimit;

	if (ghes_severity(estatus->error_severity) <= GHES_SEV_CORRECTED)
		ratelimit = &ratelimit_corrected;
	else
		ratelimit = &ratelimit_uncorrected;
	if (__ratelimit(ratelimit)) {
		__ghes_print_estatus(pfx, generic, estatus);
		return 1;
	}
	return 0;
}

#ifdef CONFIG_ACPI_APEI
static void __iomem *ghes_map(u64 pfn, enum fixed_addresses fixmap_idx)
{
	phys_addr_t paddr;
	pgprot_t prot;

	paddr = PFN_PHYS(pfn);
	prot = arch_apei_get_mem_attribute(paddr);
	__set_fixmap(fixmap_idx, paddr, prot);

	return (void __iomem *) __fix_to_virt(fixmap_idx);
}

static void ghes_unmap(void __iomem *vaddr, enum fixed_addresses fixmap_idx)
{
	int _idx = virt_to_fix((unsigned long)vaddr);

	WARN_ON_ONCE(fixmap_idx != _idx);
	clear_fixmap(fixmap_idx);
}

static void ghes_ack_error(struct acpi_hest_generic_v2 *gv2)
{
	int rc;
	u64 val = 0;

	rc = apei_read(&val, &gv2->read_ack_register);
	if (rc)
		return;

	val &= gv2->read_ack_preserve << gv2->read_ack_register.bit_offset;
	val |= gv2->read_ack_write    << gv2->read_ack_register.bit_offset;

	apei_write(val, &gv2->read_ack_register);
}

static int map_gen_v2(struct ghes *ghes)
{
	return apei_map_generic_address(&ghes->generic_v2->read_ack_register);
}

static void unmap_gen_v2(struct ghes *ghes)
{
	apei_unmap_generic_address(&ghes->generic_v2->read_ack_register);
}

struct ghes *ghes_new(struct acpi_hest_generic *generic)
{
	struct ghes *ghes;
	unsigned int error_block_length;
	int rc;

	ghes = kzalloc_obj(*ghes);
	if (!ghes)
		return ERR_PTR(-ENOMEM);

	ghes->generic = generic;
	if (is_hest_type_generic_v2(ghes)) {
		rc = map_gen_v2(ghes);
		if (rc)
			goto err_free;
	}

	rc = apei_map_generic_address(&generic->error_status_address);
	if (rc)
		goto err_unmap_read_ack_addr;
	error_block_length = generic->error_block_length;
	if (error_block_length > GHES_ESTATUS_MAX_SIZE) {
		pr_warn(FW_WARN GHES_PFX
			"Error status block length is too long: %u for "
			"generic hardware error source: %d.\n",
			error_block_length, generic->header.source_id);
		error_block_length = GHES_ESTATUS_MAX_SIZE;
	}
	ghes->estatus = kmalloc(error_block_length, GFP_KERNEL);
	ghes->estatus_length = error_block_length;
	if (!ghes->estatus) {
		rc = -ENOMEM;
		goto err_unmap_status_addr;
	}

	return ghes;

err_unmap_status_addr:
	apei_unmap_generic_address(&generic->error_status_address);
err_unmap_read_ack_addr:
	if (is_hest_type_generic_v2(ghes))
		unmap_gen_v2(ghes);
err_free:
	kfree(ghes);
	return ERR_PTR(rc);
}

void ghes_fini(struct ghes *ghes)
{
	kfree(ghes->estatus);
	apei_unmap_generic_address(&ghes->generic->error_status_address);
	if (is_hest_type_generic_v2(ghes))
		unmap_gen_v2(ghes);
}

static void ghes_copy_tofrom_phys(void *buffer, u64 paddr, u32 len,
				  int from_phys,
				  enum fixed_addresses fixmap_idx)
{
	void __iomem *vaddr;
	u64 offset;
	u32 trunk;

	while (len > 0) {
		offset = paddr - (paddr & PAGE_MASK);
		vaddr = ghes_map(PHYS_PFN(paddr), fixmap_idx);
		trunk = PAGE_SIZE - offset;
		trunk = min(trunk, len);
		if (from_phys)
			memcpy_fromio(buffer, vaddr + offset, trunk);
		else
			memcpy_toio(vaddr + offset, buffer, trunk);
		len -= trunk;
		paddr += trunk;
		buffer += trunk;
		ghes_unmap(vaddr, fixmap_idx);
	}
}

/* Check the top-level record header has an appropriate size. */
int __ghes_check_estatus(struct ghes *ghes,
			 struct acpi_hest_generic_status *estatus)
{
	u32 len = cper_estatus_len(estatus);
	u32 max_len = min(ghes->generic->error_block_length,
			  ghes->estatus_length);

	if (len < sizeof(*estatus)) {
		pr_warn_ratelimited(FW_WARN GHES_PFX "Truncated error status block!\n");
		return -EIO;
	}

	if (!len || len > max_len) {
		pr_warn_ratelimited(FW_WARN GHES_PFX "Invalid error status block length!\n");
		return -EIO;
	}

	if (cper_estatus_check_header(estatus)) {
		pr_warn_ratelimited(FW_WARN GHES_PFX "Invalid CPER header!\n");
		return -EIO;
	}

	return 0;
}

/* Read the CPER block, returning its address, and header in estatus. */
int __ghes_peek_estatus(struct ghes *ghes,
			struct acpi_hest_generic_status *estatus,
			u64 *buf_paddr, enum fixed_addresses fixmap_idx)
{
	struct acpi_hest_generic *g = ghes->generic;
	int rc;

	rc = apei_read(buf_paddr, &g->error_status_address);
	if (rc) {
		*buf_paddr = 0;
		pr_warn_ratelimited(FW_WARN GHES_PFX
				    "Failed to read error status block address for hardware error source: %d.\n",
				   g->header.source_id);
		return -EIO;
	}
	if (!*buf_paddr)
		return -ENOENT;

	ghes_copy_tofrom_phys(estatus, *buf_paddr, sizeof(*estatus), 1,
			      fixmap_idx);
	if (!estatus->block_status) {
		*buf_paddr = 0;
		return -ENOENT;
	}

	return 0;
}

int __ghes_read_estatus(struct acpi_hest_generic_status *estatus,
			u64 buf_paddr, enum fixed_addresses fixmap_idx,
			size_t buf_len)
{
	ghes_copy_tofrom_phys(estatus, buf_paddr, buf_len, 1, fixmap_idx);
	if (cper_estatus_check(estatus)) {
		pr_warn_ratelimited(FW_WARN GHES_PFX
				    "Failed to read error status block!\n");
		return -EIO;
	}

	return 0;
}

int ghes_read_estatus(struct ghes *ghes,
		      struct acpi_hest_generic_status *estatus,
		      u64 *buf_paddr, enum fixed_addresses fixmap_idx)
{
	int rc;

	rc = __ghes_peek_estatus(ghes, estatus, buf_paddr, fixmap_idx);
	if (rc)
		return rc;

	rc = __ghes_check_estatus(ghes, estatus);
	if (rc)
		return rc;

	return __ghes_read_estatus(estatus, *buf_paddr, fixmap_idx,
				   cper_estatus_len(estatus));
}

void ghes_clear_estatus(struct ghes *ghes,
			struct acpi_hest_generic_status *estatus,
			u64 buf_paddr, enum fixed_addresses fixmap_idx)
{
	estatus->block_status = 0;

	if (!buf_paddr)
		return;

	ghes_copy_tofrom_phys(estatus, buf_paddr,
			      sizeof(estatus->block_status), 0,
			      fixmap_idx);

	/*
	 * GHESv2 type HEST entries introduce support for error acknowledgment,
	 * so only acknowledge the error if this support is present.
	 */
	if (is_hest_type_generic_v2(ghes))
		ghes_ack_error(ghes->generic_v2);
}
#endif /* CONFIG_ACPI_APEI */

static BLOCKING_NOTIFIER_HEAD(vendor_record_notify_list);

int ghes_register_vendor_record_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&vendor_record_notify_list, nb);
}
EXPORT_SYMBOL_GPL(ghes_register_vendor_record_notifier);

void ghes_unregister_vendor_record_notifier(struct notifier_block *nb)
{
	blocking_notifier_chain_unregister(&vendor_record_notify_list, nb);
}
EXPORT_SYMBOL_GPL(ghes_unregister_vendor_record_notifier);

static void ghes_vendor_record_work_func(struct work_struct *work)
{
	struct ghes_vendor_record_entry *entry;
	struct acpi_hest_generic_data *gdata;
	u32 len;

	entry = container_of(work, struct ghes_vendor_record_entry, work);
	gdata = GHES_GDATA_FROM_VENDOR_ENTRY(entry);

	blocking_notifier_call_chain(&vendor_record_notify_list,
				     entry->error_severity, gdata);

	len = GHES_VENDOR_ENTRY_LEN(acpi_hest_get_record_size(gdata));
	gen_pool_free(ghes_estatus_pool, (unsigned long)entry, len);
}

void ghes_defer_non_standard_event(struct acpi_hest_generic_data *gdata,
				   int sev)
{
	struct acpi_hest_generic_data *copied_gdata;
	struct ghes_vendor_record_entry *entry;
	u32 len;

	len = GHES_VENDOR_ENTRY_LEN(acpi_hest_get_record_size(gdata));
	entry = (void *)gen_pool_alloc(ghes_estatus_pool, len);
	if (!entry)
		return;

	copied_gdata = GHES_GDATA_FROM_VENDOR_ENTRY(entry);
	memcpy(copied_gdata, gdata, acpi_hest_get_record_size(gdata));
	entry->error_severity = sev;

	INIT_WORK(&entry->work, ghes_vendor_record_work_func);
	schedule_work(&entry->work);
}

void ghes_cper_handle_status(struct device *dev,
			     const struct acpi_hest_generic *generic,
			     const struct acpi_hest_generic_status *estatus,
			     bool sync)
{
	int sev, sec_sev;
	struct acpi_hest_generic_data *gdata;
	guid_t *sec_type;
	const guid_t *fru_id = &guid_null;
	char *fru_text = "";
	bool queued = false;

	sev = ghes_severity(estatus->error_severity);
	apei_estatus_for_each_section(estatus, gdata) {
		sec_type = (guid_t *)gdata->section_type;
		sec_sev = ghes_severity(gdata->error_severity);
		if (gdata->validation_bits & CPER_SEC_VALID_FRU_ID)
			fru_id = (guid_t *)gdata->fru_id;

		if (gdata->validation_bits & CPER_SEC_VALID_FRU_TEXT)
			fru_text = gdata->fru_text;

		ghes_log_hwerr(sev, sec_type);
		if (guid_equal(sec_type, &CPER_SEC_PLATFORM_MEM)) {
			struct cper_sec_mem_err *mem_err = acpi_hest_get_payload(gdata);

			atomic_notifier_call_chain(&ghes_report_chain, sev, mem_err);

			arch_apei_report_mem_error(sev, mem_err);
			queued = ghes_handle_memory_failure(gdata, sev, sync);
		} else if (guid_equal(sec_type, &CPER_SEC_PCIE)) {
			ghes_handle_aer(gdata);
		} else if (guid_equal(sec_type, &CPER_SEC_PROC_ARM)) {
			queued = ghes_handle_arm_hw_error(gdata, sev, sync);
		} else if (guid_equal(sec_type, &CPER_SEC_CXL_PROT_ERR)) {
			struct cxl_cper_sec_prot_err *prot_err = acpi_hest_get_payload(gdata);

			cxl_cper_post_prot_err(prot_err, gdata->error_severity);
		} else if (guid_equal(sec_type, &CPER_SEC_CXL_GEN_MEDIA_GUID)) {
			struct cxl_cper_event_rec *rec = acpi_hest_get_payload(gdata);

			cxl_cper_post_event(CXL_CPER_EVENT_GEN_MEDIA, rec);
		} else if (guid_equal(sec_type, &CPER_SEC_CXL_DRAM_GUID)) {
			struct cxl_cper_event_rec *rec = acpi_hest_get_payload(gdata);

			cxl_cper_post_event(CXL_CPER_EVENT_DRAM, rec);
		} else if (guid_equal(sec_type, &CPER_SEC_CXL_MEM_MODULE_GUID)) {
			struct cxl_cper_event_rec *rec = acpi_hest_get_payload(gdata);

			cxl_cper_post_event(CXL_CPER_EVENT_MEM_MODULE, rec);
		} else {
			void *err = acpi_hest_get_payload(gdata);

			ghes_defer_non_standard_event(gdata, sev);
			log_non_standard_event(sec_type, fru_id, fru_text,
					       sec_sev, err,
					       gdata->error_data_length);
		}
	}

	/*
	 * If no memory failure work is queued for abnormal synchronous
	 * errors, do a force kill.
	 */
	if (sync && !queued) {
		dev_err(dev,
			HW_ERR GHES_PFX "%s:%d: synchronous unrecoverable error (SIGBUS)\n",
			current->comm, task_pid_nr(current));
		force_sig(SIGBUS);
	}
}
/* Room for 8 entries */
#define CXL_CPER_PROT_ERR_FIFO_DEPTH 8
static DEFINE_KFIFO(cxl_cper_prot_err_fifo, struct cxl_cper_prot_err_work_data,
		    CXL_CPER_PROT_ERR_FIFO_DEPTH);

/* Synchronize schedule_work() with cxl_cper_prot_err_work changes */
static DEFINE_SPINLOCK(cxl_cper_prot_err_work_lock);
struct work_struct *cxl_cper_prot_err_work;

void cxl_cper_post_prot_err(struct cxl_cper_sec_prot_err *prot_err,
			    int severity)
{
#ifdef CONFIG_ACPI_APEI_PCIEAER
	struct cxl_cper_prot_err_work_data wd;

	if (cxl_cper_sec_prot_err_valid(prot_err))
		return;

	guard(spinlock_irqsave)(&cxl_cper_prot_err_work_lock);

	if (!cxl_cper_prot_err_work)
		return;

	if (cxl_cper_setup_prot_err_work_data(&wd, prot_err, severity))
		return;

	if (!kfifo_put(&cxl_cper_prot_err_fifo, wd)) {
		pr_err_ratelimited("CXL CPER kfifo overflow\n");
		return;
	}

	schedule_work(cxl_cper_prot_err_work);
#endif
}

int cxl_cper_register_prot_err_work(struct work_struct *work)
{
	if (cxl_cper_prot_err_work)
		return -EINVAL;

	guard(spinlock)(&cxl_cper_prot_err_work_lock);
	cxl_cper_prot_err_work = work;
	return 0;
}
EXPORT_SYMBOL_NS_GPL(cxl_cper_register_prot_err_work, "CXL");

int cxl_cper_unregister_prot_err_work(struct work_struct *work)
{
	if (cxl_cper_prot_err_work != work)
		return -EINVAL;

	guard(spinlock)(&cxl_cper_prot_err_work_lock);
	cxl_cper_prot_err_work = NULL;
	return 0;
}
EXPORT_SYMBOL_NS_GPL(cxl_cper_unregister_prot_err_work, "CXL");

int cxl_cper_prot_err_kfifo_get(struct cxl_cper_prot_err_work_data *wd)
{
	return kfifo_get(&cxl_cper_prot_err_fifo, wd);
}
EXPORT_SYMBOL_NS_GPL(cxl_cper_prot_err_kfifo_get, "CXL");

/* Room for 8 entries for each of the 4 event log queues */
#define CXL_CPER_FIFO_DEPTH 32
static DEFINE_KFIFO(cxl_cper_fifo, struct cxl_cper_work_data, CXL_CPER_FIFO_DEPTH);

/* Synchronize schedule_work() with cxl_cper_work changes */
static DEFINE_SPINLOCK(cxl_cper_work_lock);
struct work_struct *cxl_cper_work;

void cxl_cper_post_event(enum cxl_event_type event_type,
			 struct cxl_cper_event_rec *rec)
{
	struct cxl_cper_work_data wd;

	if (rec->hdr.length <= sizeof(rec->hdr) ||
	    rec->hdr.length > sizeof(*rec)) {
		pr_err(FW_WARN "CXL CPER Invalid section length (%u)\n",
		       rec->hdr.length);
		return;
	}

	if (!(rec->hdr.validation_bits & CPER_CXL_COMP_EVENT_LOG_VALID)) {
		pr_err(FW_WARN "CXL CPER invalid event\n");
		return;
	}

	guard(spinlock_irqsave)(&cxl_cper_work_lock);

	if (!cxl_cper_work)
		return;

	wd.event_type = event_type;
	memcpy(&wd.rec, rec, sizeof(wd.rec));

	if (!kfifo_put(&cxl_cper_fifo, wd)) {
		pr_err_ratelimited("CXL CPER kfifo overflow\n");
		return;
	}

	schedule_work(cxl_cper_work);
}

int cxl_cper_register_work(struct work_struct *work)
{
	if (cxl_cper_work)
		return -EINVAL;

	guard(spinlock)(&cxl_cper_work_lock);
	cxl_cper_work = work;
	return 0;
}
EXPORT_SYMBOL_NS_GPL(cxl_cper_register_work, "CXL");

int cxl_cper_unregister_work(struct work_struct *work)
{
	if (cxl_cper_work != work)
		return -EINVAL;

	guard(spinlock)(&cxl_cper_work_lock);
	cxl_cper_work = NULL;
	return 0;
}
EXPORT_SYMBOL_NS_GPL(cxl_cper_unregister_work, "CXL");

int cxl_cper_kfifo_get(struct cxl_cper_work_data *wd)
{
	return kfifo_get(&cxl_cper_fifo, wd);
}
EXPORT_SYMBOL_NS_GPL(cxl_cper_kfifo_get, "CXL");

/*
 * GHES error status reporting throttle, to report more kinds of
 * errors, instead of just most frequently occurred errors.
 */
int ghes_estatus_cached(struct acpi_hest_generic_status *estatus)
{
	u32 len;
	int i, cached = 0;
	unsigned long long now;
	struct ghes_estatus_cache *cache;
	struct acpi_hest_generic_status *cache_estatus;

	len = cper_estatus_len(estatus);
	rcu_read_lock();
	for (i = 0; i < GHES_ESTATUS_CACHES_SIZE; i++) {
		cache = rcu_dereference(ghes_estatus_caches[i]);
		if (cache == NULL)
			continue;
		if (len != cache->estatus_len)
			continue;
		cache_estatus = GHES_ESTATUS_FROM_CACHE(cache);
		if (memcmp(estatus, cache_estatus, len))
			continue;
		atomic_inc(&cache->count);
		now = sched_clock();
		if (now - cache->time_in < GHES_ESTATUS_IN_CACHE_MAX_NSEC)
			cached = 1;
		break;
	}
	rcu_read_unlock();
	return cached;
}

static struct ghes_estatus_cache *ghes_estatus_cache_alloc(
	struct acpi_hest_generic *generic,
	struct acpi_hest_generic_status *estatus)
{
	int alloced;
	u32 len, cache_len;
	struct ghes_estatus_cache *cache;
	struct acpi_hest_generic_status *cache_estatus;

	alloced = atomic_add_return(1, &ghes_estatus_cache_alloced);
	if (alloced > GHES_ESTATUS_CACHE_ALLOCED_MAX) {
		atomic_dec(&ghes_estatus_cache_alloced);
		return NULL;
	}
	len = cper_estatus_len(estatus);
	cache_len = GHES_ESTATUS_CACHE_LEN(len);
	cache = (void *)gen_pool_alloc(ghes_estatus_pool, cache_len);
	if (cache == NULL) {
		atomic_dec(&ghes_estatus_cache_alloced);
		return NULL;
	}
	cache_estatus = GHES_ESTATUS_FROM_CACHE(cache);
	memcpy(cache_estatus, estatus, len);
	cache->estatus_len = len;
	atomic_set(&cache->count, 0);
	cache->generic = generic;
	cache->time_in = sched_clock();
	return cache;
}

static void ghes_estatus_cache_rcu_free(struct rcu_head *head)
{
	struct ghes_estatus_cache *cache;
	u32 len;

	cache = container_of(head, struct ghes_estatus_cache, rcu);
	len = cper_estatus_len(GHES_ESTATUS_FROM_CACHE(cache));
	len = GHES_ESTATUS_CACHE_LEN(len);
	gen_pool_free(ghes_estatus_pool, (unsigned long)cache, len);
	atomic_dec(&ghes_estatus_cache_alloced);
}

void
ghes_estatus_cache_add(struct acpi_hest_generic *generic,
		       struct acpi_hest_generic_status *estatus)
{
	unsigned long long now, duration, period, max_period = 0;
	struct ghes_estatus_cache *cache, *new_cache;
	struct ghes_estatus_cache __rcu *victim;
	int i, slot = -1, count;

	new_cache = ghes_estatus_cache_alloc(generic, estatus);
	if (!new_cache)
		return;

	rcu_read_lock();
	now = sched_clock();
	for (i = 0; i < GHES_ESTATUS_CACHES_SIZE; i++) {
		cache = rcu_dereference(ghes_estatus_caches[i]);
		if (cache == NULL) {
			slot = i;
			break;
		}
		duration = now - cache->time_in;
		if (duration >= GHES_ESTATUS_IN_CACHE_MAX_NSEC) {
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
		 * Use release semantics to ensure that ghes_estatus_cached()
		 * running on another CPU will see the updated cache fields if
		 * it can see the new value of the pointer.
		 */
		victim = xchg_release(&ghes_estatus_caches[slot],
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
				 ghes_estatus_cache_rcu_free);
	}
}
