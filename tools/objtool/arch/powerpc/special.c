// SPDX-License-Identifier: GPL-2.0-or-later
#include <string.h>
#include <stdlib.h>
#include <objtool/special.h>
#include <objtool/builtin.h>
#include <objtool/warn.h>
#include <asm/byteorder.h>
#include <errno.h>

struct section *ftr_alt;

struct fixup_entry *fes;
unsigned int nr_fes;

uint64_t fe_alt_start = -1;
uint64_t fe_alt_end;

bool arch_support_alt_relocation(struct special_alt *special_alt,
				 struct instruction *insn,
				 struct reloc *reloc)
{
	exit(-1);
}

struct reloc *arch_find_switch_table(struct objtool_file *file,
				     struct instruction *insn,
				     unsigned long *table_size)
{
	exit(-1);
}

const char *arch_cpu_feature_name(int feature_number)
{
	return NULL;
}


int process_alt_data(struct objtool_file *file)
{
	struct section *section;

	section = find_section_by_name(file->elf, ".__ftr_alternates.text");
	ftr_alt = section;

	if (!ftr_alt) {
		WARN(".__ftr_alternates.text section not found\n");
		return -1;
	}

	fe_alt_start = ftr_alt->sh.sh_addr;
	fe_alt_end = ftr_alt->sh.sh_addr + ftr_alt->sh.sh_size;

	return 0;

}

static int is_le(struct objtool_file *file)
{
	return file->elf->ehdr.e_ident[EI_DATA] == ELFDATA2LSB;
}

static int is_64bit(struct objtool_file *file)
{
	return file->elf->ehdr.e_ident[EI_CLASS] == ELFCLASS64;
}

static uint32_t f32_to_cpu(struct objtool_file *file, uint32_t val)
{
	if (is_le(file))
		return __le32_to_cpu(val);
	else
		return __be32_to_cpu(val);
}

static uint64_t f64_to_cpu(struct objtool_file *file, uint64_t val)
{
	if (is_le(file))
		return __le64_to_cpu(val);
	else
		return __be64_to_cpu(val);
}

static uint32_t cpu_to_f32(struct objtool_file *file, uint32_t val)
{
	if (is_le(file))
		return __cpu_to_le32(val);
	else
		return __cpu_to_be32(val);
}

int process_fixup_entries(struct objtool_file *file)
{
	struct section *sec;
	int i;

	for_each_sec(file->elf, sec) {
		Elf_Data *data;
		unsigned int nr;

		if (strstr(sec->name, "_ftr_fixup") == NULL)
			continue;

		if (strstr(sec->name, ".rela") != NULL)
			continue;

		data = sec->data;
		if (!data || data->d_size == 0)
			continue;

		if (is_64bit(file))
			nr = data->d_size / sizeof(struct fixup_entry_64);
		else
			nr = data->d_size / sizeof(struct fixup_entry_32);

		for (i = 0; i < nr; i++) {
			unsigned long idx;
			unsigned long long off;
			struct fixup_entry *dst;

			if (is_64bit(file)) {
				struct fixup_entry_64 *src;

				idx = i * sizeof(struct fixup_entry_64);
				off = sec->sh.sh_addr + data->d_off + idx;
				src = data->d_buf + idx;

				if (src->alt_start_off == src->alt_end_off)
					continue;

				fes = realloc(fes, (nr_fes + 1) * sizeof(struct fixup_entry));
				dst = &fes[nr_fes];
				nr_fes++;

				dst->mask = f64_to_cpu(file, src->mask);
				dst->value = f64_to_cpu(file, src->value);
				dst->start_off = f64_to_cpu(file, src->start_off) + off;
				dst->end_off = f64_to_cpu(file, src->end_off) + off;
				dst->alt_start_off = f64_to_cpu(file, src->alt_start_off) + off;
				dst->alt_end_off = f64_to_cpu(file, src->alt_end_off) + off;
			} else {
				struct fixup_entry_32 *src;

				idx = i * sizeof(struct fixup_entry_32);
				off = sec->sh.sh_addr + data->d_off + idx;
				src = data->d_buf + idx;

				if (src->alt_start_off == src->alt_end_off)
					continue;

				fes = realloc(fes, (nr_fes + 1) * sizeof(struct fixup_entry));
				dst = &fes[nr_fes];
				nr_fes++;

				dst->mask = f32_to_cpu(file, src->mask);
				dst->value = f32_to_cpu(file, src->value);
				dst->start_off = (int32_t)f32_to_cpu(file, src->start_off) + off;
				dst->end_off = (int32_t)f32_to_cpu(file, src->end_off) + off;
				dst->alt_start_off = (int32_t)f32_to_cpu(file,
								src->alt_start_off) + off;
				dst->alt_end_off = (int32_t)f32_to_cpu(file,
								src->alt_end_off) + off;
			}
		}
	}
	return 0;
}

struct fixup_entry *find_fe_altaddr(uint64_t addr)
{
	unsigned int i;

	if (addr < fe_alt_start)
		return NULL;
	if (addr >= fe_alt_end)
		return NULL;

	for (i = 0; i < nr_fes; i++) {
		if (addr >= fes[i].alt_start_off && addr < fes[i].alt_end_off)
			return &fes[i];
	}
	return NULL;
}

int set_uncond_branch_target(uint32_t *insn,
		const uint64_t addr, uint64_t target)
{
	uint32_t i = *insn;
	int64_t offset;

	offset = target;
	if (!(i & BRANCH_ABSOLUTE))
		offset = offset - addr;

	/* Check we can represent the target in the instruction format */
	if (offset < -0x2000000 || offset > 0x1fffffc || offset & 0x3)
		return -EOVERFLOW;

	/* Mask out the flags and target, so they don't step on each other. */
	*insn = 0x48000000 | (i & 0x3) | (offset & 0x03FFFFFC);

	return 0;
}

int set_cond_branch_target(uint32_t *insn,
		const uint64_t addr, uint64_t target)
{
	uint32_t i = *insn;
	int64_t offset;

	offset = target;

	if (!(i & BRANCH_ABSOLUTE))
		offset = offset - addr;

	/* Check we can represent the target in the instruction format */
	if (offset < -0x8000 || offset > 0x7FFF || offset & 0x3)
		return -EOVERFLOW;

	/* Mask out the flags and target, so they don't step on each other. */
	*insn = 0x40000000 | (i & 0x3FF0003) | (offset & 0xFFFC);

	return 0;
}

void check_and_flatten_fixup_entries(void)
{
	static struct fixup_entry *fe;
	unsigned int i;

	i = nr_fes;
	while (i) {
		static struct fixup_entry *parent;
		uint64_t nested_off; /* offset from start of parent */
		uint64_t size;

		i--;
		fe = &fes[i];

		parent = find_fe_altaddr(fe->start_off);
		if (!parent) {
			parent = find_fe_altaddr(fe->end_off);
			continue;
		}

		size = fe->end_off - fe->start_off;
		nested_off = fe->start_off - parent->alt_start_off;

		fe->start_off = parent->start_off + nested_off;
		fe->end_off = fe->start_off + size;
	}
}


static struct symbol *find_symbol_at_address_within_section(struct section *sec,
				unsigned long address)
{
	struct symbol *sym;

	sec_for_each_sym(sec, sym) {
		if (sym->sym.st_value <= address && address < sym->sym.st_value + sym->len)
			return sym;
	}

	return NULL;
}

static int is_local_symbol(uint8_t st_other)
{
	return (st_other & 0x3) != 0;
}

static struct symbol *find_symbol_at_address(struct objtool_file *file,
					unsigned long address)
{
	struct section *sec;
	struct symbol *sym;

	list_for_each_entry(sec, &file->elf->sections, list) {
		sym = find_symbol_at_address_within_section(sec, address);
		if (sym)
			return sym;
		}
	return NULL;
}

int process_alt_relocations(struct objtool_file *file)
{
	struct section *section;
	size_t n = 0;
	struct reloc *relocation;
	struct symbol *sym;
	struct fixup_entry *fe;
	uint64_t addr;
	uint64_t scn_delta;
	uint64_t dst_addr;
	const char *insn_ptr;
	unsigned long target;
	struct symbol *symbol;
	int is_local;
	int j;
	uint32_t new_insn;
	uint32_t file_insn;
	struct instruction decoded_insn = {0};
	uint32_t *insn_ptr_raw;
	uint32_t insn;

	section = find_section_by_name(file->elf, ".rela.__ftr_alternates.text");
	if (!section) {
		printf(".rela.__ftr_alternates.text section not found.\n");
		return 0;
	}

	for (j = 0; j < sec_num_entries(section); j++) {

		relocation = &section->relocs[j];
		sym = relocation->sym;
		addr = reloc_offset(relocation);
		target = sym->sym.st_value + reloc_addend(relocation);
		symbol = find_symbol_at_address(file, target);

		if (symbol) {
			is_local = is_local_symbol(symbol->sym.st_other);
			if (!is_local)
				target = target + 0x8;
		}

		n++;
		fe = find_fe_altaddr(addr);
		if (!fe)
			continue;

		if (target >= fe->alt_start_off && target < fe->alt_end_off)
			continue;

		if (target >= ftr_alt->sh.sh_addr &&
			target < ftr_alt->sh.sh_addr + ftr_alt->sh.sh_size)
			return -1;

		scn_delta = addr - ftr_alt->sh.sh_addr;
		dst_addr = addr - fe->alt_start_off + fe->start_off;

		if (arch_decode_instruction(file, ftr_alt, scn_delta, 4, &decoded_insn) < 0)
			continue;

		insn_ptr_raw = (uint32_t *)(ftr_alt->data->d_buf + scn_delta);
		if (!insn_ptr_raw || !ftr_alt->data->d_buf)
			continue;

		insn = f32_to_cpu(file, *insn_ptr_raw);
		new_insn = insn;

		switch (decoded_insn.type) {
		case INSN_JUMP_CONDITIONAL:
			if (set_cond_branch_target(&new_insn, dst_addr, target) != 0)
				continue;
			break;

		case INSN_JUMP_UNCONDITIONAL:
			if (set_uncond_branch_target(&new_insn, dst_addr, target) != 0)
				continue;
			break;

		case INSN_CALL:
			if (set_uncond_branch_target(&new_insn, dst_addr, target) != 0)
				continue;
			break;
		default:
			continue;
		}

		if (new_insn == insn)
			continue;

		file_insn = cpu_to_f32(file, new_insn);
		insn_ptr = (const char *)&file_insn;
		elf_write_insn(file->elf, ftr_alt, scn_delta, sizeof(file_insn), insn_ptr);
	}
	return 0;
}

int process_exception_entries(struct objtool_file *file)
{
	struct section *section;
	Elf_Data *data;
	unsigned int nr, i;

	section = find_section_by_name(file->elf, "__ex_table");
	if (!section) {
		printf("__ex_table section not found\n");
		return 0;
	}

	data = section->data;
	if (!data || data->d_size == 0)
		return 0;

	nr = data->d_size / sizeof(struct exception_entry);

	for (i = 0; i < nr; i++) {
		struct exception_entry *ex;
		unsigned long idx;
		uint64_t exaddr;
		unsigned long long off;

		idx = i * sizeof(struct exception_entry);
		off = section->sh.sh_addr + data->d_off + idx;
		ex = data->d_buf + idx;

		exaddr = off + (int32_t)f32_to_cpu(file, ex->insn);

		if (exaddr < fe_alt_start)
			continue;
		if (exaddr >= fe_alt_end)
			continue;

		return -1;
	}
	return 0;
}

int process_bug_entries(struct objtool_file *file)
{
	struct section *section;
	Elf_Data *data;
	unsigned int nr, i;

	section = find_section_by_name(file->elf, "__bug_table");
	if (!section) {
		printf("__bug_table section not found\n");
		return 0;
	}

	data = section->data;
	if (!data || data->d_size == 0)
		return 0;

	if (data->d_size % sizeof(struct bug_entry) != 0)
		return -1;

	nr = data->d_size / sizeof(struct bug_entry);
	for (i = 0; i < nr; i++) {
		struct bug_entry *bug;
		unsigned long idx;
		uint64_t entry_addr;
		uint64_t bugaddr;
		int32_t bug_disp;

		idx = i * sizeof(struct bug_entry);
		entry_addr = section->sh.sh_addr + data->d_off + idx;
		bug = (struct bug_entry *)(data->d_buf + idx);
		bug_disp = f32_to_cpu(file, bug->bug_addr_disp);
		bugaddr = entry_addr + bug_disp;

		if (bugaddr < fe_alt_start)
			continue;
		if (bugaddr >= fe_alt_end)
			continue;

		return -1;
	}
	return 0;
}
