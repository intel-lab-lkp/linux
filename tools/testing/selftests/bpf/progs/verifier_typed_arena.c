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

SEC("syscall")
__description("a scalar field is written and read natively, on a chunk faulted to scratch")
__success __retval(7)
__stderr("ERROR: Typed arena WRITE access to unallocated struct typed_obj at 0x{{[0-9a-f]+}}")
int access_scalar_field(void *ctx)
{
	struct typed_obj *obj;

	arena_bind();
	obj = ptr;
	obj->value = 7;
	return obj->value;
}

SEC("syscall")
__description("atomics run natively on a scalar field")
__success __retval(3)
int access_atomic(void *ctx)
{
	struct typed_obj *obj;

	arena_bind();
	obj = ptr;
	obj->value = 1;
	__sync_fetch_and_add(&obj->value, 2);
	return obj->value;
}

SEC("syscall")
__description("pointer arithmetic stays inside the object")
__success __retval(9)
int access_after_arithmetic(void *ctx)
{
	struct typed_obj *obj;
	void *p;

	arena_bind();
	obj = ptr;
	p = obj;
	asm volatile("%[p] += 8" : [p] "+r"(p));
	*(__u64 *)p = 9;
	return obj->value;
}

SEC("syscall")
__description("an access beyond the object is rejected")
__failure __msg("access beyond struct typed_obj")
int access_beyond_object(void *ctx)
{
	struct typed_obj *obj;

	arena_bind();
	obj = ptr;
	((__u64 *)obj)[2] = 1;
	return 0;
}

SEC("syscall")
__description("a negative offset is rejected")
__failure __msg("invalid negative access")
int access_negative_offset(void *ctx)
{
	struct typed_obj *obj;
	void *p;

	arena_bind();
	obj = ptr;
	p = (void *)obj - 8;
	return *(__u64 *)p;
}

SEC("syscall")
__description("a variable offset is rejected")
__failure __msg("{{variable (offset|typed_arena_ptr_ access)}}")
int access_variable_offset(void *ctx)
{
	struct typed_obj *obj;
	void *p;

	arena_bind();
	obj = ptr;
	p = (void *)obj + (bpf_get_prandom_u32() & 8);
	return *(__u64 *)p;
}

struct mixed_obj {
	struct task_struct __kptr *task;
	struct {
		__u32 a;
		__u32 b;
	} inner;
	struct task_struct *ptr;
};

SEC("syscall")
__description("nested structs are walked and pointers to kernel structs load as scalars")
__success __retval(3)
int access_nested_and_kernel_pointer_fields(void *ctx)
{
	struct mixed_obj *obj;

	arena_bind();
	obj = ptr;
	obj->inner.b = 3;
	if (obj->ptr)
		return 1;
	return obj->inner.b;
}

SEC("syscall")
__description("helpers do not take typed arena pointers as memory")
__failure __msg("R1 type=typed_arena_ptr_ expected=")
int helper_rejects_typed_pointer(void *ctx)
{
	struct typed_obj *obj;
	__u64 src = 0;

	arena_bind();
	obj = ptr;
	bpf_probe_read_kernel(&obj->value, sizeof(obj->value), &src);
	return 0;
}

struct arena_node {
	__u64 v;
};

struct kptr_obj {
	struct task_struct __kptr *task;
	struct arena_node __kptr *node;
	__u64 value;
};

SEC("syscall")
__description("a kernel kptr is exchanged into and out of an object")
__success __retval(0)
int kptr_xchg_task(void *ctx)
{
	struct task_struct *task, *old;
	struct kptr_obj *obj;

	arena_bind();
	obj = ptr;
	task = bpf_task_acquire(bpf_get_current_task_btf());
	if (!task)
		return 2;
	old = bpf_kptr_xchg(&obj->task, task);
	if (old)
		bpf_task_release(old);
	old = bpf_kptr_xchg(&obj->task, NULL);
	if (!old)
		return 3;
	bpf_task_release(old);
	return 0;
}

SEC("syscall")
__description("a local kptr left in a dummy object is dropped with the map")
__success __retval(0)
int kptr_xchg_local(void *ctx)
{
	struct arena_node *n, *old;
	struct kptr_obj *obj;

	arena_bind();
	obj = ptr;
	n = bpf_obj_new(struct arena_node);
	if (!n)
		return 2;
	n->v = 42;
	old = bpf_kptr_xchg(&obj->node, n);
	if (old)
		bpf_obj_drop(old);
	return 0;
}

SEC("syscall")
__description("a kptr field is not read directly")
__failure __msg("direct access to kptr is disallowed")
int kptr_read_directly(void *ctx)
{
	struct kptr_obj *obj;

	arena_bind();
	obj = ptr;
	return obj->task != NULL;
}

SEC("syscall")
__description("a kptr field is not written directly")
__failure __msg("direct access to kptr is disallowed")
int kptr_write_directly(void *ctx)
{
	struct kptr_obj *obj;

	arena_bind();
	obj = ptr;
	obj->task = NULL;
	return 0;
}

SEC("syscall")
__description("an exchange needs a kptr field")
__failure __msg("off=16 doesn't point to kptr")
int kptr_xchg_scalar_field(void *ctx)
{
	struct kptr_obj *obj;

	arena_bind();
	obj = ptr;
	bpf_kptr_xchg(&obj->value, NULL);
	return 0;
}

SEC("syscall")
__description("an exchange checks the value against the field's type")
__failure __msg("invalid kptr access, R2 type=ptr_arena_node expected=ptr_task_struct")
int kptr_xchg_wrong_type(void *ctx)
{
	struct kptr_obj *obj;
	struct arena_node *n;

	arena_bind();
	obj = ptr;
	n = bpf_obj_new(struct arena_node);
	if (!n)
		return 2;
	n = bpf_kptr_xchg(&obj->task, n);
	if (n)
		bpf_obj_drop(n);
	return 0;
}

/* 32-byte slot, linked through a typed pointer field: the sanitize mask is 134217696 */
struct node_obj {
	struct task_struct __kptr *task;
	struct node_obj *next;
	__u64 value;
};

/* The same layout as node_obj, a different typed arena */
struct pair_obj {
	struct task_struct __kptr *task;
	struct pair_obj *next;
	__u64 value;
};

/* 8-byte slot, typed only by its pointer to a typed struct: the cast mask is 134217720 */
struct head_obj {
	struct node_obj *first;
};

SEC("syscall")
__description("a typed pointer field loads unsanitized, and a dereference sanitizes it in place")
__success __retval(0) __log_level(2)
__msg("R{{[0-9]}}=unsanitized_typed_arena_ptr_node_obj(")
__xlated("r{{[0-9]}} &= 134217720")
__xlated("r12 = 0x{{[0-9a-f]+}}")
__xlated("r{{[0-9]}} += r12")
__xlated("...")
__xlated("r{{[0-9]}} = *(u64 *)(r{{[0-9]}} +0)")
__xlated("r{{[0-9]}} &= 134217696")
__xlated("r12 = 0x{{[0-9a-f]+}}")
__xlated("r{{[0-9]}} += r12")
__xlated("r{{[0-9]}} = *(u64 *)(r{{[0-9]}} +16)")
int ptr_field_deref_sanitizes(void *ctx)
{
	struct head_obj *h;
	struct node_obj *n;

	arena_bind();
	h = ptr;
	n = h->first;
	return n->value;
}

SEC("syscall")
__description("a compare and a store of a loaded typed pointer use the raw value")
__success __retval(0) __log_level(2)
__msg("R{{[0-9]}}=unsanitized_typed_arena_ptr_node_obj(")
__msg("if r{{[0-9]}} == 0x0 goto")
__msg("R{{[0-9]}}=unsanitized_typed_arena_ptr_node_obj(")
__msg("*(u64 *)(r1 +8) = r2")
int ptr_field_compare_and_store_stay_raw(void *ctx)
{
	struct node_obj *n, *m;

	arena_bind();
	n = ptr;
	m = n->next;
	/* Opaque to the compiler, so that the compare is emitted. */
	barrier_var(m);
	if (!m)
		return 0;
	/* The store is written by hand: the compiler would cast the value first. */
	asm volatile("r1 = %[n];"
		     "r2 = %[m];"
		     "*(u64 *)(r1 + 8) = r2;"
		     :: [n] "r"(n), [m] "r"(m)
		     : "r1", "r2", "memory");
	return 0;
}

SEC("syscall")
__description("a register sanitized by one dereference is not sanitized again by the next")
__success __retval(5)
__xlated("r1 = *(u64 *)(r1 +8)")
__xlated("r2 = 5")
__xlated("r1 &= 134217696")
__xlated("r12 = 0x{{[0-9a-f]+}}")
__xlated("r1 += r12")
__xlated("*(u64 *)(r1 +16) = r2")
__xlated("r0 = *(u64 *)(r1 +16)")
int ptr_field_second_deref_not_sanitized_again(void *ctx)
{
	struct node_obj *n;
	__u64 ret;

	arena_bind();
	n = ptr;
	/* Written by hand: the compiler would cast copies rather than reuse the register. */
	asm volatile("r1 = %[n];"
		     "r1 = *(u64 *)(r1 + 8);"
		     "r2 = 5;"
		     "*(u64 *)(r1 + 16) = r2;"
		     "r0 = *(u64 *)(r1 + 16);"
		     "%[ret] = r0;"
		     : [ret] "=r"(ret) : [n] "r"(n)
		     : "r0", "r1", "r2", "memory");
	return ret;
}

SEC("syscall")
__description("pointer arithmetic on a loaded typed pointer sanitizes it first")
__success __retval(0)
__xlated("r{{[0-9]}} = *(u64 *)(r{{[0-9]}} +8)")
__xlated("...")
__xlated("r1 &= 134217696")
__xlated("r12 = 0x{{[0-9a-f]+}}")
__xlated("r1 += r12")
__xlated("r1 += 16")
__xlated("*(u64 *)(r1 +0) = r2")
int ptr_field_arithmetic_sanitizes(void *ctx)
{
	struct node_obj *n, *m;

	arena_bind();
	n = ptr;
	m = n->next;
	asm volatile("r1 = %[m];"
		     "r2 = 0;"
		     "r1 += 16;"
		     "*(u64 *)(r1 + 0) = r2;"
		     :: [m] "r"(m)
		     : "r1", "r2", "memory");
	return 0;
}

SEC("syscall")
__description("a NULL typed pointer dereferences object 0 of the slice, held directly or loaded from a field")
__success __retval(42)
int ptr_field_null_lands_on_object_zero(void *ctx)
{
	struct node_obj *zero = NULL, *n, *m;

	arena_bind();
	/*
	 * The compiler treats a NULL dereference as undefined and would drop
	 * the store, fold the stored NULL into the load and the load into
	 * nothing; the barriers keep each value opaque so that the accesses
	 * are emitted.
	 */
	barrier_var(zero);
	zero->value = 42;
	n = ptr;
	n->next = NULL;
	barrier_var(n);
	m = n->next;
	barrier_var(m);
	return m->value;
}

SEC("syscall")
__description("a typed pointer field takes a typed pointer, a loaded one, or NULL")
__success __retval(0)
int ptr_field_store_accepted(void *ctx)
{
	struct node_obj *n, *m, *p;

	arena_bind();
	n = ptr;
	m = ptr2;
	n->next = m;
	p = m->next;
	n->next = p;
	n->next = NULL;
	return 0;
}

SEC("syscall")
__description("a typed pointer field does not take a scalar")
__failure __msg("store into typed pointer field of struct node_obj expects a typed arena pointer to struct node_obj or NULL")
int ptr_field_store_scalar_rejected(void *ctx)
{
	struct node_obj *n;

	arena_bind();
	n = ptr;
	/* Written by hand: the compiler would cast the value before the store. */
	asm volatile("r6 = %[n];"
		     "call %[bpf_get_prandom_u32];"
		     "r1 = r6;"
		     "*(u64 *)(r1 + 8) = r0;"
		     :: [n] "r"(n), __imm(bpf_get_prandom_u32)
		     : "r0", "r1", "r2", "r3", "r4", "r5", "r6", "memory");
	return 0;
}

SEC("syscall")
__description("a scalar assigned to a typed pointer field is cast by the compiler first")
__success __retval(0)
__xlated("call unknown")
__xlated("...")
__xlated("r{{[0-9]}} &= 134217696")
__xlated("r12 = 0x{{[0-9a-f]+}}")
__xlated("r{{[0-9]}} += r12")
__xlated("*(u64 *)(r{{[0-9]}} +8) = r{{[0-9]}}")
int ptr_field_store_scalar_cast_by_compiler(void *ctx)
{
	struct node_obj *n;

	arena_bind();
	n = ptr;
	n->next = (void *)(long)bpf_get_prandom_u32();
	return 0;
}

SEC("syscall")
__description("a typed pointer field does not take a pointer to another typed struct")
__failure __msg("store into typed pointer field of struct node_obj expects a typed arena pointer to struct node_obj or NULL")
int ptr_field_store_other_type_rejected(void *ctx)
{
	struct typed_obj *other;
	struct node_obj *n;

	arena_bind();
	n = ptr;
	other = ptr;
	/* Written by hand: the compiler would re-cast the value to node_obj first. */
	asm volatile("r1 = %[n];"
		     "r2 = %[o];"
		     "*(u64 *)(r1 + 8) = r2;"
		     :: [n] "r"(n), [o] "r"(other)
		     : "r1", "r2", "memory");
	return 0;
}

SEC("syscall")
__description("a pointer to another typed struct assigned to a typed pointer field is re-cast by the compiler")
__success __retval(0)
int ptr_field_store_other_type_recast(void *ctx)
{
	struct typed_obj *other;
	struct node_obj *n;

	arena_bind();
	n = ptr;
	other = ptr;
	n->next = (struct node_obj *)other;
	return n->next == NULL;
}

SEC("syscall")
__description("a typed pointer field is loaded whole")
__failure __msg("typed pointer field of struct node_obj must be accessed with a 64-bit load or store")
int ptr_field_narrow_load_rejected(void *ctx)
{
	struct node_obj *n;

	arena_bind();
	n = ptr;
	asm volatile("r1 = %[n];"
		     "w2 = *(u32 *)(r1 + 8);"
		     :: [n] "r"(n)
		     : "r1", "r2");
	return 0;
}

SEC("syscall")
__description("a typed pointer field is stored whole")
__failure __msg("typed pointer field of struct node_obj must be accessed with a 64-bit load or store")
int ptr_field_narrow_store_rejected(void *ctx)
{
	struct node_obj *n;

	arena_bind();
	n = ptr;
	asm volatile("r1 = %[n];"
		     "w2 = 0;"
		     "*(u32 *)(r1 + 8) = w2;"
		     :: [n] "r"(n)
		     : "r1", "r2", "memory");
	return 0;
}

SEC("syscall")
__description("a typed pointer field takes no atomic operation")
__failure __msg("typed pointer field of struct node_obj")
int ptr_field_atomic_rejected(void *ctx)
{
	struct node_obj *n, *m;

	arena_bind();
	n = ptr;
	m = ptr2;
	__sync_val_compare_and_swap((__u64 *)&n->next, 0, (__u64)m);
	return 0;
}

/* The same member outside a typed object is data */
struct raw_holder {
	struct node_obj *n;
	__u64 v;
};

SEC("syscall")
__description("a pointer stored in raw arena memory loads as a scalar and casts back to its object")
__success __retval(0)
int ptr_field_in_raw_memory_is_scalar(void *ctx)
{
	struct raw_holder __arena *h;
	struct node_obj *n, *again;

	h = bpf_arena_alloc_pages(&arena, NULL, 1, NUMA_NO_NODE, 0);
	if (!h)
		return 1;
	n = ptr;
	n->value = 7;
	h->n = n;
	again = h->n;
	return again != n || again->value != 7;
}

SEC("syscall")
__description("a struct typed only by a pointer to a typed struct gets a typed arena of its own")
__success __retval(0) __log_level(2)
__msg("typed arena for struct head_obj: slot 8 bytes")
int ptr_field_makes_struct_typed(void *ctx)
{
	struct head_obj *h;
	struct node_obj *n;

	arena_bind();
	h = ptr;
	n = ptr2;
	h->first = n;
	return h->first != n;
}

/*
 * The cast the compiler inserts is one instruction on every path through it:
 * a loaded typed pointer needs the sanitizing sequence there, an allocated
 * object needs the identity, and the two cannot share a lowering.
 */
SEC("syscall")
__description("one cast cannot both sanitize an arena pointer and pass an allocated object")
__failure __msg("casts values that need different treatment on different paths")
int cast_conflicts_between_arena_and_allocated(void *ctx)
{
	struct node_obj *n, *a;
	struct head_obj *h;

	arena_bind();
	a = bpf_obj_new(struct node_obj);
	if (!a)
		return 1;
	h = ptr;
	if (bpf_get_prandom_u32() & 1)
		n = h->first;
	else
		n = a;
	n->value = 1;
	bpf_obj_drop(a);
	return 0;
}

SEC("syscall")
__description("one instruction sanitizes one typed arena: two types on two paths are rejected")
__failure __msg("sanitizes typed arena pointers of different types on different paths")
int ptr_field_sanitize_two_types_at_one_insn(void *ctx)
{
	struct node_obj *n;
	struct pair_obj *p;

	arena_bind();
	n = ptr;
	p = ptr;
	asm volatile("call %[bpf_get_prandom_u32];"
		     "if r0 == 0 goto 1f;"
		     "r1 = %[n];"
		     "r1 = *(u64 *)(r1 + 8);"
		     "goto 2f;"
		     "1: r1 = %[p];"
		     "r1 = *(u64 *)(r1 + 8);"
		     "2: r2 = *(u64 *)(r1 + 16);"
		     :: [n] "r"(n), [p] "r"(p), __imm(bpf_get_prandom_u32)
		     : "r0", "r1", "r2", "r3", "r4", "r5", "memory");
	return 0;
}

SEC("syscall")
__description("one instruction sanitizes one typed arena: a typed pointer on one path only is rejected")
__failure __msg("sanitizes a typed arena pointer only on some paths")
int ptr_field_sanitize_on_one_path(void *ctx)
{
	struct node_obj *n;

	arena_bind();
	n = ptr;
	asm volatile("r2 = 0;"
		     "*(u64 *)(r10 - 16) = r2;"
		     "call %[bpf_get_prandom_u32];"
		     "if r0 == 0 goto 1f;"
		     "r1 = %[n];"
		     "r1 = *(u64 *)(r1 + 8);"
		     "goto 2f;"
		     "1: r1 = r10;"
		     "r1 += -32;"
		     "2: r2 = *(u64 *)(r1 + 16);"
		     :: [n] "r"(n), __imm(bpf_get_prandom_u32)
		     : "r0", "r1", "r2", "r3", "r4", "r5", "memory");
	return 0;
}

#define TYPE_ID(T) bpf_core_type_id_local(T)

SEC("syscall")
__description("allocated pages hold real objects, reachable through any value that lands in them")
__success __retval(0)
__xlated("r2 = 0x{{[0-9a-f]+[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]}}")
__xlated("call kernel-function")
int pages_alloc(void *ctx)
{
	struct typed_obj *obj, *again, *next;
	void *opaque;
	__u32 cnt = 1;

	obj = bpf_typed_arena_alloc_pages(&arena, TYPE_ID(struct typed_obj), NULL, &cnt,
					  NUMA_NO_NODE);
	if (!obj)
		return 1;
	if (cnt != 1)
		return 2;
	obj->value = 5;
	/* The object through an opaque value, and its neighbor by arithmetic on that value */
	opaque = obj;
	again = opaque;
	if (again != obj || again->value != 5)
		return 3;
	next = opaque + sizeof(*obj);
	next->value = 6;
	if (next == obj || next->value != 6 || obj->value != 5)
		return 4;
	return 0;
}

SEC("syscall")
__description("a fixed request takes its chunk once")
__success __retval(0)
int pages_alloc_fixed(void *ctx)
{
	struct typed_obj *obj, *hint;
	__u32 cnt;

	/* The chunk user space names, the first one here */
	hint = ptr;
	cnt = 2;
	obj = bpf_typed_arena_alloc_pages(&arena, TYPE_ID(struct typed_obj), hint, &cnt,
					  NUMA_NO_NODE);
	if (!obj)
		return 1;
	if (obj != hint || cnt != 2)
		return 2;
	cnt = 1;
	if (bpf_typed_arena_alloc_pages(&arena, TYPE_ID(struct typed_obj), hint, &cnt,
					NUMA_NO_NODE))
		return 3;
	/* The page after it, part of the first request */
	hint = (void *)hint + __PAGE_SIZE;
	cnt = 1;
	if (bpf_typed_arena_alloc_pages(&arena, TYPE_ID(struct typed_obj), hint, &cnt,
					NUMA_NO_NODE))
		return 4;
	return 0;
}

SEC("syscall")
__description("a request is rounded up to whole chunks and the granted count written back")
__success __retval(0)
int pages_alloc_granted_count(void *ctx)
{
	__u32 cnt = 1, chunk_pages = 16384 > __PAGE_SIZE ? 16384 / __PAGE_SIZE : 1;
	struct big_obj *obj;

	obj = bpf_typed_arena_alloc_pages(&arena, TYPE_ID(struct big_obj), NULL, &cnt,
					  NUMA_NO_NODE);
	if (!obj)
		return 1;
	if (cnt != chunk_pages)
		return 2;
	obj->pad[8191] = 1;
	return obj->pad[8191] - 1;
}

SEC("syscall")
__description("a released chunk keeps its objects and stays taken until the grace period has passed")
__success __retval(0)
int pages_free(void *ctx)
{
	struct typed_obj *obj;
	__u32 cnt = 1;

	obj = bpf_typed_arena_alloc_pages(&arena, TYPE_ID(struct typed_obj), NULL, &cnt,
					  NUMA_NO_NODE);
	if (!obj)
		return 1;
	obj->value = 7;
	bpf_typed_arena_free_pages(&arena, TYPE_ID(struct typed_obj), obj, 1);
	if (obj->value != 7)
		return 2;
	cnt = 1;
	if (bpf_typed_arena_alloc_pages(&arena, TYPE_ID(struct typed_obj), obj, &cnt, NUMA_NO_NODE))
		return 3;
	return 0;
}

SEC("syscall")
__description("an allocation must be checked for NULL")
__failure __msg("invalid mem access 'typed_arena_ptr_or_null_'")
int pages_alloc_null_check(void *ctx)
{
	struct typed_obj *obj;
	__u32 cnt = 1;

	obj = bpf_typed_arena_alloc_pages(&arena, TYPE_ID(struct typed_obj), NULL, &cnt,
					  NUMA_NO_NODE);
	obj->value = 1;
	return 0;
}

SEC("syscall")
__description("the page kfuncs register the type like a cast: a struct without special fields belongs in the raw arena")
__failure __msg("struct plain_obj has no special fields and needs no typed arena")
int pages_alloc_plain_struct(void *ctx)
{
	struct plain_obj *obj;
	__u32 cnt = 1;

	obj = bpf_typed_arena_alloc_pages(&arena, TYPE_ID(struct plain_obj), NULL, &cnt,
					  NUMA_NO_NODE);
	return obj == NULL;
}

SEC("syscall")
__description("the page kfuncs need the program's arena")
__failure __msg("can only be used in a program that has an associated arena")
int pages_alloc_needs_arena(void *ctx)
{
	struct typed_obj *obj;
	__u32 cnt = 1;

	obj = bpf_typed_arena_alloc_pages(&not_an_arena, TYPE_ID(struct typed_obj), NULL, &cnt,
					  NUMA_NO_NODE);
	return obj == NULL;
}

#endif /* __BPF_FEATURE_TYPED_ARENA_CAST */

char _license[] SEC("license") = "GPL";
