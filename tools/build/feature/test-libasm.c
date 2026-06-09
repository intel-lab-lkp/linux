// SPDX-License-Identifier: GPL-2.0
#include <stddef.h>
#include <elfutils/libasm.h>
#include <gelf.h>

struct ebl;
extern struct ebl *ebl_openbackend(Elf *elf);
extern void ebl_closebackend(struct ebl *ebl);

int main(void)
{
	Elf *elf = elf_begin(0, ELF_C_READ, NULL);
	struct ebl *ebl = ebl_openbackend(elf);
	DisasmCtx_t *ctx = disasm_begin(ebl, elf, NULL);

	disasm_end(ctx);
	ebl_closebackend(ebl);
	elf_end(elf);
	return 0;
}
