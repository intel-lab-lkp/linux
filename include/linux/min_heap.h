/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MIN_HEAP_H
#define _LINUX_MIN_HEAP_H

#include <linux/bug.h>
#include <linux/string.h>
#include <linux/types.h>

/**
 * struct __min_heap - Data structure to hold a min-heap.
 * @data: Start of array holding the heap elements.
 * @nr: Number of elements currently in the heap.
 * @size: Maximum number of elements that can be held in current storage.
 */
struct __min_heap {
	void *data;
	int nr;
	int size;
};

/*
 * We use a 0 size array to stash the type we're storing without taking any
 * space at runtime - then the various accessor macros can use typeof() to get
 * to it for casts/sizeof - we also force the alignment so that storing a type
 * with a ridiculous alignment doesn't blow up the alignment or size of the
 * min_heap.
 */
#define MIN_HEAP(_type, _name)			\
struct _name {				\
	struct __min_heap	heap;		\
	_type type[0] __aligned(1);	\
}

#define __minheap_cast(_heap)		(typeof((_heap)->type[0]) *)
#define __minheap_obj_size(_heap)	sizeof((_heap)->type[0])

/**
 * struct min_heap_callbacks - Data/functions to customise the min_heap.
 * @less: Partial order function for this heap.
 * @swp: Swap elements function.
 */
struct min_heap_callbacks {
	bool (*less)(const void *lhs, const void *rhs, void *args);
	void (*swp)(void *lhs, void *rhs, void *args);
};

/* Initialize a min-heap. */
static __always_inline
void __min_heap_init(struct __min_heap *heap, void *data, int size)
{
	heap->data = data;
	heap->nr = 0;
	heap->size = size;
}

#define min_heap_init(_heap, _data, _size)	\
	__min_heap_init(&(_heap)->heap, _data, _size)

/* Get the minimum element from the heap. */
static __always_inline
void *__min_heap_peek(struct __min_heap *heap)
{
	return heap->nr ? heap->data : NULL;
}

#define min_heap_peek(_heap)	\
	(__minheap_cast(_heap) __min_heap_peek(&(_heap)->heap))

/* Check if the heap is full. */
static __always_inline
bool __min_heap_full(struct __min_heap *heap)
{
	return heap->nr == heap->size;
}

#define min_heap_full(_heap)	\
	__min_heap_full(&(_heap)->heap)

/* Sift the element at pos down the heap. */
static __always_inline
void __min_heapify(struct __min_heap *heap, int pos, size_t elem_size,
		const struct min_heap_callbacks *func, void *args)
{
	void *left, *right;
	void *data = heap->data;
	void *root = data + pos * elem_size;
	int i = pos, j;

	/* Find the sift-down path all the way to the leaves. */
	for (;;) {
		if (i * 2 + 2 >= heap->nr)
			break;
		left = data + (i * 2 + 1) * elem_size;
		right = data + (i * 2 + 2) * elem_size;
		i = func->less(left, right, args) ? i * 2 + 1 : i * 2 + 2;
	}

	/* Special case for the last leaf with no sibling. */
	if (i * 2 + 2 == heap->nr)
		i = i * 2 + 1;

	/* Backtrack to the correct location. */
	while (i != pos && func->less(root, data + i * elem_size, args))
		i = (i - 1) / 2;

	/* Shift the element into its correct place. */
	j = i;
	while (i != pos) {
		i = (i - 1) / 2;
		func->swp(data + i * elem_size, data + j * elem_size, args);
	}
}

#define min_heapify(_heap, _pos, _func, _args)	\
	__min_heapify(&(_heap)->heap, _pos, __minheap_obj_size(_heap), _func, _args)

/* Floyd's approach to heapification that is O(nr). */
static __always_inline
void __min_heapify_all(struct __min_heap *heap, size_t elem_size,
		const struct min_heap_callbacks *func, void *args)
{
	int i;

	for (i = heap->nr / 2 - 1; i >= 0; i--)
		__min_heapify(heap, i, elem_size, func, args);
}

#define min_heapify_all(_heap, _func, _args)	\
	__min_heapify_all(&(_heap)->heap, __minheap_obj_size(_heap), _func, _args)

/* Remove minimum element from the heap, O(log2(nr)). */
static __always_inline
bool __min_heap_pop(struct __min_heap *heap, size_t elem_size,
		const struct min_heap_callbacks *func, void *args)
{
	void *data = heap->data;

	if (WARN_ONCE(heap->nr <= 0, "Popping an empty heap"))
		return false;

	/* Place last element at the root (position 0) and then sift down. */
	heap->nr--;
	memcpy(data, data + (heap->nr * elem_size), elem_size);
	__min_heapify(heap, 0, elem_size, func, args);

	return true;
}

#define min_heap_pop(_heap, _func, _args)	\
	__min_heap_pop(&(_heap)->heap, __minheap_obj_size(_heap), _func, _args)

/*
 * Remove the minimum element and then push the given element. The
 * implementation performs 1 sift (O(log2(nr))) and is therefore more
 * efficient than a pop followed by a push that does 2.
 */
static __always_inline
void __min_heap_pop_push(struct __min_heap *heap,
		const void *element, size_t elem_size,
		const struct min_heap_callbacks *func,
		void *args)
{
	memcpy(heap->data, element, elem_size);
	__min_heapify(heap, 0, elem_size, func, args);
}

#define min_heap_pop_push(_heap, _element, _func, _args)	\
	__min_heap_pop_push(&(_heap)->heap, _element, __minheap_obj_size(_heap), _func, _args)

/* Push an element on to the heap, O(log2(nr)). */
static __always_inline
bool __min_heap_push(struct __min_heap *heap, const void *element, size_t elem_size,
		const struct min_heap_callbacks *func, void *args)
{
	void *data = heap->data;
	void *child, *parent;
	int pos;

	if (WARN_ONCE(heap->nr >= heap->size, "Pushing on a full heap"))
		return false;

	/* Place at the end of data. */
	pos = heap->nr;
	memcpy(data + (pos * elem_size), element, elem_size);
	heap->nr++;

	/* Sift child at pos up. */
	for (; pos > 0; pos = (pos - 1) / 2) {
		child = data + (pos * elem_size);
		parent = data + ((pos - 1) / 2) * elem_size;
		if (func->less(parent, child, args))
			break;
		func->swp(parent, child, args);
	}

	return true;
}

#define min_heap_push(_heap, _element, _func, _args)	\
	__min_heap_push(&(_heap)->heap, _element, __minheap_obj_size(_heap), _func, _args)

/* Sift up ith element from the heap, O(log2(nr)). */
static __always_inline
void __min_heap_sift_up(struct __min_heap *heap, size_t elem_size, size_t idx,
		const struct min_heap_callbacks *func, void *args)
{
	void *data = heap->data;
	size_t parent;

	while (idx) {
		parent = (idx - 1) / 2;
		if (func->less(data + parent * elem_size, data + idx * elem_size, args))
			break;
		func->swp(data + parent * elem_size, data + idx * elem_size, args);
		idx = parent;
	}
}

#define min_heap_sift_up(_heap, _idx, _func, _args)	\
	__min_heap_sift_up(&(_heap)->heap, __minheap_obj_size(_heap), _idx, _func, _args)

/* Remove ith element from the heap, O(log2(nr)). */
static __always_inline
bool __min_heap_del(struct __min_heap *heap, size_t elem_size, size_t idx,
		const struct min_heap_callbacks *func, void *args)
{
	void *data = heap->data;

	if (WARN_ONCE(heap->nr <= 0, "Popping an empty heap"))
		return false;

	/* Place last element at the root (position 0) and then sift down. */
	heap->nr--;
	if (idx == heap->nr)
		return true;
	memcpy(data, data + (heap->nr * elem_size), elem_size);
	__min_heap_sift_up(heap, elem_size, idx, func, args);
	__min_heapify(heap, idx, elem_size, func, args);

	return true;
}

#define min_heap_del(_heap, _idx, _func, _args)	\
	__min_heap_del(&(_heap)->heap, __minheap_obj_size(_heap), _idx, _func, _args)

#endif /* _LINUX_MIN_HEAP_H */
