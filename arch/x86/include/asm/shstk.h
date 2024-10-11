/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_SHSTK_H
#define _ASM_X86_SHSTK_H

#ifndef __ASSEMBLY__
#include <linux/types.h>

struct task_struct;
struct ksignal;

#ifdef CONFIG_X86_USER_SHADOW_STACK
struct thread_shstk {
	u64	base;
	u64	size;
};

long shstk_prctl(struct task_struct *task, int option, unsigned long arg2);
void reset_thread_features(void);
unsigned long shstk_alloc_thread_stack(struct task_struct *p, unsigned long clone_flags,
				       unsigned long stack_size);
void shstk_free(struct task_struct *p);
int setup_signal_shadow_stack(struct ksignal *ksig);
int restore_signal_shadow_stack(void);
int shstk_update_last_frame(unsigned long val);
bool shstk_is_enabled(void);
#else
static inline long shstk_prctl(struct task_struct *task, int option,
			       unsigned long arg2) { return -EINVAL; }
static inline void reset_thread_features(void) {}
static inline unsigned long shstk_alloc_thread_stack(struct task_struct *p,
						     unsigned long clone_flags,
						     unsigned long stack_size) { return 0; }
static inline void shstk_free(struct task_struct *p) {}
static inline int setup_signal_shadow_stack(struct ksignal *ksig) { return 0; }
static inline int restore_signal_shadow_stack(void) { return 0; }
static inline int shstk_update_last_frame(unsigned long val) { return 0; }
static inline bool shstk_is_enabled(void) { return false; }
#endif /* CONFIG_X86_USER_SHADOW_STACK */

int arch_create_rstor_token(unsigned long ssp, unsigned long *token_addr);
bool arch_cpu_supports_shadow_stack(void);
bool arch_is_shstk_enabled(struct task_struct *task);
void arch_set_shstk_base_size(struct task_struct *task, unsigned long base,
			unsigned long size);
void arch_get_shstk_base_size(struct task_struct *task, unsigned long *base,
			unsigned long *size);
void arch_set_shstk_ptr_and_enable(unsigned long ssp);
void arch_set_thread_shstk_status(bool enable);
#endif /* __ASSEMBLY__ */

#endif /* _ASM_X86_SHSTK_H */
