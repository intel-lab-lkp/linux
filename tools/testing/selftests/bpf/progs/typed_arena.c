// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026 Meta Platforms, Inc. and affiliates. */
#include <vmlinux.h>
#include <bpf/bpf_helpers.h>
#include "bpf_experimental.h"
#include "bpf_arena_common.h"
#include "../test_kmods/bpf_testmod_kfunc.h"

struct {
	__uint(type, BPF_MAP_TYPE_ARENA);
	__uint(map_flags, BPF_F_MMAPABLE);
	__uint(max_entries, 8);
#ifdef __TARGET_ARCH_arm64
	__ulong(map_extra, 0x1ull << 32);
#else
	__ulong(map_extra, 0x1ull << 44);
#endif
} arena SEC(".maps");

/* Tells the runner whether the compiler emits the cast, and so whether the programs below exist. */
const volatile bool typed_arena_supported =
#ifdef __BPF_FEATURE_TYPED_ARENA_CAST
	true;
#else
	false;
#endif

/* 16-byte slots: a 256 KiB typed arena, 64 pages of 4 KiB, one word of the chunk bitmap */
struct obj {
	struct prog_test_ref_kfunc __kptr *ref;
	__u64 value;
} __typed_arena_size(256K);

/* 16-byte slots: an 8 MiB typed arena, 2048 pages of 4 KiB, more than one batch of the allocator */
struct wide_obj {
	struct prog_test_ref_kfunc __kptr *ref;
	__u64 value;
} __typed_arena_size(8M);

/* An object of 8 KiB, two pages of 4 KiB: its chunk is the object */
struct big_obj {
	struct prog_test_ref_kfunc __kptr *ref;
	__u64 value;
	char pad[8192 - 16];
} __typed_arena_size(64K);

/* 32-byte slots, linked through a typed pointer field */
struct arena_list_node {
	struct prog_test_ref_kfunc __kptr *ref;
	struct arena_list_node *next;
	__u64 value;
} __typed_arena_size(64K);

#define LIST_LEN 64
/* Objects sit at slot strides, the power of two covering the struct, not at sizeof. */
#define NODE_SLOT 32

/* An object as an opaque 64-bit value user space hands back; any value casts to an object. */
void *ptr;
/* Pages requested; the allocator writes back what it granted. */
__u32 page_cnt = 1;
__u64 value;
__u64 ref_cnt;
void *list_head;
__u64 list_sum;

#ifdef __BPF_FEATURE_TYPED_ARENA_CAST

/*
 * A program is associated with an arena by referencing the map. Programs that
 * only cast values they hold reference it explicitly.
 */
#define arena_bind() asm volatile("r0 = %[m] ll" :: [m] "i"(&arena) : "r0")

#define TYPE_ID(T) bpf_core_type_id_local(T)

SEC("syscall")
int alloc(void *ctx)
{
	struct obj *o;

	o = bpf_typed_arena_alloc_pages(&arena, TYPE_ID(struct obj), NULL, &page_cnt, NUMA_NO_NODE);
	if (!o)
		return 1;
	ptr = o;
	return 0;
}

/* Take the chunks at ptr: 0 when granted, 1 when refused. */
SEC("syscall")
int alloc_at(void *ctx)
{
	struct obj *hint, *o;

	hint = ptr;
	o = bpf_typed_arena_alloc_pages(&arena, TYPE_ID(struct obj), hint, &page_cnt, NUMA_NO_NODE);
	if (!o)
		return 1;
	return o != hint;
}

SEC("syscall")
int free_pages(void *ctx)
{
	struct obj *o = ptr;

	bpf_typed_arena_free_pages(&arena, TYPE_ID(struct obj), o, page_cnt);
	return 0;
}

SEC("syscall")
int write_value(void *ctx)
{
	struct obj *o;

	arena_bind();
	o = ptr;
	o->value = value;
	return 0;
}

SEC("syscall")
int read_value(void *ctx)
{
	struct obj *o;

	arena_bind();
	o = ptr;
	value = o->value;
	return 0;
}

/* Leave a reference to the test module's object in the kptr field. */
SEC("syscall")
int stash_ref(void *ctx)
{
	struct prog_test_ref_kfunc *p, *old;
	unsigned long sp = 0;
	struct obj *o;

	arena_bind();
	p = bpf_kfunc_call_test_acquire(&sp);
	if (!p)
		return 1;
	o = ptr;
	old = bpf_kptr_xchg(&o->ref, p);
	if (old)
		bpf_kfunc_call_test_release(old);
	return 0;
}

SEC("syscall")
int read_ref_cnt(void *ctx)
{
	struct prog_test_ref_kfunc *p;
	unsigned long sp = 0;

	p = bpf_kfunc_call_test_acquire(&sp);
	if (!p)
		return 1;
	/* the acquire above holds one */
	ref_cnt = p->cnt.refs.counter - 1;
	bpf_kfunc_call_test_release(p);
	return 0;
}

SEC("syscall")
int wide_alloc(void *ctx)
{
	struct wide_obj *o;

	o = bpf_typed_arena_alloc_pages(&arena, TYPE_ID(struct wide_obj), NULL, &page_cnt,
					NUMA_NO_NODE);
	if (!o)
		return 1;
	ptr = o;
	return 0;
}

/* Write the value into the object at ptr and read it back. */
SEC("syscall")
int wide_touch(void *ctx)
{
	struct wide_obj *o;

	arena_bind();
	o = ptr;
	o->value = value;
	return o->value != value;
}

SEC("syscall")
int wide_free(void *ctx)
{
	struct wide_obj *o = ptr;

	bpf_typed_arena_free_pages(&arena, TYPE_ID(struct wide_obj), o, page_cnt);
	return 0;
}

SEC("syscall")
int big_alloc(void *ctx)
{
	struct big_obj *o;

	o = bpf_typed_arena_alloc_pages(&arena, TYPE_ID(struct big_obj), NULL, &page_cnt,
					NUMA_NO_NODE);
	if (!o)
		return 1;
	ptr = o;
	return 0;
}

/* Write both pages of the object at ptr and read them back. */
SEC("syscall")
int big_touch(void *ctx)
{
	struct big_obj *o;

	arena_bind();
	o = ptr;
	o->value = value;
	o->pad[sizeof(o->pad) - 1] = 1;
	return o->value != value || o->pad[sizeof(o->pad) - 1] != 1;
}

SEC("syscall")
int big_free(void *ctx)
{
	struct big_obj *o = ptr;

	bpf_typed_arena_free_pages(&arena, TYPE_ID(struct big_obj), o, page_cnt);
	return 0;
}

/* A page of nodes, the first LIST_LEN linked in order with values 1..LIST_LEN, ending in NULL. */
SEC("syscall")
int list_build(void *ctx)
{
	struct arena_list_node *n;
	__u32 cnt = 1;
	void *page;
	int i;

	page = bpf_typed_arena_alloc_pages(&arena, TYPE_ID(struct arena_list_node), NULL, &cnt,
					   NUMA_NO_NODE);
	if (!page)
		return 1;
	for (i = 0; i < LIST_LEN; i++) {
		n = page + i * NODE_SLOT;
		n->value = i + 1;
		if (i + 1 < LIST_LEN)
			n->next = page + (i + 1) * NODE_SLOT;
		else
			n->next = NULL;
	}
	list_head = page;
	return 0;
}

/*
 * Walk the list through its typed pointer fields with no cast; the NULL that
 * ends it is compared raw.
 */
SEC("syscall")
int list_sum_values(void *ctx)
{
	struct arena_list_node *n;
	__u64 sum = 0;
	int i;

	arena_bind();
	n = list_head;
	for (i = 0; n && i < LIST_LEN + 1; i++) {
		sum += n->value;
		n = n->next;
	}
	list_sum = sum;
	return 0;
}

SEC("syscall")
int list_bump_values(void *ctx)
{
	struct arena_list_node *n;
	int i;

	arena_bind();
	n = list_head;
	for (i = 0; n && i < LIST_LEN + 1; i++) {
		n->value += 1;
		n = n->next;
	}
	return 0;
}

/*
 * Dereference the NULL that ends the list: it lands on object 0 of the node
 * slice, marked with the value first through a NULL pointer held directly.
 */
SEC("syscall")
int list_deref_end(void *ctx)
{
	struct arena_list_node *zero = NULL, *n, *m;
	int i;

	arena_bind();
	/*
	 * The compiler treats a NULL dereference as undefined and would drop
	 * the store, the checks and the last hop; the barriers keep each
	 * pointer opaque so that the store, the walk and the final dereference
	 * are emitted.
	 */
	barrier_var(zero);
	zero->value = value;
	n = list_head;
	for (i = 0; i < LIST_LEN; i++) {
		m = n->next;
		barrier_var(m);
		if (!m)
			break;
		n = m;
	}
	m = n->next;
	barrier_var(m);
	list_sum = m->value;
	return 0;
}

#else /* !__BPF_FEATURE_TYPED_ARENA_CAST */

/* Keep the skeleton's shape without the compiler support; the runner skips the tests. */
#define STUB(name) SEC("syscall") int name(void *ctx) { return 0; }
STUB(alloc)
STUB(alloc_at)
STUB(free_pages)
STUB(write_value)
STUB(read_value)
STUB(stash_ref)
STUB(read_ref_cnt)
STUB(wide_alloc)
STUB(wide_touch)
STUB(wide_free)
STUB(big_alloc)
STUB(big_touch)
STUB(big_free)
STUB(list_build)
STUB(list_sum_values)
STUB(list_bump_values)
STUB(list_deref_end)

#endif /* __BPF_FEATURE_TYPED_ARENA_CAST */

char _license[] SEC("license") = "GPL";
