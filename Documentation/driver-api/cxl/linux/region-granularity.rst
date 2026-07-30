.. SPDX-License-Identifier: GPL-2.0

========================================
Multi-Level Interleave Region Geometry
========================================

CXL regions may route an address through a root decoder and one or more
switch decoders before reaching an endpoint decoder. Each interleaving
decoder selects one of its targets from the region address. The decoder
levels must use a compatible combination of interleave ways and
granularities so that their routing decisions form one region interleave.

The CXL specification defines the legal multi-level combinations and is
the authority for the interleave geometry:

* CXL 4.0 Section 9.13.1.1, ``Legal Interleaving Configurations``
* CXL 4.0 Tables 9-6, 9-7, and 9-8
* CXL 4.0 Figures 9-17 and 9-18

Those sections cover both power-of-2 interleaves and the 3-way interleave
family, including regions whose granularity is finer than the root decoder
granularity.

Linux implementation
====================

The CXL core validates region geometry while walking the decoder topology
in ``drivers/cxl/core/region.c``.

For each port decoder, Linux:

* collects the HPA selector bits already used by the root and ancestor
  switch decoders;
* rejects overlapping selectors or selectors outside the region selector;
* derives the port decoder granularity from the selector bits available at
  that level;
* validates firmware-programmed values for auto-discovered regions and
  programs the derived values for user-created regions; and
* checks that endpoint positions agree with the decoder target lists.

Mixed-granularity regions require different endpoint position arithmetic
because one decoder target may cover multiple region positions before the
next target is selected. The driver expresses that as the number of region
positions covered by one decoder target and uses it while reconstructing
endpoint positions.

For 3-way-family roots, selector bits alone do not describe the full
interleave span. Linux therefore also checks that the root and region cover
the same span.

Implementation details
======================

The implementation is documented with the functions that enforce each
part of the model:

* :c:func:`get_selector`
* :c:func:`get_parent_selectors`
* :c:func:`derive_port_granularity`
* :c:func:`root_positions_per_target`
* :c:func:`cxl_calc_interleave_pos`
* :c:func:`check_gran_ordering`
* :c:func:`cxl_port_setup_targets`

The region-level granularity and 3-way-family span checks are performed in
``cxl_region_attach()``.
