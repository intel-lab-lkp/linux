// SPDX-License-Identifier: GPL-2.0
//
// Copyright (C) 2025, 2026 Red Hat Inc.:
//   - Philipp Stanner <pstanner@redhat.com>

//! DMA Fence support.
//!
//! Reference: <https://docs.kernel.org/driver-api/dma-buf.html#c.dma_fence>
//!
//! C header: [`include/linux/dma-fence.h`](srctree/include/linux/dma-fence.h)

use crate::{
    bindings,
    prelude::*,
    types::Opaque, //
};

/// A dma fence.
#[pin_data]
#[repr(transparent)]
pub struct Fence {
    #[pin]
    inner: Opaque<bindings::dma_fence>,
}

impl Fence {
    /// Access the raw dma fence.
    #[inline]
    pub fn as_raw(&self) -> *mut bindings::dma_fence {
        self.inner.get()
    }
}
