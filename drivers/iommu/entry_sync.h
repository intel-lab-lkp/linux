/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Many IOMMU implementations store data structures in host memory that can be
 * quite big. The iommu is able to DMA read the host memory using an atomic
 * quanta, usually 64 or 128 bits, and will read an entry using multiple quanta
 * reads.
 *
 * Updating the host memory datastructure entry while the HW is concurrently
 * DMA'ing it is a little bit involved, but if you want to do this hitlessly,
 * while never making the entry non-valid, then it becomes quite complicated.
 *
 * entry_sync is a library to handle this task. It works on the notion of "used
 * bits" which reflect which bits the HW is actually sensitive to and which bits
 * are ignored by hardware. Many hardware specifications say things like 'if
 * mode is X then bits ABC are ignored'.
 *
 * Using the ignored bits entry_sync can often compute a series of ordered
 * writes and flushes that will allow the entry to be updated while keeping it
 * valid. If such an update is not possible then entry will be made temporarily
 * non-valid.
 *
 * A 64 and 128 bit quanta version is provided to support existing iommus.
 */
#ifndef IOMMU_ENTRY_SYNC_H
#define IOMMU_ENTRY_SYNC_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/bug.h>

/* Caller allocates a stack array of this length to call entry_sync_write() */
#define ENTRY_SYNC_MEMORY_LEN(writer) ((writer)->num_quantas * 3)

struct entry_sync_writer_ops64;
struct entry_sync_writer64 {
	const struct entry_sync_writer_ops64 *ops;
	size_t num_quantas;
	size_t vbit_quanta;
};

struct entry_sync_writer_ops64 {
	void (*get_used)(const __le64 *entry, __le64 *used);
	void (*sync)(struct entry_sync_writer64 *writer);
};

void entry_sync_write64(struct entry_sync_writer64 *writer, __le64 *entry,
			const __le64 *target, __le64 *memory,
			size_t memory_len);

struct entry_sync_writer_ops128;
struct entry_sync_writer128 {
	const struct entry_sync_writer_ops128 *ops;
	size_t num_quantas;
	size_t vbit_quanta;
};

struct entry_sync_writer_ops128 {
	void (*get_used)(const u128 *entry, u128 *used);
	void (*sync)(struct entry_sync_writer128 *writer);
};

void entry_sync_write128(struct entry_sync_writer128 *writer, u128 *entry,
			 const u128 *target, u128 *memory,
			 size_t memory_len);

#endif
