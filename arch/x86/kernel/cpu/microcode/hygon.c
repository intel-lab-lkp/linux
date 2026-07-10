// SPDX-License-Identifier: GPL-2.0-only
/*
 *  Hygon CPU Microcode Update Driver for Linux
 *
 *  This driver allows to upgrade microcode on Hygon CPUs.
 *
 */
#define pr_fmt(fmt) "microcode: " fmt

#include <linux/earlycpio.h>
#include <linux/firmware.h>
#include <linux/bsearch.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/initrd.h>
#include <linux/kernel.h>
#include <linux/pci.h>

#include <asm/microcode.h>
#include <asm/processor.h>
#include <asm/setup.h>
#include <asm/cpu.h>
#include <asm/msr.h>
#include <asm/tlb.h>

#include "internal.h"

#define HYGON_UCODE_MAGIC		0x4859474F
#define UCODE_EQUIV_CPU_TABLE_TYPE	0x00000000
#define UCODE_UCODE_TYPE		0x00000001

#define HYGON_CONTAINER_HDR_SIZE		12
#define HYGON_CONTAINER_HDR_MAGIC_OFFSET	0
#define HYGON_CONTAINER_HDR_TYPE_OFFSET		4
#define HYGON_CONTAINER_HDR_LENGTH_OFFSET	8

#define HYGON_SECTION_HDR_SIZE			8
#define HYGON_SECTION_HDR_TYPE_OFFSET		0
#define HYGON_SECTION_HDR_SIZE_OFFSET		4

#define HYGON_MSR_PATCH_LEVEL			0x0000008b
#define HYGON_MSR_PATCH_LOADER			0xc0010020

#define HYGON_PROC_REV_ID_FAMILY_SHIFT		12
#define HYGON_PROC_REV_ID_BASE_FAMILY		0xF

#define HYGON_UCODE_FW_NAME		"hygon-ucode/microcode_hygon_fam18h.bin"
#define HYGON_UCODE_PATH		"kernel/x86/microcode/HygonGenuine.bin"

#define hygon_container_hdr(_buf, _field) \
	(*(const u32 *)((_buf) + HYGON_CONTAINER_HDR_##_field##_OFFSET))

#define hygon_section_hdr(_buf, _field) \
	(*(const u32 *)((_buf) + HYGON_SECTION_HDR_##_field##_OFFSET))

#define hygon_extract_proc_family(_proc_rev) \
	(HYGON_PROC_REV_ID_BASE_FAMILY + \
	 ((_proc_rev) >> HYGON_PROC_REV_ID_FAMILY_SHIFT))

#define hygon_pages_crossed(_start, _size) \
	(((unsigned long)(_start) ^ ((unsigned long)(_start) + (_size) - 1)) >> PAGE_SHIFT)

#define hygon_container_data(_buf)  ((_buf) + HYGON_CONTAINER_HDR_SIZE)
#define hygon_section_data(_buf)    ((_buf) + HYGON_SECTION_HDR_SIZE)

struct hygon_ucode_patch {
	struct list_head list;
	void *data;
	unsigned int patch_size;
	u32 patch_level;
	u16 equiv_id;
};

struct hygon_microcode_header {
	u32	data;
	u32	patch_level;
	u32	reserved0[4];
	u16	processor_rev_id;
	u16	reserved1;
	u32	reserved2[9];
} __packed;

struct hygon_microcode {
	struct hygon_microcode_header	header;
	unsigned int			payload[];
};

struct hygon_cpu_equiv_entry {
	u32	cpu_signature;
	u16	equiv_id;
	u16	reserved;
} __packed;

struct hygon_cpu_equiv_table {
	unsigned int entry_count;
	struct hygon_cpu_equiv_entry *entry;
};

struct hygon_container_desc {
	struct hygon_microcode *mc;
	u32		     patch_size;
	u8		     *data;
	size_t		     size;
};

static const char ucode_path[] __maybe_unused = HYGON_UCODE_PATH;
static u32 bsp_cpuid_1_eax __ro_after_init;
static LIST_HEAD(microcode_cache);
static struct hygon_cpu_equiv_table equiv_table;

static u32 get_current_patch_level(void)
{
	u32 rev, dummy __always_unused;

	native_rdmsr(HYGON_MSR_PATCH_LEVEL, rev, dummy);

	return rev;
}

static u16 find_equiv_id(struct hygon_cpu_equiv_table *et, u32 sig)
{
	unsigned int i;

	if (!et || !et->entry_count)
		return 0;

	for (i = 0; i < et->entry_count; i++) {
		if (sig == et->entry[i].cpu_signature)
			return et->entry[i].equiv_id;
	}
	return 0;
}

static bool is_valid_microcode_container(const u8 *buf, size_t buf_size)
{
	if (buf_size <= HYGON_CONTAINER_HDR_SIZE) {
		ucode_dbg("Truncated microcode container header.\n");
		return false;
	}

	if (hygon_container_hdr(buf, MAGIC) != HYGON_UCODE_MAGIC) {
		ucode_dbg("Invalid magic value (0x%08x).\n",
			  hygon_container_hdr(buf, MAGIC));
		return false;
	}

	return true;
}

static bool verify_cpu_equivalence_table(const u8 *buf, size_t buf_size)
{
	u32 cont_type, equiv_tbl_len;

	if (!is_valid_microcode_container(buf, buf_size))
		return false;

	cont_type = hygon_container_hdr(buf, TYPE);
	if (cont_type != UCODE_EQUIV_CPU_TABLE_TYPE) {
		ucode_dbg("Wrong microcode container equivalence table type: %u.\n",
			  cont_type);
		return false;
	}

	buf_size -= HYGON_CONTAINER_HDR_SIZE;

	equiv_tbl_len = hygon_container_hdr(buf, LENGTH);
	if (equiv_tbl_len < sizeof(struct hygon_cpu_equiv_entry) ||
	    buf_size < equiv_tbl_len) {
		ucode_dbg("Truncated equivalence table.\n");
		return false;
	}

	return true;
}

static bool is_valid_patch_section(const u8 *buf, size_t buf_size, u32 *patch_size)
{
	if (buf_size < HYGON_SECTION_HDR_SIZE) {
		ucode_dbg("Truncated patch section.\n");
		return false;
	}

	*patch_size = hygon_section_hdr(buf, SIZE);

	if (hygon_section_hdr(buf, TYPE) != UCODE_UCODE_TYPE) {
		ucode_dbg("Invalid type field (0x%x) in container file section header.\n",
			  hygon_section_hdr(buf, TYPE));
		return false;
	}

	if (*patch_size < sizeof(struct hygon_microcode_header)) {
		ucode_dbg("Patch of size %u too short.\n", *patch_size);
		return false;
	}

	return true;
}

static int validate_patch_section(const u8 *buf, size_t buf_size, u32 *patch_size)
{
	u8 family = x86_family(bsp_cpuid_1_eax);
	struct hygon_microcode_header *mc_hdr;
	u16 proc_id;

	if (!is_valid_patch_section(buf, buf_size, patch_size))
		return -1;

	buf_size -= HYGON_SECTION_HDR_SIZE;

	if (buf_size < *patch_size) {
		ucode_dbg("Patch of size %u truncated.\n", *patch_size);
		return -1;
	}

	mc_hdr	= (struct hygon_microcode_header *)hygon_section_data(buf);

	proc_id	= mc_hdr->processor_rev_id;

	if (hygon_extract_proc_family(proc_id) != family)
		return 1;

	return 0;
}

/*
 * Attempt to consume one container from the ucode blob.
 *
 * This function processes a single container starting at @ucode. It verifies
 * the container's CPU equivalence table, locates the equivalence ID for the
 * current CPU, and then scans all patch sections inside the container.
 *
 * If a patch matching the CPU's equivalence ID is found, the function fills
 * @desc with the patch metadata and the container's bounds, and returns 0.
 * Otherwise, it returns the total number of bytes consumed (the entire size
 * of this container) so the caller can skip it and continue scanning.
 */
static size_t try_consume_container(u8 *ucode, size_t size, struct hygon_container_desc *desc)
{
	struct hygon_cpu_equiv_table table;
	size_t orig_size = size;
	u32 equiv_data_len;
	u16 cpu_equiv_id;
	u8 *buf;

	if (!verify_cpu_equivalence_table(ucode, size))
		return 0;

	buf = ucode;
	equiv_data_len = hygon_container_hdr(buf, LENGTH);

	table.entry = (struct hygon_cpu_equiv_entry *)hygon_container_data(buf);
	table.entry_count = equiv_data_len / sizeof(struct hygon_cpu_equiv_entry);

	cpu_equiv_id = find_equiv_id(&table, bsp_cpuid_1_eax);

	buf  += equiv_data_len + HYGON_CONTAINER_HDR_SIZE;
	size -= equiv_data_len + HYGON_CONTAINER_HDR_SIZE;

	while (size > 0) {
		struct hygon_microcode *mc;
		u32 patch_size;
		int ret;

		ret = validate_patch_section(buf, size, &patch_size);
		if (ret < 0)
			goto out;
		else if (ret > 0)
			goto skip;

		mc = (struct hygon_microcode *)hygon_section_data(buf);

		if (cpu_equiv_id == mc->header.processor_rev_id) {
			desc->patch_size = patch_size;
			desc->mc = mc;

			ucode_dbg(" match: size: %d\n", patch_size);
		}

skip:
		buf  += patch_size + HYGON_SECTION_HDR_SIZE;
		size -= patch_size + HYGON_SECTION_HDR_SIZE;
	}

out:
	/*
	 * If we have found a matching patch, record the container's base pointer
	 * and its total size, then return 0 to indicate that @ucode already points
	 * to the correct container.
	 *
	 * Otherwise, return the total number of bytes scanned, allowing the caller
	 * to skip this entire container and try the next one.
	 */
	if (desc->mc) {
		desc->data = ucode;
		desc->size = orig_size - size;

		return 0;
	}

	return orig_size - size;
}

/*
 * Find the container that contains a patch for the current CPU
 *
 * The ucode blob may consist of multiple containers concatenated together.
 * This function iterates through them by repeatedly calling try_consume_container().
 * Once a container with a matching patch is found, it fills @desc and returns.
 * If no matching container exists in the entire blob, @desc remains unchanged.
 */
static void locate_patch_container(u8 *ucode, size_t size, struct hygon_container_desc *desc)
{
	while (size) {
		size_t consumed = try_consume_container(ucode, size, desc);

		if (!consumed)
			return;

		if (size >= consumed) {
			ucode += consumed;
			size  -= consumed;
		} else {
			return;
		}
	}
}

static bool apply_hygon_patch(struct hygon_microcode *mc, u32 *cur_rev,
				  unsigned int patch_size)
{
	unsigned long patch_start = (unsigned long)&mc->header.data;
	unsigned long patch_end = patch_start + patch_size - 1;

	native_wrmsrq(HYGON_MSR_PATCH_LOADER, patch_start);
	invlpg(patch_start);

	/*
	 * Invalidate the next page if the patch image crosses a page boundary.
	 */
	if (hygon_pages_crossed(patch_start, patch_size))
		invlpg(patch_end);

	if (IS_ENABLED(CONFIG_MICROCODE_DBG) && x86_hypervisor_present)
		microcode_rev[smp_processor_id()] = mc->header.patch_level;

	*cur_rev = get_current_patch_level();

	ucode_dbg("updated rev: 0x%x\n", *cur_rev);

	return *cur_rev == mc->header.patch_level;
}

static bool load_builtin_microcode(struct cpio_data *cp)
{
	struct firmware fw;

	if (IS_ENABLED(CONFIG_X86_32))
		return false;

	if (firmware_request_builtin(&fw, HYGON_UCODE_FW_NAME)) {
		cp->size = fw.size;
		cp->data = (void *)fw.data;
		return true;
	}

	return false;
}

static bool __init find_microcode_blob(struct cpio_data *ret)
{
	struct cpio_data cp;

	if (!load_builtin_microcode(&cp))
		cp = find_microcode_in_initrd(ucode_path);

	if (cp.data && cp.size) {
		*ret = cp;
		return true;
	}

	return false;
}

/*
 * This function handles microcode loading during the VERY EARLY boot stage
 * (before vmalloc is available). The entire operation must be done in-place
 * within the initrd memory.
 *
 * Flow:
 * 1. Read current microcode revision.
 * 2. Locate the microcode container blob from initrd.
 * 3. Traverse the Equivalent CPU Table to find the matching microcode patch.
 * 4. Apply the patch if it is newer than the current revision.
 */
void __init load_ucode_hygon_bsp(struct early_load_data *ed, unsigned int cpuid_1_eax)
{
	struct hygon_container_desc desc = { };
	struct hygon_microcode *mc;
	struct cpio_data cp = { };
	u32 rev;

	bsp_cpuid_1_eax = cpuid_1_eax;

	rev = get_current_patch_level();
	ed->old_rev = rev;

	/* Needed in hygon_load_microcode() */
	ucode_cpu_info[0].cpu_sig.sig = cpuid_1_eax;

	if (!find_microcode_blob(&cp))
		return;

	locate_patch_container(cp.data, cp.size, &desc);

	mc = desc.mc;

	if (!mc || (ed->old_rev > mc->header.patch_level))
		return;

	if (apply_hygon_patch(mc, &rev, desc.patch_size))
		ed->new_rev = rev;
}

/*
 * a small, trivial cache of per-family ucode patches
 */
static struct hygon_ucode_patch *find_cached_patch(u16 equiv_id)
{
	struct hygon_ucode_patch *p;

	list_for_each_entry(p, &microcode_cache, list)
		if (p->equiv_id == equiv_id)
			return p;

	return NULL;
}

static void add_to_cache(struct hygon_ucode_patch *new_patch)
{
	struct hygon_ucode_patch *p;

	list_for_each_entry(p, &microcode_cache, list) {
		if (p->equiv_id == new_patch->equiv_id) {
			if (p->patch_level >= new_patch->patch_level) {
				/* we already have the latest patch */
				kfree(new_patch->data);
				kfree(new_patch);
				return;
			}

			list_replace(&p->list, &new_patch->list);
			kfree(p->data);
			kfree(p);
			return;
		}
	}
	/* no patch found, add it */
	list_add_tail(&new_patch->list, &microcode_cache);
}

static void clear_microcode_cache(void)
{
	struct hygon_ucode_patch *p, *tmp;

	list_for_each_entry_safe(p, tmp, &microcode_cache, list) {
		__list_del(p->list.prev, p->list.next);
		kfree(p->data);
		kfree(p);
	}
}

static struct hygon_ucode_patch *find_hygon_patch(unsigned int cpu)
{
	struct ucode_cpu_info *u_cpu_info = ucode_cpu_info + cpu;
	u16 equiv_id;

	u_cpu_info->cpu_sig.rev = get_current_patch_level();

	equiv_id = find_equiv_id(&equiv_table, u_cpu_info->cpu_sig.sig);

	return equiv_id ? find_cached_patch(equiv_id) : NULL;
}

void reload_ucode_hygon(unsigned int cpu)
{
	struct hygon_microcode *mc;
	struct hygon_ucode_patch *p;
	u32 rev;

	p = find_hygon_patch(cpu);
	if (!p)
		return;

	mc = p->data;

	rev = get_current_patch_level();
	if (rev < mc->header.patch_level) {
		if (apply_hygon_patch(mc, &rev, p->patch_size))
			pr_info_once("reload revision: 0x%08x\n", rev);
	}
}

static int hygon_collect_cpu_info(int cpu, struct cpu_signature *csig)
{
	struct ucode_cpu_info *u_cpu_info = ucode_cpu_info + cpu;
	struct hygon_ucode_patch *p;

	csig->sig = cpuid_eax(0x00000001);
	csig->rev = get_current_patch_level();

	p = find_hygon_patch(cpu);
	if (p && (p->patch_level == csig->rev))
		u_cpu_info->mc = p->data;

	return 0;
}

static enum ucode_state hygon_apply_microcode(int cpu)
{
	struct cpuinfo_x86 *c = &cpu_data(cpu);
	struct hygon_microcode *mc_hygon;
	struct ucode_cpu_info *u_cpu_info;
	struct hygon_ucode_patch *p;
	enum ucode_state ret;
	u32 rev;

	if (raw_smp_processor_id() != cpu) {
		WARN_ON_ONCE(1);
		return UCODE_ERROR;
	}

	u_cpu_info = ucode_cpu_info + cpu;

	p = find_hygon_patch(cpu);
	if (!p)
		return UCODE_NFOUND;

	rev = u_cpu_info->cpu_sig.rev;

	mc_hygon  = p->data;
	u_cpu_info->mc = p->data;

	 /* Skip update if current revision is already newer than the patch */
	if (rev > mc_hygon->header.patch_level) {
		ret = UCODE_OK;
		goto out;
	}

	if (!apply_hygon_patch(mc_hygon, &rev, p->patch_size)) {
		pr_err("CPU%d: update failed for patch_level=0x%08x\n",
			cpu, mc_hygon->header.patch_level);
		return UCODE_ERROR;
	}

	rev = mc_hygon->header.patch_level;
	ret = UCODE_UPDATED;

out:
	u_cpu_info->cpu_sig.rev = rev;
	c->microcode	 = rev;

	/* Update boot CPU's microcode revision as well */
	if (c->cpu_index == boot_cpu_data.cpu_index)
		boot_cpu_data.microcode = rev;

	return ret;
}

void load_ucode_hygon_ap(unsigned int cpuid_1_eax)
{
	unsigned int cpu = smp_processor_id();

	ucode_cpu_info[cpu].cpu_sig.sig = cpuid_1_eax;
	hygon_apply_microcode(cpu);
}

static size_t load_equiv_cpu_table(const u8 *buf, size_t buf_size)
{
	u32 equiv_tbl_len;

	if (!verify_cpu_equivalence_table(buf, buf_size))
		return 0;

	equiv_tbl_len = hygon_container_hdr(buf, LENGTH);

	equiv_table.entry = vmalloc(equiv_tbl_len);
	if (!equiv_table.entry) {
		pr_err("failed to allocate equivalent CPU table\n");
		return 0;
	}

	memcpy(equiv_table.entry, hygon_container_data(buf), equiv_tbl_len);
	equiv_table.entry_count = equiv_tbl_len / sizeof(struct hygon_cpu_equiv_entry);

	return equiv_tbl_len + HYGON_CONTAINER_HDR_SIZE;
}

static void clear_equiv_cpu_table(void)
{
	vfree(equiv_table.entry);
	memset(&equiv_table, 0, sizeof(equiv_table));
}

static void hygon_cleanup_microcode(void)
{
	clear_equiv_cpu_table();
	clear_microcode_cache();
}

/*
 * Return a non-negative value even if some of the checks failed so that
 * we can skip over the next patch. If we return a negative value, we
 * signal a grave error like a memory allocation has failed and the
 * driver cannot continue functioning normally. In such cases, we tear
 * down everything we've used up so far and exit.
 */
static int try_verify_and_cache_patch(u8 family, u8 *fw, unsigned int leftover,
				unsigned int *patch_size)
{
	struct hygon_microcode_header *mc_hdr;
	struct hygon_ucode_patch *patch;
	int ret;

	ret = validate_patch_section(fw, leftover, patch_size);
	if (ret)
		return ret;

	patch = kzalloc_obj(*patch);
	if (!patch) {
		pr_err("Patch allocation failure.\n");
		return -EINVAL;
	}

	patch->data = kmemdup(hygon_section_data(fw), *patch_size, GFP_KERNEL);
	if (!patch->data) {
		pr_err("Patch data allocation failure.\n");
		kfree(patch);
		return -EINVAL;
	}
	patch->patch_size = *patch_size;

	mc_hdr = (struct hygon_microcode_header *)hygon_section_data(fw);

	INIT_LIST_HEAD(&patch->list);
	patch->patch_level  = mc_hdr->patch_level;
	patch->equiv_id = mc_hdr->processor_rev_id;

	ucode_dbg("%s: Adding patch_level: 0x%08x, proc_id: 0x%04x\n",
		 __func__, patch->patch_level, patch->equiv_id);

	add_to_cache(patch);

	return 0;
}

/* Scan the blob in @data and add microcode patches to the cache. */
static enum ucode_state parse_and_cache_patches(u8 family, const u8 *data, size_t size)
{
	u8 *fw = (u8 *)data;
	size_t offset;

	offset = load_equiv_cpu_table(data, size);
	if (!offset)
		return UCODE_ERROR;

	fw   += offset;
	size -= offset;
	if (size < HYGON_SECTION_HDR_SIZE) {
		pr_err("no patch sections in container file\n");
		return UCODE_ERROR;
	}

	if (hygon_section_hdr(fw, TYPE) != UCODE_UCODE_TYPE) {
		pr_err("invalid type field in container file section header\n");
		clear_equiv_cpu_table();
		return UCODE_ERROR;
	}

	while (size > 0) {
		unsigned int crnt_size = 0;
		int ret;

		ret = try_verify_and_cache_patch(family, fw, size, &crnt_size);
		if (ret < 0)
			return UCODE_ERROR;

		fw   +=  crnt_size + HYGON_SECTION_HDR_SIZE;
		size -= (crnt_size + HYGON_SECTION_HDR_SIZE);
	}

	return UCODE_OK;
}

static enum ucode_state prepare_microcode_blob(u8 family, const u8 *data, size_t size)
{
	enum ucode_state ret;

	clear_equiv_cpu_table();

	ret = parse_and_cache_patches(family, data, size);
	if (ret != UCODE_OK)
		hygon_cleanup_microcode();

	return ret;
}

static enum ucode_state hygon_load_microcode(u8 family, const u8 *data, size_t size)
{
	struct cpuinfo_x86 *c;
	unsigned int nid, cpu;
	struct hygon_ucode_patch *p;
	enum ucode_state ret;

	ret = prepare_microcode_blob(family, data, size);
	if (ret != UCODE_OK)
		return ret;

	for_each_node_with_cpus(nid) {
		cpu = cpumask_first(cpumask_of_node(nid));
		c = &cpu_data(cpu);

		p = find_hygon_patch(cpu);
		if (!p)
			continue;

		if (c->microcode >= p->patch_level)
			continue;

		ret = UCODE_NEW;
	}

	return ret;
}

static int __init save_microcode_in_initrd(void)
{
	struct cpuinfo_x86 *c = &boot_cpu_data;
	struct hygon_container_desc desc = { 0 };
	unsigned int cpuid_1_eax;
	enum ucode_state ret;
	struct cpio_data cp;

	if (microcode_loader_disabled() || (c->x86_vendor != X86_VENDOR_HYGON))
		return 0;

	cpuid_1_eax = native_cpuid_eax(1);

	if (!find_microcode_blob(&cp))
		return -EINVAL;

	locate_patch_container(cp.data, cp.size, &desc);
	if (!desc.mc)
		return -EINVAL;

	ret = prepare_microcode_blob(x86_family(cpuid_1_eax), desc.data, desc.size);
	return (ret > UCODE_UPDATED) ? -EINVAL : 0;
}
early_initcall(save_microcode_in_initrd);

/*
 * Hygon microcode firmware naming are in family-specific firmware files:
 *    hygon-ucode/microcode_hygon_fam18h.bin
 */
static enum ucode_state hygon_request_microcode(int cpu, struct device *device)
{
	struct cpuinfo_x86 *c = &cpu_data(cpu);
	enum ucode_state ret = UCODE_NFOUND;
	const struct firmware *fw;

	if (force_minrev)
		return UCODE_NFOUND;

	if (request_firmware_direct(&fw, (const char *)HYGON_UCODE_FW_NAME, device)) {
		ucode_dbg("failed to load file %s\n", HYGON_UCODE_FW_NAME);
		goto out;
	}

	ret = UCODE_ERROR;
	if (!is_valid_microcode_container(fw->data, fw->size))
		goto fw_release;

	ret = hygon_load_microcode(c->x86, fw->data, fw->size);

 fw_release:
	release_firmware(fw);

 out:
	return ret;
}

static void hygon_microcode_fini_cpu(int cpu)
{
	struct ucode_cpu_info *u_cpu_info = ucode_cpu_info + cpu;

	u_cpu_info->mc = NULL;
}

static void hygon_finalize_late_load(int result)
{
	if (result)
		hygon_cleanup_microcode();
}

static struct microcode_ops microcode_hygon_ops = {
	.request_microcode_fw	= hygon_request_microcode,
	.collect_cpu_info	= hygon_collect_cpu_info,
	.apply_microcode	= hygon_apply_microcode,
	.microcode_fini_cpu	= hygon_microcode_fini_cpu,
	.finalize_late_load	= hygon_finalize_late_load,
	.nmi_safe		= true,
};

struct microcode_ops * __init init_hygon_microcode(void)
{
	if (boot_cpu_data.x86_vendor != X86_VENDOR_HYGON)
		return NULL;

	return &microcode_hygon_ops;
}

void __exit exit_hygon_microcode(void)
{
	hygon_cleanup_microcode();
}
