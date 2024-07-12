/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_GENERIC_ASI_H
#define __ASM_GENERIC_ASI_H

#include <linux/types.h>

#ifndef CONFIG_MITIGATION_ADDRESS_SPACE_ISOLATION

#define ASI_MAX_NUM_ORDER		0
#define ASI_MAX_NUM			0

#ifndef _ASSEMBLY_

struct asi_hooks {};
struct asi {};

static inline
int asi_register_class(const char *name, const struct asi_hooks *ops)
{
	return 0;
}

static inline void asi_unregister_class(int asi_index) { }

static inline void asi_init_mm_state(struct mm_struct *mm) { }

static inline int asi_init(struct mm_struct *mm, int asi_index,
			   struct asi **asi_out)
{
	return 0;
}

static inline void asi_destroy(struct asi *asi) { }

static inline void asi_enter(struct asi *asi) { }

static inline void asi_relax(void) { }

static inline bool asi_is_relaxed(void) { return true; }

static inline bool asi_is_tense(void) { return false; }

static inline bool asi_in_critical_section(void) { return false; }

static inline void asi_exit(void) { }

static inline bool asi_is_restricted(void) { return false; }

static inline struct asi *asi_get_current(void) { return NULL; }

static inline struct asi *asi_get_target(struct task_struct *p) { return NULL; }

static inline pgd_t *asi_pgd(struct asi *asi) { return NULL; }

static inline void asi_init_thread_state(struct thread_struct *thread) { }

static inline void asi_intr_enter(void) { }

static inline int asi_intr_nest_depth(void) { return 0; }

static inline void asi_intr_exit(void) { }

static inline int asi_map(struct asi *asi, void *addr, size_t len)
{
	return 0;
}

static inline
void asi_unmap(struct asi *asi, void *addr, size_t len) { }

static inline
void asi_flush_tlb_range(struct asi *asi, void *addr, size_t len) { }

#define static_asi_enabled() false

static inline void asi_check_boottime_disable(void) { }

#endif  /* !_ASSEMBLY_ */

#endif /* !CONFIG_MITIGATION_ADDRESS_SPACE_ISOLATION */

#endif
