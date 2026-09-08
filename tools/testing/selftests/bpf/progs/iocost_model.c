// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026 Tao Cui */
/*
 * Example iocost cost model: the builtin linear HDD formula with all
 * costs doubled.
 *
 * The constants mirror what calc_lcoefs() derives from the AUTOP_HDD
 * defaults (rbps=174019176 rseqiops=41708 rrandiops=370, w-side
 * analog) in vtime units where 1s == 2^37, expressed with the same
 * round-up divisions so they cannot drift from the kernel, so a
 * device switched to ctrl=bpf with this model registered charges
 * exactly twice the builtin model under the same workload, which
 * makes it a convenient way to verify that accounting goes through
 * the BPF path.
 */
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

/* VTIME_PER_SEC comes from vmlinux.h (a BTF enum constant) */
#define IOC_PAGE_SIZE		4096
#define IOC_SECT_TO_PAGE_SHIFT	3	/* 512B sectors to 4kB pages */
#define LCOEF_RANDIO_PAGES	4096	/* 16MB seek threshold */
#define IOCOST_COST_F_MERGE	(1ULL << 0)	/* not in BTF: a plain macro */

/* DIV64_U64_ROUND_UP / DIV_ROUND_UP_ULL equivalents, folded at
 * compile time */
#define RU(x, y)		((x) / (y) + (((x) % (y)) ? 1 : 0))

#define RBPS	174019176ULL
#define RSEQIOPS	41708ULL
#define RRANDIOPS	370ULL
#define WBPS	178075866ULL
#define WSEQIOPS	42705ULL
#define WRANDIOPS	378ULL

#define RPAGE	(RU(VTIME_PER_SEC, RU(RBPS, IOC_PAGE_SIZE)))
#define RSEQIO	(RU(VTIME_PER_SEC, RSEQIOPS) - RPAGE)
#define RRANDIO	(RU(VTIME_PER_SEC, RRANDIOPS) - RPAGE)
#define WPAGE	(RU(VTIME_PER_SEC, RU(WBPS, IOC_PAGE_SIZE)))
#define WSEQIO	(RU(VTIME_PER_SEC, WSEQIOPS) - WPAGE)
#define WRANDIO	(RU(VTIME_PER_SEC, WRANDIOPS) - WPAGE)

static __always_inline u64 builtin_cost(u64 op, u64 nbytes, u64 sector,
					u64 cursor, u64 flags)
{
	u64 pages, seek_pages = 0, base, coef_page;

	if (!nbytes)
		return 0;

	switch (op) {
	case REQ_OP_READ:
		base = RSEQIO;
		coef_page = RPAGE;
		break;
	case REQ_OP_WRITE:
		base = WSEQIO;
		coef_page = WPAGE;
		break;
	default:
		return 0;
	}

	if (cursor) {
		seek_pages = sector > cursor ? sector - cursor
					     : cursor - sector;
		seek_pages >>= IOC_SECT_TO_PAGE_SHIFT;
		if (seek_pages > LCOEF_RANDIO_PAGES)
			base = (op == REQ_OP_READ) ? RRANDIO : WRANDIO;
	}

	pages = RU(nbytes, IOC_PAGE_SIZE);
	if (flags & IOCOST_COST_F_MERGE)
		base = 0;
	return base + pages * coef_page;
}

SEC("struct_ops")
u64 BPF_PROG(iocost_2x_calc_cost, u64 op, u64 nbytes, u64 sector,
	     u64 cursor, u64 iocg_id, u64 flags)
{
	return 2 * builtin_cost(op, nbytes, sector, cursor, flags);
}

SEC(".struct_ops")
struct iocost_model_ops iocost_2x = {
	.calc_cost = (void *)iocost_2x_calc_cost,
};

char LICENSE[] SEC("license") = "GPL";
