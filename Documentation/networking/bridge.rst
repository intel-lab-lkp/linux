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

Here are the core structs of bridge code.

.. kernel-doc:: include/uapi/linux/if_bridge.h
   :identifiers: __bridge_info

.. kernel-doc:: include/uapi/linux/if_bridge.h
   :identifiers: __port_info

Bridge uAPI
===========

The Linux bridge uAPI are exported via the netlink interface. Here are
all the bridge and bridge port netlink attribute definations.

Bridge netlink attributes
-------------------------

.. kernel-doc:: include/uapi/linux/if_link.h
   :doc: The bridge emum defination

Bridge port netlink attributes
------------------------------

.. kernel-doc:: include/uapi/linux/if_link.h
   :doc: The bridge port emum defination

Bridge sysfs
------------

All the sysfs parameters are also exported via the bridge netlink API.
Here you can find the explanation based on the correspond netlink attributes.

.. kernel-doc:: net/bridge/br_sysfs_br.c
   :doc: The sysfs bridge attrs

STP
===

The STP (Spanning Tree Protocol) function in a Linux bridge is a critical
feature that helps prevent loops in Ethernet networks by identifying and
disabling redundant links within a network. The primary purpose of STP is
to ensure network reliability and redundancy while preventing broadcast
storms and other undesirable network behaviors. In a Linux bridge context,
STP is crucial for network stability and availability.

STP is a Layer 2 protocol that operates at the Data Link Layer of the OSI
model. It was originally developed as IEEE 802.1D and has since evolved into
multiple versions, including Rapid Spanning Tree Protocol (RSTP) and
Multiple Spanning Tree Protocol (MSTP). The Linux bridge typically support
the original Spanning Tree Protocol (STP) and Rapid Spanning Tree Protocol
(RSTP), but not MSTP.

Bridge Ports and STP States
---------------------------

In the context of STP, bridge ports can be in one of the following states:
  * Blocking: The port is disabled for data traffic and only listens to
    BPDUs (Bridge Protocol Data Units) from other devices to determine the
    network topology.
  * Listening: The port begins to participate in the STP process and listens
    for BPDUs.
  * Learning: The port continues to listen to BPDUs and begins to learn MAC
    addresses from incoming frames but does not forward data frames.
  * Forwarding: The port is fully operational and forwards both BPDUs and
    data frames.
  * Disabled: The port is administratively disabled and does not participate
    in the STP process.

Root Bridge and Convergence
---------------------------

Within a network, one bridge is elected as the "Root Bridge." All other
bridges participate in STP to determine the shortest path to the Root Bridge.

STP ensures network convergence by calculating the shortest path and disabling
redundant links. When network topology changes occur (e.g., a link failure),
STP recalculates the network topology to restore connectivity while avoiding loops.

Proper configuration of STP parameters, such as the bridge priority, can
influence which bridge becomes the Root Bridge. Careful configuration can
optimize network performance and path selection.

Multicast
=========

The multicast functionality in a Linux bridge refers to the ability of the
bridge to efficiently forward multicast traffic, such as Internet Group
Management Protocol (IGMP) or Multicast Listener Discovery (MLD) messages,
and multicast data packets within a local network segment. This is an
important capability in environments where applications or services rely
on multicast communication.

By default, Linux bridges are capable of forwarding multicast traffic.
The bridge acts as a Layer 2 (Data Link Layer) device and forwards multicast
packets to all bridge ports (except the source port) within the same VLAN.

After enable multicast snooping, the Linux bridge can filter multicast
traffic based on the destination MAC address, making it more efficient in
forwarding multicast frames. It maintains a Multicast Filtering Database (MFD)
that records which multicast groups are associated with each bridge port.
Multicast traffic is forwarded only to ports with associated group
memberships.

VLAN
====

VLAN (Virtual LAN) functionality can be integrated with the Linux bridge to
provide a way to manage and segregate network traffic into different virtual
LANs within a single physical network infrastructure. This integration allows
for greater flexibility in network configuration and traffic isolation.

After enable VLAN filter on bridge, the bridge can handle VLAN-tagged frames
and forward them to the appropriate destinations based on the VLAN tag.

The Linux bridge supports the IEEE 802.1Q and 802.1AD protocol for VLAN
tagging.

Switchdev
=========

Linux Bridge Switchdev is a feature in the Linux kernel that extends the
capabilities of the traditional Linux bridge to work more efficiently with
hardware switches that support switchdev. This technology is particularly
useful in data center and networking environments where high-performance
and low-latency packet forwarding is essential.

With Linux Bridge Switchdev, certain networking functions like forwarding,
filtering, and learning of Ethernet frames can be offloaded to the hardware
switch. This offloading reduces the burden on the Linux kernel and CPU,
leading to improved network performance and lower latency.

To use Linux Bridge Switchdev, you need hardware switches that support the
switchdev interface. This means that the switch hardware needs to have the
necessary drivers and functionality to work in conjunction with the Linux
kernel.

Netfilter
=========

The bridge netfilter module allows packet filtering and firewall functionality
on bridge interfaces. As the Linux bridge, which traditionally operates at
Layer 2 and connects multiple network interfaces or segments, doesn't have
built-in packet filtering capabilities.

With bridge netfilter, you can define rules to filter or manipulate Ethernet
frames as they traverse the bridge. These rules are typically based on
Ethernet frame attributes such as MAC addresses, VLAN tags, and more.
You can use the *ebtables* or *nftables* tools to create and manage these
rules. *ebtables* is a tool specifically designed for managing Ethernet frame
filtering rules, while *nftables* is a more versatile framework for managing
rules that can also be used for bridge filtering.

The bridge netfilter is commonly used in scenarios where you want to apply
security policies to the traffic at the data link layer. This can be useful
for segmenting and securing networks, enforcing access control policies,
and isolating different parts of a network.

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
