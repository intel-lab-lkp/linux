/* SPDX-License-Identifier: GPL-2.0 */
/*
 * This file includes Microsoft Hypervisor definitions from hyperv-tlfs.h, or
 * hvhdk.h when HYPERV_NONTLFS_HEADERS is defined.
 */
/*
 * NOTE:
 * The typical #ifdef guard to prevent redefinition errors is intentionally
 * omitted. This makes compiler error (either via #error or redefinition) in
 * the case where hyperv-tlfs.h is accidentally included, followed by
 * definition of HYPERV_NON_TLFS_HEADERS and inclusion of this file.
 * If this file could only be included once, the compiler would ignore the
 * attempt to use HYPERV_NONTLFS_HEADERS to include hvhdk.h.
 */

#ifdef HYPERV_NONTLFS_HEADERS

#ifdef HYPERV_TLFS_HEADERS_INCLUDED
#error "hyperv-tlfs.h was already included before HYPERV_NONTLFS_HEADERS was defined"
#else
#include <hyperv/hvhdk.h>
#endif

#else /* HYPERV_NONTLFS_HEADERS */

#include <asm/hyperv-tlfs.h>
#define HYPERV_TLFS_HEADERS_INCLUDED

#endif /* !HYPERV_NONTLFS_HEADERS */
