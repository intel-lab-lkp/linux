// SPDX-License-Identifier: GPL-2.0-only

#include <linux/init.h>
#include <linux/slab.h>
#include <linux/elfcore.h>
#include <linux/kmemdump.h>
#include <linux/vmcore_info.h>

#define CORE_STR "CORE"

#define MAX_NUM_ENTRIES	201

static struct elfhdr *ehdr;
static size_t elf_offset;

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

/**
 * clear_elfheader() - Remove the program header for a specific memory zone
 * @z: pointer to the kmemdump zone
 *
 * Return: On success, it returns 0, errno otherwise
 */
int clear_elfheader(const struct kmemdump_zone *z)
{
	struct elf_phdr *phdr;
	struct elf_phdr *tmp_phdr;
	unsigned int phidx;
	unsigned int i;

	for (i = 0; i < ehdr->e_phnum; i++) {
		phdr = elf_phdr_entry_addr(ehdr, i);
		if (phdr->p_paddr == virt_to_phys(z->zone) &&
		    phdr->p_memsz == ALIGN(z->size, 4))
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
		phdr->p_offset = phdr->p_offset - ALIGN(z->size, 4);
	}
	memset(tmp_phdr, 0, sizeof(*tmp_phdr));
	ehdr->e_phnum--;

	elf_offset -= ALIGN(z->size, 4);

	return 0;
}

/**
 * update_elfheader() - Add the program header for a specific memory zone
 * @z: pointer to the kmemdump zone
 *
 * Return: None
 */
void update_elfheader(const struct kmemdump_zone *z)
{
	struct elf_phdr *phdr;

	phdr = elf_phdr_entry_addr(ehdr, ehdr->e_phnum++);

	phdr->p_type = PT_LOAD;
	phdr->p_offset = elf_offset;
	phdr->p_vaddr = (elf_addr_t)z->zone;
	phdr->p_paddr = (elf_addr_t)virt_to_phys(z->zone);
	phdr->p_filesz = phdr->p_memsz = ALIGN(z->size, 4);
	phdr->p_flags = PF_R | PF_W;

	elf_offset += ALIGN(z->size, 4);
}

/**
 * init_elfheader() - Prepare coreinfo elf header
 *		This function prepares the elf header for the coredump image.
 *		Initially there is a single program header for the elf NOTE.
 *		The note contains the usual core dump information, and the
 *		vmcoreinfo.
 *
 * Return: 0 on success, errno otherwise
 */
int init_elfheader(void)
{
	struct elf_phdr *phdr;
	void *notes;
	unsigned int elfh_size;
	unsigned int phdr_off;
	size_t note_len, i = 0;

	struct elf_prstatus prstatus = {};
	struct elf_prpsinfo prpsinfo = {
		.pr_sname = 'R',
		.pr_fname = "vmlinux",
	};

	/*
	 * Header buffer contains:
	 * ELF header, Note entry with PR status, PR ps info, and vmcoreinfo
	 * MAX_NUM_ENTRIES Program headers,
	 */
	elfh_size = sizeof(*ehdr);
	elfh_size += sizeof(struct elf_prstatus);
	elfh_size += sizeof(struct elf_prpsinfo);
	elfh_size += sizeof(VMCOREINFO_NOTE_NAME);
	elfh_size += ALIGN(vmcoreinfo_size, 4);
	elfh_size += (sizeof(*phdr)) * (MAX_NUM_ENTRIES);

	elfh_size = ALIGN(elfh_size, 4);

	/* Never freed */
	ehdr = kzalloc(elfh_size, GFP_KERNEL);
	if (!ehdr)
		return -ENOMEM;

	/* Assign Program headers offset, it's right after the elf header. */
	phdr = (struct elf_phdr *)(ehdr + 1);
	phdr_off = sizeof(*ehdr);

	memcpy(ehdr->e_ident, ELFMAG, SELFMAG);
	ehdr->e_ident[EI_CLASS] = ELF_CLASS;
	ehdr->e_ident[EI_DATA] = ELF_DATA;
	ehdr->e_ident[EI_VERSION] = EV_CURRENT;
	ehdr->e_ident[EI_OSABI] = ELF_OSABI;
	ehdr->e_type = ET_CORE;
	ehdr->e_machine  = ELF_ARCH;
	ehdr->e_version = EV_CURRENT;
	ehdr->e_ehsize = sizeof(*ehdr);
	ehdr->e_phentsize = sizeof(*phdr);

	elf_offset = elfh_size;

	notes = (void *)(((char *)ehdr) + elf_offset);

	/* we have a single program header now */
	ehdr->e_phnum = 1;

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

	/* This is the first kmemdump region, the ELF header */
	kmemdump_register_id(KMEMDUMP_ID_COREIMAGE_ELF, ehdr,
			     elfh_size + note_len - ALIGN(vmcoreinfo_size, 4));

	/*
	 * The second region is the vmcoreinfo, which goes right after.
	 * It's being registered through vmcoreinfo.
	 */

	return 0;
}

