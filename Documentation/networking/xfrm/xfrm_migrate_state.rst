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
the reqid, addresses, encapsulation, and other SA-specific parameters.

Because IKE daemons such as strongSwan manage policies independently of
the kernel, this interface allows precise per-SA migration without
requiring policy involvement. Optional XFRM attributes follow an
omit-to-inherit model.

SA Identification
=================

The struct is defined in ``include/uapi/linux/xfrm.h``. The SA is looked
up using ``xfrm_state_lookup()`` with ``id.spi``,
``id.daddr``, ``id.proto``, ``id.family``, and ``old_mark``::

    struct xfrm_user_migrate_state {
        struct xfrm_usersa_id id;       /* spi, daddr, proto, family */
        xfrm_address_t        new_daddr;
        xfrm_address_t        new_saddr;
        __u16                 new_family;
        __u16                 reserved;
        __u32                 new_reqid;
        struct xfrm_mark      old_mark; /* SA lookup */
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
     - Hardware offload configuration. Set ``ifindex=0`` to remove offload.
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

``XFRM_MSG_MIGRATE_STATE`` atomically deletes the old SA and installs
the new one with the sequence counter and replay window copied. The
block policy ensures no outgoing packets are sent in the migration
window, preventing IV reuse under the same key.

Feature Detection
=================

Userspace can probe for kernel support by sending a minimal
``XFRM_MSG_MIGRATE_STATE`` message with a non-existent SPI:

- ``-ENOPROTOOPT``: not supported (``CONFIG_XFRM_MIGRATE`` not enabled)
- any other error: supported

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
