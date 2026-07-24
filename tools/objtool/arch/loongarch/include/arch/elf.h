/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _OBJTOOL_ARCH_ELF_H
#define _OBJTOOL_ARCH_ELF_H

/*
 * See the following link for more info about ELF Relocation types:
 * https://loongson.github.io/LoongArch-Documentation/LoongArch-ELF-ABI-EN.html#_relocations
 */
#ifndef R_LARCH_NONE
#define R_LARCH_NONE		0
#endif
#ifndef R_LARCH_32
#define R_LARCH_32		1
#endif
#ifndef R_LARCH_64
#define R_LARCH_64		2
#endif
#ifndef R_LARCH_ADD32
#define R_LARCH_ADD32		50
#endif
#ifndef R_LARCH_ADD64
#define R_LARCH_ADD64		51
#endif
#ifndef R_LARCH_SUB32
#define R_LARCH_SUB32		55
#endif
#ifndef R_LARCH_SUB64
#define R_LARCH_SUB64		56
#endif
#ifndef R_LARCH_32_PCREL
#define R_LARCH_32_PCREL	99
#endif
#ifndef R_LARCH_64_PCREL
#define R_LARCH_64_PCREL	109
#endif

#ifndef EM_LOONGARCH
#define EM_LOONGARCH		258
#endif

#define R_NONE			R_LARCH_NONE
#define R_ABS32			R_LARCH_32
#define R_ABS64			R_LARCH_64
#define R_DATA32		R_LARCH_32_PCREL
#define R_DATA64		R_LARCH_32_PCREL
#define R_TEXT32		R_LARCH_32_PCREL
#define R_TEXT64		R_LARCH_32_PCREL

#define ARCH_HAS_PAIRED_RELOCS	1

struct elf;
struct reloc;
int arch_normalize_paired_reloc(struct elf *elf, struct reloc *reloc);

#endif /* _OBJTOOL_ARCH_ELF_H */
