.. SPDX-License-Identifier: GPL-2.0-only OR MIT

====================================================
Texas Instruments NPAC (Network Packet ACcelerator)
====================================================

Overview
========

NPAC (Network Packet ACcelerator) is a hardware accelerator available
on Texas Instruments SoCs in the TDA5 family that provides network packet
processing acceleration [0]_.

The NPAC Host Driver runs on a dedicated remote processor and manages
NPAC hardware.

The NPAC client driver provides offloading support for ethernet-related tasks
and communicates with the NPAC Host Driver over RPMsg (Remote Processor
Messaging).

The driver exposes /dev/accel/accel* as the userspace interface for accelerator
configuration and management operations.

The NPAC Host Driver supports ACL (Access Control
List) [1]_ rule offload. ACL configuration requests are sent by the client
driver to the NPAC Host Driver.

Communication Model
-------------------

The NPAC client driver registers with the RPMsg subsystem during driver
initialization.

When the NPAC host driver announces the NPAC RPMsg service endpoint, the
RPMsg core matches the announced service against the registered NPAC client
driver and invokes the driver probe callback.

Offload command received from userspace are formatted into RPMsgs and sent
to the NPAC Host Driver.

NPAC Host Driver responses are received through RPMsg callbacks.

ACL Rule Offload
----------------

ACL (Access Control List) rules are packet filtering policies that define how
network traffic should be handled — for example, dropping packets from a
blacklisted source IP or allowing traffic only from trusted MAC addresses.

In the standard Linux networking stack, ACL enforcement is handled in software
by the netfilter subsystem. Each packet is inspected against the configured
ruleset on the host CPU. On embedded SoCs in the TDA5 family such as TDA54,
the per-packet overhead of software-based ACL enforcement accumulates under
high traffic rates, increasing CPU utilization and reducing the processing
capacity available for other system functions.

The NPAC client driver offloads rules from Linux to NPAC hardware.
Once a rule is programmed, matching packets are handled entirely by NPAC,
reducing CPU load.

For example, a rule to drop all packets from a specific source IP or to
allow packets from a trusted source is offloaded to NPAC. Instead of the
host parsing each packet and applying the ACL rules, NPAC enforces the
rule directly in hardware.

Userspace Interface
-------------------

The driver exposes /dev/accel/accel* as the userspace control interface for ACL
rule management operations.

The interface uses IOCTL-based request handling for accelerator configuration
and rule management operations.

IOCTL Commands
~~~~~~~~~~~~~~

DRM_IOCTL_NPAC_ACL_ADD_RULE

  This IOCTL allows userspace to add an ACL rule to the accelerator. The
  call will forward the rule to the NPAC host driver for hardware programming.
  The NPAC Host Driver is responsible for validating the rule and applying it to
  the NPAC hardware.

DRM_IOCTL_NPAC_ACL_DELETE_RULE

  This IOCTL allows userspace to remove an ACL rule from the accelerator.
  The call will forward the deletion request to the NPAC host driver, which
  will remove the rule from the NPAC hardware.

Driver Lifecycle
----------------

The driver lifecycle is coordinated through the RPMsg framework.

Probe Flow
~~~~~~~~~~

1. The NPAC client driver registers with the RPMsg subsystem

2. NPAC host driver initializes on the remote processor

3. NPAC Host Driver announces its RPMsg service endpoint

4. The RPMsg subsystem in Linux matches the announced service against
   the registered NPAC client driver

5. The RPMsg subsystem invokes the driver probe callback

6. Driver communication resources are initialized

7. /dev/accel/accel* becomes available to userspace

Request Flow
~~~~~~~~~~~~

1. Userspace submits an ACL configuration request through /dev/accel/accel*

2. The request is forwarded to the NPAC host driver using RPMsg

3. The NPAC Host Driver validates the request and performs the required hardware
   programming operations

4. NPAC Host Driver responses are received by the Linux driver through the RPMsg
   callback.

RPMsg Integration
-----------------

The driver integrates with the Linux RPMsg subsystem for communication with
the NPAC host driver processor.

RPMsg is used as the transport mechanism for:

- Accelerator configuration requests
- NPAC Host Driver responses

References
----------

.. [0] https://www.ti.com/lit/ds/symlink/tda54-q1.pdf
.. [1] https://en.wikipedia.org/wiki/Access-control_list#Networking_ACLs
