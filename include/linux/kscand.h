/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_KSCAND_H_
#define _LINUX_KSCAND_H_

#ifdef CONFIG_KSCAND
extern void __kscand_enter(struct mm_struct *mm);
extern void __kscand_exit(struct mm_struct *mm);

static inline void kscand_execve(struct mm_struct *mm)
{
	__kscand_enter(mm);
}

static inline void kscand_fork(struct mm_struct *mm, struct mm_struct *oldmm)
{
	__kscand_enter(mm);
}

static inline void kscand_exit(struct mm_struct *mm)
{
	__kscand_exit(mm);
}
#else /* !CONFIG_KSCAND */
static inline void __kscand_enter(struct mm_struct *mm) {}
static inline void __kscand_exit(struct mm_struct *mm) {}
static inline void kscand_execve(struct mm_struct *mm) {}
static inline void kscand_fork(struct mm_struct *mm, struct mm_struct *oldmm) {}
static inline void kscand_exit(struct mm_struct *mm) {}
#endif
#endif /* _LINUX_KSCAND_H_ */
