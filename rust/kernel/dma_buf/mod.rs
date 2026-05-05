// SPDX-License-Identifier: GPL-2.0 OR MIT

//! DMA-buf subsystem abstractions.

pub mod dma_fence;

pub use self::dma_fence::Fence;

/// How the fences from a `dma_resv` obj are used.
///
/// Please see [the C-side documentation][dma_resv_usage] for more details.
///
/// [dma_resv_usage]: https://docs.kernel.org/driver-api/dma-buf.html#c.dma_resv_usage
#[repr(u32)]
pub enum DmaResvUsage {
    /// For in kernel memory management only (e.g. copying, clearing memory).
    Kernel = bindings::dma_resv_usage_DMA_RESV_USAGE_KERNEL,
    /// Implicit write synchronization for userspace submissions.
    Write = bindings::dma_resv_usage_DMA_RESV_USAGE_WRITE,
    /// Implicit read synchronization for userspace submissions.
    Read = bindings::dma_resv_usage_DMA_RESV_USAGE_READ,
    /// No implicit sync (e.g. preemption fences, page table updates, TLB flushes).
    Bookkeep = bindings::dma_resv_usage_DMA_RESV_USAGE_BOOKKEEP,
}
