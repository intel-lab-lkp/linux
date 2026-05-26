// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Google LLC.

//! Adapters which allow the user to supply a write or read implementation as a value rather
//! than a trait implementation. If provided, it will override the trait implementation.

use super::{
    Reader,
    Writer, //
};

use crate::{
    fmt,
    prelude::*,
    uaccess::UserSliceReader, //
};

use core::{
    marker::PhantomData,
    ops::Deref, //
};

/// Indicates that operations implemented for `Self` may operate on file private
/// data pointing to `D`.
///
/// # Safety
///
/// Operations may reconstruct a shared reference to `Self` from a pointer to
/// `D`. Implementers must also arrange for any additional invariants of `Self`
/// to hold whenever such an operation is called.
pub(super) unsafe trait Adapter<D> {}

// SAFETY: A pointer to `D` may be reconstructed as a shared reference to `D`.
unsafe impl<D> Adapter<D> for D {}

/// Adapter to implement `Reader` via a callback with the same representation as `D`.
///
/// * Layer it on top of `FormatAdapter` for a read-write callback file.
/// * Layer it on top of `NoWriter` for a write-only callback file.
///
/// # Invariants
///
/// When `WritableAdapter<_, W>` is used for file operations, `W` is inhabited.
#[repr(transparent)]
pub(super) struct WritableAdapter<D, W> {
    inner: D,
    _writer: PhantomData<W>,
}

impl<D: Writer, W> Writer for WritableAdapter<D, W> {
    fn write(&self, fmt: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.inner.write(fmt)
    }
}

impl<D: Deref, W> Reader for WritableAdapter<D, W>
where
    W: Fn(&D::Target, &mut UserSliceReader) -> Result + Send + Sync + 'static,
{
    fn read_from_slice(&self, reader: &mut UserSliceReader) -> Result {
        // SAFETY: The callback API obtains this implementation only after
        // receiving a `&'static W`, so `W` is inhabited.
        let w: &W = unsafe { materialize_zst() };
        w(self.inner.deref(), reader)
    }
}

/// Adapter to implement `Writer` via a callback with the same representation as `D`.
///
/// # Invariants
///
/// When `FormatAdapter<_, F>` is used for file operations, `F` is inhabited.
#[repr(transparent)]
pub(super) struct FormatAdapter<D, F> {
    inner: D,
    _formatter: PhantomData<F>,
}

impl<D, F> Deref for FormatAdapter<D, F> {
    type Target = D;
    fn deref(&self) -> &D {
        &self.inner
    }
}

impl<D, F> Writer for FormatAdapter<D, F>
where
    F: Fn(&D, &mut fmt::Formatter<'_>) -> fmt::Result + 'static,
{
    fn write(&self, fmt: &mut fmt::Formatter<'_>) -> fmt::Result {
        // SAFETY: The callback API obtains this implementation only after
        // receiving a `&'static F`, so `F` is inhabited.
        let f: &F = unsafe { materialize_zst() };
        f(&self.inner, fmt)
    }
}

// SAFETY: `FormatAdapter<D, F>` is transparent over `D`. The callback API
// receives a `&'static F`, which establishes its inhabitation invariant.
unsafe impl<D, F> Adapter<D> for FormatAdapter<D, F> {}

#[repr(transparent)]
pub(super) struct NoWriter<D> {
    inner: D,
}

impl<D> Deref for NoWriter<D> {
    type Target = D;
    fn deref(&self) -> &D {
        &self.inner
    }
}

// SAFETY: The nested adapters are transparent over `D`. The callback APIs
// receive `&'static F` and `&'static W`, which establish their inhabitation
// invariants.
unsafe impl<D, F, W> Adapter<D> for WritableAdapter<FormatAdapter<D, F>, W> {}

// SAFETY: The nested adapters are transparent over `D`. The callback API
// receives a `&'static W`, which establishes its inhabitation invariant.
unsafe impl<D, W> Adapter<D> for WritableAdapter<NoWriter<D>, W> {}

/// For types with a unique value, produce a static reference to it.
///
/// # Safety
///
/// The caller asserts that `F` is inhabited.
unsafe fn materialize_zst<F>() -> &'static F {
    const { assert!(core::mem::size_of::<F>() == 0) };
    let zst_dangle: core::ptr::NonNull<F> = core::ptr::NonNull::dangling();
    // SAFETY: While the pointer is dangling, it is a dangling pointer to a ZST, based on the
    // assertion above. The type is also inhabited, by the caller's assertion. This means
    // we can materialize it.
    unsafe { zst_dangle.as_ref() }
}
