/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2025 Amazon.com Inc. or its affiliates.
 * Pratyush Yadav <pratyush@kernel.org>
 */

/**
 * DOC: KHO Array
 *
 * The KHO Array is a data structure that behaves like a sparse array of
 * pointers. It is designed to be preserved and restored over Kexec Handover
 * (KHO), and targets only 64-bit platforms. It can store 8-byte aligned
 * pointers. It can also store integers between 0 and LONG_MAX. It supports
 * sparse indices, though it performs best with densely clustered indices. The
 * data structure does not provide any locking. Callers must ensure they have
 * exclusive access.
 *
 * To keep the data format simple, the data structure is designed to only be
 * accessed linearly. When reading or writing the data structure, the values
 * should be accessed from the lowest index to the highest.
 *
 * The data format consists of a descriptor of the array which contains a magic
 * number, format version, and pointer to the first page. Each page contains the
 * starting position of the entries in the page and a pointer to the next page,
 * forming a linked list. This linked list allows for the array to be built with
 * non-contiguous pages.
 *
 * The starting position of each page an offset that is applied to calculate the
 * index of each entry in the array. For example, of the starting position is
 * 1000, entry 0 has index 1000, entry 1 has index 1001, and so on. This
 * facilitates memory-efficient handling of holes in the array.
 *
 * The diagram below shows the data format visually:
 *
 *   kho_array
 *  +----------+
 *  |  Magic   |
 *  +----------+                   kho_array_page
 *  | Version  |         +----------+----------+-----------
 *  +----------+    +--->|   Next   | Startpos | Entries...
 *  | Reserved |    |    +----------+----------+-----------
 *  +----------+    |          |               kho_array_page
 *  |  First   |----+          |    +----------+----------+-----------
 *  +----------+               +--->|   Next   | Startpos | Entries...
 *                                  +----------+----------+-----------
 *                                        |
 *                                        |
 *                                        +--->...
 */

#ifndef LINUX_KHO_ARRAY_H
#define LINUX_KHO_ARRAY_H

#include <linux/bug.h>

#define KHO_ARRAY_MAGIC		0x4b415252 /* ASCII for 'KARR' */
#define KHO_ARRAY_VERSION	0

/**
 * struct kho_array - Descriptor for a KHO array.
 * @magic: Magic number to ensure valid descriptor.
 * @version: Data format version.
 * @__reserved: Reserved bytes. Must be set to 0.
 * @first: Physical address of the first page in the list of pages. If 0, the
 *         list is empty.
 */
struct kho_array {
	u32		magic;
	u16		version;
	u16		__reserved;
	__aligned_u64	first;
} __packed;

/**
 * struct kho_array_page - A page in the KHO array.
 * @next: Physical address of the next page in the list. If 0, there is no next
 *        page.
 * @startpos: Position at which entries in this page start.
 * @entries: Entries in the array.
 */
struct kho_array_page {
	__aligned_u64	next;
	__aligned_u64	startpos;
	__aligned_u64	entries[];
} __packed;

#define KA_PAGE_NR_ENTRIES ((PAGE_SIZE - sizeof(struct kho_array_page)) / sizeof(u64))

#define KA_ITER_PAGEPOS(iter) ((iter)->pos - (iter)->cur->startpos)
#define KA_PAGE(phys) ((phys) ? (struct kho_array_page *)__va((phys)) : NULL)

/**
 * kho_array_valid() - Validate KHO array descriptor.
 * @ka: KHO array.
 *
 * Return: %true if valid, %false otherwise.
 */
bool kho_array_valid(struct kho_array *ka);

/**
 * kho_array_init() - Initialize an empty KHO array.
 * @ka: KHO array.
 *
 * Initilizes @ka to an empty KHO array full of NULL entries.
 */
void kho_array_init(struct kho_array *ka);

/**
 * kho_array_destroy() - Free the KHO array.
 * @ka: KHO array.
 *
 * After calling this function, @ka is destroyed and all its pages have been
 * freed. It must be initialized again before reuse.
 */
void kho_array_destroy(struct kho_array *ka);

/**
 * kho_array_preserve() - KHO-preserve all pages of the array
 * @ka: KHO array.
 *
 * Mark all pages of the array to be preserved across KHO.
 *
 * Note: the memory for the struct @ka itself is not marked as preserved. The
 * caller must take care of doing that, likely embedding it in a larger
 * serialized data structure.
 *
 * Return: 0 on success, -errno on failure.
 */
int kho_array_preserve(struct kho_array *ka);

/**
 * kho_array_restore() - KHO-restore all pages of the array
 * @ka: KHO array.
 *
 * Validate the magic and version of @ka, and if they match, restore all pages
 * ka from KHO to set the array up for being accessed.
 *
 * Note: the memory for the struct @ka itself is not KHO-restored. The caller
 * must take care of doing that, likely embedding it in a larger serialized data
 * structure.
 *
 * Return: 0 on success, -errno on failure.
 */
int kho_array_restore(struct kho_array *ka);

/**
 * ka_is_value() - Determine if an entry is a value.
 * @entry: KHO array entry.
 *
 * Return: %true if the entry is a value, %false if it is a pointer.
 */
static inline bool ka_is_value(const void *entry)
{
	return (unsigned long)entry & 1;
}

/**
 * ka_to_value() - Get value stored in an KHO array entry.
 * @entry: KHO array entry.
 *
 * Return: The value stored in @entry.
 */
static inline unsigned long ka_to_value(const void *entry)
{
	return (unsigned long)entry >> 1;
}

/**
 * ka_mk_value() - Create an KHO array entry from an integer.
 * @v: Value to store in KHO array.
 *
 * Return: An entry suitable for storing in a KHO array.
 */
static inline void *ka_mk_value(unsigned long v)
{
	WARN_ON((long)v < 0);
	return (void *)((v << 1) | 1);
}

enum ka_iter_mode {
	KA_ITER_READ,
	KA_ITER_WRITE,
};

struct ka_iter {
	struct kho_array	*ka;
	struct kho_array_page	*cur;
	unsigned long		pos;
	enum ka_iter_mode	mode;
};

/**
 * ka_iter_init_read() - Initialize iterator for reading.
 * @iter: KHO array iterator.
 * @ka: KHO array.
 *
 * Initialize @iter in read mode for reading @ka. After the function returns,
 * @iter points to the first non-empty entry in the array, if any. @ka must be a
 * valid KHO array. No validation on @ka is performed.
 */
void ka_iter_init_read(struct ka_iter *iter, struct kho_array *ka);

/**
 * ka_iter_init_write() - Initialize iterator for writing.
 * @iter: KHO array iterator.
 * @ka: KHO array.
 *
 * Initialize @ka to an empty array and then initialize @iter in write mode
 * for building @ka. All data in @ka is over-written, so it must be an
 * un-initialized array. After the function returns, @iter points to the first
 * entry in the array.
 */
void ka_iter_init_write(struct ka_iter *iter, struct kho_array *ka);

/**
 * ka_iter_init_restore() - Restore KHO array and initialize iterator for reading.
 * @iter: KHO array iterator.
 * @ka: KHO array.
 *
 * KHO-restore @ka, performing version and format validation, and initialize
 * @iter in read mode for reading the array. After the function returns, @iter
 * points to the first non-empty entry in the array, if any
 *
 * Returns: 0 on success, -errno on failure.
 */
int ka_iter_init_restore(struct ka_iter *iter, struct kho_array *ka);

/**
 * ka_iter_setentry() - Set entry at current iterator position.
 * @iter: KHO array iterator in write mode.
 * @value: Value or pointer to store.
 *
 * Store @value at the current position of @iter. @iter must be in write mode.
 * The iterator position is not advanced.
 *
 * Return: 0 on success, -errno on failure.
 */
int ka_iter_setentry(struct ka_iter *iter, const void *value);

/**
 * ka_iter_nextentry() - Advance iterator to next non-empty entry.
 * @iter: KHO array iterator.
 *
 * Advance @iter to the next non-empty entry in the array, skipping over
 * empty entries and holes between pages.
 *
 * Return: The entry, or %NULL if end of array reached.
 */
void *ka_iter_nextentry(struct ka_iter *iter);

/**
 * ka_iter_setpos() - Set iterator position.
 * @iter: KHO array iterator.
 * @pos: New position (must be >= current position).
 *
 * Set the iterator position to @pos. The position can only be moved forward.
 * The iterator will point to the appropriate page for the given position.
 *
 * Return: 0 on success, -EINVAL if @pos is less than current position.
 */
int ka_iter_setpos(struct ka_iter *iter, unsigned long pos);

/**
 * ka_iter_end() - Check if iterator has reached end of array.
 * @iter: KHO array iterator.
 *
 * Return: %true if iterator is at end of array, %false otherwise.
 */
bool ka_iter_end(struct ka_iter *iter);

/**
 * ka_iter_getpos() - Get current iterator position.
 * @iter: KHO array iterator.
 *
 * Return: Current position in the array.
 */
static inline unsigned long ka_iter_getpos(struct ka_iter *iter)
{
	return iter->pos;
}

/**
 * ka_iter_getentry() - Get entry at current iterator position.
 * @iter: KHO array iterator.
 *
 * Return: Pointer to entry at current position, or %NULL if none.
 */
void *ka_iter_getentry(struct ka_iter *iter);

/**
 * ka_iter_for_each - Iterate over all non-empty entries in array.
 * @iter: KHO array iterator.
 * @entry: Variable to store current entry.
 *
 * Loop over all non-empty entries in the array starting from current position.
 */
#define ka_iter_for_each(iter, entry)					\
	for ((entry) = ka_iter_getentry(iter); (entry); (entry) = ka_iter_nextentry((iter)))

#endif /* LINUX_KHO_ARRAY_H */
