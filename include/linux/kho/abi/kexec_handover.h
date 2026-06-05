/* SPDX-License-Identifier: GPL-2.0-only */

/*
 * Copyright (C) 2023 Alexander Graf <graf@amazon.com>
 * Copyright (C) 2025 Microsoft Corporation, Mike Rapoport <rppt@kernel.org>
 * Copyright (C) 2025 Google LLC, Changyuan Lyu <changyuanl@google.com>
 */

#ifndef _LINUX_KHO_ABI_KEXEC_HANDOVER_H
#define _LINUX_KHO_ABI_KEXEC_HANDOVER_H
#include <linux/kho/abi/compat.h>
#include <linux/kho/abi/radix_tree.h>
#include <linux/kho/abi/vmalloc.h>

/**
 * DOC: Kexec Handover ABI
 *
 * Kexec Handover uses the ABI defined below for passing preserved data from
 * one kernel to the next.
 * The ABI uses Flattened Device Tree (FDT) format. The first kernel creates an
 * FDT which is then passed to the next kernel during a kexec handover.
 *
 * This interface is a contract. Any modification to the FDT structure, node
 * properties, compatible string, or the layout of the data structures
 * referenced here constitutes a breaking change. Such changes require
 * incrementing the version number in KHO_FDT_COMPATIBLE to prevent a new kernel
 * from misinterpreting data from an older kernel. Changes are allowed provided
 * the compatibility version is incremented. However, backward/forward
 * compatibility is only guaranteed for kernels supporting the same ABI version.
 *
 * FDT Structure Overview:
 *   The FDT serves as a central registry for physical addresses of preserved
 *   data structures. The first kernel populates this FDT with references to
 *   memory regions and other metadata that need to persist across the kexec
 *   transition. The subsequent kernel then parses this FDT to locate and
 *   restore the preserved data.::
 *
 *     / {
 *         compatible = "kho-v3";
 *
 *         preserved-memory-map = <0x...>;
 *
 *         <subnode-name-1> {
 *             preserved-data = <0x...>;
 *             blob-size = <0x...>;
 *         };
 *
 *         <subnode-name-2> {
 *             preserved-data = <0x...>;
 *             blob-size = <0x...>;
 *         };
 *               ... ...
 *         <subnode-name-N> {
 *             preserved-data = <0x...>;
 *             blob-size = <0x...>;
 *         };
 *     };
 *
 *   Root KHO Node (/):
 *     - compatible: "kho-v3"
 *
 *       Identifies the overall KHO ABI version.
 *
 *     - preserved-memory-map: u64
 *
 *       Physical memory address pointing to the root of the
 *       preserved memory map data structure.
 *
 *   Subnodes (<subnode-name-N>):
 *     Subnodes can also be added to the root node to
 *     describe other preserved data blobs. The <subnode-name-N>
 *     is provided by the subsystem that uses KHO for preserving its
 *     data.
 *
 *     - preserved-data: u64
 *
 *       Physical address pointing to a subnode data blob that is also
 *       being preserved.
 *
 *     - blob-size: u64
 *
 *       Size in bytes of the preserved data blob. This is needed because
 *       blobs may use arbitrary formats (not just FDT), so the size
 *       cannot be determined from the blob content alone.
 */

/* The compatible string for the KHO FDT root node. */
#define KHO_FDT_COMPAT_BASE "kho-v4"
#define KHO_FDT_COMPATIBLE						\
	KHO_FDT_COMPAT_BASE						\
	KHO_SUB_COMPAT(KHO_RADIX_COMPATIBLE)				\
	KHO_SUB_COMPAT(KHO_VMALLOC_COMPATIBLE)

/* The FDT property for the preserved memory map. */
#define KHO_FDT_MEMORY_MAP_PROP_NAME "preserved-memory-map"

/* The FDT property for preserved data blobs. */
#define KHO_SUB_TREE_PROP_NAME "preserved-data"

/* The FDT property for the size of preserved data blobs. */
#define KHO_SUB_TREE_SIZE_PROP_NAME "blob-size"

#endif	/* _LINUX_KHO_ABI_KEXEC_HANDOVER_H */

