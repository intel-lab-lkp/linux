======================
Kernel page table dump
======================

ptdump is a debugfs interface that provides a detailed dump of the
kernel page tables. It offers a comprehensive overview of the kernel
virtual memory layout as well as the attributes associated with the
various regions in a human-readable format. It is useful to dump the
kernel page tables to verify permissions and memory types. Examining the
page table entries and permissions helps identify potential security
vulnerabilities such as mappings with overly permissive access rights or
improper memory protections.

Memory hotplug allows dynamic expansion or contraction of available
memory without requiring a system reboot. To maintain the consistency
and integrity of the memory management data structures, arm64 makes use
of the ``mem_hotplug_lock`` semaphore in write mode. Additionally, in
read mode, ``mem_hotplug_lock`` supports an efficient implementation of
``get_online_mems()`` and ``put_online_mems()``. These protect the
offlining of memory being accessed by the ptdump code.

In order to dump the kernel page tables, enable the following
configurations and mount debugfs::

 CONFIG_GENERIC_PTDUMP=y
 CONFIG_PTDUMP_CORE=y
 CONFIG_PTDUMP_DEBUGFS=y

 mount -t debugfs nodev /sys/kernel/debug
 cat /sys/kernel/debug/kernel_page_tables

``/sys/kernel/debug/kernel_page_tables`` provides a line of information
for each group of page table entries sharing the same attributes and
type of mapping, i.e. page descriptor PTE or table descriptor PGD, PMD,
and PUD.  Assessing these attributes can assist in determining memory
layout, access patterns and security characteristics of the kernel
pages.

Lines are formatted as follows::

 <start_vaddr>-<end_vaddr> <size> <type> <attributes>

Note that the set of attributes, and therefore formatting, is not
equivalent between block (or page) and table descriptor entries. For
example, PMD table descriptors can support the PXNTable permission bit
and do not share that same set of attributes as PTEs.

The following attributes are presently supported::

F		Entry is invalid
RO		Memory is read-only
RW		Memory is read-write
X		Memory is privileged executable
NX		Memory is privileged execute never
UXN		Memory is unprivileged execute never
USR		Memory is unprivileged accessible
KRN		Memory is unprivileged inaccessible (e.g. APTable bits)
SHD		Memory is shared
AF		Entry accessed flag is set
NG		Entry Not-Global flag is set
CON		Entry contiguous bit is set
GP		Page is guarded with branch target integrity protection
TBL		Entry is a table descriptor
BLK		Entry is a block descriptor
DEVICE/*	Entry is device memory, see ARM reference for types
MEM/*		Entry is non-device memory, see ARM reference for types

The beginning and end of each region is also delineated by a single line
tag in the following format::

 ---[ <marker_name> ]---

With supported address markers including the kernel's linear mapping,
kasan shadow memory, kernel modules memory, vmalloc memory, PCI I/O
memory, and the kernel's fixmap region.

Example ``cat /sys/kernel/debug/kernel_page_tables`` output::

 ---[ Linear Mapping start ]---
 0xffff000000000000-0xffff1affffffffff                  27T PGD
 0xffff1b0000000000-0xffffffffffffffff                 229T PGD   TBL    NX UXN      RW
     0xffff1b0000000000-0xffff1b397fffffff             230G PUD
     0xffff1b3980000000-0xffff1b39bfffffff               1G PUD   TBL    NX UXN      RW
       0xffff1b3980000000-0xffff1b39801fffff             2M PMD   TBL    NX UXN      RW
         0xffff1b3980000000-0xffff1b39801fffff           2M PTE       RW NX SHD AF NG         UXN    MEM/NORMAL-TAGGED
       0xffff1b3980200000-0xffff1b39803fffff             2M PMD   TBL    NX UXN      RW
         0xffff1b3980200000-0xffff1b398020ffff          64K PTE       RW NX SHD AF NG         UXN    MEM/NORMAL-TAGGED
         0xffff1b3980210000-0xffff1b39803fffff        1984K PTE       RO NX SHD AF NG         UXN    MEM/NORMAL
       0xffff1b3980400000-0xffff1b3981dfffff            26M PMD       RO NX SHD AF NG     BLK UXN    MEM/NORMAL
       0xffff1b3981e00000-0xffff1b3981ffffff             2M PMD   TBL    NX UXN      RW
         0xffff1b3981e00000-0xffff1b3981e1ffff         128K PTE       RO NX SHD AF NG         UXN    MEM/NORMAL
         0xffff1b3981e20000-0xffff1b3981ffffff        1920K PTE       RW NX SHD AF NG         UXN    MEM/NORMAL-TAGGED
