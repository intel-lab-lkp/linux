.. SPDX-License-Identifier: GPL-2.0

=================
Ethernet Bridging
=================

Introduction
============

A bridge is a way to connect two Ethernet segments together in a protocol
independent way. Packets are forwarded based on Ethernet address, rather
than IP address (like a router). Since forwarding is done at Layer 2, all
protocols can go transparently through a bridge.

Bridge internals
================

.. kernel-doc:: include/uapi/linux/if_bridge.h
   :identifiers: __bridge_info

.. kernel-doc:: include/uapi/linux/if_bridge.h
   :identifiers: __port_info

Bridge uAPI
===========

Bridge netlink attributes
-------------------------

.. kernel-doc:: include/uapi/linux/if_link.h
   :doc: The bridge emum defination

Bridge sysfs
------------

Most of them are same with netlink attributes. What about the read only
parameters like gc_timer, tcn_timer? Should we doc them?

STP
===

Multicast
=========

VLAN
====

Switchdev
=========

Netfilter
=========

FAQ
===

What does a bridge do?
----------------------

A bridge transparently relays traffic between multiple network interfaces.
In plain English this means that a bridge connects two or more physical
Ethernets together to form one bigger (logical) Ethernet.

Is it protocol independent?
---------------------------

Yes. The bridge knows nothing about protocols, it only sees Ethernet frames.
As such, the bridging functionality is protocol independent, and there should
be no trouble relaying IPX, NetBEUI, IP, IPv6, etc.

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

