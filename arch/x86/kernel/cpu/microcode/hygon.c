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

struct ucode_patch {
	struct list_head plist;
	void *data;
	unsigned int size;
	u32 patch_id;
	u16 equiv_cpu;
};

static LIST_HEAD(microcode_cache);

#define UCODE_MAGIC			0x00414d44
#define UCODE_EQUIV_CPU_TABLE_TYPE	0x00000000
#define UCODE_UCODE_TYPE		0x00000001

#define SECTION_HDR_SIZE		8
#define CONTAINER_HDR_SZ		12

struct equiv_cpu_entry {
	u32	installed_cpu;
	u32	fixed_errata_mask;
	u32	fixed_errata_compare;
	u16	equiv_cpu;
	u16	res;
} __packed;

struct microcode_header_hygon {
	u32	data_code;
	u32	patch_id;
	u16	mc_patch_data_id;
	u8	mc_patch_data_len;
	u8	init_flag;
	u32	mc_patch_data_checksum;
	u32	nb_dev_id;
	u32	sb_dev_id;
	u16	processor_rev_id;
	u8	nb_rev_id;
	u8	sb_rev_id;
	u8	bios_api_rev;
	u8	reserved1[3];
	u32	match_reg[8];
} __packed;

struct microcode_hygon {
	struct microcode_header_hygon	hdr;
	unsigned int			mpb[];
};

static struct equiv_cpu_table {
	unsigned int num_entries;
	struct equiv_cpu_entry *entry;
} equiv_table;

union cpuid_1_eax {
	struct {
		__u32 stepping    : 4,
		      model	  : 4,
		      family	  : 4,
		      __reserved0 : 4,
		      ext_model   : 4,
		      ext_fam     : 8,
		      __reserved1 : 4;
	};
	__u32 full;
};

/*
 * This points to the current valid container of microcode patches which we will
 * save from the initrd/builtin before jettisoning its contents. @mc is the
 * microcode patch we found to match.
 */
struct cont_desc {
	struct microcode_hygon *mc;
	u32		     psize;
	u8		     *data;
	size_t		     size;
};

/*
 * Microcode patch container file is prepended to the initrd in cpio
 * format. See Documentation/arch/x86/microcode.rst
 */
static const char
ucode_path[] __maybe_unused = "kernel/x86/microcode/HygonGenuine.bin";

static u32 bsp_cpuid_1_eax __ro_after_init;

static u32 get_patch_level(void)
{
	u32 rev, dummy __always_unused;

	native_rdmsr(MSR_AMD64_PATCH_LEVEL, rev, dummy);

	return rev;
}

static u16 find_equiv_id(struct equiv_cpu_table *et, u32 sig)
{
	unsigned int i;

	if (!et || !et->num_entries)
		return 0;

	for (i = 0; i < et->num_entries; i++) {
		struct equiv_cpu_entry *e = &et->entry[i];

		if (sig == e->installed_cpu)
			return e->equiv_cpu;
	}
	return 0;
}

/*
 * Check whether there is a valid microcode container file at the beginning
 * of @buf of size @buf_size.
 */
static bool verify_container(const u8 *buf, size_t buf_size)
{
	u32 cont_magic;

	if (buf_size <= CONTAINER_HDR_SZ) {
		ucode_dbg("Truncated microcode container header.\n");
		return false;
	}

	cont_magic = *(const u32 *)buf;
	if (cont_magic != UCODE_MAGIC) {
		ucode_dbg("Invalid magic value (0x%08x).\n", cont_magic);
		return false;
	}

	return true;
}

/*
 * Check whether there is a valid, non-truncated CPU equivalence table at the
 * beginning of @buf of size @buf_size.
 */
static bool verify_equivalence_table(const u8 *buf, size_t buf_size)
{
	const u32 *hdr = (const u32 *)buf;
	u32 cont_type, equiv_tbl_len;

	if (!verify_container(buf, buf_size))
		return false;

	cont_type = hdr[1];
	if (cont_type != UCODE_EQUIV_CPU_TABLE_TYPE) {
		ucode_dbg("Wrong microcode container equivalence table type: %u.\n",
			  cont_type);
		return false;
	}

	buf_size -= CONTAINER_HDR_SZ;

	equiv_tbl_len = hdr[2];
	if (equiv_tbl_len < sizeof(struct equiv_cpu_entry) ||
	    buf_size < equiv_tbl_len) {
		ucode_dbg("Truncated equivalence table.\n");
		return false;
	}

	return true;
}

/*
 * Check whether there is a valid, non-truncated microcode patch section at the
 * beginning of @buf of size @buf_size.
 *
 * On success, @sh_psize returns the patch size according to the section header,
 * to the caller.
 */
static bool __verify_patch_section(const u8 *buf, size_t buf_size, u32 *sh_psize)
{
	u32 p_type, p_size;
	const u32 *hdr;

	if (buf_size < SECTION_HDR_SIZE) {
		ucode_dbg("Truncated patch section.\n");
		return false;
	}

	hdr = (const u32 *)buf;
	p_type = hdr[0];
	p_size = hdr[1];

	if (p_type != UCODE_UCODE_TYPE) {
		ucode_dbg("Invalid type field (0x%x) in container file section header.\n",
			  p_type);
		return false;
	}

	if (p_size < sizeof(struct microcode_header_hygon)) {
		ucode_dbg("Patch of size %u too short.\n", p_size);
		return false;
	}

	*sh_psize = p_size;

	return true;
}

/*
 * Check whether the passed remaining file @buf_size is large enough to contain
 * a patch of the indicated @sh_psize (and also whether this size does not
 * exceed the per-family maximum). @sh_psize is the size read from the section
 * header.
 */
static bool __verify_patch_size(u32 sh_psize, size_t buf_size)
{
	/* Working with the whole buffer so < is ok. */
	return sh_psize <= buf_size;
}

/*
 * Verify the patch in @buf.
 *
 * Returns:
 * negative: on error
 * positive: patch is not for this family, skip it
 * 0: success
 */
static int verify_patch(const u8 *buf, size_t buf_size, u32 *patch_size)
{
	u8 family = x86_family(bsp_cpuid_1_eax);
	struct microcode_header_hygon *mc_hdr;
	u32 sh_psize;
	u16 proc_id;
	u8 patch_fam;

	if (!__verify_patch_section(buf, buf_size, &sh_psize))
		return -1;

	/*
	 * The section header length is not included in this indicated size
	 * but is present in the leftover file length so we need to subtract
	 * it before passing this value to the function below.
	 */
	buf_size -= SECTION_HDR_SIZE;

	/*
	 * Check if the remaining buffer is big enough to contain a patch of
	 * size sh_psize, as the section claims.
	 */
	if (buf_size < sh_psize) {
		ucode_dbg("Patch of size %u truncated.\n", sh_psize);
		return -1;
	}

	if (!__verify_patch_size(sh_psize, buf_size)) {
		ucode_dbg("Per-family patch size mismatch.\n");
		return -1;
	}

	*patch_size = sh_psize;

	mc_hdr	= (struct microcode_header_hygon *)(buf + SECTION_HDR_SIZE);
	if (mc_hdr->nb_dev_id || mc_hdr->sb_dev_id) {
		pr_err("Patch-ID 0x%08x: chipset-specific code unsupported.\n", mc_hdr->patch_id);
		return -1;
	}

	proc_id	= mc_hdr->processor_rev_id;
	patch_fam = 0xf + (proc_id >> 12);

	if (patch_fam != family)
		return 1;

	return 0;
}

/*
 * This scans the ucode blob for the proper container as we can have multiple
 * containers glued together.
 *
 * Returns the amount of bytes consumed while scanning. @desc contains all the
 * data we're going to use in later stages of the application.
 */
static size_t parse_container(u8 *ucode, size_t size, struct cont_desc *desc)
{
	struct equiv_cpu_table table;
	size_t orig_size = size;
	u32 *hdr = (u32 *)ucode;
	u16 eq_id;
	u8 *buf;

	if (!verify_equivalence_table(ucode, size))
		return 0;

	buf = ucode;

	table.entry = (struct equiv_cpu_entry *)(buf + CONTAINER_HDR_SZ);
	table.num_entries = hdr[2] / sizeof(struct equiv_cpu_entry);

	/*
	 * Find the equivalence ID of our CPU in this table. Even if this table
	 * doesn't contain a patch for the CPU, scan through the whole container
	 * so that it can be skipped in case there are other containers appended.
	 */
	eq_id = find_equiv_id(&table, bsp_cpuid_1_eax);

	buf  += hdr[2] + CONTAINER_HDR_SZ;
	size -= hdr[2] + CONTAINER_HDR_SZ;

	/*
	 * Scan through the rest of the container to find where it ends. We do
	 * some basic sanity-checking too.
	 */
	while (size > 0) {
		struct microcode_hygon *mc;
		u32 patch_size;
		int ret;

		ret = verify_patch(buf, size, &patch_size);
		if (ret < 0) {
			/*
			 * Patch verification failed, skip to the next container, if
			 * there is one. Before exit, check whether that container has
			 * found a patch already. If so, use it.
			 */
			goto out;
		} else if (ret > 0) {
			goto skip;
		}

		mc = (struct microcode_hygon *)(buf + SECTION_HDR_SIZE);

		if (eq_id == mc->hdr.processor_rev_id) {
			desc->psize = patch_size;
			desc->mc = mc;

			ucode_dbg(" match: size: %d\n", patch_size);
		}

skip:
		/* Skip patch section header too: */
		buf  += patch_size + SECTION_HDR_SIZE;
		size -= patch_size + SECTION_HDR_SIZE;
	}

out:
	/*
	 * If we have found a patch (desc->mc), it means we're looking at the
	 * container which has a patch for this CPU so return 0 to mean, @ucode
	 * already points to the proper container. Otherwise, we return the size
	 * we scanned so that we can advance to the next container in the
	 * buffer.
	 */
	if (desc->mc) {
		desc->data = ucode;
		desc->size = orig_size - size;

		return 0;
	}

	return orig_size - size;
}

/*
 * Scan the ucode blob for the proper container as we can have multiple
 * containers glued together.
 */
static void scan_containers(u8 *ucode, size_t size, struct cont_desc *desc)
{
	while (size) {
		size_t s = parse_container(ucode, size, desc);

		if (!s)
			return;

		/* catch wraparound */
		if (size >= s) {
			ucode += s;
			size  -= s;
		} else {
			return;
		}
	}
}

static bool __apply_microcode_hygon(struct microcode_hygon *mc, u32 *cur_rev,
				  unsigned int psize)
{
	unsigned long p_addr, p_addr_end;

	p_addr = (unsigned long)&mc->hdr.data_code;
	native_wrmsrq(MSR_AMD64_PATCH_LOADER, p_addr);
	p_addr_end = p_addr + psize - 1;
	invlpg(p_addr);

	/*
	 * Flush next page too if patch image is crossing a page
	 * boundary.
	 */
	if (p_addr >> PAGE_SHIFT != p_addr_end >> PAGE_SHIFT)
		invlpg(p_addr_end);

	if (IS_ENABLED(CONFIG_MICROCODE_DBG) && hypervisor_present)
		microcode_rev[smp_processor_id()] = mc->hdr.patch_id;

	/* verify patch application was successful */
	*cur_rev = get_patch_level();

	ucode_dbg("updated rev: 0x%x\n", *cur_rev);

	if (*cur_rev != mc->hdr.patch_id)
		return false;

	return true;
}

static bool get_builtin_microcode(struct cpio_data *cp)
{
	char fw_name[40] = "hygon-ucode/microcode_hygon_fam18h.bin";
	struct firmware fw;

	if (IS_ENABLED(CONFIG_X86_32))
		return false;

	if (firmware_request_builtin(&fw, fw_name)) {
		cp->size = fw.size;
		cp->data = (void *)fw.data;
		return true;
	}

	return false;
}

static bool __init find_blobs_in_containers(struct cpio_data *ret)
{
	struct cpio_data cp;
	bool found;

	if (!get_builtin_microcode(&cp))
		cp = find_microcode_in_initrd(ucode_path);

	found = cp.data && cp.size;
	if (found)
		*ret = cp;

	return found;
}

/*
 * Early load occurs before we can vmalloc(). So we look for the microcode
 * patch container file in initrd, traverse equivalent cpu table, look for a
 * matching microcode patch, and update, all in initrd memory in place.
 * When vmalloc() is available for use later -- on 64-bit during first AP load,
 * and on 32-bit during save_microcode_in_initrd() -- we can call
 * load_microcode_hygon() to save equivalent cpu table and microcode patches in
 * kernel heap memory.
 */
void __init load_ucode_hygon_bsp(struct early_load_data *ed, unsigned int cpuid_1_eax)
{
	struct cont_desc desc = { };
	struct microcode_hygon *mc;
	struct cpio_data cp = { };
	u32 rev;

	bsp_cpuid_1_eax = cpuid_1_eax;

	rev = get_patch_level();
	ed->old_rev = rev;

	/* Needed in load_microcode_hygon() */
	ucode_cpu_info[0].cpu_sig.sig = cpuid_1_eax;

	if (!find_blobs_in_containers(&cp))
		return;

	scan_containers(cp.data, cp.size, &desc);

	mc = desc.mc;
	if (!mc)
		return;

	/*
	 * Allow application of the same revision to pick up SMT-specific
	 * changes even if the revision of the other SMT thread is already
	 * up-to-date.
	 */
	if (ed->old_rev > mc->hdr.patch_id)
		return;

	if (__apply_microcode_hygon(mc, &rev, desc.psize))
		ed->new_rev = rev;
}

/*
 * a small, trivial cache of per-family ucode patches
 */
static struct ucode_patch *cache_find_patch(struct ucode_cpu_info *uci, u16 equiv_cpu)
{
	struct ucode_patch *p;

	list_for_each_entry(p, &microcode_cache, plist)
		if (p->equiv_cpu == equiv_cpu)
			return p;

	return NULL;
}

static void update_cache(struct ucode_patch *new_patch)
{
	struct ucode_patch *p;

	list_for_each_entry(p, &microcode_cache, plist) {
		if (p->equiv_cpu == new_patch->equiv_cpu) {
			if (p->patch_id >= new_patch->patch_id) {
				/* we already have the latest patch */
				kfree(new_patch->data);
				kfree(new_patch);
				return;
			}

			list_replace(&p->plist, &new_patch->plist);
			kfree(p->data);
			kfree(p);
			return;
		}
	}
	/* no patch found, add it */
	list_add_tail(&new_patch->plist, &microcode_cache);
}

static void free_cache(void)
{
	struct ucode_patch *p, *tmp;

	list_for_each_entry_safe(p, tmp, &microcode_cache, plist) {
		__list_del(p->plist.prev, p->plist.next);
		kfree(p->data);
		kfree(p);
	}
}

static struct ucode_patch *find_patch(unsigned int cpu)
{
	struct ucode_cpu_info *uci = ucode_cpu_info + cpu;
	u16 equiv_id = 0;

	uci->cpu_sig.rev = get_patch_level();

	equiv_id = find_equiv_id(&equiv_table, uci->cpu_sig.sig);
	if (!equiv_id)
		return NULL;

	return cache_find_patch(uci, equiv_id);
}

void reload_ucode_hygon(unsigned int cpu)
{
	u32 rev, dummy __always_unused;
	struct microcode_hygon *mc;
	struct ucode_patch *p;

	p = find_patch(cpu);
	if (!p)
		return;

	mc = p->data;

	rev = get_patch_level();
	if (rev < mc->hdr.patch_id) {
		if (__apply_microcode_hygon(mc, &rev, p->size))
			pr_info_once("reload revision: 0x%08x\n", rev);
	}
}

static int collect_cpu_info_hygon(int cpu, struct cpu_signature *csig)
{
	struct ucode_cpu_info *uci = ucode_cpu_info + cpu;
	struct ucode_patch *p;

	csig->sig = cpuid_eax(0x00000001);
	csig->rev = get_patch_level();

	/*
	 * a patch could have been loaded early, set uci->mc so that
	 * mc_bp_resume() can call apply_microcode()
	 */
	p = find_patch(cpu);
	if (p && (p->patch_id == csig->rev))
		uci->mc = p->data;

	return 0;
}

static enum ucode_state apply_microcode_hygon(int cpu)
{
	struct cpuinfo_x86 *c = &cpu_data(cpu);
	struct microcode_hygon *mc_hygon;
	struct ucode_cpu_info *uci;
	struct ucode_patch *p;
	enum ucode_state ret;
	u32 rev;

	BUG_ON(raw_smp_processor_id() != cpu);

	uci = ucode_cpu_info + cpu;

	p = find_patch(cpu);
	if (!p)
		return UCODE_NFOUND;

	rev = uci->cpu_sig.rev;

	mc_hygon  = p->data;
	uci->mc = p->data;

	/* need to apply patch? */
	if (rev > mc_hygon->hdr.patch_id) {
		ret = UCODE_OK;
		goto out;
	}

	if (!__apply_microcode_hygon(mc_hygon, &rev, p->size)) {
		pr_err("CPU%d: update failed for patch_level=0x%08x\n",
			cpu, mc_hygon->hdr.patch_id);
		return UCODE_ERROR;
	}

	rev = mc_hygon->hdr.patch_id;
	ret = UCODE_UPDATED;

out:
	uci->cpu_sig.rev = rev;
	c->microcode	 = rev;

	/* Update boot_cpu_data's revision too, if we're on the BSP: */
	if (c->cpu_index == boot_cpu_data.cpu_index)
		boot_cpu_data.microcode = rev;

	return ret;
}

void load_ucode_hygon_ap(unsigned int cpuid_1_eax)
{
	unsigned int cpu = smp_processor_id();

	ucode_cpu_info[cpu].cpu_sig.sig = cpuid_1_eax;
	apply_microcode_hygon(cpu);
}

static size_t install_equiv_cpu_table(const u8 *buf, size_t buf_size)
{
	u32 equiv_tbl_len;
	const u32 *hdr;

	if (!verify_equivalence_table(buf, buf_size))
		return 0;

	hdr = (const u32 *)buf;
	equiv_tbl_len = hdr[2];

	equiv_table.entry = vmalloc(equiv_tbl_len);
	if (!equiv_table.entry) {
		pr_err("failed to allocate equivalent CPU table\n");
		return 0;
	}

	memcpy(equiv_table.entry, buf + CONTAINER_HDR_SZ, equiv_tbl_len);
	equiv_table.num_entries = equiv_tbl_len / sizeof(struct equiv_cpu_entry);

	/* add header length */
	return equiv_tbl_len + CONTAINER_HDR_SZ;
}

static void free_equiv_cpu_table(void)
{
	vfree(equiv_table.entry);
	memset(&equiv_table, 0, sizeof(equiv_table));
}

static void cleanup(void)
{
	free_equiv_cpu_table();
	free_cache();
}

/*
 * Return a non-negative value even if some of the checks failed so that
 * we can skip over the next patch. If we return a negative value, we
 * signal a grave error like a memory allocation has failed and the
 * driver cannot continue functioning normally. In such cases, we tear
 * down everything we've used up so far and exit.
 */
static int verify_and_add_patch(u8 family, u8 *fw, unsigned int leftover,
				unsigned int *patch_size)
{
	struct microcode_header_hygon *mc_hdr;
	struct ucode_patch *patch;
	u16 proc_id;
	int ret;

	ret = verify_patch(fw, leftover, patch_size);
	if (ret)
		return ret;

	patch = kzalloc_obj(*patch);
	if (!patch) {
		pr_err("Patch allocation failure.\n");
		return -EINVAL;
	}

	patch->data = kmemdup(fw + SECTION_HDR_SIZE, *patch_size, GFP_KERNEL);
	if (!patch->data) {
		pr_err("Patch data allocation failure.\n");
		kfree(patch);
		return -EINVAL;
	}
	patch->size = *patch_size;

	mc_hdr      = (struct microcode_header_hygon *)(fw + SECTION_HDR_SIZE);
	proc_id     = mc_hdr->processor_rev_id;

	INIT_LIST_HEAD(&patch->plist);
	patch->patch_id  = mc_hdr->patch_id;
	patch->equiv_cpu = proc_id;

	ucode_dbg("%s: Adding patch_id: 0x%08x, proc_id: 0x%04x\n",
		 __func__, patch->patch_id, proc_id);

	/* ... and add to cache. */
	update_cache(patch);

	return 0;
}

/* Scan the blob in @data and add microcode patches to the cache. */
static enum ucode_state __load_microcode_hygon(u8 family, const u8 *data, size_t size)
{
	u8 *fw = (u8 *)data;
	size_t offset;

	offset = install_equiv_cpu_table(data, size);
	if (!offset)
		return UCODE_ERROR;

	fw   += offset;
	size -= offset;

	if (*(u32 *)fw != UCODE_UCODE_TYPE) {
		pr_err("invalid type field in container file section header\n");
		free_equiv_cpu_table();
		return UCODE_ERROR;
	}

	while (size > 0) {
		unsigned int crnt_size = 0;
		int ret;

		ret = verify_and_add_patch(family, fw, size, &crnt_size);
		if (ret < 0)
			return UCODE_ERROR;

		fw   +=  crnt_size + SECTION_HDR_SIZE;
		size -= (crnt_size + SECTION_HDR_SIZE);
	}

	return UCODE_OK;
}

static enum ucode_state _load_microcode_hygon(u8 family, const u8 *data, size_t size)
{
	enum ucode_state ret;

	/* free old equiv table */
	free_equiv_cpu_table();

	ret = __load_microcode_hygon(family, data, size);
	if (ret != UCODE_OK)
		cleanup();

	return ret;
}

static enum ucode_state load_microcode_hygon(u8 family, const u8 *data, size_t size)
{
	struct cpuinfo_x86 *c;
	unsigned int nid, cpu;
	struct ucode_patch *p;
	enum ucode_state ret;

	ret = _load_microcode_hygon(family, data, size);
	if (ret != UCODE_OK)
		return ret;

	for_each_node_with_cpus(nid) {
		cpu = cpumask_first(cpumask_of_node(nid));
		c = &cpu_data(cpu);

		p = find_patch(cpu);
		if (!p)
			continue;

		if (c->microcode >= p->patch_id)
			continue;

		ret = UCODE_NEW;
	}

	return ret;
}

static int __init save_microcode_in_initrd(void)
{
	struct cpuinfo_x86 *c = &boot_cpu_data;
	struct cont_desc desc = { 0 };
	unsigned int cpuid_1_eax;
	enum ucode_state ret;
	struct cpio_data cp;

	if (microcode_loader_disabled() || (c->x86_vendor != X86_VENDOR_HYGON))
		return 0;

	cpuid_1_eax = native_cpuid_eax(1);

	if (!find_blobs_in_containers(&cp))
		return -EINVAL;

	scan_containers(cp.data, cp.size, &desc);
	if (!desc.mc)
		return -EINVAL;

	ret = _load_microcode_hygon(x86_family(cpuid_1_eax), desc.data, desc.size);
	if (ret > UCODE_UPDATED)
		return -EINVAL;

	return 0;
}
early_initcall(save_microcode_in_initrd);

/*
 * Hygon microcode firmware naming are in family-specific firmware files:
 *    hygon-ucode/microcode_hygon_fam18h.bin
 */
static enum ucode_state request_microcode_hygon(int cpu, struct device *device)
{
	char fw_name[40] = "hygon-ucode/microcode_hygon_fam18h.bin";
	struct cpuinfo_x86 *c = &cpu_data(cpu);
	enum ucode_state ret = UCODE_NFOUND;
	const struct firmware *fw;

	if (force_minrev)
		return UCODE_NFOUND;

	if (request_firmware_direct(&fw, (const char *)fw_name, device)) {
		ucode_dbg("failed to load file %s\n", fw_name);
		goto out;
	}

	ret = UCODE_ERROR;
	if (!verify_container(fw->data, fw->size))
		goto fw_release;

	ret = load_microcode_hygon(c->x86, fw->data, fw->size);

 fw_release:
	release_firmware(fw);

 out:
	return ret;
}

static void microcode_fini_cpu_hygon(int cpu)
{
	struct ucode_cpu_info *uci = ucode_cpu_info + cpu;

	uci->mc = NULL;
}

static void finalize_late_load_hygon(int result)
{
	if (result)
		cleanup();
}

static struct microcode_ops microcode_hygon_ops = {
	.request_microcode_fw	= request_microcode_hygon,
	.collect_cpu_info	= collect_cpu_info_hygon,
	.apply_microcode	= apply_microcode_hygon,
	.microcode_fini_cpu	= microcode_fini_cpu_hygon,
	.finalize_late_load	= finalize_late_load_hygon,
	.nmi_safe		= true,
};

struct microcode_ops * __init init_hygon_microcode(void)
{
	struct cpuinfo_x86 *c = &boot_cpu_data;

	if (c->x86_vendor != X86_VENDOR_HYGON)
		return NULL;

	return &microcode_hygon_ops;
}

void __exit exit_hygon_microcode(void)
{
	cleanup();
}
