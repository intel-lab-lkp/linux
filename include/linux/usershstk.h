/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SHSTK_H
#define _SHSTK_H

#ifndef __ASSEMBLY__
#include <linux/types.h>

unsigned long alloc_shstk(unsigned long addr, unsigned long size,
				 unsigned long token_offset, bool set_res_tok);
int shstk_setup(void);
int create_rstor_token(unsigned long ssp, unsigned long *token_addr);
bool cpu_supports_shadow_stack(void);
bool is_shstk_enabled(struct task_struct *task);
void set_shstk_base_size(struct task_struct *task, unsigned long base,
			unsigned long size);
void get_shstk_base_size(struct task_struct *task, unsigned long *base,
			unsigned long *size);
void set_shstk_ptr_and_enable(unsigned long ssp);
void set_thread_shstk_status(bool enable);
unsigned long adjust_shstk_size(unsigned long size);
void unmap_shadow_stack(u64 base, u64 size);

#endif /* __ASSEMBLY__ */

#endif /* _SHSTK_H */
