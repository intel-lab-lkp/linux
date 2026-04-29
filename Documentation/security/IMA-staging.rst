.. SPDX-License-Identifier: GPL-2.0

==================================
IMA Measurements Staging Mechanism
==================================


Introduction
============

The IMA measurements list is currently stored in the kernel memory. Memory
occupation grows linearly with the number of entries, and can become a
problem especially in environments with reduced resources.

While there is an advantage in keeping the IMA measurements list in kernel
memory, so that it is always available for reading from the securityfs
interfaces, storing it elsewhere would make it possible to free precious
memory for other kernel components.

Storing the IMA measurements list outside the kernel does not introduce
security issues, since its integrity is anyway protected by the TPM.

Hence, the new IMA staging mechanism is introduced to allow user space
to remove the desired portion of the measurements list from the kernel.


Usage
=====

The IMA staging mechanism can be enabled from the kernel configuration with
the CONFIG_IMA_STAGING option.

If it is enabled, IMA duplicates the current measurements interfaces (both
binary and ASCII), by adding the ``_staged`` file suffix. Both the original
and the staging interfaces gain the write permission for the root user and
group, but require the process to have CAP_SYS_ADMIN set.

The staging mechanism supports two flavors.


Staging with prompt
~~~~~~~~~~~~~~~~~~~

The current measurements list is moved to a temporary staging area, and
staged measurements are deleted upon confirmation.

This staging process is achieved with the following steps.

 1. ``echo A > <original interface>``: the user requests IMA to stage the
    entire measurements list;
 2. ``cat <_staged interface>``: the user reads the staged measurements;
 3. ``echo D > <_staged interface>``: the user requests IMA to delete
    staged measurements.


Staging and deleting
~~~~~~~~~~~~~~~~~~~~

N measurements are staged to a temporary staging area, and immediately
deleted without further confirmation.

This staging process is achieved with the following steps.

 1. ``cat <original interface>``: the user reads the current measurements
    list and determines what the value N for staging should be;
 2. ``echo N > <original interface>``: the user requests IMA to delete N
    measurements from the current measurements list.


Interface Access
================

In order to avoid the IMA measurements list be suddenly truncated by the
staging mechanism during a read, or having multiple concurrent staging, a
semaphore-like locking scheme has been implemented on all the measurements
list interfaces.

Multiple readers can access concurrently the original and staged
interfaces, and they can be in mutual exclusion with one writer.

If an illegal access occurs, the open to the measurements list interface is
denied.


Management of Staged Measurements
=================================

Since with the staging mechanism measurement entries are removed from the
kernel, the user needs to save the staged ones in a storage and concatenate
them together, so that it can present them to remote attestation agents as
if staging was never done.

Coordination is necessary in the case where there are multiple actors
requesting measurements to be staged.

In the staging with prompt case, the measurement interfaces can be accessed
only by one actor (writer) at a time, so the others will get an error until
the former closes it. Since the actors don't care about N, when they gain
access to the interface, they will get all the staged measurements at the
time of their request.

In the case of staging and deleting, coordination is more important, since
there is the risk that two actors unaware of each other compute the value N
on the current measurements list and request IMA to stage N twice.


Remote Attestation Agent Workflow
=================================

Users can choose the staging method they find more appropriate for their
workflow.

If, as an example, a remote attestation agent would like to present to the
remote attestation server only the measurements that are required to
verify the TPM quote, its workflow would be the following.

With staging with prompt, the agent stages the current measurements list,
reads and stores the measurements in a storage and immediately requests
IMA to delete the staged measurements from kernel memory. Afterwards, it
calculates N by replaying the PCR extend on the stored measurements until
the calculated PCRs match the quoted PCRs. It then keeps the measurements
in excess for the next attestation request.

At the next attestation request, the agent performs the same steps above,
and concatenates the new measurements to the ones in excess from the
previous request. Also in this case, the agent replays the PCR extend until
it matches the currently quoted PCRs, keeps the measurements in excess and
presents the new N measurements entries to the remote attestation server.

With the staging and deleting method, the agent reads the current
measurements list, calculates N and requests IMA to delete only those. The
measurements in excess are kept in the IMA measurements list and can be
retrieved at the next remote attestation request.

Kexec
=====

In the event a kexec() system call occurs between staging and deleting, the
staged measurements entries are prepended to the current measurements list,
so that they are both available when the secondary kernel starts. In that
case, IMA returns an error to the user when attempting to delete staged
measurements to notify about their copy to the kexec buffer, so that the
user does not store them twice.


Hash table
==========

By default, the template digest of staged measurement entries are kept in
kernel memory (only template data are freed), to be able to detect
duplicate entries independently of staging.

The new kernel option ``ima_flush_htable`` has been introduced to
explicitly request a complete deletion of the staged measurements, for
maximum kernel memory saving. If the option has been specified, duplicate
entries are still avoided on entries of the current measurements list,
but there can be duplicates between different groups of staged
measurements.

Flushing the hash table is supported only for the staging with prompt
flavor. For the staging and deleting flavor, it would have been necessary
to lock the hot path adding new measurements for the time needed to remove
each selected measurement individually.
