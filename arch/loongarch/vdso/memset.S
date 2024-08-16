// SPDX-License-Identifier: GPL-2.0
/*
 * A copy of __memset_generic from arch/loongarch/lib/memset.S for vDSO.
 *
 * Copyright (C) 2020-2024 Loongson Technology Corporation Limited
 */

#include <asm/regdef.h>
#include <linux/linkage.h>

SYM_FUNC_START(memset)
	move	a3, a0
	beqz	a2, 2f

1:	st.b	a1, a0, 0
	addi.d	a0, a0, 1
	addi.d	a2, a2, -1
	bgt	a2, zero, 1b

2:	move	a0, a3
	jr	ra
SYM_FUNC_END(memset)

.hidden memset
