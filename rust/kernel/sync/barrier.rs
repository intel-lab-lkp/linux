// SPDX-License-Identifier: GPL-2.0

//! Memory barriers.
//!
//! These primitives have the same semantics as their C counterparts: and the precise definitions
//! of semantics can be found at [`LKMM`].
//!
//! [`LKMM`]: srctree/tools/memory-model/

#![expect(private_bounds, reason = "sealed implementation")]

pub use super::atomic::ordering::{
    Acquire,
    Full,
    Release, //
};

/// The annotation type for read operations.
pub struct Read;

/// The annotation type for write operations.
pub struct Write;

struct Smp;
struct Dma;

/// A compiler barrier.
///
/// A barrier that prevents compiler from reordering memory accesses across the barrier.
#[inline(always)]
pub(crate) fn barrier() {
    // By default, Rust inline asms are treated as being able to access any memory or flags, hence
    // it suffices as a compiler barrier.
    //
    // SAFETY: An empty asm block.
    unsafe { core::arch::asm!("") };
}

trait MemoryBarrier<Flavour = ()> {
    fn run();
}

// Currently kernel only support `rmb`, `wmb` and full `mb`.
// Upgrade `Acquire`/`Release` barriers to full barriers.

impl<F> MemoryBarrier<F> for Acquire
where
    Full: MemoryBarrier<F>,
{
    #[inline]
    fn run() {
        Full::run();
    }
}

impl<F> MemoryBarrier<F> for Release
where
    Full: MemoryBarrier<F>,
{
    #[inline]
    fn run() {
        Full::run();
    }
}

// Specific barrier implementations.

impl MemoryBarrier for Read {
    #[inline]
    fn run() {
        // SAFETY: `rmb()` is safe to call.
        unsafe { bindings::rmb() };
    }
}

impl MemoryBarrier for Write {
    #[inline]
    fn run() {
        // SAFETY: `wmb()` is safe to call.
        unsafe { bindings::wmb() };
    }
}

impl MemoryBarrier for Full {
    #[inline]
    fn run() {
        // SAFETY: `mb()` is safe to call.
        unsafe { bindings::mb() };
    }
}

impl MemoryBarrier<Dma> for Read {
    #[inline]
    fn run() {
        // SAFETY: `dma_rmb()` is safe to call.
        unsafe { bindings::dma_rmb() };
    }
}

impl MemoryBarrier<Dma> for Write {
    #[inline]
    fn run() {
        // SAFETY: `dma_wmb()` is safe to call.
        unsafe { bindings::dma_wmb() };
    }
}

impl MemoryBarrier<Dma> for Full {
    #[inline]
    fn run() {
        // SAFETY: `dma_mb()` is safe to call.
        unsafe { bindings::dma_mb() };
    }
}

impl MemoryBarrier<Smp> for Read {
    #[inline]
    fn run() {
        // SAFETY: `smp_rmb()` is safe to call.
        unsafe { bindings::smp_rmb() };
    }
}

impl MemoryBarrier<Smp> for Write {
    #[inline]
    fn run() {
        // SAFETY: `smp_wmb()` is safe to call.
        unsafe { bindings::smp_wmb() };
    }
}

impl MemoryBarrier<Smp> for Full {
    #[inline]
    fn run() {
        // SAFETY: `smp_mb()` is safe to call.
        unsafe { bindings::smp_mb() };
    }
}

/// Memory barrier.
///
/// A barrier that prevents compiler and CPU from reordering memory accesses across the barrier.
///
/// The specific forms of reordering can be specified using the parameter.
/// - `mb(Read)` provides a read-read barrier.
/// - `mb(Write)` provides a write-write barrier.
/// - `mb(Full)` provides a full barrier.
/// - `mb(Acquire)` prevents preceding read from being ordered against succeeding memory
///    operations.
/// - `mb(Release)` prevents preceding memory operations from being ordered against succeeding
///    writes.
///
/// # Examples
///
/// ```
/// # use kernel::sync::barrier::*;
/// mb(Read);
/// mb(Write);
/// mb(Acquire);
/// mb(Release);
/// mb(Full);
/// ```
#[inline]
#[doc(alias = "rmb")]
#[doc(alias = "wmb")]
pub fn mb<T: MemoryBarrier>(_: T) {
    T::run()
}

/// Memory barrier between CPUs.
///
/// A barrier that prevents compiler and CPU from reordering memory accesses across the barrier.
/// Does not prevent re-ordering with respect to other bus-mastering devices.
///
/// Prefer using `Acquire` [loads](super::atomic::Atomic::load) to `Acquire` barriers, and `Release`
/// [stores](super::atomic::Atomic::store) to `Release` barriers.
///
/// See [`mb`] for usage.
#[inline]
#[doc(alias = "smp_rmb")]
#[doc(alias = "smp_wmb")]
pub fn smp_mb<T: MemoryBarrier<Smp>>(_: T) {
    if cfg!(CONFIG_SMP) {
        T::run()
    } else {
        barrier()
    }
}

/// Memory barrier between local CPU and bus-mastering devices.
///
/// A barrier that prevents compiler and CPU from reordering memory accesses across the barrier.
/// Does not prevent re-ordering with respect to other CPUs.
///
/// See [`mb`] for usage.
#[inline]
#[doc(alias = "dma_rmb")]
#[doc(alias = "dma_wmb")]
pub fn dma_mb<T: MemoryBarrier<Dma>>(_: T) {
    T::run()
}
