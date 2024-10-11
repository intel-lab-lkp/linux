// SPDX-License-Identifier: GPL-2.0-or-later
#include <objtool/special.h>

bool arch_support_alt_relocation(struct special_alt *special_alt,
				 struct instruction *insn,
				 struct reloc *reloc)
{
	return false;
}
