// SPDX-License-Identifier: GPL-2.0

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/crash_core.h>
#include <linux/dma-map-ops.h>
#include <linux/vmcore_info.h>
#include <linux/notifier.h>
#include <linux/meminspect.h>

static DEFINE_MUTEX(meminspect_lock);
static struct inspect_entry inspect_entries[MEMINSPECT_ID_MAX];

ATOMIC_NOTIFIER_HEAD(meminspect_notifier_list);

#ifdef CONFIG_CRASH_DUMP

#define CORE_STR "CORE"

static struct elfhdr *ehdr;
static size_t elf_offset;
static bool elf_hdr_ready;

static void append_kcore_note(char *notes, size_t *i, const char *name,
			      unsigned int type, const void *desc,
			      size_t descsz)
{
	struct elf_note *note = (struct elf_note *)&notes[*i];

	note->n_namesz = strlen(name) + 1;
	note->n_descsz = descsz;
	note->n_type = type;
	*i += sizeof(*note);
	memcpy(&notes[*i], name, note->n_namesz);
	*i = ALIGN(*i + note->n_namesz, 4);
	memcpy(&notes[*i], desc, descsz);
	*i = ALIGN(*i + descsz, 4);
}

static void append_kcore_note_nodesc(char *notes, size_t *i, const char *name,
				     unsigned int type, size_t descsz)
{
	struct elf_note *note = (struct elf_note *)&notes[*i];

	note->n_namesz = strlen(name) + 1;
	note->n_descsz = descsz;
	note->n_type = type;
	*i += sizeof(*note);
	memcpy(&notes[*i], name, note->n_namesz);
	*i = ALIGN(*i + note->n_namesz, 4);
}

static struct elf_phdr *elf_phdr_entry_addr(struct elfhdr *ehdr, int idx)
{
	struct elf_phdr *ephdr = (struct elf_phdr *)((size_t)ehdr + ehdr->e_phoff);

	return &ephdr[idx];
}

static int clear_elfheader(const struct inspect_entry *e)
{
	struct elf_phdr *phdr;
	struct elf_phdr *tmp_phdr;
	unsigned int phidx;
	unsigned int i;

	for (i = 0; i < ehdr->e_phnum; i++) {
		phdr = elf_phdr_entry_addr(ehdr, i);
		if (phdr->p_paddr == e->pa &&
		    phdr->p_memsz == ALIGN(e->size, 4))
			break;
	}

	if (i == ehdr->e_phnum) {
		pr_debug("Cannot find program header entry in elf\n");
		return -EINVAL;
	}

	phidx = i;

	/* Clear program header */
	tmp_phdr = elf_phdr_entry_addr(ehdr, phidx);
	for (i = phidx; i < ehdr->e_phnum - 1; i++) {
		tmp_phdr = elf_phdr_entry_addr(ehdr, i + 1);
		phdr = elf_phdr_entry_addr(ehdr, i);
		memcpy(phdr, tmp_phdr, sizeof(*phdr));
		phdr->p_offset = phdr->p_offset - ALIGN(e->size, 4);
	}
	memset(tmp_phdr, 0, sizeof(*tmp_phdr));
	ehdr->e_phnum--;

	elf_offset -= ALIGN(e->size, 4);

	return 0;
}

static void update_elfheader(const struct inspect_entry *e)
{
	struct elf_phdr *phdr;

	phdr = elf_phdr_entry_addr(ehdr, ehdr->e_phnum++);

	phdr->p_type = PT_LOAD;
	phdr->p_offset = elf_offset;
	phdr->p_vaddr = (elf_addr_t)e->va;
	if (e->pa)
		phdr->p_paddr = (elf_addr_t)e->pa;
	else
		phdr->p_paddr = (elf_addr_t)virt_to_phys(e->va);
	phdr->p_filesz = phdr->p_memsz = ALIGN(e->size, 4);
	phdr->p_flags = PF_R | PF_W;

	elf_offset += ALIGN(e->size, 4);
}

/*
 * This function prepares the elf header for the coredump image.
 * Initially there is a single program header for the elf NOTE.
 * The note contains the usual core dump information, and the vmcoreinfo.
 */
static int init_elfheader(void)
{
	struct elf_phdr *phdr;
	void *notes;
	unsigned int elfh_size, buf_sz;
	unsigned int phdr_off;
	size_t note_len, i = 0;
	struct page *p;

	struct elf_prstatus prstatus = {};
	struct elf_prpsinfo prpsinfo = {
		.pr_sname = 'R',
		.pr_fname = "vmlinux",
	};

	/*
	 * Header buffer contains:
	 * ELF header, Note entry with PR status, PR ps info, and vmcoreinfo.
	 * Also, MEMINSPECT_ID_MAX program headers.
	 */
	elfh_size = sizeof(*ehdr);
	elfh_size += sizeof(struct elf_prstatus);
	elfh_size += sizeof(struct elf_prpsinfo);
	elfh_size += sizeof(VMCOREINFO_NOTE_NAME);
	elfh_size += ALIGN(vmcoreinfo_size, 4);
	elfh_size += (sizeof(*phdr)) * (MEMINSPECT_ID_MAX);

	elfh_size = ALIGN(elfh_size, 4);

	/* Length of the note is made of :
	 * 3 elf notes structs (prstatus, prpsinfo, vmcoreinfo)
	 * 3 notes names (2 core strings, 1 vmcoreinfo name)
	 * sizeof each note
	 */
	note_len = (3 * sizeof(struct elf_note) +
		    2 * ALIGN(sizeof(CORE_STR), 4) +
		    VMCOREINFO_NOTE_NAME_BYTES +
		    ALIGN(sizeof(struct elf_prstatus), 4) +
		    ALIGN(sizeof(struct elf_prpsinfo), 4) +
		    ALIGN(vmcoreinfo_size, 4));

	buf_sz = elfh_size + note_len - ALIGN(vmcoreinfo_size, 4);

	/* Never freed */
	p = dma_alloc_from_contiguous(NULL, buf_sz >> PAGE_SHIFT,
				      get_order(buf_sz), true);
	if (!p)
		return -ENOMEM;

	ehdr = dma_common_contiguous_remap(p, buf_sz,
			pgprot_decrypted(pgprot_dmacoherent(PAGE_KERNEL)),
			__builtin_return_address(0));
	if (!ehdr) {
		dma_release_from_contiguous(NULL, p, buf_sz >> PAGE_SHIFT);
		return -ENOMEM;
	}

	memset(ehdr, 0, elfh_size);

	/* Assign Program headers offset, it's right after the elf header. */
	phdr = (struct elf_phdr *)(ehdr + 1);
	phdr_off = sizeof(*ehdr);

	memcpy(ehdr->e_ident, ELFMAG, SELFMAG);
	ehdr->e_ident[EI_CLASS] = ELF_CLASS;
	ehdr->e_ident[EI_DATA] = ELF_DATA;
	ehdr->e_ident[EI_VERSION] = EV_CURRENT;
	ehdr->e_ident[EI_OSABI] = ELF_OSABI;
	ehdr->e_type = ET_CORE;
	ehdr->e_machine = ELF_ARCH;
	ehdr->e_version = EV_CURRENT;
	ehdr->e_ehsize = sizeof(*ehdr);
	ehdr->e_phentsize = sizeof(*phdr);

	elf_offset = elfh_size;

	notes = (void *)(((char *)ehdr) + elf_offset);

	/* we have a single program header now */
	ehdr->e_phnum = 1;

	phdr->p_type = PT_NOTE;
	phdr->p_offset = elf_offset;
	phdr->p_filesz = note_len;

	/* advance elf offset */
	elf_offset += note_len;

	strscpy(prpsinfo.pr_psargs, saved_command_line,
		sizeof(prpsinfo.pr_psargs));

	append_kcore_note(notes, &i, CORE_STR, NT_PRSTATUS, &prstatus,
			  sizeof(prstatus));
	append_kcore_note(notes, &i, CORE_STR, NT_PRPSINFO, &prpsinfo,
			  sizeof(prpsinfo));
	append_kcore_note_nodesc(notes, &i, VMCOREINFO_NOTE_NAME, 0,
				 ALIGN(vmcoreinfo_size, 4));

	ehdr->e_phoff = phdr_off;

	/* This is the first coredump region, the ELF header */
	meminspect_register_id_pa(MEMINSPECT_ID_ELF, page_to_phys(p),
				  buf_sz, MEMINSPECT_TYPE_REGULAR);

	/*
	 * The second region is the vmcoreinfo, which goes right after.
	 * It's being registered through vmcoreinfo.
	 */

	return 0;
}
#endif

/**
 * meminspect_unregister_id() - Unregister region from inspection table.
 * @id: region's id in the table
 *
 * Return: None
 */
void meminspect_unregister_id(enum meminspect_uid id)
{
	struct inspect_entry *e;

	WARN_ON(!mutex_is_locked(&meminspect_lock));

	e = &inspect_entries[id];
	if (!e->id)
		return;

	atomic_notifier_call_chain(&meminspect_notifier_list,
				   MEMINSPECT_NOTIFIER_REMOVE, e);
#ifdef CONFIG_CRASH_DUMP
	if (elf_hdr_ready)
		clear_elfheader(e);
#endif
	memset(e, 0, sizeof(*e));
}
EXPORT_SYMBOL_GPL(meminspect_unregister_id);

/**
 * meminspect_unregister_pa() - Unregister region from inspection table.
 * @pa: Physical address of the memory region to remove
 * @size: Size of the memory region to remove
 *
 * Return: None
 */
void meminspect_unregister_pa(phys_addr_t pa, size_t size)
{
	struct inspect_entry *e;
	enum meminspect_uid i;

	WARN_ON(!mutex_is_locked(&meminspect_lock));

	for (i = MEMINSPECT_ID_STATIC; i < MEMINSPECT_ID_MAX; i++) {
		e = &inspect_entries[i];
		if (e->pa != pa)
			continue;
		if (e->size != size)
			continue;
		meminspect_unregister_id(e->id);
		return;
	}
}
EXPORT_SYMBOL_GPL(meminspect_unregister_pa);

/**
 * meminspect_register_id_pa() - Register region into inspection table
 *		 with given ID and physical address.
 * @req_id: Requested unique meminspect_uid that identifies the region
 *	This can be MEMINSPECT_ID_DYNAMIC, in which case the function will
 *	find an unused ID and register with it.
 * @pa: physical address of the memory region
 * @size: region size
 * @type: region type
 *
 * Return: None
 */
void meminspect_register_id_pa(enum meminspect_uid req_id, phys_addr_t pa,
			       size_t size, unsigned int type)
{
	struct inspect_entry *e;
	enum meminspect_uid uid = req_id;

	WARN_ON(!mutex_is_locked(&meminspect_lock));

	if (uid < MEMINSPECT_ID_STATIC || uid >= MEMINSPECT_ID_MAX)
		return;

	if (uid == MEMINSPECT_ID_DYNAMIC)
		while (uid < MEMINSPECT_ID_MAX) {
			if (!inspect_entries[uid].id)
				break;
			uid++;
		}

	if (uid == MEMINSPECT_ID_MAX)
		return;

	e = &inspect_entries[uid];

	if (e->id)
		meminspect_unregister_id(e->id);

	e->pa = pa;
	e->va = phys_to_virt(pa);
	e->size = size;
	e->id = uid;
	e->type = type;
#ifdef CONFIG_CRASH_DUMP
	if (elf_hdr_ready)
		update_elfheader(e);
#endif
	atomic_notifier_call_chain(&meminspect_notifier_list,
				   MEMINSPECT_NOTIFIER_ADD, e);
}
EXPORT_SYMBOL_GPL(meminspect_register_id_pa);

/**
 * meminspect_table_lock() - Lock the mutex on the inspection table
 *
 * Return: None
 */
void meminspect_table_lock(void)
{
	mutex_lock(&meminspect_lock);
}
EXPORT_SYMBOL_GPL(meminspect_table_lock);

/**
 * meminspect_table_unlock() - Unlock the mutex on the inspection table
 *
 * Return: None
 */
void meminspect_table_unlock(void)
{
	mutex_unlock(&meminspect_lock);
}
EXPORT_SYMBOL_GPL(meminspect_table_unlock);

/**
 * meminspect_traverse() - Traverse the meminspect table and call the
 *		callback function for each valid entry.
 * @priv: private data to be called to the callback
 * @cb: meminspect iterator callback that should be called for each entry
 *
 * Return: None
 */
void meminspect_traverse(void *priv, MEMINSPECT_ITERATOR_CB cb)
{
	const struct inspect_entry *e;
	int i;

	WARN_ON(!mutex_is_locked(&meminspect_lock));

	for (i = MEMINSPECT_ID_STATIC; i < MEMINSPECT_ID_MAX; i++) {
		e = &inspect_entries[i];
		if (e->id)
			cb(priv, e);
	}
}
EXPORT_SYMBOL_GPL(meminspect_traverse);

/**
 * meminspect_notifier_register() - Register a notifier to meminspect table
 * @n: notifier block to register. This will be called whenever an entry
 *		is being added or removed.
 *
 * Return: errno
 */
int meminspect_notifier_register(struct notifier_block *n)
{
	return atomic_notifier_chain_register(&meminspect_notifier_list, n);
}
EXPORT_SYMBOL_GPL(meminspect_notifier_register);

/**
 * meminspect_notifier_unregister() - Unregister a previously registered
 *		notifier from meminspect table.
 * @n: notifier block to unregister.
 *
 * Return: errno
 */
int meminspect_notifier_unregister(struct notifier_block *n)
{
	return atomic_notifier_chain_unregister(&meminspect_notifier_list, n);
}
EXPORT_SYMBOL_GPL(meminspect_notifier_unregister);

#ifdef CONFIG_CRASH_DUMP
static int __init meminspect_prepare_crashdump(void)
{
	const struct inspect_entry *e;
	int ret;
	enum meminspect_uid i;

	ret = init_elfheader();

	if (ret < 0)
		return ret;

	/*
	 * Some regions may have been registered very early.
	 * Update the elf header for all existing regions,
	 * except for MEMINSPECT_ID_ELF and MEMINSPECT_ID_VMCOREINFO,
	 * those are included in the ELF header upon its creation.
	 */
	for (i = MEMINSPECT_ID_VMCOREINFO + 1; i < MEMINSPECT_ID_MAX; i++) {
		e = &inspect_entries[i];
		if (e->id)
			update_elfheader(e);
	}

	elf_hdr_ready = true;

	return 0;
}
#endif

static int __init meminspect_prepare_table(void)
{
	const struct inspect_entry *e;
	enum meminspect_uid i;

	meminspect_table_lock();
	/*
	 * First, copy all entries from the compiler built table
	 * In case some entries are registered multiple times,
	 * the last chronological entry will be stored.
	 * Previusly registered entries will be dropped.
	 */
	for_each_meminspect_entry(e) {
		inspect_entries[e->id] = *e;
	}
#ifdef CONFIG_CRASH_DUMP
	meminspect_prepare_crashdump();
#endif
	/* if we have early notifiers registered, call them now */
	for (i = MEMINSPECT_ID_STATIC; i < MEMINSPECT_ID_MAX; i++)
		if (inspect_entries[i].id)
			atomic_notifier_call_chain(&meminspect_notifier_list,
						   MEMINSPECT_NOTIFIER_ADD,
						   &inspect_entries[i]);
	meminspect_table_unlock();

	pr_debug("Memory inspection table initialized");

	return 0;
}
late_initcall(meminspect_prepare_table);
