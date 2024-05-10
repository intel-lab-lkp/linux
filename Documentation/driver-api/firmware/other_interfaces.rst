Other Firmware Interfaces
=========================

DMI Interfaces
--------------

.. kernel-doc:: drivers/firmware/dmi_scan.c
   :export:

EDD Interfaces
--------------

.. kernel-doc:: drivers/firmware/edd.c
   :internal:

Generic System Framebuffers Interface
-------------------------------------

.. kernel-doc:: drivers/firmware/sysfb.c
   :export:

Intel Stratix10 SoC Service Layer
---------------------------------
Some features of the Intel Stratix10 SoC require a level of privilege
higher than the kernel is granted. Such secure features include
FPGA programming. In terms of the ARMv8 architecture, the kernel runs
at Exception Level 1 (EL1), access to the features requires
Exception Level 3 (EL3).

The Intel Stratix10 SoC service layer provides an in kernel API for
drivers to request access to the secure features. The requests are queued
and processed one by one. ARM’s SMCCC is used to pass the execution
of the requests on to a secure monitor (EL3).

.. kernel-doc:: include/linux/firmware/intel/stratix10-svc-client.h
   :functions: stratix10_svc_command_code

.. kernel-doc:: include/linux/firmware/intel/stratix10-svc-client.h
   :functions: stratix10_svc_client_msg

.. kernel-doc:: include/linux/firmware/intel/stratix10-svc-client.h
   :functions: stratix10_svc_command_config_type

.. kernel-doc:: include/linux/firmware/intel/stratix10-svc-client.h
   :functions: stratix10_svc_cb_data

.. kernel-doc:: include/linux/firmware/intel/stratix10-svc-client.h
   :functions: stratix10_svc_client

.. kernel-doc:: drivers/firmware/stratix10-svc.c
   :export:

NXP Secure Enclave Firmware Interface
=====================================

Introduction
------------
The NXP's i.MX HW IP like EdgeLock-Enclave, V2X etc., creats an embedded secure
enclave within the SoC boundary to enable features like
 - Hardware Security Module (HSM)
 - Security Hardware Extension (SHE)
 - Vehicular to Anything (V2X)

Each of the above feature, is enabled through dedicated NXP H/W IP on the SoC.
On a single SoC, multiple hardware IP (or can say more than one secure enclave)
can exists.

NXP SoC(s) enabled with the such secure enclave(se) IP(s) are:
i.MX93, i.MX8ULP

To communicate with one or more co-existing 'se'(s) on SoC, there is/are dedicated
messaging units(MU) per 'se'. Each co-existing 'se' can have one or multiple exclusive
MU(s), dedicated to itself. None of the MU is shared between two se(s).
Communication of the MU is realized using the Linux mailbox driver.

NXP Secure Enclave(SE) Interface
--------------------------------
All those SE interface(s) 'se-if(s)' that is/are dedicated to a particular 'se', will be
enumerated and provisioned under the very single 'se' node.

Each 'se-if', comprise of twp layers:
- (C_DEV Layer) User-Space software-access interface.
- (Service Layer) OS-level software-access interface.

   +--------------------------------------------+
   |            Character Device(C_DEV)         |
   |                                            |
   |   +---------+ +---------+     +---------+  |
   |   | misc #1 | | misc #2 | ... | misc #n |  |
   |   |  dev    | |  dev    |     | dev     |  |
   |   +---------+ +---------+     +---------+  |
   |        +-------------------------+         |
   |        | Misc. Dev Synchr. Logic |         |
   |        +-------------------------+         |
   |                                            |
   +--------------------------------------------+

   +--------------------------------------------+
   |               Service Layer                |
   |                                            |
   |      +-----------------------------+       |
   |      | Message Serialization Logic |       |
   |      +-----------------------------+       |
   |          +---------------+                 |
   |          |  imx-mailbox  |                 |
   |          |   mailbox.c   |                 |
   |          +---------------+                 |
   |                                            |
   +--------------------------------------------+

- service layer:
  This layer is responsible for ensuring the communication protocol, that is defined
  for communication with firmware.

  FW Communication protocol ensures two things:
  - Serializing the multiple message(s) to be sent over an MU.
    A mutex locks instance "mu_lock" is instantiated per MU. It is taken to ensure
    one message is sent over MU at a time. The lock "mu_lock" is unlocked, post sending
    the message using the mbox api(s) exposed by mailbox kernel driver.

  - FW can handle one command-message at a time.
    Second command-message must wait till first command message is completely processed.
    Hence, another mutex lock instance "mu_cmd_lock" is instantiated per MU. It is taken
    to ensure one command-message is sent at a time, towards FW. This lock is not unlocked,
    for the next command-message, till previous command message is processed completely.

- c_dev:
  This layer offers character device contexts, created as '/dev/<se>_mux_chx'.
  Using these multiple device contexts, that are getting multiplexed over a single MU,
  user-space application(s) can call fops like write/read to send the command-message,
  and read back the command-response-message to/from Firmware.
  fops like read & write uses the above defined service layer API(s) to communicate with
  Firmware.

  Misc-device(/dev/<se>_mux_chn) synchronization protocol:

                                Non-Secure               +   Secure
                                                         |
                                                         |
                  +---------+      +-------------+       |
                  | se_fw.c +<---->+imx-mailbox.c|       |
                  |         |      |  mailbox.c  +<-->+------+    +------+
                  +---+-----+      +-------------+    | MU X +<-->+ ELE |
                      |                               +------+    +------+
                      +----------------+                 |
                      |                |                 |
                      v                v                 |
                  logical           logical              |
                  receiver          waiter               |
                     +                 +                 |
                     |                 |                 |
                     |                 |                 |
                     |            +----+------+          |
                     |            |           |          |
                     |            |           |          |
              device_ctx     device_ctx     device_ctx   |
                                                         |
                User 0        User 1       User Y        |
                +------+      +------+     +------+      |
                |misc.c|      |misc.c|     |misc.c|      |
 kernel space   +------+      +------+     +------+      |
                                                         |
 +------------------------------------------------------ |
                    |             |           |          |
 userspace     /dev/ele_muXch0    |           |          |
                          /dev/ele_muXch1     |          |
                                        /dev/ele_muXchY  |
                                                         |

When a user sends a command to the firmware, it registers its device_ctx
as waiter of a response from firmware.

Enclave's Firmware owns the storage management, over linux filesystem.
For this c_dev provisions a dedicated slave device called "receiver".

.. kernel-doc:: drivers/firmware/imx/se_fw.c
   :export:
