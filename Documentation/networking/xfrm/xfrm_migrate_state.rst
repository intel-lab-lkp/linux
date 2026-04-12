.. SPDX-License-Identifier: GPL-2.0

=====================
XFRM SA Migrate State
=====================

Overview
========

``XFRM_MSG_MIGRATE_STATE`` migrates a single SA, looked up using SPI and
mark, without involving policies. Unlike ``XFRM_MSG_MIGRATE``, which couples
SA and policy migration and allows migrating multiple SAs in one call, this
interface identifies the SA unambiguously via SPI and supports changing
the reqid, addresses, encapsulation, selector, and offload.

Because IKE daemons such as *wan manage policies independently of
the kernel, this interface allows precise per-SA migration without
requiring policy involvement. Optional XFRM attributes follow an
omit-to-inherit model: omitting an attribute preserves the value from
the old SA. Hardware offload is an exception. It is inherited by default
but can be disabled with the ``XFRM_MIGRATE_STATE_NO_OFFLOAD``
flag or set to a new offload configuration with the
``XFRMA_OFFLOAD_DEV`` attribute.

SA Identification
=================

The struct is defined in ``include/uapi/linux/xfrm.h``. The SA is looked
up using ``xfrm_state_lookup()`` with ``id.spi``,
``id.daddr``, ``id.proto``, ``id.family``, and
``old_mark.v & old_mark.m`` as the mark key::

    struct xfrm_user_migrate_state {
        struct xfrm_usersa_id  id;       /* spi, daddr, proto, family */
        xfrm_address_t         new_daddr;
        xfrm_address_t         new_saddr;
        struct xfrm_mark       old_mark; /* SA lookup: key = v & m */
        struct xfrm_selector   new_sel;  /* new selector (see Flags) */
        __u32                  new_reqid;
        __u32                  flags;    /* XFRM_MIGRATE_STATE_* */
        __u16                  new_family;
        __u16                  reserved;
    };

Supported Attributes
====================

The following fields in ``xfrm_user_migrate_state`` are always explicit
and are not inherited from the existing SA. Passing zero is not equivalent
to "keep unchanged" — zero is used as-is:

- ``new_daddr`` - new destination address
- ``new_saddr`` - new source address
- ``new_family`` - new address family
- ``new_reqid`` - new reqid (0 = no reqid)
- ``new_sel`` - new selector; used when ``XFRM_MIGRATE_STATE_UPDATE_SEL`` is
  not set (see `Flags`_ below)
- ``flags`` - bitmask of ``XFRM_MIGRATE_STATE_*`` flags (see `Flags`_ below)

The following netlink attributes are also accepted. Omitting an attribute
inherits the value from the existing SA (omit-to-inherit).

.. list-table::
   :widths: 30 70
   :header-rows: 1

   * - Attribute
     - Description
   * - ``XFRMA_MARK``
     - Mark on the migrated SA (``struct xfrm_mark``). Absent inherits
       ``old_mark``. To use no mark on the new SA, send ``XFRMA_MARK``
       with ``{0, 0}``.
   * - ``XFRMA_ENCAP``
     - UDP encapsulation template; only ``UDP_ENCAP_ESPINUDP`` is supported.
       Set ``encap_type=0`` to remove encap.
   * - ``XFRMA_OFFLOAD_DEV``
     - Hardware offload configuration (``struct xfrm_user_offload``). Absent
       copies offload from the existing SA. When
       ``XFRM_MIGRATE_STATE_NO_OFFLOAD`` is set in ``flags``, the new SA has
       no offload; this flag is mutually exclusive with ``XFRMA_OFFLOAD_DEV``
       and sending both returns ``-EINVAL``.
   * - ``XFRMA_SET_MARK``
     - Output mark on the migrated SA; pair with ``XFRMA_SET_MARK_MASK``.
       Send 0 to clear.
   * - ``XFRMA_NAT_KEEPALIVE_INTERVAL``
     - NAT keepalive interval in seconds. Requires encap. Send 0 to clear.
       Automatically cleared when encap is removed; setting a non-zero
       value without encap returns ``-EINVAL``.
   * - ``XFRMA_MTIMER_THRESH``
     - Mapping maxage threshold. Requires encap. Send 0 to clear.
       Automatically cleared when encap is removed; setting a non-zero
       value without encap returns ``-EINVAL``.

The following SA properties are immutable and cannot be changed via
``XFRM_MSG_MIGRATE_STATE``: algorithms (``XFRMA_ALG_*``), replay state,
direction (``XFRMA_SA_DIR``), and security context (``XFRMA_SEC_CTX``).

Flags
=====

The ``flags`` field in ``xfrm_user_migrate_state`` controls optional
migration behaviour. Unknown flag bits are rejected with ``-EINVAL``.

.. list-table::
   :widths: 40 60
   :header-rows: 1

   * - Flag
     - Description
   * - ``XFRM_MIGRATE_STATE_NO_OFFLOAD``
     - When set, the new SA has no hardware offload even when
       ``XFRMA_OFFLOAD_DEV`` is absent. Without this flag, omitting
       ``XFRMA_OFFLOAD_DEV`` copies the existing offload to the new SA.
       Mutually exclusive with ``XFRMA_OFFLOAD_DEV``; sending both
       returns ``-EINVAL``.
   * - ``XFRM_MIGRATE_STATE_UPDATE_SEL``
     - When set, the kernel validates that the existing SA selector is a
       single-host entry matching the SA addresses (``prefixlen_s ==
       prefixlen_d`` equal to 32 for IPv4 or 128 for IPv6, and addresses
       matching ``id.daddr`` and ``props.saddr``). If the check passes,
       the new selector is derived from ``new_daddr`` and ``new_saddr``
       with the single-host mask for ``new_family``. A mismatch returns
       ``-EINVAL``. When this flag is not set, ``new_sel`` is used as-is
       for the migrated SA.

Migration Steps
===============

#. Install a block policy to drop traffic on the affected selector.
#. Remove the old policy.
#. Call ``XFRM_MSG_MIGRATE_STATE`` for each SA.
#. Reinstall the policies.
#. Remove the block policy.

Block Policy and IV Safety
--------------------------

Installing a block policy before migration is required to prevent
traffic leaks and IV reuse.

AES-GCM IV uniqueness is critical: reusing a (key, IV) pair allows
an attacker to recover the authentication subkey and forge
authentication tags, breaking both confidentiality and integrity.

``XFRM_MSG_MIGRATE_STATE`` atomically copies the sequence number and
replay window from the old SA to the new SA and deletes the old SA.
The block policy ensures no outgoing packets are sent in the migration
window, preventing IV reuse under the same key.

Feature Detection
=================

Userspace can probe for kernel support by sending a minimal
``XFRM_MSG_MIGRATE_STATE`` message with a non-existent SPI:

- ``-ENOPROTOOPT``: not supported (``CONFIG_XFRM_MIGRATE`` not enabled)
- any other error: supported

Userspace Notification on Success
=================================

On successful migration the kernel multicasts an
``XFRM_MSG_MIGRATE_STATE`` message to the ``XFRMNLGRP_MIGRATE`` group.
The fixed header is ``struct xfrm_user_migrate_state`` copied from the
request, followed by the same set of netlink attributes that are
accepted as input, with the differences noted below.

Differences from the request
-----------------------------

.. list-table::
   :widths: 25 75
   :header-rows: 1

   * - Field / Attribute
     - Difference
   * - ``new_sel``
     - Contains the actual selector of the newly installed SA, not the
       ``new_sel`` from the request. When
       ``XFRM_MIGRATE_STATE_UPDATE_SEL`` is set the kernel derives the
       selector from ``new_daddr`` / ``new_saddr``; the caller's
       ``new_sel`` field is ignored in that case. The notification
       always carries the real selector of the new SA.
   * - ``XFRMA_SA_DIR``
     - Present in the notification (set from the direction of the new
       SA) but **not accepted as input** — direction is immutable.
   * - ``flags``
     - Echoed back as-is. ``XFRM_MIGRATE_STATE_NO_OFFLOAD`` and
       ``XFRM_MIGRATE_STATE_UPDATE_SEL`` describe the request that was
       made, not a property of the resulting SA.

Attributes in the notification
-------------------------------

.. list-table::
   :widths: 30 70
   :header-rows: 1

   * - Attribute
     - Description
   * - ``XFRMA_ENCAP``
     - UDP encapsulation template, if configured on the new SA.
   * - ``XFRMA_OFFLOAD_DEV``
     - Hardware offload configuration, if active on the new SA.
   * - ``XFRMA_MARK``
     - Mark on the new SA, if set.
   * - ``XFRMA_SET_MARK``
     - Output mark on the new SA, if set.
   * - ``XFRMA_SET_MARK_MASK``
     - Output mark mask, present together with ``XFRMA_SET_MARK``.
   * - ``XFRMA_MTIMER_THRESH``
     - Mapping maxage threshold, if non-zero.
   * - ``XFRMA_NAT_KEEPALIVE_INTERVAL``
     - NAT keepalive interval, if non-zero.
   * - ``XFRMA_SA_DIR``
     - Direction of the new SA.

Error Handling
==============

If the target SA tuple (daddr, SPI, proto, family) is occupied by an existing
unrelated SA, the operation returns ``-EEXIST``. In this case both the old and
the new SA are gone. The old SA cannot be restored as doing so would risk
duplicate sequence number and IV reuse, which must not occur. Userspace should
handle ``-EEXIST``, for example by re-establishing the SA at the IKE level.

If the multicast notification (``XFRMNLGRP_MIGRATE``) fails to send,
the migration itself has already completed successfully and the new SA
is installed. The operation returns success, 0, with an extack warning,
but listeners will not receive the migration event.
