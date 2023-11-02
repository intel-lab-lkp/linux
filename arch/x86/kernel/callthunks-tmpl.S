// SPDX-License-Identifier: GPL-2.0-only

#include <asm/nospec-branch.h>

	.section .rodata
	.global skl_call_thunk_template
	.global skl_call_thunk_tail

skl_call_thunk_template:
	INCREMENT_CALL_DEPTH
skl_call_thunk_tail:
