.. SPDX-License-Identifier: GPL-2.0

.. _discoverable_root:

Discoverable root partitions
============================

On EFI systems using a supported architecture, the kernel can discover the root
block device from GPT partition type UUID metadata on the disk containing the
active EFI System Partition.

This follows the `Discoverable Partitions Specification`_ which defines a list
of architecture-specific root partition type UUIDs.

Specifying ``root=`` on the kernel command line takes precedence and entirely
disables this automatic root partition discovery.

The disk to search is identified by the Boot Loader Interface
``LoaderDevicePartUUID`` EFI variable. If multiple partitions on that disk match
the architecture root partition type UUID, the kernel selects the first match in
block device enumeration order. Systems should not expose multiple eligible root
partitions unless that ordering is intended.

Partitions marked with the DPS ``no-auto`` GPT attribute are skipped. This
allows a partition with an otherwise discoverable type UUID to opt out from
automatic discovery.

The DPS read-only attribute is not enforced by kernel root discovery. The
root filesystem is mounted read-only by default unless ``rw`` is specified,
and user space remains responsible for later remount policy.

.. _Discoverable Partitions Specification:
   https://uapi-group.org/specifications/specs/discoverable_partitions_specification/
