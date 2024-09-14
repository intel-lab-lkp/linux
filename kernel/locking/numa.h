/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_NUMA_LOCK_H
#define __LINUX_NUMA_LOCK_H
#include "mcs_spinlock.h"

struct optimistic_spin_node {
	struct optimistic_spin_node *next, *prev;
	int numa;
	int locked; /* 1 if lock acquired */
	int cpu; /* encoded CPU # + 1 value */
	u32 serial;
};


struct _numa_buf {
	void *numa_ptr;
	struct list_head list;
	u32 lockaddr;
	u32 highaddr;
	u8 idle;
	u8 type;
	u16 index;
};

struct cache_padding {
	char x[0];
} ____cacheline_internodealigned_in_smp;
#define CACHE_PADDING(name)	struct cache_padding name

struct _numa_lock {
	atomic_t tail ____cacheline_aligned_in_smp;
	atomic_t addr;
	u8 shift;
	u8 stopping;
	u16 numa_nodes;
	u32 accessed;
	uint64_t totalaccessed;
	u32 nodeswitched;
	atomic_t initlock;
	atomic_t pending;
	union {
		struct mcs_spinlock mcs_node;
		struct optimistic_spin_node osq_node;
	};
	CACHE_PADDING(pad);
};

struct numa_cpu_info {
	__u8	x86_model;
	/* CPU family */
	__u8	x86;
	/* CPU vendor */
	__u8	x86_vendor;
	__u8	x86_reserved;
	u32	feature1;
};

#define NUMAEXPAND 1

#define COHORT_START 1
#define ACQUIRE_NUMALOCK (UINT_MAX-1)
#define NODE_WAIT UINT_MAX
#define LOCK_NUMALOCK 1
#define UNLOCK_NUMALOCK 0

#define NUMALOCKDYNAMIC 0xff
#define TURNTONUMAREADY 0xa5a5
#define NUMATURNBACKREADY 0x5a5a

#define NUMA_LOCKED_VAL 0xf5efef
#define NUMA_UNLOCKED_VAL 0

#define NUMASTEERMASK 0xf0000000
#define HIGH32BITMASK 0xffffffff00000000
#define LOW32MASK 0xffffffff

extern int NUMASHIFT;
extern int NUMACLUSTERS;
extern int zx_numa_lock_total;
extern struct _numa_buf *zx_numa_entry;
extern atomic_t numa_count;
extern int enable_zx_numa_osq_lock;
extern u32 zx_numa_lock;
extern int dynamic_enable;
extern struct kmem_cache *zx_numa_lock_cachep;

static inline u32 ptrmask(void *s)
{
	return (uint64_t)s & LOW32MASK;
}
inline void *get_numa_lock(int index);

int zx_check_numa_dynamic_locked(u32 lockaddr, struct _numa_lock *_numa_lock,
		int t);
int zx_numa_lock_ptr_get(void *p);
void numa_lock_init_data(struct _numa_lock *s, int clusters, u32 lockval,
		u32 lockaddr);
#endif
