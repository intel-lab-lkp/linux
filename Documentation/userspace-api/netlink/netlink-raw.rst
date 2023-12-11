.. SPDX-License-Identifier: BSD-3-Clause

======================================================
Netlink specification support for raw Netlink families
======================================================

This document describes the additional properties required by raw Netlink
families such as ``NETLINK_ROUTE`` which use the ``netlink-raw`` protocol
specification.

Specification
=============

The netlink-raw schema extends the :doc:`genetlink-legacy <genetlink-legacy>`
schema with properties that are needed to specify the protocol numbers and
multicast IDs used by raw netlink families. See :ref:`classic_netlink` for more
information. The raw netlink families also make use of type-specific
sub-messages.

Globals
-------

protonum
~~~~~~~~

The ``protonum`` property is used to specify the protocol number to use when
opening a netlink socket.

.. code-block:: yaml

  # SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause)

  name: rt-addr
  protocol: netlink-raw
  protonum: 0             # part of the NETLINK_ROUTE protocol


Multicast group properties
--------------------------

value
~~~~~

The ``value`` property is used to specify the group ID to use for multicast
group registration.

.. code-block:: yaml

  mcast-groups:
    list:
      -
        name: rtnlgrp-ipv4-ifaddr
        value: 5
      -
        name: rtnlgrp-ipv6-ifaddr
        value: 9
      -
        name: rtnlgrp-mctp-ifaddr
        value: 34

Sub-messages
------------

Several raw netlink families such as rt_link and tc have type-specific
sub-messages. These sub-messages can appear as an attribute in a top-level or a
nested attribute space.

A sub-message attribute uses the value of another attribute as a selector key to
choose the right sub-message format. For example if the following attribute has
already been decoded:

.. code-block:: json

  { "kind": "gre" }

and we encounter the following attribute spec:

.. code-block:: yaml

  -
    name: data
    type: sub-message
    sub-message: linkinfo-data-msg
    selector: kind

Then we look for a sub-message definition called 'linkinfo-data-msg' and use the
value of the 'kind' attribute i.e. 'gre' as the key to choose the correct format
for the sub-message:

.. code-block:: yaml

  sub-messages:
    name: linkinfo-data-msg
    formats:
      -
        value: bridge
        attribute-set: linkinfo-bridge-attrs
      -
        value: gre
        attribute-set: linkinfo-gre-attrs
      -
        value: geneve
        attribute-set: linkinfo-geneve-attrs

This would decode the attribute value as a sub-message with the attribute-set
called 'linkinfo-gre-attrs' as the attribute space.

A sub-message can have an optional 'fixed-header' followed by zero or more
attributes from an attribute-set. For example the following 'tc-options-msg'
sub-message defines message formats that use a mixture of fixed-header,
attribute-set or both together:

.. code-block:: yaml

  sub-messages:
    -
      name: tc-options-msg
      formats:
        -
          value: bfifo
          fixed-header: tc-fifo-qopt
        -
          value: cake
          attribute-set: tc-cake-attrs
        -
          value: netem
          fixed-header: tc-netem-qopt
          attribute-set: tc-netem-attrs

Note that a selector attribute must appear in a netlink message before any
sub-message attributes that depend on it.
