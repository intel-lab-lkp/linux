.. SPDX-License-Identifier: GPL-2.0

==========================================
DAMON TLB Flush Policy
==========================================

:Author: Kunwu Chan <kunwu.chan@gmail.com>
:Author: Wang Lian <lianux.mm@gmail.com>

Overview
========

DAMON monitors data access by sampling PTE (Page Table Entry) Accessed bits
using ``ptep_test_and_clear_young()`` and ``pmdp_test_and_clear_young()``.
These functions clear the Accessed bit but do **not** flush the TLB
(Translation Lookaside Buffer).  This is an intentional design choice.

Questions about this behavior come up repeatedly, both on the mailing list
and in private inquiries.  This document describes the reasoning, the
trade-offs, and recommendations for users and testers.

Background
==========

DAMON's access check works as follows:

1. Clear the PTE Accessed bit for a sampled page.
2. Wait for one ``sampling interval``.
3. Check if the Accessed bit has been set again by the hardware.

If the bit was set again, the page was accessed during the sampling interval.

On architectures with hardware-managed TLB (e.g., x86, arm64), the CPU may
cache the Accessed bit state in the TLB.  After DAMON clears the Accessed bit
in the page table, a stale TLB entry with the old Accessed bit remains in the
TLB.  When the workload accesses the page, the access hits the stale TLB
entry and does not trigger a page table walk, so the Accessed bit in the page
table is not set again.  DAMON therefore fails to detect real accesses on its
next check, reporting false negatives.

Flushing the TLB after clearing the Accessed bit prevents stale TLB entries
and eliminates this problem.  Functions such as ``ptep_clear_flush_young()`` and
``pmdp_clear_flush_young()`` provide this behavior.  However, TLB flushes come
at a performance cost.

Why DAMON Does Not Flush TLB
============================

DAMON intentionally avoids TLB flushes to keep monitoring overhead low.
The decision was made after measuring the performance impact of adding TLB
flushes to the sampling path.  The measurement showed the overhead is
significant enough to matter for production use [1]_.

Production workloads typically have large working sets that flush TLB buffers
anyway through normal memory access patterns.  Stale TLB entries that could
cause monitoring inaccuracies are evicted by the workload's own memory activity
before the next sampling interval.  The accuracy impact is therefore negligible in
production.

The following table summarizes the trade-off:

+---------------------+-----------------------------+---------------------------+
|                     | Without TLB Flush (current) | With TLB Flush            |
+---------------------+-----------------------------+---------------------------+
| Monitoring Overhead | Low                         | Higher (flush cost)       |
+---------------------+-----------------------------+---------------------------+
| Accuracy (prod)     | Good                        | Good                      |
+---------------------+-----------------------------+---------------------------+
| Accuracy (test)     | May degrade                 | Good                      |
+---------------------+-----------------------------+---------------------------+

Impact on Testing and Small Workloads
=====================================

The lack of TLB flush becomes problematic when the working set is small enough
to fit entirely within the TLB reach.  This is common in test environments
and synthetic benchmarks.  In such cases, stale TLB entries persist across
sampling intervals, so DAMON reports false accesses and monitoring results
become incorrect.

For example, on a machine with a large TLB buffer, a test workload of a few
tens of megabytes may never experience TLB eviction.  DAMON's WSS (Working Set
Size) estimation can report 100% error (all regions reported as accessed,
or none reported as accessed depending on timing), and DAMOS schemes may never
trigger correctly.

This issue was observed in DAMON selftests and was addressed by increasing the
test working set size to simulate production-like conditions, rather than
changing DAMON's TLB flush behavior [2]_.  The selftest working set size was
increased up to 160 MiB for this reason.

Recommendations
===============

For Users
---------

If you observe unexpected ``nr_accesses`` values or inaccurate working
set size estimates, the cause is likely stale TLB entries from DAMON's
sampling without TLB flushes.  This happens when the working set fits
within the TLB reach, which is uncommon for production workloads but can
occur with small workloads.  See the For Testers section below for
how to verify this.

For Testers and Developers
--------------------------

When writing DAMON tests, ensure the test workload's working set is large
enough to trigger natural TLB eviction on the target test machine.  The
exact size depends on the CPU's TLB configuration.  The DAMON selftest for
WSS estimation uses 160 MiB per region after finding smaller sizes
unreliable on systems with large TLB buffers [2]_.

For out-of-tree tests, gradually increase the working set size until DAMON
reports stable and accurate results, then use that size as the baseline for
subsequent tests on the same hardware.

If DAMON reports unexpectedly high ``nr_accesses`` or empty
``tried_regions``, the ``diagnose_empty_tried_regions.py`` script from
DAMON selftests can help determine whether stale TLB entries are the cause.

The existing DAMON selftests follow this approach [2]_.

References
==========

.. [1] `DAMON TLB flush overhead measurement
   <https://lore.kernel.org/20200403103059.12762-1-sjpark@amazon.com/>`_

.. [2] `DAMON selftest: increase working set size for reliable results
   <https://lore.kernel.org/20260117020731.226785-3-sj@kernel.org/>`_
