// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026 Meta Platforms, Inc. and affiliates. */

#include <vmlinux.h>
#include <errno.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include "bpf_misc.h"
#include "bpf_experimental.h"
#include "bpf_arena_common.h"

#ifdef __TARGET_ARCH_arm64
#define ARENA_VM_START ((1ull << 32) | (~0u - __PAGE_SIZE * 2 + 1))
#else
#define ARENA_VM_START ((1ull << 44) | (~0u - __PAGE_SIZE * 2 + 1))
#endif

struct {
	__uint(type, BPF_MAP_TYPE_ARENA);
	__uint(map_flags, BPF_F_MMAPABLE);
	__uint(max_entries, 2);
	__ulong(map_extra, ARENA_VM_START);
} arena SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, __u64);
} not_an_arena SEC(".maps");

/* Tells the runner whether the compiler emits the cast, and so whether the tests below exist. */
const volatile bool typed_arena_supported =
#ifdef __BPF_FEATURE_TYPED_ARENA_CAST
	true;
#else
	false;
#endif

#ifdef __BPF_FEATURE_TYPED_ARENA_CAST

/* 16-byte slot, 8 Mi objects in the default 128 MiB typed arena: the cast mask is 134217712 */
struct typed_obj {
	struct task_struct __kptr *task;
	__u64 value;
};

/* 32-byte slot */
struct other_obj {
	struct task_struct __kptr *task;
	__u64 a;
	__u64 b;
};

struct plain_obj {
	__u64 value;
};

/*
 * A program is associated with an arena by referencing the map. Programs that
 * only convert values they already hold reference it explicitly.
 */
#define arena_bind() asm volatile("r0 = %[m] ll" :: [m] "i"(&arena) : "r0")

/*
 * Objects as the opaque 64-bit values user space hands in. Converting one to a
 * pointer to a typed struct is where the compiler inserts the typed_arena_cast,
 * as it does before every use of such a pointer as an address. A cast of a
 * pointer the verifier already trusts lowers to nothing, so a lowered program
 * shows one sequence per value that enters. The values stay unset here: the
 * verifier does not care where a value lands, and at run time an unset one
 * names object 0 of its slice.
 */
void *ptr;
void *ptr2;

SEC("syscall")
__description("a cast masks the value into a slot and adds the typed arena base")
__success __retval(0) __log_level(2)
__msg("typed arena for struct typed_obj: slot 16 bytes")
__msg("R{{[0-9]}}=typed_arena_ptr_typed_obj(")
__xlated("r{{[0-9]}} &= 134217712")
__xlated("r12 = 0x{{[0-9a-f]+}}")
__xlated("r{{[0-9]}} += r12")
int cast_lowers_to_mask_and_base(void *ctx)
{
	struct typed_obj *obj;

	arena_bind();
	obj = ptr;
	return obj->value;
}

SEC("syscall")
__description("any 64-bit value casts: a raw arena pointer lands on an object")
__success __retval(0)
int cast_raw_arena_pointer(void *ctx)
{
	struct typed_obj *obj;
	void __arena *page;

	page = bpf_arena_alloc_pages(&arena, NULL, 1, NUMA_NO_NODE, 0);
	if (!page)
		return 1;
	obj = (void *)page;
	obj->value = 3;
	return obj->value - 3;
}

SEC("syscall")
__description("a typed pointer casts to itself, and to another type without knowing the source")
__success __retval(0)
int cast_typed_pointer_again(void *ctx)
{
	struct typed_obj *obj, *again;
	struct other_obj *other;
	void *opaque;

	arena_bind();
	obj = ptr;
	opaque = obj;
	again = opaque;
	if (again != obj)
		return 1;
	other = (struct other_obj *)obj;
	other->a = 1;
	return other->a - 1;
}

/*
 * The same struct lives in allocated objects, in map values and on the stack,
 * and the compiler casts every use of a pointer to it there too. Such a cast
 * is the identity: the pointer keeps its type and its state, nothing is
 * lowered, and the access follows that pointer's own rules. These programs
 * reference no arena, since nothing in them is sanitized.
 */
SEC("syscall")
__description("a cast of an allocated object of a typed struct is the identity")
__success __retval(0) __log_level(2)
__msg("typed_arena_cast(r{{[0-9]}}, {{[0-9]+}})")
__msg("R{{[0-9]}}=ptr_typed_obj(")
int cast_allocated_object_is_identity(void *ctx)
{
	struct typed_obj *obj;

	obj = bpf_obj_new(struct typed_obj);
	if (!obj)
		return 1;
	obj->value = 1;
	bpf_obj_drop(obj);
	return 0;
}

struct value_with_typed {
	__u64 pad;
	struct typed_obj obj;
};

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, struct value_with_typed);
} typed_in_map SEC(".maps");

SEC("syscall")
__description("a cast of a typed struct inside a map value is the identity")
__success __retval(0) __log_level(2)
__msg("typed_arena_cast(r{{[0-9]}}, {{[0-9]+}})")
__msg("R{{[0-9]}}=map_value(")
int cast_map_value_is_identity(void *ctx)
{
	struct value_with_typed *v;
	struct typed_obj *obj;
	__u32 key = 0;

	v = bpf_map_lookup_elem(&typed_in_map, &key);
	if (!v)
		return 1;
	obj = &v->obj;
	obj->value = 1;
	return 0;
}

SEC("syscall")
__description("a cast of a typed struct on the stack is the identity")
__success __retval(0) __log_level(2)
__msg("typed_arena_cast(r{{[0-9]}}, {{[0-9]+}})")
__msg("R{{[0-9]}}=fp-")
int cast_stack_object_is_identity(void *ctx)
{
	struct typed_obj local = {};
	struct typed_obj *obj = &local;

	barrier_var(obj);
	obj->value = 1;
	return local.value - 1;
}

SEC("syscall")
__description("the cast needs the program's arena")
__failure __msg("typed_arena_cast insn can only be used in a program that has an associated arena")
int cast_needs_arena(void *ctx)
{
	struct typed_obj *obj;

	obj = ptr;
	return obj->value;
}

struct locked_obj {
	struct bpf_spin_lock lock;
	__u64 value;
};

SEC("syscall")
__description("spin locks are not supported in typed arena objects yet")
__failure __msg("struct locked_obj field bpf_spin_lock is not supported in a typed arena")
int cast_rejects_spin_lock(void *ctx)
{
	struct locked_obj *obj;

	arena_bind();
	obj = ptr;
	return obj->value;
}

struct res_locked_obj {
	struct bpf_res_spin_lock lock;
	__u64 value;
};

SEC("syscall")
__description("resilient spin locks are not supported in typed arena objects yet")
__failure __msg("struct res_locked_obj field bpf_res_spin_lock is not supported in a typed arena")
int cast_rejects_res_spin_lock(void *ctx)
{
	struct res_locked_obj *obj;

	arena_bind();
	obj = ptr;
	return obj->value;
}

struct timer_obj {
	struct bpf_timer timer;
	__u64 value;
};

SEC("syscall")
__description("timers are not supported in typed arena objects")
__failure __msg("struct timer_obj field bpf_timer is not supported in a typed arena")
int cast_rejects_timer(void *ctx)
{
	struct timer_obj *obj;

	arena_bind();
	obj = ptr;
	return obj->value;
}

struct refcount_obj {
	struct bpf_refcount ref;
	struct task_struct __kptr *task;
	__u64 value;
};

SEC("syscall")
__description("refcounts are not supported in typed arena objects")
__failure __msg("struct refcount_obj field bpf_refcount is not supported in a typed arena")
int cast_rejects_refcount(void *ctx)
{
	struct refcount_obj *obj;

	arena_bind();
	obj = ptr;
	return obj->value;
}

struct list_node_obj {
	struct bpf_list_node node;
	__u64 value;
};

struct list_obj {
	struct bpf_list_head head __contains(list_node_obj, node);
	struct bpf_spin_lock lock;
};

/* The node structs must reach the BTF for the contains tags to resolve; nothing else uses them. */
struct list_node_obj *list_node_in_btf;

SEC("syscall")
__description("list heads are not supported in typed arena objects")
__failure __msg("struct list_obj field bpf_list_head is not supported in a typed arena")
int cast_rejects_list_head(void *ctx)
{
	struct list_obj *obj;

	arena_bind();
	obj = ptr;
	return obj == NULL;
}

struct rb_node_obj {
	struct bpf_rb_node node;
	__u64 value;
};

struct rb_obj {
	struct bpf_rb_root root __contains(rb_node_obj, node);
	struct bpf_spin_lock lock;
};

struct rb_node_obj *rb_node_in_btf;

SEC("syscall")
__description("rbtree roots are not supported in typed arena objects")
__failure __msg("struct rb_obj field bpf_rb_root is not supported in a typed arena")
int cast_rejects_rb_root(void *ctx)
{
	struct rb_obj *obj;

	arena_bind();
	obj = ptr;
	return obj == NULL;
}

struct size_not_pow2_obj {
	struct task_struct __kptr *task;
	__u64 value;
} __typed_arena_size(3M);

SEC("syscall")
__description("a typed arena size must be a power of two")
__failure __msg("struct size_not_pow2_obj has invalid typed arena size '3M'")
int size_rejects_not_pow2(void *ctx)
{
	struct size_not_pow2_obj *obj;

	arena_bind();
	obj = ptr;
	return obj->value;
}

struct size_below_page_obj {
	struct task_struct __kptr *task;
	__u64 value;
} __typed_arena_size(2048);

SEC("syscall")
__description("a typed arena is at least a page")
__failure __msg("struct size_below_page_obj has invalid typed arena size '2048'")
int size_rejects_below_page(void *ctx)
{
	struct size_below_page_obj *obj;

	arena_bind();
	obj = ptr;
	return obj->value;
}

struct size_above_region_obj {
	struct task_struct __kptr *task;
	__u64 value;
} __typed_arena_size(8G);

SEC("syscall")
__description("a typed arena is at most the region")
__failure __msg("struct size_above_region_obj has invalid typed arena size '8G'")
int size_rejects_above_region(void *ctx)
{
	struct size_above_region_obj *obj;

	arena_bind();
	obj = ptr;
	return obj->value;
}

/* 128 KiB slot in a 64 KiB typed arena */
struct size_below_slot_obj {
	struct task_struct __kptr *task;
	char pad[65536 + 8];
} __typed_arena_size(64K);

SEC("syscall")
__description("a typed arena holds at least one object")
__failure __msg("struct size_below_slot_obj does not fit its typed arena: slot 131072 bytes, size 65536 bytes")
int size_rejects_below_slot(void *ctx)
{
	struct size_below_slot_obj *obj;

	arena_bind();
	obj = ptr;
	return obj->pad[0];
}

struct size_conflict_obj {
	struct task_struct __kptr *task;
	__u64 value;
} __typed_arena_size(64K) __typed_arena_size(128K);

SEC("syscall")
__description("a struct declares one typed arena size")
__failure __msg("struct size_conflict_obj has conflicting typed arena size declarations")
int size_rejects_conflict(void *ctx)
{
	struct size_conflict_obj *obj;

	arena_bind();
	obj = ptr;
	return obj->value;
}

struct size_64k_obj {
	struct task_struct __kptr *task;
	__u64 value;
} __typed_arena_size(64K);

struct size_2m_obj {
	struct task_struct __kptr *task;
	__u64 value;
} __typed_arena_size(2M);

SEC("syscall")
__description("a typed arena size takes a suffix and sets the object count")
__success __retval(0) __log_level(2)
__msg("typed arena for struct size_64k_obj: slot 16 bytes")
__msg("size 65536 bytes")
__msg("typed arena for struct size_2m_obj: slot 16 bytes")
__msg("size 2097152 bytes")
int size_accepts_suffix(void *ctx)
{
	struct size_64k_obj *small;
	struct size_2m_obj *large;

	arena_bind();
	small = ptr;
	large = ptr;
	small->value = 1;
	large->value = 2;
	return small->value + large->value - 3;
}

/* 16 KiB slot: an object of more than a page, four of them in a 64 KiB typed arena */
struct big_obj {
	struct task_struct __kptr *task;
	char pad[8192];
} __typed_arena_size(64K);

SEC("syscall")
__description("an object larger than a page takes a slot of whole pages")
__success __retval(0) __log_level(2)
__msg("typed arena for struct big_obj: slot 16384 bytes")
int size_object_beyond_a_page(void *ctx)
{
	struct big_obj *obj;

	arena_bind();
	obj = ptr;
	obj->pad[0] = 1;
	obj->pad[8191] = 2;
	return obj->pad[0] + obj->pad[8191] - 3;
}

/* A slice is capped at 2 GiB, half the region: two of them fill it. */
struct huge_obj {
	struct task_struct __kptr *task;
	__u64 value;
} __typed_arena_size(2G);

struct huge_obj2 {
	struct task_struct __kptr *task;
	__u64 value;
} __typed_arena_size(2G);

SEC("syscall")
__description("the region has room for 4 GiB of typed arenas")
__failure __msg("no room in the typed arena region for struct typed_obj")
int size_region_runs_out(void *ctx)
{
	struct huge_obj *huge;
	struct huge_obj2 *huge2;
	struct typed_obj *obj;

	arena_bind();
	huge = ptr;
	huge2 = ptr;
	obj = ptr;
	return huge->value + huge2->value + obj->value;
}

SEC("syscall")
__description("a 32-bit copy of a typed pointer is a scalar, as for any pointer a privileged program holds")
__success __retval(0) __log_level(2)
__msg("R2=scalar(smin=0,smax=umax=0xffffffff,var_off=(0x0; 0xffffffff))")
int narrow_copy_yields_scalar(void *ctx)
{
	struct typed_obj *obj;

	arena_bind();
	obj = ptr;
	asm volatile("r1 = %[p];"
		     "w2 = w1;"
		     :: [p] "r"(obj)
		     : "r1", "r2");
	return 0;
}

SEC("syscall")
__description("a narrow store of a typed pointer to the stack is an invalid spill")
__failure __msg("invalid size of register spill")
int narrow_store_is_invalid_spill(void *ctx)
{
	struct typed_obj *obj;

	arena_bind();
	obj = ptr;
	asm volatile("r1 = %[p];"
		     "*(u32 *)(r10 - 8) = r1;"
		     :: [p] "r"(obj)
		     : "r1", "memory");
	return 0;
}

#endif /* __BPF_FEATURE_TYPED_ARENA_CAST */

char _license[] SEC("license") = "GPL";
