// SPDX-License-Identifier: GPL-2.0 OR MIT

//! DRM connector.
//!
//! C header: [`include/drm/drm_connector.h`](srctree/include/drm/drm_connector.h)

use core::marker::PhantomPinned;
use kernel::prelude::*;
use kernel::types::{ForeignOwnable, Opaque};

/// A DRM connector representation that extends `struct drm_connector`.
///
/// This connector implementation enables DRM connector API development in Rust
/// and exposing said functionality to both C and Rust DRM consumers.
///
/// # Invariants
///
/// `raw_connector` is a valid pointer to a `struct drm_connector`.
///
/// [`struct drm_connector`]: srctree/include/drm/drm_connector.h
#[pin_data]
pub struct Connector {
    #[pin]
    raw_connector: Opaque<*mut bindings::drm_connector>,
    rust_only_attribute: bool,

    /// A connector needs to be pinned since it is referred to using a raw
    /// pointer field `rust` in the C DRM `struct drm_connector` implementation.
    ///
    /// [`struct drm_connector`]: srctree/include/drm/drm_connector.h
    #[pin]
    _pin: PhantomPinned,
}

/// C entry point for initializing the Rust extension for a DRM connector.
///
/// When a DRM connector is being initialized in the core C stack, the Rust
/// `Connector` extension needs to be allocated and initialized.
///
/// * `raw_connector`: A pointer to `struct drm_connector`, the C DRM connector
///   implementation.
///
/// # Safety
///
/// * `raw_connector` must point to a valid, though partially initialized,
///   `struct drm_connector` where the `rust` field is not already initialized.
///
/// `raw_connector` must point to a valid `struct drm_connector` for the
/// duration of the function call.
///
/// [`struct drm_connector`]: srctree/include/drm/drm_connector.h
#[export]
pub unsafe extern "C" fn drm_connector_init_rust(
    raw_connector: *mut bindings::drm_connector,
) -> kernel::ffi::c_int {
    let connector = match KBox::pin_init(
        try_pin_init!(Connector{
            raw_connector <- Opaque::new(raw_connector),
            rust_only_attribute: true,
            _pin: PhantomPinned,
        }),
        GFP_KERNEL,
    ) {
        Ok(kbox) => kbox,
        Err(_) => return -ENOMEM.to_errno(),
    };

    // Provide the C `struct drm_connector` instance a handle to the Rust
    // `drm::connector:Connector` implementation for Rust connector APIs and the
    // `drm_connector_cleanup_rust` cleanup call.
    //
    // SAFETY: `raw_connector` is a valid pointer with a `rust` field that does
    // not already point to an initialized `drm::connector::Connector`
    unsafe { (*raw_connector).rust = connector.into_foreign() };

    0
}

/// C entry point for tearing down the Rust extension for a DRM connector.
///
/// When a DRM connector is being cleaned up from the core C stack, the Rust
/// `Connector` extension instance needs to be dropped.
///
/// * `raw_connector`: A pointer to `struct drm_connector`, the C DRM connector
///   implementation.
///
/// # Safety
///
/// * `raw_connector` must be valid and have the `rust` field initialized by
///   `drm_connector_init_rust()`.
///
/// `raw_connector` must remain valid for the duration of the function call and
/// the `rust` field must be preserved since the `drm_connector_init_rust()`
/// invocation.
///
/// [`struct drm_connector`]: srctree/include/drm/drm_connector.h
#[export]
pub unsafe extern "C" fn drm_connector_cleanup_rust(raw_connector: *mut bindings::drm_connector) {
    // SAFETY: By the safety requirements of this function, the `rust` field of
    // `raw_connector`, a valid pointer, is initialized by the `into_foreign()`
    // call made by `drm_connector_init_rust()`.
    drop(unsafe { <Pin<KBox<Connector>>>::from_foreign((*raw_connector).rust) });
}
