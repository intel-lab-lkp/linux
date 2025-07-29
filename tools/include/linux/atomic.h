/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __TOOLS_LINUX_ATOMIC_H
#define __TOOLS_LINUX_ATOMIC_H

#include <asm/atomic.h>

void atomic_long_set(atomic_long_t *v, long i);

/* atomic_cmpxchg_relaxed */
#ifndef atomic_cmpxchg_relaxed
#define  atomic_cmpxchg_relaxed		atomic_cmpxchg
#define  atomic_cmpxchg_release         atomic_cmpxchg
#endif /* atomic_cmpxchg_relaxed */

#ifndef atomic_cmpxchg_acquire
#define atomic_cmpxchg_acquire		atomic_cmpxchg
#endif

#ifndef atomic_try_cmpxchg_acquire
#define atomic_try_cmpxchg_acquire	atomic_try_cmpxchg
#endif

#ifndef atomic_try_cmpxchg_relaxed
#define atomic_try_cmpxchg_relaxed	atomic_try_cmpxchg
#endif

#ifndef atomic_fetch_or_acquire
#define atomic_fetch_or_acquire		atomic_fetch_or
#endif

#ifndef xchg_relaxed
#define xchg_relaxed		xchg
#endif

#ifndef cmpxchg_release
#define cmpxchg_release		cmpxchg
#endif

#define atomic_cond_read_acquire(v, c) smp_cond_load_acquire(&(v)->counter, (c))
#define atomic_cond_read_relaxed(v, c) smp_cond_load_relaxed(&(v)->counter, (c))

#endif /* __TOOLS_LINUX_ATOMIC_H */
