.. SPDX-License-Identifier: GPL-2.0

==========
meminspect
==========

This document provides information about the meminspect feature.

Overview
========

meminspect is a mechanism that allows the kernel to register a chunk of
memory into a table, to be used at a later time for a specific
inspection purpose like debugging, memory dumping or statistics.

meminspect allows drivers to traverse the inspection table on demand,
or to register a notifier to be called whenever a new entry is being added
or removed.

The reasoning for meminspect is also to minimize the required information
in case of a kernel problem. For example a traditional debug method involves
dumping the whole kernel memory and then inspecting it. Meminspect allows the
users to select which memory is of interest, in order to help this specific
use case in production, where memory and connectivity are limited.

Although the kernel has multiple internal mechanisms, meminspect fits
a particular model which is not covered by the others.

meminspect Internals
====================

API
---

Static memory can be registered at compile time, by instructing the compiler
to create a separate section with annotation info.
For each such annotated memory (variables usually), a dedicated struct
is being created with the required information.
To achieve this goal, some basic APIs are available:

  MEMINSPECT_ENTRY(idx, sym, sz)
is the basic macro that takes an ID, the symbol, and a size.

To make it easier, some wrappers are also defined:
  MEMINSPECT_SIMPLE_ENTRY(sym)
will use the dedicated MEMINSPECT_ID_##sym with a size equal to sizeof(sym)

  MEMINSPECT_NAMED_ENTRY(name, sym)
will be a simple entry that has an id that cannot be derived from the sym,
so a name has to be provided

  MEMINSPECT_AREA_ENTRY(sym, sz)
this will register sym, but with the size given as sz, useful for e.g.
arrays which do not have a fixed size at compile time.

For dynamically allocated memory, or for other cases, the following APIs
are being defined:
  meminspect_register_id_pa(enum meminspect_uid id, phys_addr_t zone,
                            size_t size, unsigned int type);
which takes the ID and the physical address.
Similarly there are variations:
  meminspect_register_pa() omits the ID
  meminspect_register_id_va() requires the ID but takes a virtual address
  meminspect_register_va() omits the ID and requires a virtual address

If the ID is not given, the next avialable dynamic ID is allocated.

To unregister a dynamic entry, some APIs are being defined:
  meminspect_unregister_pa(phys_addr_t zone, size_t size);
  meminspect_unregister_id(enum meminspect_uid id);
  meminspect_unregister_va(va, size);

All of the above have a lock variant that ensures the lock on the table
is taken.


meminspect drivers
------------------

Drivers are free to traverse the table by using a dedicated function
meminspect_traverse(void *priv, MEMINSPECT_ITERATOR_CB cb)
The callback will be called for each entry in the table.

Drivers can also register a notifier with
  meminspect_notifier_register()
and unregister with
  meminspect_notifier_unregister()
to be called when a new entry is being added or removed.

Data structures
---------------

The regions are being stored in a simple fixed size array. It avoids
memory allocation overhead. This is not performance critical nor does
allocating a few hundred entries create a memory consumption problem.

The static variables registered into meminspect are being annotated into
a dedicated .inspect_table memory section. This is then walked by meminspect
at a later time and each variable is then copied to the whole inspect table.

meminspect Initialization
-------------------------

At any time, meminspect will be ready to accept region registration
from any part of the kernel. The table does not require any initialization.
In case CONFIG_CRASH_DUMP is enabled, meminspect will create an ELF header
corresponding to a core dump image, in which each region is added as a
program header. In this scenario, the first region is this ELF header, and
the second region is the vmcoreinfo ELF note.
By using this mechanism, all the meminspect table, if dumped, can be
concatenated to obtain a core image that is loadable with the `crash` tool.

meminspect example
==================

A simple scenario for meminspect is the following:
The kernel registers the linux_banner variable into meminspect with
a simple annotation like:

  MEMINSPECT_SIMPLE_ENTRY(linux_banner);

The meminspect late initcall will parse the compilation time created table
and copy the entry information into the inspection table.
At a later point, any interested driver can call the traverse function to
find out all entries in the table.
A specific driver will then note into a specific table the address of the
banner and the size of it.
The specific table is then written to a shared memory area that can be
read by upper level firmware.
When the kernel freezes (hypothetically), the kernel will no longer feed
the watchdog. The watchdog will trigger a higher exception level interrupt
which will be handled by the upper level firmware. This firmware will then
read the shared memory table and find an entry with the start and size of
the banner. It will then copy it for debugging purpose. The upper level
firmware will then be able to provide useful debugging information,
like in this example, the banner.

As seen here, meminspect facilitates the interaction between the kernel
and a specific firmware.
