// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2015-2017 Josh Poimboeuf <jpoimboe@redhat.com>
 */

#include <objtool/arch.h>
#include <objtool/check.h>
#include <objtool/disas.h>
#include <objtool/warn.h>

#include <bfd.h>
#include <linux/string.h>
#include <tools/dis-asm-compat.h>

struct disas_context {
	struct objtool_file *file;
	struct instruction *insn;
	disassembler_ftype disassembler;
	struct disassemble_info info;
};

#define DINFO_FPRINTF(dinfo, ...)	\
	((*(dinfo)->fprintf_func)((dinfo)->stream, __VA_ARGS__))

static void disas_print_address(bfd_vma addr, struct disassemble_info *dinfo)
{
	struct disas_context *dctx = dinfo->application_data;
	struct instruction *insn = dctx->insn;
	struct objtool_file *file = dctx->file;
	struct instruction *jump_dest;
	struct symbol *call_dest;
	unsigned long offset;
	struct reloc *reloc;
	bool is_reloc;
	char symstr[1024];
	char *str;

	/*
	 * If the instruction is a call/jump and it references a
	 * destination then this is likely the address we are looking
	 * up. So check it first.
	 */
	jump_dest = insn->jump_dest;
	if (jump_dest && jump_dest->sym && jump_dest->offset == addr) {
		sprint_name(symstr, jump_dest->sym->name, jump_dest->offset - jump_dest->sym->offset);
		DINFO_FPRINTF(dinfo, "%lx <%s>", addr, symstr);
		return;
	}

	/*
	 * Assume the address is a relocation if it points to the next
	 * instruction.
	 */
	is_reloc = (addr == insn->offset + insn->len);

	/*
	 * The call destination offset can be the address we are looking
	 * up, or 0 if there is a relocation.
	 */
	call_dest = insn_call_dest(insn);
	if (call_dest) {
		if (call_dest->offset == addr) {
			DINFO_FPRINTF(dinfo, "%lx <%s>", addr, call_dest->name);
			return;
		}
		if (call_dest->offset == 0 && is_reloc) {
			DINFO_FPRINTF(dinfo, "%s", call_dest->name);
			return;
		}
	}

	if (!is_reloc) {
		DINFO_FPRINTF(dinfo, "0x%lx", addr);
		return;
	}

	/*
	 * If this is a relocation, check if we have relocation information
	 * for this instruction.
	 */
	reloc = find_reloc_by_dest_range(file->elf, insn->sec,
					 insn->offset, insn->len);
	if (!reloc) {
		DINFO_FPRINTF(dinfo, "0x%lx", addr);
		return;
	}

	if (arch_pc_relative_reloc(reloc))
		offset = arch_pc_relative_offset(insn, reloc);
	else
		offset = reloc_addend(reloc);

	/*
	 * If the relocation symbol is a section name (for example ".bss")
	 * then we try to further resolve the name.
	 */
	if (reloc->sym->type == STT_SECTION) {
		str = offstr(reloc->sym->sec, reloc->sym->offset + offset);
		DINFO_FPRINTF(dinfo, "%s", str);
		free(str);
	} else {
		sprint_name(symstr, reloc->sym->name, offset);
		DINFO_FPRINTF(dinfo, "%s", symstr);
	}
}

/*
 * Initialize disassemble info arch, mach (32 or 64-bit) and options.
 */
int disas_info_init(struct disassemble_info *dinfo,
		    int arch, int mach32, int mach64,
		    const char *options)
{
	struct disas_context *dctx = dinfo->application_data;
	struct objtool_file *file = dctx->file;

	dinfo->arch = arch;

	switch (file->elf->ehdr.e_ident[EI_CLASS]) {
	case ELFCLASS32:
		dinfo->mach = mach32;
		break;
	case ELFCLASS64:
		dinfo->mach = mach64;
		break;
	default:
		return -1;
	}

	dinfo->disassembler_options = options;

	return 0;
}

struct disas_context *disas_context_create(struct objtool_file *file)
{
	struct disas_context *dctx;
	struct disassemble_info *dinfo;
	int err;

	dctx = malloc(sizeof(*dctx));
	if (!dctx) {
		ERROR_GLIBC("failed to allocate disassembly context");
		return NULL;
	}

	dctx->file = file;
	dinfo = &dctx->info;

	init_disassemble_info_compat(dinfo, stdout,
				     (fprintf_ftype)fprintf,
				     fprintf_styled);

	dinfo->read_memory_func = buffer_read_memory;
	dinfo->print_address_func = disas_print_address;
	dinfo->application_data = dctx;

	/*
	 * bfd_openr() is not used to avoid doing ELF data processing
	 * and caching that has already being done. Here, we just need
	 * to identify the target file so we call an arch specific
	 * function to fill some disassemble info (arch, mach).
	 */

	dinfo->arch = bfd_arch_unknown;
	dinfo->mach = 0;

	err = arch_disas_info_init(dinfo);
	if (err || dinfo->arch == bfd_arch_unknown || dinfo->mach == 0) {
		WARN("failed to init disassembly arch");
		goto error;
	}

	dinfo->endian = (file->elf->ehdr.e_ident[EI_DATA] == ELFDATA2MSB) ?
		BFD_ENDIAN_BIG : BFD_ENDIAN_LITTLE;

	disassemble_init_for_target(dinfo);

	dctx->disassembler = disassembler(dinfo->arch,
					  dinfo->endian == BFD_ENDIAN_BIG,
					  dinfo->mach, NULL);
	if (!dctx->disassembler) {
		WARN("failed to create disassembler function");
		goto error;
	}

	return dctx;

error:
	free(dctx);
	return NULL;
}

void disas_context_destroy(struct disas_context *dctx)
{
	free(dctx);
}

/*
 * Disassemble a single instruction. Return the size of the instruction.
 */
static size_t disas_insn(struct disas_context *dctx,
			 struct instruction *insn)
{
	disassembler_ftype disasm = dctx->disassembler;
	struct disassemble_info *dinfo = &dctx->info;

	dctx->insn = insn;

	/*
	 * Set the disassembler buffer to read data from the section
	 * containing the instruction to disassemble.
	 */
	dinfo->buffer = insn->sec->data->d_buf;
	dinfo->buffer_vma = 0;
	dinfo->buffer_length = insn->sec->sh.sh_size;

	return disasm(insn->offset, &dctx->info);
}

/*
 * Disassemble a function.
 */
static void disas_func(struct disas_context *dctx, struct symbol *func)
{
	struct instruction *insn;
	size_t addr;

	printf("%s:\n", func->name);
	sym_for_each_insn(dctx->file, func, insn) {
		addr = insn->offset;
		printf(" %6lx:  %s+0x%-6lx      ",
		       addr, func->name, addr - func->offset);
		disas_insn(dctx, insn);
		printf("\n");
	}
	printf("\n");
}

/*
 * Disassemble all warned functions.
 */
void disas_warned_funcs(struct disas_context *dctx)
{
	struct symbol *sym;

	if (!dctx)
		return;

	for_each_sym(dctx->file, sym) {
		if (sym->warned)
			disas_func(dctx, sym);
	}
}
