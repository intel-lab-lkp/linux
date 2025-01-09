// SPDX-License-Identifier: GPL-2.0

//! Abstraction for system resources.
//!
//! C header: [`include/linux/ioport.h`](../../../../include/linux/ioport.h)

use crate::str::CStr;
use crate::types::Opaque;

/// A resource abstraction.
#[repr(transparent)]
pub struct Resource(Opaque<bindings::resource>);

impl Resource {
    /// Creates a reference to a [`Resource`] from a valid pointer.
    ///
    /// # Safety
    ///
    /// The caller must ensure that for the duration of 'a, the pointer will
    /// point at a valid `bindings::resource`
    ///
    /// The caller must also ensure that the `Resource` is only accessed via the
    /// returned reference for the duration of 'a.
    pub(crate) unsafe fn from_ptr<'a>(ptr: *mut bindings::resource) -> &'a Self {
        // SAFETY: Self is a transparent wrapper around `Opaque<bindings::resource>`.
        unsafe { &mut *ptr.cast() }
    }

    /// Returns the size of the resource.
    pub fn size(&self) -> bindings::resource_size_t {
        let inner = self.0.get();
        // SAFETY: safe as per the invariants of `Resource`
        unsafe { bindings::resource_size(inner) }
    }

    /// Returns the start address of the resource.
    pub fn start(&self) -> u64 {
        let inner = self.0.get();
        // SAFETY: safe as per the invariants of `Resource`
        unsafe { *inner }.start
    }

    /// Returns the name of the resource.
    pub fn name(&self) -> &CStr {
        let inner = self.0.get();
        // SAFETY: safe as per the invariants of `Resource`
        unsafe { CStr::from_char_ptr((*inner).name) }
    }
}
