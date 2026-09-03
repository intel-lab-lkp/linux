/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/*
 * Copyright(c) 2017 Intel Corporation.
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 */

#ifndef _HFI2_EXP_RCV_H
#define _HFI2_EXP_RCV_H
#include "hfi2.h"

#define EXP_TID_SET_EMPTY(set) (set.count == 0 && list_empty(&set.list))

#define EXP_TID_TIDLEN_MASK   0x7FFULL
#define EXP_TID_TIDLEN_SHIFT  0
#define EXP_TID_TIDCTRL_MASK  0x3ULL
#define EXP_TID_TIDCTRL_SHIFT 20
#define EXP_TID_TIDIDX_MASK   0x3FFULL
#define EXP_TID_TIDIDX_SHIFT  22

/* Expected buffer entry (TID) addressing supported by the hardware */
#define EXP_TID_ADDR_SHIFT    12
#define EXP_TID_ADDR_SIZE     BIT(EXP_TID_ADDR_SHIFT)

#define EXP_TID_GET(tid, field)	\
	(((tid) >> EXP_TID_TID##field##_SHIFT) & EXP_TID_TID##field##_MASK)

#define EXP_TID_SET(field, value)			\
	(((value) & EXP_TID_TID##field##_MASK) <<	\
	 EXP_TID_TID##field##_SHIFT)
#define EXP_TID_CLEAR(tid, field) ({					\
		(tid) &= ~(EXP_TID_TID##field##_MASK <<			\
			   EXP_TID_TID##field##_SHIFT);			\
		})
#define EXP_TID_RESET(tid, field, value) do {				\
		EXP_TID_CLEAR(tid, field);				\
		(tid) |= EXP_TID_SET(field, (value));			\
	} while (0)

/*
 * Define fields in the KDETH header so we can update the header
 * template.
 */
#define KDETH_OFFSET_SHIFT        0
#define KDETH_OFFSET_MASK         0x7fff
#define KDETH_OM_SHIFT            15
#define KDETH_OM_MASK             0x1
#define KDETH_TID_SHIFT           16
#define KDETH_TID_MASK            0x3ff
#define KDETH_TIDCTRL_SHIFT       26
#define KDETH_TIDCTRL_MASK        0x3
#define KDETH_INTR_SHIFT          28
#define KDETH_INTR_MASK           0x1
#define KDETH_SH_SHIFT            29
#define KDETH_SH_MASK             0x1
#define KDETH_KVER_SHIFT          30
#define KDETH_KVER_MASK           0x3
#define KDETH_JKEY_SHIFT          0x0
#define KDETH_JKEY_MASK           0xff
#define KDETH_HCRC_UPPER_SHIFT    16
#define KDETH_HCRC_UPPER_MASK     0xff
#define KDETH_HCRC_LOWER_SHIFT    24
#define KDETH_HCRC_LOWER_MASK     0xff

#define KDETH_GET(val, field)						\
	(((le32_to_cpu((val))) >> KDETH_##field##_SHIFT) & KDETH_##field##_MASK)
#define KDETH_SET(dw, field, val) do {					\
		u32 dwval = le32_to_cpu(dw);				\
		dwval &= ~(KDETH_##field##_MASK << KDETH_##field##_SHIFT); \
		dwval |= (((val) & KDETH_##field##_MASK) << \
			  KDETH_##field##_SHIFT);			\
		dw = cpu_to_le32(dwval);				\
	} while (0)

#define KDETH_RESET(dw, field, val) ({ dw = 0; KDETH_SET(dw, field, val); })

/* KDETH OM multipliers and switch over point */
#define KDETH_OM_SMALL     4
#define KDETH_OM_SMALL_SHIFT     2
#define KDETH_OM_LARGE     64
#define KDETH_OM_LARGE_SHIFT     6
#define KDETH_OM_MAX_SIZE  (1 << ((KDETH_OM_LARGE / KDETH_OM_SMALL) + 1))

struct tid_group {
	struct list_head list;
	u32 base;
	u8 size;
	u8 used;
	u8 map;
};

static inline void tid_group_add_tail(struct tid_group *grp,
				      struct exp_tid_set *set)
{
	list_add_tail(&grp->list, &set->list);
	set->count++;
}

static inline void tid_group_remove(struct tid_group *grp,
				    struct exp_tid_set *set)
{
	list_del_init(&grp->list);
	set->count--;
}

static inline void tid_group_move(struct tid_group *group,
				  struct exp_tid_set *s1,
				  struct exp_tid_set *s2)
{
	tid_group_remove(group, s1);
	tid_group_add_tail(group, s2);
}

static inline struct tid_group *tid_group_pop(struct exp_tid_set *set)
{
	struct tid_group *grp =
		list_first_entry(&set->list, struct tid_group, list);
	list_del_init(&grp->list);
	set->count--;
	return grp;
}

static inline u32 create_tid(u32 rcventry, u32 npages)
{
	u32 pair = rcventry & ~0x1;

	return EXP_TID_SET(IDX, pair >> 1) |
		EXP_TID_SET(CTRL, 1 << (rcventry - pair)) |
		EXP_TID_SET(LEN, npages);
}

/**
 * hfi2_tid_group_to_idx - convert an index to a group
 * @rcd - the receive context
 * @grp - the group pointer
 */
static inline u16
hfi2_tid_group_to_idx(struct hfi2_ctxtdata *rcd, struct tid_group *grp)
{
	return grp - &rcd->groups[0];
}

/**
 * hfi2_idx_to_tid_group - convert a group to an index
 * @rcd - the receive context
 * @idx - the index
 */
static inline struct tid_group *
hfi2_idx_to_tid_group(struct hfi2_ctxtdata *rcd, u16 idx)
{
	return &rcd->groups[idx];
}

int hfi2_alloc_ctxt_rcv_groups(struct hfi2_ctxtdata *rcd);
void hfi2_free_ctxt_rcv_groups(struct hfi2_ctxtdata *rcd);
void hfi2_exp_tid_group_init(struct hfi2_ctxtdata *rcd);

#endif /* _HFI2_EXP_RCV_H */
