.. SPDX-License-Identifier: GPL-2.0

============================================
DMA Direct and Memory Encryption Integration
============================================

Introduction
------------
Modern platforms introduce memory encryption features (e.g., AMD SEV, Intel TDX,
ARM CCA, and pKVM) typically for CoCo when running protected virtual machines.

These guests typically boot with their memory encrypted by default.

In some cases this memory needs to be accessed by the untrusted host or the
VMM which then requires this memory to be decrypted. One typical case is
dealing with emulated device (e.g., virtio) which are handled by direct-dma
code as these devices are not behind an IOMMU.

That means, the memory used by these devices must be decrypted before accessed
by the untrusted host.

It must be clarified that encrypted/decrypted may not always be
cryptographic; in a broader sense, a decrypted page means that it is
accessible or "shared" with the untrusted host.

Ownership
---------
The direct-dma layer deals with memory encryption in two distinct scenarios:

1. **Externally Managed Decryption (e.g., Restricted DMA Pools)**
   In some setups (like a device restricted to a specific SWIOTLB pool, i.e.,
   `DMA_RESTRICTED_POOL`), an entire region of memory is pre-decrypted during
   boot or pool initialization. The memory is owned by the pool, and the
   transitions (encryption/decryption) are **not** managed by direct-dma on
   a per-allocation basis.
   See Documentation/core-api/swiotlb.rst

2. **DMA Direct Managed Decryption (e.g., `force_dma_unencrypted()`)**
   For standard coherent DMA allocations the direct-dma layer is explicitly
   responsible for managing the decryption. It must decrypt the pages upon
   allocation and re-encrypt them upon freeing.

To cleanly separate these concerns, the core logic is abstracted via three
internal helpers:

* ``dma_external_decryption(dev)``: Returns true if the pages are decrypted but
  managed externally. For example, if the device allocates from a restricted
  DMA pool.
* ``dma_owns_decryption(dev)``: Returns true if the pages need to be explicitly
  decrypted and managed by the direct-dma layer (i.e., the architecture forces
  unencrypted DMA, and it's not handled by an external pool).
* ``is_dma_decrypted(dev)``: Returns true if the memory being used is in a
  decrypted state, regardless of who manages it.

Addressing and Page Protections
-------------------------------
When memory is decrypted (whether externally or by direct-dma), the layer must
adjust physical-to-DMA address conversions and page protections:

* **DMA Address Conversion:**
  Decrypted memory often requires a specific bit to be cleared or set in the DMA
  address (e.g., stripping the encryption bit). If ``is_dma_decrypted(dev)`` is
  true, the conversion uses ``phys_to_dma_unencrypted()`` instead of the standard
  ``phys_to_dma()``.

* **Page Protections (Remap and Mmap):**
  When remapping decrypted pages into the kernel virtual address space (vmalloc)
  or mapping them to user space via ``mmap()``, the page protection attributes
  must reflect the decrypted state. If ``is_dma_decrypted(dev)`` is true, the
  layer applies ``pgprot_decrypted(prot)`` to ensure the CPU accesses the memory
  with the correct encryption attributes.

Notes
-----
In many cases when memory encryption/decryption fails the page will be leaked,
that's was added for TDX, where ``set_memory_encrypted()`` or
``set_memory_decrypted()``may fail while the page remains shared.
