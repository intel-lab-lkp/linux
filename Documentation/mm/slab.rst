.. SPDX-License-Identifier: GPL-2.0

===============
Slab Allocation
===============

Overview
========

The slab allocator is responsible for efficient allocation and reuse of
small kernel objects. It reduces internal fragmentation and improves
performance by caching frequently used objects.

The Linux kernel provides multiple slab allocator implementations,
including SLAB, SLUB, and SLOB. Among these, SLUB is the default
allocator on most modern systems.

SLUB Allocator
==============

Overview
--------

SLUB is a slab allocator designed to replace the legacy SLAB allocator
(mm/slab.c). It addresses the complexity, scalability limitations, and
memory overhead of the SLAB implementation.

The primary goal of SLUB is to simplify slab allocation while improving
performance on both uniprocessor (UP) and symmetric multiprocessing (SMP)
systems.


Functions and structures
========================

.. kernel-doc:: mm/slab.h
.. kernel-doc:: mm/slub.c
   :internal:
