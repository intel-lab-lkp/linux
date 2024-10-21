.. SPDX-License-Identifier: GPL-2.0

===========================================================
Generic Netlink for the Android Binder Driver (Binder Genl)
===========================================================

The Generic Netlink subsystem in the Linux kernel provides a generic way for
the Linux kernel to communicate to the user space applications. In the kernel
binder driver, it is used to report various kinds of binder transactions to
user space administratioin process. The binder driver allows multiple binder
devices and their correspondign binder contexts. Each binder context has a
independent Generic Netlink for security reason. To prevent untrusted user
applications from accessing the netlink data, the kernel driver uses unicast
mode instead of multicast.

Using Binder Genl
-----------------

The Binder Genl can be used in the same way as any other generic netlink
drivers. The user space application uses a raw netlink socket to send commands
to and receive packets from the kernel driver.

NOTE: if the user applications that talks to the Binder Genl driver exits,
the kernel driver will automatically reset the configuration to the default
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
    send(fd, id, BINDER_GENL_SET_REPORT, 0, BINDER_REPORT_ALL);

    // confirm the per-context configuration
    void *data = recv(fd, BINDER_GENL_CMD_REPLY);
    __u32 pid =  nla(data)[BINDER_GENL_ATTR_PID];
    __u32 flags = nla(data)[BINDER_GENL_ATTR_FLAGS];

    // set optional per-process report, overriding the per-context one
    send(fd, id, BINDER_GENL_SET_REPORT, getpid(),
                    BINDER_REPORT_FAILED | BINDER_REPORT_OVERRIDE);

    // confirm the optional per-process configuration
    void *data = recv(fd, BINDER_GENL_CMD_REPLY);
    __u32 pid =  nla(data)[BINDER_GENL_ATTR_PID];
    __u32 flags = nla(data)[BINDER_GENL_ATTR_FLAGS];

    // wait and read all binder reports
    while (running) {
            void *data = recv(fd, BINDER_GENL_CMD_REPORT);
            struct binder_report report = nla(data)[BINDER_GENL_ATTR_REPORT];

            // process struct binder_report
            do_something(&report);
    }

    // clean up
    send(fd, id, BINDER_GENL_SET_REPORT, 0, 0);
    send(fd, id, BINDER_GENL_SET_REPORT, getpid(), 0);
    close(fd);
