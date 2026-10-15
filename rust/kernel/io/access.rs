// SPDX-License-Identifier: GPL-2.0

//! Access modes of I/O locations.
//!
//! An I/O location ([`IoLoc`](super::IoLoc)) has an access mode, which applies to the views
//! projected through it: a [`ReadWrite`] location gives the view as it is, a [`ReadOnly`] location
//! gives a [`ReadOnly`](super::ReadOnly) view, and a [`WriteOnly`] location gives a
//! [`WriteOnly`](super::WriteOnly) view.

use crate::ptr::KnownSize;

use super::{
    IoBackend,
    ReadOnlyBackend,
    WriteOnlyBackend, //
};

/// An access mode of an I/O location.
pub trait Mode {
    /// Backend of the views this mode gives for views of backend `B`.
    type Backend<B: IoBackend>: IoBackend;

    /// Applies this mode to `view`.
    fn restrict<'a, B: IoBackend, T: ?Sized + KnownSize>(
        view: B::View<'a, T>,
    ) -> <Self::Backend<B> as IoBackend>::View<'a, T>;
}

/// View of `T` that access mode `M` gives for views of backend `B`.
pub type RestrictedView<'a, M, B, T> = <<M as Mode>::Backend<B> as IoBackend>::View<'a, T>;

/// Read-write access mode.
///
/// Views are given as they are.
pub struct ReadWrite;

impl Mode for ReadWrite {
    type Backend<B: IoBackend> = B;

    #[inline(always)]
    fn restrict<'a, B: IoBackend, T: ?Sized + KnownSize>(view: B::View<'a, T>) -> B::View<'a, T> {
        view
    }
}

/// Read-only access mode.
///
/// Views are wrapped in [`ReadOnly`](super::ReadOnly).
pub struct ReadOnly;

impl Mode for ReadOnly {
    type Backend<B: IoBackend> = ReadOnlyBackend<B>;

    #[inline(always)]
    fn restrict<'a, B: IoBackend, T: ?Sized + KnownSize>(
        view: B::View<'a, T>,
    ) -> super::ReadOnly<B::View<'a, T>> {
        super::ReadOnly(view)
    }
}

/// Write-only access mode.
///
/// Views are wrapped in [`WriteOnly`](super::WriteOnly).
pub struct WriteOnly;

impl Mode for WriteOnly {
    type Backend<B: IoBackend> = WriteOnlyBackend<B>;

    #[inline(always)]
    fn restrict<'a, B: IoBackend, T: ?Sized + KnownSize>(
        view: B::View<'a, T>,
    ) -> super::WriteOnly<B::View<'a, T>> {
        super::WriteOnly(view)
    }
}
