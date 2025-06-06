// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2015-2017 Josh Poimboeuf <jpoimboe@redhat.com>
 */

#include <objtool/arch.h>
#include <objtool/check.h>
#include <objtool/warn.h>

#include <bfd.h>
#include <linux/string.h>
#include <tools/dis-asm-compat.h>

struct dbuffer {
	char *addr;
	size_t size;
	size_t used;
};

struct disas_context {
	struct objtool_file *file;
	struct instruction *insn;
	struct dbuffer result;
	disassembler_ftype disassembler;
	struct disassemble_info info;
};

#define DINFO_FPRINTF(dinfo, ...)	\
	((*(dinfo)->fprintf_func)((dinfo)->stream, __VA_ARGS__))


static int dbuffer_init(struct dbuffer *dbuf, size_t size)
{
	dbuf->used = 0;
	dbuf->size = size;

	if (!size) {
		dbuf->addr = NULL;
		return 0;
	}

	dbuf->addr = malloc(size);
	if (!dbuf->addr)
		return -1;

	return 0;
}

static void dbuffer_fini(struct dbuffer *dbuf)
{
	free(dbuf->addr);
	dbuf->size = 0;
	dbuf->used = 0;
}

static void dbuffer_reset(struct dbuffer *dbuf)
{
	dbuf->used = 0;
}

static char *dbuffer_data(struct dbuffer *dbuf)
{
	return dbuf->addr;
}

static int dbuffer_expand(struct dbuffer *dbuf, size_t space)
{
	size_t size;
	char *addr;

	size = dbuf->size + space;
	addr = realloc(dbuf->addr, size);
	if (!addr)
		return -1;

	dbuf->addr = addr;
	dbuf->size = size;

	return 0;
}

static int dbuffer_vappendf_noexpand(struct dbuffer *dbuf, const char *fmt, va_list ap)
{
	int free, len;

	free = dbuf->size - dbuf->used;

	len = vsnprintf(dbuf->addr + dbuf->used, free, fmt, ap);

	if (len < 0)
		return -1;

	if (len < free) {
		dbuf->used += len;
		return 0;
	}

	return (len - free) + 1;
}

static int dbuffer_vappendf(struct dbuffer *dbuf, const char *fmt, va_list ap)
{
	int space_needed, err;

	space_needed = dbuffer_vappendf_noexpand(dbuf, fmt, ap);
	if (space_needed <= 0)
		return space_needed;

	/*
	 * The buffer is not large enough to store all data. Expand
	 * the buffer and retry. The buffer is expanded with enough
	 * space to store all data.
	 */
	err = dbuffer_expand(dbuf, space_needed * 2);
	if (err) {
		WARN("failed to expand buffer\n");
		return -1;
	}

	return dbuffer_vappendf_noexpand(dbuf, fmt, ap);
}

static int disas_fprintf(void *stream, const char *fmt, ...)
{
	va_list arg;
	int len;

	va_start(arg, fmt);
	len = dbuffer_vappendf(stream, fmt, arg);
	va_end(arg);

	return len == 0 ? 0 : -1;
}

/*
 * For init_disassemble_info_compat().
 */
static int disas_fprintf_styled(void *stream,
				enum disassembler_style style,
				const char *fmt, ...)
{
	va_list arg;
	int len;

	(void)style;

	va_start(arg, fmt);
	len = dbuffer_vappendf(stream, fmt, arg);
	va_end(arg);

	return len == 0 ? 0 : -1;
}

static void disas_print_address(bfd_vma addr, struct disassemble_info *dinfo)
{
	struct disas_context *dctx = dinfo->application_data;
	struct instruction *insn = dctx->insn;
	struct objtool_file *file = dctx->file;
	struct symbol *call_dest, *sym;
	struct instruction *jump_dest;
	struct section *sec;
	struct reloc *reloc;
	bool is_reloc;
	s64 offset;

	/*
	 * If the instruction is a call/jump and it references a
	 * destination then this is likely the address we are looking
	 * up. So check it first.
	 */
	jump_dest = insn->jump_dest;
	if (jump_dest && jump_dest->offset == addr) {
		DINFO_FPRINTF(dinfo, "%lx <%s+0x%lx>", addr,
			      jump_dest->sym->name,
			      jump_dest->offset - jump_dest->sym->offset);
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

	if (reloc_type(reloc) == R_X86_64_PC32 ||
	    reloc_type(reloc) == R_X86_64_PLT32)
		offset = arch_dest_reloc_offset(reloc_addend(reloc));
	else
		offset = reloc_addend(reloc);

	/*
	 * If the relocation symbol is a section name (for example ".bss")
	 * then we try to further resolve the name.
	 */
	sec = find_section_by_name(file->elf, reloc->sym->name);
	if (sec) {
		sym = find_symbol_containing(sec, offset);
		if (sym) {
			if (sym->offset == offset)
				DINFO_FPRINTF(dinfo, "%s+0x%lx = %s",
					     reloc->sym->name, offset, sym->name);
			else
				DINFO_FPRINTF(dinfo, "%s+0x%lx = %s+0x%lx",
					      reloc->sym->name, offset,
					      sym->name, offset - sym->offset);
			return;
		}
	}

	if (offset)
		DINFO_FPRINTF(dinfo, "%s+0x%lx", reloc->sym->name, offset);
	else
		DINFO_FPRINTF(dinfo, "%s", reloc->sym->name);
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
	struct dbuffer *dbuf;
	int err;

	dctx = malloc(sizeof(*dctx));
	if (!dctx) {
		WARN("failed too allocate disassembly context\n");
		return NULL;
	}

	dctx->file = file;
	dinfo = &dctx->info;
	dbuf = &dctx->result;

	err = dbuffer_init(dbuf, 1024);
	if (err) {
		WARN("failed to initialize buffer\n");
		return NULL;
	}

	init_disassemble_info_compat(dinfo, dbuf,
				     disas_fprintf, disas_fprintf_styled);

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
		WARN("failed to init disassembly arch\n");
		goto error;
	}

	dinfo->endian = (file->elf->ehdr.e_ident[EI_DATA] == ELFDATA2MSB) ?
		BFD_ENDIAN_BIG : BFD_ENDIAN_LITTLE;

	disassemble_init_for_target(dinfo);

	dctx->disassembler = disassembler(dinfo->arch,
					       dinfo->endian == BFD_ENDIAN_BIG,
					       dinfo->mach, NULL);
	if (!dctx->disassembler) {
		WARN("failed to create disassembler function\n");
		goto error;
	}

	return dctx;

error:
	free(dctx);
	return NULL;
}

void disas_context_destroy(struct disas_context *dctx)
{
	if (!dctx)
		return;

	dbuffer_fini(&dctx->result);
	free(dctx);
}

char *disas_result(struct disas_context *dctx)
{
	return dbuffer_data(&dctx->result);
}

/*
 * Disassemble a single instruction. Return the size of the instruction.
 */
size_t disas_insn(struct disas_context *dctx, struct instruction *insn)
{
	disassembler_ftype disasm = dctx->disassembler;
	struct disassemble_info *dinfo = &dctx->info;

	dbuffer_reset(&dctx->result);
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
	size_t addr, size;

	printf("%s:\n", func->name);
	sym_for_each_insn(dctx->file, func, insn) {

		addr = insn->offset;
		size = disas_insn(dctx, insn);

		printf(" %6lx:  %s+0x%-6lx      %s\n",
		       addr, func->name, addr - func->offset,
		       disas_result(dctx));

		if (size != insn->len)
			WARN("inconsistent insn size (%ld and %d)\n", size, insn->len);
	}
	printf("\n");
}

/*
 * Disassemble all warned functions.
 */
void disas_warned_funcs(struct disas_context *dctx)
{
	struct symbol *sym;

	if (!dctx) {
		ERROR("disassembly context is not defined");
		return;
	}

	for_each_sym(dctx->file, sym) {
		if (sym->warned)
			disas_func(dctx, sym);
	}
}
