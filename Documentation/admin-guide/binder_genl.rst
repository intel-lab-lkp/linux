.. SPDX-License-Identifier: GPL-2.0

===========================================================
Generic Netlink for the Android Binder Driver (Binder Genl)
===========================================================

The Generic Netlink subsystem in the Linux kernel provides a generic way for
the Linux kernel to communicate to the user space applications via binder
driver. It is used to report various kinds of binder transactions to user
space administration process. The driver allows multiple binder devices and
their corresponding binder contexts. Each context has an independent Generic
Netlink for security reason. To prevent untrusted user applications from
accessing the netlink data, the kernel driver uses unicast mode instead of
multicast.

Basically, the user space code uses the "set" command to request what kinds
of binder transactions reported by the kernel binder driver. The driver then
uses "reply" command to acknowledge the request. The "set" command also
registers the current user space process to receive the reports. When the
user space process exits, the previous request will be reset to prevent any
potential leaks.

Currently the driver can report binder trasnactiosn that "failed" to reach
the target process, or that are "delayed" due to the target process being
frozen by cgroup freezer, or that are considered "spam" according to existing
logic in binder_alloc.c.

When the specified binder transactions happen, the driver uses the "report"
command to send a generic netlink message to the registered process,
containing the payload struct binder_report.

More details about the flags, attributes and operations can be found at the
the doc sections in Documentations/netlink/specs/binder_genl.yaml and the
kernel-doc comments of the new source code in binder.{h|c}.

Using Binder Genl
-----------------

The Binder Genl can be used in the same way as any other generic netlink
drivers. Userspace application uses a raw netlink socket to send commands
to and receive packets from the kernel driver.

.. note::
    If the user application that talks to the Binder Genl driver exits, the
    kernel driver will automatically reset the configuration to the default
    and stop sending more reports to prevent leaking memory.

Usage example (user space pseudo code):

::

    // open netlink socket
    int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);

    // bind netlink socket
    bind(fd, struct socketaddr);

    // get the family id of the binder genl
    send(fd, CTRL_CMD_GETFAMILY, CTRL_ATTR_FAMILY_NAME, "binder");
    void *data = recv(CTRL_CMD_NEWFAMILY);
    __u16 id = nla(data)[CTRL_ATTR_FAMILY_ID];

    // enable per-context binder report
    send(fd, id, BINDER_GENL_CMD_SET, 0, BINDER_GENL_FLAG_FAILED |
            BINDER_GENL_FLAG_DELAYED);

    // confirm the per-context configuration
    void *data = recv(fd, BINDER_GENL_CMD_REPLY);
    __u32 pid =  nla(data)[BINDER_GENL_ATTR_PID];
    __u32 flags = nla(data)[BINDER_GENL_ATTR_FLAGS];

    // set optional per-process report, overriding the per-context one
    send(fd, id, BINDER_GENL_CMD_SET, getpid(),
            BINDER_GENL_FLAG_SPAM | BINDER_REPORT_OVERRIDE);

    // confirm the optional per-process configuration
    void *data = recv(fd, BINDER_GENL_CMD_REPLY);
    __u32 pid =  nla(data)[BINDER_GENL_A_ATTR_PID];
    __u32 flags = nla(data)[BINDER_GENL_A_ATTR_FLAGS];

    // wait and read all binder reports
    while (running) {
            void *data = recv(fd, BINDER_GENL_CMD_REPORT);
            struct binder_report report = nla(data)[BINDER_GENL_A_ATTR_REPORT];

            // process struct binder_report
            do_something(&report);
    }

    // clean up
    send(fd, id, BINDER_GENL_CMD_SET, 0, 0);
    send(fd, id, BINDER_GENL_CMD_SET, getpid(), 0);
    close(fd);
