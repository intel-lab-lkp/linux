/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Based on arch/x86/include/asm/trace/common.h
 *
 */

#ifndef _ASM_TRACE_COMMON_H
#define _ASM_TRACE_COMMON_H

#ifdef CONFIG_TRACING
DECLARE_STATIC_KEY_FALSE(trace_memabort_key);
#define trace_memabort_enabled()			\
	static_branch_unlikely(&trace_memabort_key)
#else
static inline bool trace_memabort_enabled(void) { return false; }
#endif

#endif
