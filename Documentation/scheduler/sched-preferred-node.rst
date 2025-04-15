.. SPDX-License-Identifier: GPL-2.0

Prctl for Explicitly Setting Task's Preferred Node
####################################################

This feature is an addition to Auto NUMA Balancing. Auto NUMA balancing by
default scans a task's address space removing address translations such that
subsequent faults can indicate the predominant node from which memory is being
accessed. A task's numa_preferred_nid is set to the node ID.

The numa_preferred_nid is used to both consolidate physical pages and assist the
scheduler in making NUMA friendly load balancing decisions.

While quite useful for some workloads, this has two issues that this prctl() can
help solve:

- There is a trade-off between faulting overhead and the ability to detect
dynamic access patterns. In cases where the task or user understand the NUMA
sensitivities, this patch can enable the benefits of setting a preferred node
used either in conjunction with Auto NUMA Balancing's default parameters or
adjusting the NUMA balance parameters to reduce the faulting rate
(potentially to 0).

- Memory pinned to nodes or to physical addresses such as RDMA cannot be
migrated and have thus far been excluded from the scanning. Not taking
those faults however can prevent Auto NUMA Balancing from reliably detecting a
node preference with the scheduler load balancer then possibly operating with
incorrect NUMA information.


Usage
*******

    Note: Auto NUMA Balancing must be enabled to get the effects.

    #include <sys/prctl.h>

    int prctl(int option, unsigned long arg2, unsigned long arg3, unsigned long arg4, unsigned long arg5);

option:
    ``PR_PREFERRED_NID``

arg2:
    Command for operation, must be one of:

    - ``PR_PREFERRED_NID_GET`` -- get the forced preferred node ID for ``pid``.
    - ``PR_PREFERRED_NID_SET`` -- set the forced preferred node ID for ``pid``.

    Returns ERANGE for an illegal command.

arg3:
    ``pid`` of the task for which the operation applies. ``0`` implies current.

    Returns ESRCH if ``pid`` is not found.

arg4:
    ``node_id`` for PR_PREFERRED_NID_SET. Between ``-1`` and ``num_possible_nodes()``.
    ``-1`` indicates no preference.

    Returns EINVAL for an illegal command.

arg5:
    userspace pointer to an integer for returning the Node ID from
    ``PR_PREFERRED_NID_GET``. Should be 0 for all other commands.

Must have the ptrace access mode: `PTRACE_MODE_READ_REALCREDS` to get/set
the preferred node ID to a process otherwise returns EPERM.
