.. SPDX-License-Identifier: GPL-2.0

================================
Arm Core Local Accelerator (CLA)
================================

Arm CLA is an interface local to a CPU for programming accelerators that access
memory via the CPU's MMU:

              ┌───────┬───────┬───────┐  ┌───────┐
              │  CPU  │  MMU  │  CLA  │  │ Accel │─┐
              │       │       │       │  │       │ │
              │      ---MMIO-->       <-->       │ │
              │       │       │       │  │       │ │
              │       │    ,-----DMA----->       │ │
              └───────┴────|──┴───────┘  └───────┘ │
                ┌──────────v────────┐      └───────┘
                │ Caches and memory │
                └───────────────────┘


Hardware
========

The CLA supports up to 8 attached accelerators, which are accessed by
programming the CLA's MMIO registers. Operations are launched to an accelerator
and are polled for completion. CLA does not raise interrupts.

            CPU                     CLA              Accel
             |--- write DATA[7:0] -->|                 |
             |--- write LAUNCH ----->|---- launch ---->|
             |<--- poll LRESP -------|                 |
             |                       |                 |
             |<--- poll STATUS ------|<--- complete ---|

Each operation can take a 512-bit payload in the DATA registers. After handling
a LAUNCH write, CLA indicates the launch status in the LRESP register. A further
operation can only be launched after LRESP indicates completion of the previous
launch.

Some operations continue to run asynchronously on the accelerator after launch
completion. In this case progress is tracked by polling the STATUS register.
When the CLA updates the STATUS register, it also raises an event which will
wake an in-progress WFE (wait for event) instruction on the local CPU.

The CLA's MMIO registers are not accessible from remote CPUs. Although each CLA
has a unique physical address, accesses from remote CPUs are read as zero and
write ignored.

The CLA registers are accessible from four different Privilege Level (PL)
frames, with usage inteded to map to EL0 - EL3. The PLxCTRL registers may be
written via a higher PL frame to suppress access to accelerators via a lower PL
frame.

The CPU and CLA share an MMU, although FEAT_TTCNP (common not private) is
implemented, allowing both CPU and CLA to independently opt into and out of
sharing TLB entries at runtime. TLB invalidation is performed via the CPU TLBI
instructions; any TLBI instruction that targets the CLA's local CPU will also
implicitly target the CLA.

CLA has its own set of Memory Translation Context (MTC) registers, distinct from
the CPU. A PL can set the MTC registers corresponding to an equivalent CPU
Exception Level (EL) (eg. TCR_EL2 configurable only from the PL2 and PL3 MMIO
frame).

Faults during address translation are reported by the accelerator in its
registers and in STATUS. While polling for work completion, software fixes up
the faults and notifies the accelerator with RESOLVE operations.

Accesses to the MMIO registers must be aligned 64-bit loads and stores, and the
registers are mapped with Device-nGnRE attribute. Invalid accesses (unaligned,
atomic, badly sized, etc) to the MMIO frame are either read as zero and write
ignored, or cause a data abort, depending on the platform. Invalid access will
never cause an SError.


Inter-Accelerator Communication
-------------------------------

On some platforms, multiple accelerators, each attached to a separate CLA within
a cluster, are also directly connected to each other via a shared bus to
accelerate cooperation between accelerators. The accelerators sharing a bus
cannot be isolated from each other. When collaborative operations are launched
on each of the participating accelerators, they synchronize over the bus,
stalling until all are ready.


Intended SW Usage Model
=======================

CLA is designed for its PL0 MMIO frame to be mapped into user space and for user
space to directly launch accelerator operations and poll for completion. It has
been observed that for some use cases, the operation execution time is small and
a trip through the kernel would consume a significant amount of the CPU budget
for preparing the next operation leading to a significant reduction in bandwidth
through the accelerator.

User space software is expected to create a thread to drive each CLA it is
using, and for each thread to be pinned to the CLA's local CPU.

Software should rely on WFE (wait for event) to reduce power consumption when
polling STATUS.

The CLA is intended to be configured, by privileged software via PL1 and/or PL2,
so that it shares virtual addresses with the process to which it is assigned
(SVA). In practice this means configuring the CLA's MTC to point to the same
page tables and use the same ASID (and VMID if relevant) as the process to which
it is assigned. This ensures the architectural TLB invalidations also correctly
target the CLA's TLB entries.

We investigated the possibility of having the CLA driver allocate private page
tables, private ASIDs/VMIDs and implement an MMU notifier for invalidation, but
that suffers from 2 issues; there is a possibility of over-invalidation since
the ASID and VMID spaces overlap with the CPU's (minor), and when issuing a
TLBI, VMID is implicitly taken from CPU's VTTBR_EL2.VMID, which won't match the
CLA's private VMID - therefore, for a virtualization scenario, TLB invalidation
becomes impossible (major).

Because the CLA has its own MTC registers, it is correct and safe for it to be
executing an operation on behalf of user process A, while its local CPU is
executing a thread from user process B.


Difficulties for software to deal with
--------------------------------------

Although each CLA is attached to a single CPU, not all CPUs have a CLA, and CLAs
may have a different set of accelerators attached. Users need to probe around to
find a suitable accelerator and bind their process to it.

Since remote CPUs can't access the CLA, dealing with CPU hotplug migrating tasks
is challenging. And virtualization breaks down if the hypervisor cannot
guarantee that a vCPU is pinned to a CPU.

Saving and restoring the internal state of the accelerator is an optional
feature. Current platforms only support it when the accelerator is idle, so
preempting an accelerator causes work cancellation. Software must carefully
consider how to balance forward-progress guarantees with preemption latency.


Driver
======

Booting
-------

A host expects to be booted with CLAs in the following state:
- All attached accelerators have STATUS_IDLE set.
- PL2CTRL: AVAIL all enabled (no accelerator request made at runtime).
           DBG at the firmware's discretion, preferably all enabled.
- TSCTRLOWNER.PL, TSOFFOWNER.PL, PMUOWNER.PL all EL2.

A guest expects to be booted with CLAs in the following state:
- All attached accelerators have STATUS_IDLE set.
- PL1CTRL: AVAIL all enabled (no accelerator request made at runtime).
           DBG at the hypervisor's discretion, preferably all enabled.

Linux discovers the base address of each CLA device in the firmware tables and
creates platform devices. cla_dev behaves mostly like a regular platform device,
but it can only be accessed from one specific CPU. CPU hotplug notifiers probe
and teardown the CLA device.

CLAs may be grouped into domains. The topology is described in the firmware
tables, and the driver creates cla_domain objects containing one or more
cla_dev.


Assignment to userspace
-----------------------

A char device provides enumeration and mmap abilities to userspace. The user
task queries the driver to find a suitable set of CLAs, and mmaps their
registers. For each mapped domain, the driver creates a cla_ctx context. When
the user accesses the registers, the driver's fault handler queues the cla_ctx
and waits for assignment. A context reassignment switches the whole cla_domain,
by calling each CPU in the domain to switch the CLA context. A time slice is
given to each context before being deassigned.

In order to access the registers, the task must be bound to the CLA's CPU with
sched_setaffinity(). Accesses from remote CPUs are ignored. If the CPU gets
offlined, the task is migrated to an online CPU, and the driver disables the
offlined CLA. User space may choose to use a mechanism such as restartable
sequences to be notified when its task has been migrated away from its intended
CLA.

The accelerators access memory using the CPU's MMU. When assigning a context,
the driver sets up the CLA's Memory Translation Context (MTC) with the page
directory (TTBR), address space ID (ASID) and configuration (TCR) of the
context's mm. When receiving translation faults from the accelerator, the user
space task accesses the faulting address from the CPU to trigger the fault to be
fixed up by the kernel before launching a RESOLVE operation to the accelerator.


Power management
----------------

When a CPU enters a deep low-power state, then depending on the platform, the
attached CLA and accelerators might not retain their state. In that case the
firmware is expected to save the CLA and accelerator states before entering CPU
low-power state, and restore them after exiting. The CLA driver does not expect
to notice a CPU deep idle. However some accelerators do not support context
saving, in which case userspace will notice from the STATUS register that the
work was canceled during deep idle. Given that userspace would be polling
STATUS, the CPU is unlikely to enter deep idle while the CLA is running. To
ensure forward-progress the admin can disable deep idle states (see
Documentation/admin-guide/pm/cpuidle.rst).
