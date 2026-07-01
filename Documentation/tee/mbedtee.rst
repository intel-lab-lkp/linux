.. SPDX-License-Identifier: GPL-2.0

==============================================
MbedTEE (Trusted Execution Environment)
==============================================

The MbedTEE driver supports MbedTEE-based TEEs on ARM TrustZone (SMC calls,
GIC SPI notifications) and RISC-V IMSIC (shared-memory polling, MSI
notifications) platforms.

The driver requires the REE and TEE CPUs that share the RPC shared memory to
be hardware coherent. In practice, the REE/TEE CPUs are expected to be in the
same CPU cluster, with coherent caches and shared visibility for the ring and
payload buffers.

Communication overview
======================

The driver communicates with the TEE using a fixed shared-memory RPC protocol
layered above the architecture-specific call mechanism.

ARM platforms (SMC)
-------------------

On ARM and AArch64 platforms, the driver uses ARM SMC Calling Convention
(SMCCC) to issue fast calls and yield calls to the TEE:

- **Fast calls** (MBEDTEE_RPC_OS_VERSION, MBEDTEE_RPC_SUPPORT_YIELD,
    MBEDTEE_RPC_COMPLETE_TEE) are self-contained SMC calls that do
  not rely on shared-memory RPC structures.

- **Yield calls** post the physical address of an ``rpc_cmd`` structure
  in shared memory to the TEE. The TEE may issue RPC requests back to the
  driver before the call completes; kernel-directed RPCs are handled
  directly, while supplicant RPCs (filesystem and RPMB) are forwarded to
  tee-supplicant.

TEE-to-REE notifications on ARM are delivered via a GIC SPI edge interrupt
specified in the device tree (``interrupts`` property).

RISC-V platforms
----------------

On RISC-V platforms, SMCCC is not available. Communication is split into two
directions:

- **REE to TEE (R2T)**: The driver submits commands by writing the physical
  address of an ``rpc_cmd`` structure to a REE-to-TEE ring buffer
  (``rpc-r2t-ring``). The TEE polls that ring for new commands. No interrupt
  notification is sent; the protocol relies on TEE-side polling.

- **TEE to REE (T2R)**: The TEE writes an ``rpc_cmd`` into the TEE-to-REE
  ring and raises an IMSIC MSI to notify the REE driver. The MSI is
  allocated at runtime via the ``msi-parent`` DT property and its identity
  is published in ``callee_imsic_id``; ``callee_hartid`` tracks the
  target hart for migration during CPU hotplug. No SBI ecall is involved.

RISC-V REE-to-TEE polling rationale
-----------------------------------

The REE-to-TEE direction uses polling-only on RISC-V to avoid direct
notification interrupt writes from Linux to TEE-owned interrupt files.
This design is platform-independent and does not require ownership of
TEE-only hart interrupt files. The TEE-to-REE direction remains fully
functional via standard Linux MSI notifications and does not depend on
REE-to-TEE notification latency.

Shared memory regions
=====================

Two or three fixed shared memory regions are described in the device tree:

``rpc-t2r-ring``
    Ring buffer used by the TEE to post RPC request notifications to the REE
    driver. Present on all platforms.

``rpc-t2r-shm``
    Shared memory region carrying the actual ``rpc_cmd`` payloads for
    TEE-to-REE RPCs. Present on all platforms.

``rpc-r2t-ring``
    Ring buffer used by the REE driver to submit commands to the TEE on
    RISC-V IMSIC platforms.

RPC protocol
============

The TEE and REE communicate through the ``rpc_cmd`` structure in shared memory
and ring buffers::

    struct rpc_cmd {
        u32 id;          /* RPC function ID */
        u16 size;        /* payload size in bytes */
        u8  interrupted; /* set if wait was interrupted */
        u8  reserved;    /* explicit alignment padding */
        s32 ret;         /* return value */
        u32 pad;         /* explicit alignment padding */
        u64 waiter_id;   /* sync RPC request ID echoed on completion */
        u64 shm;         /* physical address of payload (sync RPC) */
        u64 data[];      /* inline payload (async RPC) */
    };

    struct rpc_ringbuf {
        u32 wr;              /* producer write pointer */
        u32 rd;              /* consumer read pointer */
        u32 callee_ready;    /* callee ready flag */
        u32 callee_imsic_id; /* RISC-V only: IMSIC local interrupt id */
        u32 callee_hartid;   /* RISC-V only: target hart-id for T2R notification */
        u32 reserved;        /* padding, must be zero */
        u8  mem[];
    };

For RISC-V T2R MSI, one MSI message targets one hart IMSIC file at a time.
The wire-visible state is split between ``callee_imsic_id`` and
``callee_hartid`` in the ring header; Linux may retarget the MSI across
online CPUs via ``irq_set_affinity()``, and the driver updates those fields
to match the selected hart.



Architecture diagram::

    User space                 Kernel                    TEE side
    ~~~~~~~~~~                 ~~~~~~                    ~~~~~~~~~~~~
  +--------+                                           +--------------+
  | Client |                                           | Trusted App  |
  +--------+                                           +--------------+
     /\                                                      /\
     || +------------+                                       ||
     || | MbedTEE-   |                                       \/
     || | supplicant |                                 +--------------+
     || +------------+                                 | TEE Internal |
     \/      /\                                        |     API      |
  +-------+  ||                                        +--------------+
  | TEE   |  ||         +---------+---------------+    |   MbedTEE    |
  | Client|  ||         |  TEE    | MbedTEE       |    |  Trusted OS  |
  |  API  |  \/         | subsys  | client driver |    +--------------+
  +-------+-------------+-----+--------+----------+----+              |
  |    Generic TEE API        |        | RPC (cmd/ring)|              |
  |    IOCTL (TEE_IOC_*)      |        | SMC / IMSIC   |              |
  +---------------------------+        +---------------+--------------+

Device tree binding
===================

See Documentation/devicetree/bindings/firmware/mbedtee,rpc.yaml for the
complete device tree binding specification, including the RISC-V
``msi-parent`` requirement.

References
==========

- [1] MbedTEE project: https://github.com/mbedtee
- [2] ARM SMC Calling Convention: https://developer.arm.com/architectures/system-architectures/software-standards/smccc
- [3] RISC-V IMSIC specification: https://github.com/riscv/riscv-aia
