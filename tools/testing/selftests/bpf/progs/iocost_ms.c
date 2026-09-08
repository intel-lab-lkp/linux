// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026 Tao Cui */
/*
 * Example multi-stream sequentiality detection cost model.
 *
 * The builtin model keeps a single cursor per cgroup, so two
 * interleaved sequential readers in one cgroup are all priced random
 * (measured 89x overcharge, 12.9x throughput collapse), while random
 * IO inside a hot window smaller than the 16MB seek threshold is
 * priced sequential (measured 107x undercharge).  This model replaces
 * the single cursor with a per-cgroup table of stream slots, keyed by
 * the iocg_id argument: an IO is sequential iff its sector matches the
 * expected next sector of any tracked stream.  Interleaved streams
 * keep their own slots, and windowed random IO rarely matches a
 * moving expectation.
 */
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

/* VTIME_PER_SEC comes from vmlinux.h (a BTF enum constant) */
#define IOC_PAGE_SIZE		4096
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

#define NSLOTS	4

struct streams {
	__u64 expected[NSLOTS];	/* next expected sector, per stream */
	__u64 stamp[NSLOTS];	/* LRU stamp, 0 = empty */
};

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__type(key, __u64);
	__type(value, struct streams);
	__uint(max_entries, 64);
} stream_tab SEC(".maps");

SEC("struct_ops")
u64 BPF_PROG(iocost_ms_calc_cost, u64 op, u64 nbytes, u64 sector,
	     u64 cursor, u64 iocg_id, u64 flags)
{
	struct streams *s;
	u64 page, seqio, randio, base, advance, now;
	u32 i, victim = 0, found = 0xFFFFFFFF;

	if (op != REQ_OP_READ && op != REQ_OP_WRITE)
		return 0;	/* delegate the rest to builtin */
	if (!nbytes)
		return 0;

	if (op == REQ_OP_READ) {
		page = RPAGE; seqio = RSEQIO; randio = RRANDIO;
	} else {
		page = WPAGE; seqio = WSEQIO; randio = WRANDIO;
	}
	advance = RU(nbytes, 512);	/* sectors */

	s = bpf_map_lookup_elem(&stream_tab, &iocg_id);
	if (!s) {
		struct streams zero = {};

		bpf_map_update_elem(&stream_tab, &iocg_id, &zero,
				    BPF_NOEXIST);
		s = bpf_map_lookup_elem(&stream_tab, &iocg_id);
		if (!s)
			return seqio + RU(nbytes, IOC_PAGE_SIZE) * page;
	}

	if (flags & IOCOST_COST_F_MERGE) {
		base = 0;
		goto out;
	}

	now = bpf_ktime_get_ns();
	for (i = 0; i < NSLOTS; i++) {
		if (s->expected[i] == sector && s->stamp[i]) {
			found = i;
			break;
		}
	}
	if (found != 0xFFFFFFFF) {
		base = seqio;
		s->expected[found] = sector + advance;
		s->stamp[found] = now;
	} else {
		base = randio;
		for (i = 1; i < NSLOTS; i++) {
			if (s->stamp[i] < s->stamp[victim])
				victim = i;
		}
		s->expected[victim] = sector + advance;
		s->stamp[victim] = now;
	}
out:
	return base + RU(nbytes, IOC_PAGE_SIZE) * page;
}

SEC(".struct_ops")
struct iocost_model_ops iocost_ms = {
	.calc_cost = (void *)iocost_ms_calc_cost,
};

char LICENSE[] SEC("license") = "GPL";
