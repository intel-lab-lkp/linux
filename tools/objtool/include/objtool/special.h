/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2015 Josh Poimboeuf <jpoimboe@redhat.com>
 */

#ifndef _SPECIAL_H
#define _SPECIAL_H

#include <stdbool.h>
#include <objtool/check.h>
#include <objtool/elf.h>

#define C_JUMP_TABLE_SECTION ".data.rel.ro.c_jump_table"

#define BRANCH_SET_LINK 0x1
#define BRANCH_ABSOLUTE 0x2

struct bug_entry {
	int32_t  bug_addr_disp;
	int32_t  file_disp;
	uint16_t line;
	uint16_t flags;
};

struct exception_entry {
	int32_t insn;
	int32_t fixup;
};

struct fixup_entry_64 {
	uint64_t mask;
	uint64_t value;
	uint64_t start_off;
	uint64_t end_off;
	uint64_t alt_start_off;
	uint64_t alt_end_off;
};

#define fixup_entry fixup_entry_64

struct fixup_entry_32 {
	uint32_t mask;
	uint32_t value;
	uint32_t start_off;
	uint32_t end_off;
	uint32_t alt_start_off;
	uint32_t alt_end_off;
};

struct special_alt {
	struct list_head list;

	bool group;
	bool jump_or_nop;
	u8 key_addend;

	struct section *orig_sec;
	unsigned long orig_off;

	struct section *new_sec;
	unsigned long new_off;

	unsigned int orig_len, new_len, feature; /* group only */
};

int process_alt_data(struct objtool_file *file);

int special_get_alts(struct elf *elf, struct list_head *alts);

void arch_handle_alternative(struct special_alt *alt);

bool arch_support_alt_relocation(struct special_alt *special_alt,
				 struct instruction *insn,
				 struct reloc *reloc);
struct reloc *arch_find_switch_table(struct objtool_file *file,
				     struct instruction *insn,
				     unsigned long *table_size);
const char *arch_cpu_feature_name(int feature_number);

int process_fixup_entries(struct objtool_file *file);

void check_and_flatten_fixup_entries(void);

int process_exception_entries(struct objtool_file *file);

int process_bug_entries(struct objtool_file *file);

int process_alt_relocations(struct objtool_file *file);

struct fixup_entry *find_fe_altaddr(uint64_t addr);

int set_uncond_branch_target(uint32_t *insn,
		const uint64_t addr, uint64_t target);

int set_cond_branch_target(uint32_t *insn,
		const uint64_t addr, uint64_t target);


#endif /* _SPECIAL_H */
