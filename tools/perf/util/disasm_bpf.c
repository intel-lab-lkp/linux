// SPDX-License-Identifier: GPL-2.0-only

#include "util/annotate.h"
#include "util/disasm_bpf.h"
#include "util/symbol.h"
#include <linux/zalloc.h>
#include <string.h>

int symbol__disassemble_bpf(struct symbol *sym __maybe_unused, struct annotate_args *args __maybe_unused)
{
	return SYMBOL_ANNOTATE_ERRNO__NO_LIBOPCODES_FOR_BPF;
}

int symbol__disassemble_bpf_image(struct symbol *sym, struct annotate_args *args)
{
	struct annotation *notes = symbol__annotation(sym);
	struct disasm_line *dl;

	args->offset = -1;
	args->line = strdup("to be implemented");
	args->line_nr = 0;
	args->fileloc = NULL;
	dl = disasm_line__new(args);
	if (dl)
		annotation_line__add(&dl->al, &notes->src->source);

	zfree(&args->line);
	return 0;
}
