.. SPDX-License-Identifier: GPL-2.0

=================
Ethernet Bridging
=================

Introduction
============

A bridge is a way to connect multiple Ethernet segments together in a protocol
independent way. Packets are forwarded based on Layer 2 destination Ethernet
address, rather than IP address (like a router). Since forwarding is done
at Layer 2, all Layer 3 protocols can pass through a bridge transparently.

Bridge kAPI
===========

Here are some core structures of bridge code.

.. kernel-doc:: net/bridge/br_private.h
   :identifiers: net_bridge_vlan

Bridge uAPI
===========

Modern Linux bridge uAPI is accessed via Netlink interface. You can find
below files where the bridge and bridge port netlink attributes are defined.

Bridge netlink attributes
-------------------------

.. kernel-doc:: include/uapi/linux/if_link.h
   :doc: Bridge enum definition

Bridge port netlink attributes
------------------------------

.. kernel-doc:: include/uapi/linux/if_link.h
   :doc: Bridge port enum definition

Bridge sysfs
------------

All sysfs attributes are also exported via the bridge netlink API.
You can find each attribute explanation based on the correspond netlink
attribute.

NOTE: the sysfs interface is deprecated and should not be extended if new
options are added.

.. kernel-doc:: net/bridge/br_sysfs_br.c
   :doc: Bridge sysfs attributes

FAQ
===

What does a bridge do?
----------------------

A bridge transparently forwards traffic between multiple network interfaces.
In plain English this means that a bridge connects two or more physical
Ethernet networks, to form one larger (logical) Ethernet network.

Is it L3 protocol independent?
------------------------------

Yes. The bridge sees all frames, but it *uses* only L2 headers/information.
As such, the bridging functionality is protocol independent, and there should
be no trouble forwarding IPX, NetBEUI, IP, IPv6, etc.

Contact Info
============

The code is currently maintained by Roopa Prabhu <roopa@nvidia.com> and
Nikolay Aleksandrov <razor@blackwall.org>. Bridge bugs and enhancements
are discussed on the linux-netdev mailing list netdev@vger.kernel.org and
bridge@lists.linux-foundation.org.

The list is open to anyone interested: http://vger.kernel.org/vger-lists.html#netdev

External Links
==============

The old Documentation for Linux bridging is on:
https://wiki.linuxfoundation.org/networking/bridge
