.. SPDX-License-Identifier: GPL-2.0

==============================================================
Generic Netlink for the Android Binder Driver (Binder Netlink)
==============================================================

The Generic Netlink subsystem in the Linux kernel provides a generic way for
the Linux kernel to communicate with the user space applications via binder
driver. It is used to report binder transaction errors and warnings to user
space administration process. The driver allows multiple binder devices and
their corresponding binder contexts. Each context has an independent Generic
Netlink for security reason. To prevent untrusted user applications from
accessing the netlink data, the kernel driver uses unicast mode instead of
multicast.

Basically, the user space code uses the BINDER_NETLINK_CMD_REPORT_SETUP
command to request what kind of binder transactions should be reported by
the driver. The driver then echoes the attributes in a reply message to
acknowledge the request. The BINDER_NETLINK_CMD_REPORT_SETUP command also
registers the current user space process to receive the reports. When the
user space process exits, the previous request will be reset automatically.

Currently the driver reports these binder transaction errors and warnings.
1. "FAILED" transactions that fail to reach the target process;
2. "ASYNC_FROZEN" transactions that are delayed due to the target process
being frozen by cgroup freezer; or
3. "SPAM" transactions that are considered spamming according to existing
logic in binder_alloc.c.

When the specified binder transactions happen, the driver uses the
BINDER_NETLINK_CMD_REPORT command to send a generic netlink message to the
registered process, containing the payload defined in binder_netlink.yaml.

More details about the flags, attributes and operations can be found at the
the doc sections in Documentations/netlink/specs/binder_netlink.yaml and the
kernel-doc comments of the new source code in binder.{h|c}.

Using Binder Netlink
--------------------

The Binder Netlink can be used in the same way as any other generic netlink
drivers. Userspace application uses a raw netlink socket to send commands
to and receive packets from the kernel driver.

.. note::
    If the userspace application that talks to the driver exits, the kernel
    driver will automatically reset the configuration to the default and
    stop sending more reports, which would otherwise fail.

Usage example (user space pseudo code):

::
    /*
     * send() below is overloaded to pack netlink commands and attributes
     * to nlattr/genlmsghdr/nlmsghdr and then send to the netlink socket.
     *
     * recv() below is overloaded to receive the raw netlink message from
     * the netlink socket, parse nlmsghdr/genlmsghdr to find the netlink
     * command and then return the nlattr payload.
     */

    // open netlink socket
    int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);

    // bind netlink socket
    bind(fd, struct socketaddr);

    // get the family id of the binder netlink
    send(fd, CTRL_CMD_GETFAMILY, CTRL_ATTR_FAMILY_NAME,
            BINDER_NETLINK_FAMILY_NAME);
    void *data = recv(CTRL_CMD_NEWFAMILY);
    if (!has_nla_type(data, CTRL_ATTR_FAMILY_ID)) {
        // Binder Netlink isn't available on this version of Linux kernel
        return;
    }
    __u16 id = nla(data)[CTRL_ATTR_FAMILY_ID];

    // enable per-context binder report
    send(fd, id, BINDER_NETLINK_CMD_REPORT_SETUP, "binder", 0,
            BINDER_NETLINK_FLAG_FAILED | BINDER_NETLINK_FLAG_DELAYED);

    // confirm the per-context configuration
    data = recv(fd, BINDER_NETLINK_CMD_REPLY);
    char *context = nla(data)[BINDER_NETLINK_A_CMD_CONTEXT];
    __u32 pid =  nla(data)[BINDER_NETLINK_A_CMD_PID];
    __u32 flags = nla(data)[BINDER_NETLINK_A_CMD_FLAGS];

    // set optional per-process report, overriding the per-context one
    send(fd, id, BINDER_NETLINK_CMD_REPORT_SETUP, "binder", getpid(),
            BINDER_NETLINK_FLAG_SPAM | BINDER_REPORT_OVERRIDE);

    // confirm the optional per-process configuration
    data = recv(fd, BINDER_NETLINK_CMD_REPLY);
    context = nla(data)[BINDER_NETLINK_A_CMD_CONTEXT];
    pid =  nla(data)[BINDER_NETLINK_A_CMD_PID];
    flags = nla(data)[BINDER_NETLINK_A_CMD_FLAGS];

    // wait and read all binder reports
    while (running) {
            data = recv(fd, BINDER_NETLINK_CMD_REPORT);
            auto *attr = nla(data)[BINDER_NETLINK_A_REPORT_XXX];

            // process binder report
            do_something(*attr);
    }

    // clean up
    send(fd, id, BINDER_NETLINK_CMD_REPORT_SETUP, 0, 0);
    send(fd, id, BINDER_NETLINK_CMD_REPORT_SETUP, getpid(), 0);
    close(fd);
