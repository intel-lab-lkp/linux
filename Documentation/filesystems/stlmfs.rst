.. SPDX-License-Identifier: GPL-2.0

=============================================
STLMFS - Arm SCMI Telemetry Pseudo Filesystem
=============================================

.. contents::

Overview
========

ARM SCMI is a System and Configuration Management protocol, based on a
client-server model, that defines a number of messages that allows a
client/agent like Linux to discover, configure and make use of services
provided by the server/platform firmware.

SCMI v4.0 introduced support for System Telemetry, through which an agent
can dynamically enumerate configure and collect Telemetry Data Events (DE)
exposed by the platform.

This filesystem, in turn, exposes to userspace the set of discovered DEs
allowing for their configuration and retrieval.

Rationale
=========

**Why not using SysFS/KernFS or DebugFS ?**

The provided userspace interface aims to satisfy 2 main concurrent
requirements:

 - expose an FS-based human-readable interface that can be used to
   discover, configure and access Telemetry data directly from the
   shell

 - allow also alternative machine-friendly, more-performant, binary
   interfaces that can be used by custom tools without the overhead of
   multiple accesses through the VFS layers and the hassle of navigating
   vast filesystem tree structures

All of the above is meant to be available on production systems, not
simply as a tool for development or testing, so no debugFS option here.

An initial design based on SysFS and chardev/ioctl based interfaces was
dropped in favour of this full-fledged filesystem implementation since:

- SysFS is a standard way to expose device related properties using a few
  common helpers built on kernfs; this means, though, that unfortunately in
  our scenario we would have to generate a dummy simple device for each
  discovered DE.
  This by itself seems an abuse of the SysFS framework, but even ignoring
  this, the sheer number of potentially discoverable DEs (in the order of
  tens of thousands easily) would have led to the creation of a sensibly
  vast number of dummy DE devices.

- SysFS usage itself has its well-known constraints and best practices,
  like the one-file/one-value rule, that hardly cope with SCMI Telemetry
  needs.

- The need to implement more complex file operations (ioctls/mmap) in
  order to support the alternative binary interfaces does not fit with
  SysFS/kernFS facilities.

- Given the nature of the Telemetry protocol, the hybrid approach with
  chardev/ioctl was itself problematic: on one side being upper-limited
  in the number of chardev potentially created by the minor numbers
  availability, on the other side the hassle of having to maintain a
  completely different interface on the side of a FS based one.

Design
======

STLMFS is a pseudo filesystem used to expose ARM SCMI Telemetry data
discovered dynamically at run-time via SCMI.

Inodes are all dynamically created at mount-time from a dedicated
kmem_cache based on the gathered available SCMI Telemetry information.

Since inodes represent the discovered Telemetry entities, which in turn are
statically defined at the platform level and immutable throughout the same
session (boot), allocated inodes are freed only at unmount-time and the
user is not allowed to delete or create any kind of file within the STLMFS
filesystem after mount has completed.

A single instance of STLMFS is created at the filesystem level, using
get_single_tree(), given that the same SCMI backend entities will be
involved no matter how many times you mount it.

Mountpoints
===========

A pre-defined mountpoint is available at::

	/sys/fs/arm_telemetry/

Usage
=====

.. Note::
	See Documentation/ABI/testing/stlmfs for a detailed description of
	this ABI.

The filesystem can be typically mounted with::

	mount -t stlmfs none /sys/fs/arm_telemetry

It will proceed to create a top subdirectory for each of the discovered
SCMI Telemetry instances named as 'tlm_<N>' under which it will create
the following directory structure::

	/sys/fs/arm_telemetry/tlm_0/
	|-- all_des_enable
	|-- all_des_tstamp_enable
	|-- available_update_intervals_ms
	|-- control
	|-- current_update_interval_ms
	|-- de_implementation_version
	|-- des/
	|   |-- ...
	|   |-- ...
	|   `-- ...
	|-- des_bulk_read
	|-- des_single_sample_read
	|-- groups
	|   |-- ...
	|   |-- ...
	|   `-- ...
	|-- intervals_discrete
	|-- reset
	|-- tlm_enable
	`-- version

.. Note::
	The control/ special file can be used to use the alternative
	binary interface described in include/uapi/linux/scmi.h

Each subdirectory is defined as follows.

des/
----
A subtree containing in turn one subdirectory for each discovered DE and
named by Data Event ID in hexadecimal form as in::

	|-- des
	|   |-- 0x00000000
	|   |-- 0x00000016
	|   |-- 0x00001010
	|   |-- 0x0000A000
	|   |-- 0x0000A001
	|   |-- 0x0000A002
	|   |-- 0x0000A005
	|   |-- ..........
	|   |-- ..........
	|   |-- 0x0000A007
	|   |-- 0x0000A008
	|   |-- 0x0000A00A
	|   |-- 0x0000A00B
	|   |-- 0x0000A00C
	|   `-- 0x0000A010

where each dedicated DE subdirectory in turn will contain files used to
describe some DE characteristics, configure it, or read its current data
value as in::

	tlm_0/des/0xA001/
	|-- compo_instance_id
	|-- compo_type
	|-- enable
	|-- instance_id
	|-- name
	|-- persistent
	|-- tstamp_enable
	|-- tstamp_exp
	|-- type
	|-- unit
	|-- unit_exp
	`-- value

groups/
-------

An optional subtree containing in turn one subdirectory for each discovered
Group and named by Group ID as in::

	|-- groups
	|   |-- 0
	|   |-- ..
	|   `-- 1

where each dedicated GROUP subdirectory in turn will contain files used to
describe some Group characteristics, configure it, or read its current data
values, as in::

	scmi_tlm_0/groups/0/
	|-- available_update_intervals_ms
	|-- composing_des
	|-- control
	|-- current_update_interval_ms
	|-- des_bulk_read
	|-- des_single_sample_read
	|-- enable
	|-- intervals_discrete
	`-- tstamp_enable

Alternative Binary Interfaces - Special files
=============================================

Special files are populated across the filesystem so as to implement the support
of more performant alternative binary interfaces that can be used instead of the
main human readable ABI.

IOCTLs Interface
----------------

The ioctl-based interface is detailed in::

	include/uapi/linux/smci.h

The filesystem provides special files named *control/* to be used with the
ioctl interface mentioned above: note that the behaviour of some of the ioctls
is dependent on which *control/* file is used to invoke them (as detailed in the
UAPI header above).
