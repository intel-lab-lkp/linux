// SPDX-License-Identifier: GPL-2.0-or-later
#include <string.h>

#include <objtool/special.h>
#include <objtool/builtin.h>
#include <objtool/warn.h>
#include <objtool/check.h>
#include <objtool/elf.h>

#define X86_FEATURE_POPCNT (4 * 32 + 23)
#define X86_FEATURE_SMAP   (9 * 32 + 20)

void arch_handle_alternative(unsigned short feature, struct special_alt *alt)
{
	switch (feature) {
	case X86_FEATURE_SMAP:
		/*
		 * If UACCESS validation is enabled; force that alternative;
		 * otherwise force it the other way.
		 *
		 * What we want to avoid is having both the original and the
		 * alternative code flow at the same time, in that case we can
		 * find paths that see the STAC but take the NOP instead of
		 * CLAC and the other way around.
		 */
		if (opts.uaccess)
			alt->skip_orig = true;
		else
			alt->skip_alt = true;
		break;
	case X86_FEATURE_POPCNT:
		/*
		 * It has been requested that we don't validate the !POPCNT
		 * feature path which is a "very very small percentage of
		 * machines".
		 */
		alt->skip_orig = true;
		break;
	default:
		break;
	}
}

bool arch_support_alt_relocation(struct special_alt *special_alt,
				 struct instruction *insn,
				 struct reloc *reloc)
{
	return true;
}

/*
 * There are 3 basic jump table patterns:
 *
 * 1. jmpq *[rodata addr](,%reg,8)
 *
 *    This is the most common case by far.  It jumps to an address in a simple
 *    jump table which is stored in .rodata.
 *
 * 2. jmpq *[rodata addr](%rip)
 *
 *    This is caused by a rare GCC quirk, currently only seen in three driver
 *    functions in the kernel, only with certain obscure non-distro configs.
 *
 *    As part of an optimization, GCC makes a copy of an existing switch jump
 *    table, modifies it, and then hard-codes the jump (albeit with an indirect
 *    jump) to use a single entry in the table.  The rest of the jump table and
 *    some of its jump targets remain as dead code.
 *
 *    In such a case we can just crudely ignore all unreachable instruction
 *    warnings for the entire object file.  Ideally we would just ignore them
 *    for the function, but that would require redesigning the code quite a
 *    bit.  And honestly that's just not worth doing: unreachable instruction
 *    warnings are of questionable value anyway, and this is such a rare issue.
 *
 * 3. mov [rodata addr],%reg1
 *    ... some instructions ...
 *    jmpq *(%reg1,%reg2,8)
 *
 *    This is a fairly uncommon pattern which is new for GCC 6.  As of this
 *    writing, there are 11 occurrences of it in the allmodconfig kernel.
 *
 *    As of GCC 7 there are quite a few more of these and the 'in between' code
 *    is significant. Esp. with KASAN enabled some of the code between the mov
 *    and jmpq uses .rodata itself, which can confuse things.
 *
 *    TODO: Once we have DWARF CFI and smarter instruction decoding logic,
 *    ensure the same register is used in the mov and jump instructions.
 *
 *    NOTE: RETPOLINE made it harder still to decode dynamic jumps.
 */
struct reloc *arch_find_switch_table(struct objtool_file *file,
				    struct instruction *insn)
{
	struct reloc  *text_reloc, *rodata_reloc;
	struct section *table_sec;
	unsigned long table_offset;

	/* look for a relocation which references .rodata */
	text_reloc = find_reloc_by_dest_range(file->elf, insn->sec,
					      insn->offset, insn->len);
	if (!text_reloc || text_reloc->sym->type != STT_SECTION ||
	    !text_reloc->sym->sec->rodata)
		return NULL;

	table_offset = reloc_addend(text_reloc);
	table_sec = text_reloc->sym->sec;

	if (reloc_type(text_reloc) == R_X86_64_PC32)
		table_offset += 4;

	/*
	 * Make sure the .rodata address isn't associated with a
	 * symbol.  GCC jump tables are anonymous data.
	 *
	 * Also support C jump tables which are in the same format as
	 * switch jump tables.  For objtool to recognize them, they
	 * need to be placed in the C_JUMP_TABLE_SECTION section.  They
	 * have symbols associated with them.
	 */
	if (find_symbol_containing(table_sec, table_offset) &&
	    strcmp(table_sec->name, C_JUMP_TABLE_SECTION))
		return NULL;

	/*
	 * Each table entry has a rela associated with it.  The rela
	 * should reference text in the same function as the original
	 * instruction.
	 */
	rodata_reloc = find_reloc_by_dest(file->elf, table_sec, table_offset);
	if (!rodata_reloc)
		return NULL;

	/*
	 * Use of RIP-relative switch jumps is quite rare, and
	 * indicates a rare GCC quirk/bug which can leave dead
	 * code behind.
	 */
	if (reloc_type(text_reloc) == R_X86_64_PC32)
		file->ignore_unreachables = true;

	return rodata_reloc;
}

/*
 * Convert op %gs:0x28, reg -> op __stack_chk_guard(%rip), %reg
 * op is mov, sub, or cmp.
 */
int arch_hack_stackprotector(struct objtool_file *file)
{
	struct section *sec;
	struct symbol *__stack_chk_guard;
	struct instruction *insn;

	int i;

	__stack_chk_guard = find_symbol_by_name(file->elf, "__stack_chk_guard");

	for_each_sec(file, sec) {
		int count = 0;
		int idx;
		struct section *rsec = sec->rsec;

		sec_for_each_insn(file, sec, insn) {
			if (insn->type == INSN_STACKPROTECTOR)
				count++;
		}

		if (!count)
			continue;

		if (!__stack_chk_guard)
			__stack_chk_guard = elf_create_undef_symbol(file->elf, "__stack_chk_guard");

		if (!sec->rsec) {
			idx = 0;
			rsec = sec->rsec = elf_create_rela_section(file->elf, sec, count);
		} else {
			idx = sec_num_entries(rsec);
			if (elf_extend_rela_section(file->elf, rsec, count))
				return -1;
		}

		sec_for_each_insn(file, sec, insn) {
			unsigned char *data = insn->sec->data->d_buf + insn->offset;

			if (insn->type != INSN_STACKPROTECTOR)
				continue;

			if (insn->len != 9)
				goto invalid;

			/* Remove GS prefix if !SMP */
			if (data[0] != 0x65)
				goto invalid;
			if (!opts.smp)
				data[0] = 0x90;

			/* Set Mod=00, R/M=101.  Preserve Reg */
			data[3] = (data[3] & 0x38) | 5;

			/* Displacement 0 */
			data[4] = 0;
			data[5] = 0;
			data[6] = 0;
			data[7] = 0;

			/* Pad with NOP */
			data[8] = 0x90;

			mark_sec_changed(file->elf, insn->sec, true);

			if (!elf_init_reloc_data_sym(file->elf, insn->sec, insn->offset + 4, idx++, __stack_chk_guard, -4))
				return -1;

			continue;

invalid:
			fprintf(stderr, "Invalid stackprotector instruction at %s+0x%lx: ", insn->sec->name, insn->offset);
			for (i = 0; i < insn->len; i++)
				fprintf(stderr, "%02x ", data[i]);
			fprintf(stderr, "\n");
			return -1;
		}
	}

	return 0;
}
