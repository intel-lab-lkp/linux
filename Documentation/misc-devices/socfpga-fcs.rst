.. SPDX-License-Identifier: GPL-2.0

===================================
Altera SoCFPGA Crypto Service (FCS)
===================================

Overview
========

The Secure Device Manager (SDM) is the hardware security controller in Altera
SoCFPGA devices. It offers a runtime cryptographic interface, the FPGA Crypto
Service (FCS), which the host reaches through the stratix10-svc mailbox using
Arm Trusted Firmware SIP SMC calls.

This driver exposes one FCS operation: the Secure Data Object Service (SDOS),
which encrypts and decrypts data at rest. The SDM derives the working key from
a device-unique SDOS root key that never leaves the secure boundary, so the
host never handles raw key material. A primary use case is black-key
provisioning.

The crypto session and the per-request context ID are opened and closed by the
kernel around each request. Neither is part of the user ABI.

Why a dedicated ioctl
=====================

SDOS seals and unseals arbitrary application data of up to 32 KiB, not keys of
a bounded size, so the keyring payload limits of the trusted-keys subsystem do
not apply to it. The object it produces is a firmware-defined container rather
than a generic AEAD output, no key is ever set from the host so the crypto API
has nothing to bind a transform to, and the SDM returns per-operation status
values that must reach the caller alongside a valid result.

Device node
===========

The driver registers a misc device::

    /dev/socfpga-fcs

Opening it takes a reference on the driver state. If the driver is unbound
while the file is open, subsequent operations fail with ``ENODEV`` rather than
tearing the file descriptor out from under the caller.

Buffer sizes
============

An SDOS object consists of a 40-byte firmware-defined header, the payload, and
for an encrypted object a trailing 48-byte HMAC. The payload is between 32 and
32672 bytes.

==================  ==================  ==============
``op_mode``         ``src_size`` range  Maximum output
==================  ==================  ==============
non-zero (encrypt)  72 .. 32712 bytes   32760 bytes
zero (decrypt)      120 .. 32760 bytes  32712 bytes
==================  ==================  ==============

Size the output buffer for the maximum shown above. The SDM reports the exact
produced length once the operation completes.

Data structures
===============

.. kernel-doc:: include/uapi/misc/socfpga-fcs-crypto.h
   :identifiers: fcs_ioc_sdos

Driver IOCTLs
=============

.. c:macro:: FCS_IOC_SDOS
.. kernel-doc:: include/uapi/misc/socfpga-fcs-crypto.h
   :doc: FCS_IOC_SDOS

Return value
============

On success 0 is returned and the ``__u32`` addressed by ``dst_size`` holds
the number of bytes the SDM produced. On error -1 is returned and ``errno``
is set.

=========== ================================================================
Error       Description
=========== ================================================================
EINVAL      ``dst`` or ``dst_size`` was NULL, or ``src_size`` was out of
            range for ``op_mode``.
EMSGSIZE    The output did not fit in ``dst``. The operation itself
            succeeded and ``error_code`` holds the SDM status, but the
            result is discarded, so ``dst`` must be sized for the maximum
            output.
EFAULT      One of the supplied addresses was not accessible.
EIO         The SDM reported a fatal error. ``error_code`` holds the status.
ENODEV      The device was unbound while the file descriptor was open.
ENOMEM      Staging buffers could not be allocated.
ERESTARTSYS A signal arrived while waiting for another request to finish.
            Only one SDM transaction is in flight at a time.
=========== ================================================================

SDM status values
=================

The ``__s32`` addressed by ``error_code`` carries the firmware status. Two
decrypt results are successes that carry a warning: the ioctl returns 0 for
both and the plaintext is in ``dst``.

======= ================================================================
Status  Description
======= ================================================================
0x000   The operation completed successfully.
0x102   Decryption succeeded; reprovisioning the key is recommended.
0x103   Decryption succeeded, but not with the latest key.
======= ================================================================

Any other non-zero status is fatal and the ioctl fails with ``EIO``.

User-space usage
================
A sample userspace application demonstrating these ioctls can be found at
`Git Repository - altera-fpga/libfcs <https://github.com/altera-fpga/libfcs>`_

Decrypting an object::

    __u8 out[32712];                /* maximum decrypt output */
    __u32 out_len = sizeof(out);
    __s32 status = 0;

    struct fcs_ioc_sdos arg = {
            .error_code = (__u64)(uintptr_t)&status,
            .src        = (__u64)(uintptr_t)in,
            .src_size   = in_len,
            .dst        = (__u64)(uintptr_t)out,
            .dst_size   = (__u64)(uintptr_t)&out_len,
            .op_mode    = 0,        /* decrypt */
    };

    if (ioctl(fd, FCS_IOC_SDOS, &arg) == 0)
            /* out_len bytes of plaintext in out */
            write(STDOUT_FILENO, out, out_len);
    else
            fprintf(stderr, "SDOS failed: %m, SDM status 0x%x\n", status);
