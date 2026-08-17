// SPDX-License-Identifier: GPL-2.0

//! DRM connector abstractions.
//!
//! C header: [`include/drm/drm_connector.h`](srctree/include/drm/drm_connector.h)

use crate::{bindings, types::Opaque};

/// A DRM connector (`struct drm_connector`).
///
/// # Invariants
///
/// The inner pointer is always a valid, non-null pointer to a `struct drm_connector`.
#[repr(transparent)]
pub struct Connector(Opaque<bindings::drm_connector>);

impl Connector {
    /// Creates a reference to a [`Connector`] from a raw pointer.
    ///
    /// # Safety
    ///
    /// `ptr` must be a valid, non-null pointer to a `struct drm_connector` that
    /// remains valid for at least the lifetime `'a`.
    pub unsafe fn from_raw<'a>(ptr: *mut bindings::drm_connector) -> &'a Self {
        // SAFETY: Caller guarantees `ptr` is valid and lives for `'a`.
        unsafe { &*ptr.cast() }
    }

    /// Returns the raw pointer to the underlying `struct drm_connector`.
    pub fn as_raw(&self) -> *mut bindings::drm_connector {
        self.0.get()
    }
}
