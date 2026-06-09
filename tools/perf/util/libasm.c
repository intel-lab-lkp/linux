// SPDX-License-Identifier: GPL-2.0
#include "libasm.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <string.h>
#include <unistd.h>
#include <elfutils/libasm.h>
#include <gelf.h>

#include "annotate.h"
#include "debug.h"
#include "disasm.h"
#include "dso.h"
#include "map.h"
#include "namespaces.h"
#include "symbol.h"

typedef struct ebl Ebl;
extern Ebl *ebl_openbackend(Elf *elf);
extern void ebl_closebackend(Ebl *ebl);

struct disasm_output_arg {
	char *buf;
	size_t size;
};

static int disasm_output_cb(char *str, size_t len, void *arg)
{
	struct disasm_output_arg *oa = arg;
	size_t to_copy = len < oa->size - 1 ? len : oa->size - 1;

	memcpy(oa->buf, str, to_copy);
	oa->buf[to_copy] = '\0';
	return 1;
}

int symbol__disassemble_libasm(const char *filename, struct symbol *sym,
			       struct annotate_args *args)
{
	struct annotation *notes = symbol__annotation(sym);
	struct map *map = args->ms->map;
	struct dso *dso = map__dso(map);
	u64 start = map__rip_2objdump(map, sym->start);
	u64 offset;
	bool is_64bit = false;
	u8 *code_buf = NULL;
	const u8 *buf;
	u64 buf_len;
	Elf *elf = NULL;
	Ebl *ebl = NULL;
	DisasmCtx_t *handle = NULL;
	char disasm_buf[512];
	struct disasm_line *dl;
	struct nscookie nsc;
	const uint8_t *pc;
	const uint8_t *end;
	u64 addr;
	size_t insn_len;
	int ret;
	int fd = -1;
	int count = 0;

	if (args->options->objdump_path)
		return -1;

	buf = dso__read_symbol(dso, filename, map, sym,
			       &code_buf, &buf_len, &is_64bit);
	if (buf == NULL)
		return errno;

	/* add the function address and name */
	scnprintf(disasm_buf, sizeof(disasm_buf), "%#"PRIx64" <%s>:",
		  start, sym->name);

	args->offset = -1;
	args->line = disasm_buf;
	args->line_nr = 0;
	args->fileloc = NULL;
	args->ms->sym = sym;

	dl = disasm_line__new(args);
	if (dl == NULL)
		goto err;

	annotation_line__add(&dl->al, &notes->src->source);

	nsinfo__mountns_enter(dso__nsinfo(dso), &nsc);
	fd = open(filename, O_RDONLY);
	nsinfo__mountns_exit(&nsc);
	if (fd < 0)
		goto err;

	elf = elf_begin(fd, ELF_C_READ, NULL);
	if (!elf)
		goto err;

	ebl = ebl_openbackend(elf);
	if (!ebl)
		goto err;

	handle = disasm_begin(ebl, elf, NULL);
	if (!handle)
		goto err;

	pc = buf;
	end = buf + buf_len;
	addr = start;

	offset = 0;
	while (pc < end) {
		struct disasm_output_arg oa = {
			.buf = disasm_buf,
			.size = sizeof(disasm_buf),
		};
		const uint8_t *prev_pc = pc;

		ret = disasm_cb(handle, &pc, end, addr, "%7m %.1o,%.2o,%.3o,%.4o,%.5o",
				    disasm_output_cb, &oa, NULL);
		if (ret != 1 || pc == prev_pc) {
			/* Disassembly failed or got stuck */
			break;
		}

		args->offset = offset;
		args->line = disasm_buf;

		dl = disasm_line__new(args);
		if (dl == NULL)
			goto err;

		annotation_line__add(&dl->al, &notes->src->source);

		insn_len = pc - prev_pc;
		offset += insn_len;
		addr += insn_len;
		count++;
	}

	if (offset != buf_len) {
		struct list_head *list = &notes->src->source;

		/* Discard all lines and fallback to objdump */
		while (!list_empty(list)) {
			dl = list_first_entry(list, struct disasm_line, al.node);

			list_del_init(&dl->al.node);
			disasm_line__free(dl);
		}
		count = -1;
	}

out:
	if (handle)
		disasm_end(handle);
	if (ebl)
		ebl_closebackend(ebl);
	if (elf)
		elf_end(elf);
	if (fd >= 0)
		close(fd);
	free(code_buf);
	return count < 0 ? count : 0;

err:
	{
		struct disasm_line *tmp;

		/*
		 * It probably failed in the middle of the above loop.
		 * Release any resources it might add.
		 */
		list_for_each_entry_safe(dl, tmp, &notes->src->source, al.node) {
			list_del(&dl->al.node);
			disasm_line__free(dl);
		}
	}

	count = -1;
	goto out;
}
