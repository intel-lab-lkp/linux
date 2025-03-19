/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_KMMSCAND_H_
#define _LINUX_KMMSCAND_H_

#ifdef CONFIG_KMMSCAND
extern void __kmmscand_enter(struct mm_struct *mm);
extern void __kmmscand_exit(struct mm_struct *mm);

static inline void kmmscand_execve(struct mm_struct *mm)
{
	__kmmscand_enter(mm);
}

static inline void kmmscand_fork(struct mm_struct *mm, struct mm_struct *oldmm)
{
	mm->pte_scan_scale = oldmm->pte_scan_scale;
	__kmmscand_enter(mm);
}

static inline void kmmscand_exit(struct mm_struct *mm)
{
	__kmmscand_exit(mm);
}
#else /* !CONFIG_KMMSCAND */
static inline void __kmmscand_enter(struct mm_struct *mm) {}
static inline void __kmmscand_exit(struct mm_struct *mm) {}
static inline void kmmscand_execve(struct mm_struct *mm) {}
static inline void kmmscand_fork(struct mm_struct *mm, struct mm_struct *oldmm) {}
static inline void kmmscand_exit(struct mm_struct *mm) {}
#endif
#endif /* _LINUX_KMMSCAND_H_ */
