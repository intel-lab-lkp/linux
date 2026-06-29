/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * RISC-V common stack unwinder types and helpers.
 *
 * See: arch/arm64/include/asm/stacktrace/common.h for the reference
 * implementation.
 *
 * Copyright (C) 2026
 */
#ifndef __ASM_RISCV_STACKTRACE_COMMON_H
#define __ASM_RISCV_STACKTRACE_COMMON_H

#include <linux/compiler.h>
#include <linux/errno.h>
#include <linux/types.h>

#include <asm/stacktrace/frame.h>

/**
 * struct stack_info - describes the bounds of a stack.
 *
 * @low:  The lowest valid address on the stack.
 * @high: The highest valid address on the stack.
 */
struct stack_info {
	unsigned long low;
	unsigned long high;
};

/**
 * struct unwind_state - state used for robust unwinding.
 *
 * @fp:        The fp value in the frame record (or the real fp).
 * @pc:        The ra value in the frame record (or the real ra).
 *
 * @stack:     The stack currently being unwound.
 * @stacks:    An array of stacks which can be unwound.
 * @nr_stacks: The number of stacks in @stacks.
 */
struct unwind_state {
	unsigned long fp;
	unsigned long pc;

	struct stack_info stack;
	struct stack_info *stacks;
	int nr_stacks;
};

/**
 * stackinfo_get_unknown() - Get an unknown stack_info.
 *
 * Return: a stack_info with low and high set to 0.
 */
static inline struct stack_info stackinfo_get_unknown(void)
{
	return (struct stack_info) {
		.low = 0,
		.high = 0,
	};
}

/**
 * stackinfo_on_stack() - Check whether an object is fully within a stack.
 *
 * @info: The stack to check against.
 * @sp:   The base address of the object.
 * @size: The size of the object.
 *
 * Return: true if the object is fully contained within the stack.
 */
static inline bool stackinfo_on_stack(const struct stack_info *info,
				      unsigned long sp, unsigned long size)
{
	if (!info->low)
		return false;

	if (sp < info->low || sp + size < sp || sp + size > info->high)
		return false;

	return true;
}

/**
 * unwind_init_common() - Initialize the common parts of the unwind state.
 *
 * @state: the unwind state to initialize.
 */
static inline void unwind_init_common(struct unwind_state *state)
{
	state->stack = stackinfo_get_unknown();
}

/**
 * unwind_find_stack() - Find the accessible stack which entirely contains an
 * object.
 *
 * @state: the current unwind state.
 * @sp:    the base address of the object.
 * @size:  the size of the object.
 *
 * Return: a pointer to the relevant stack_info if found; NULL otherwise.
 */
static inline struct stack_info *unwind_find_stack(struct unwind_state *state,
						   unsigned long sp,
						   unsigned long size)
{
	struct stack_info *info = &state->stack;

	if (stackinfo_on_stack(info, sp, size))
		return info;

	for (int i = 0; i < state->nr_stacks; i++) {
		info = &state->stacks[i];
		if (stackinfo_on_stack(info, sp, size))
			return info;
	}

	return NULL;
}

/**
 * unwind_consume_stack() - Update stack boundaries so that future unwind steps
 * cannot consume this object again.
 *
 * @state: the current unwind state.
 * @info:  the stack_info of the stack containing the object.
 * @sp:    the base address of the object.
 * @size:  the size of the object.
 *
 * Stack transitions are strictly one-way, and once we've
 * transitioned from one stack to another, it's never valid to
 * unwind back to the old stack.
 *
 * Note that stacks can nest in several valid orders, e.g.
 *
 *   TASK -> IRQ -> OVERFLOW
 *
 * ... so we do not check the specific order of stack
 * transitions.
 */
static inline void unwind_consume_stack(struct unwind_state *state,
					struct stack_info *info,
					unsigned long sp,
					unsigned long size)
{
	struct stack_info tmp;

	tmp = *info;
	*info = stackinfo_get_unknown();
	state->stack = tmp;

	/*
	 * Future unwind steps can only consume stack above this frame record.
	 * Update the current stack to start immediately above it.
	 */
	state->stack.low = sp + size;
}

#endif /* __ASM_RISCV_STACKTRACE_COMMON_H */
