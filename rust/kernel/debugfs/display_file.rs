// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Google LLC.

use crate::prelude::*;
use crate::seq_file::SeqFile;
use crate::seq_print;
use core::fmt::{Display, Formatter, Result};
use core::marker::PhantomData;

/// Implements `open` for `file_operations` via `single_open` to fill out a `seq_file`.
///
/// # Safety
///
/// * `inode`'s private pointer must point to a value of type `T` which will outlive the `inode`
///   and will not have any unique references alias it during the call.
/// * `file` must point to a live, not-yet-initialized file object.
pub(crate) unsafe extern "C" fn display_open<T: Display + Sync>(
    inode: *mut bindings::inode,
    file: *mut bindings::file,
) -> c_int {
    // SAFETY:
    // * `file` is acceptable by caller precondition.
    // * `print_act` will be called on a `seq_file` with private data set to the third argument,
    //   so we meet its safety requirements.
    // * The `data` pointer passed in the third argument is a valid `T` pointer that outlives
    //   this call by caller preconditions.
    unsafe { bindings::single_open(file, Some(display_act::<T>), (*inode).i_private) }
}

/// Prints private data stashed in a seq_file to that seq file.
///
/// # Safety
///
/// `seq` must point to a live `seq_file` whose private data is a live pointer to a `T` which may
/// not have any unique references alias it during the call.
pub(crate) unsafe extern "C" fn display_act<T: Display + Sync>(
    seq: *mut bindings::seq_file,
    _: *mut c_void,
) -> c_int {
    // SAFETY: By caller precondition, this pointer is live, points to a value of type `T`, and
    // there are not and will not be any unique references until we are done.
    let data = unsafe { &*((*seq).private as *mut T) };
    // SAFETY: By caller precondition, `seq_file` points to a live `seq_file`, so we can lift
    // it.
    let seq_file = unsafe { SeqFile::from_raw(seq) };
    seq_print!(seq_file, "{}", data);
    0
}

// Work around lack of generic const items.
pub(crate) trait DisplayFile {
    const VTABLE: bindings::file_operations;
}

impl<T: Display + Sync> DisplayFile for T {
    const VTABLE: bindings::file_operations = bindings::file_operations {
        read: Some(bindings::seq_read),
        llseek: Some(bindings::seq_lseek),
        release: Some(bindings::single_release),
        open: Some(display_open::<Self>),
        // SAFETY: `file_operations` supports zeroes in all fields.
        ..unsafe { core::mem::zeroed() }
    };
}

/// Adapter to implement `Display` via a callback with the same representation as `T`.
///
/// # Invariants
///
/// If an instance for `FormatAdapter<_, F>` is constructed, `F` is inhabited.
#[repr(transparent)]
pub(crate) struct FormatAdapter<D, F> {
    inner: D,
    _formatter: PhantomData<F>,
}

impl<D, F> Display for FormatAdapter<D, F>
where
    F: Fn(&D, &mut Formatter<'_>) -> Result + 'static,
{
    fn fmt(&self, fmt: &mut Formatter<'_>) -> Result {
        // SAFETY: FormatAdapter<_, F> can only be constructed if F is inhabited
        let f: &F = unsafe { materialize_zst_fmt() };
        f(&self.inner, fmt)
    }
}

/// For types with a unique value, produce a static reference to it.
///
/// # Safety
///
/// The caller asserts that F is inhabited
unsafe fn materialize_zst_fmt<F>() -> &'static F {
    const { assert!(core::mem::size_of::<F>() == 0) };
    let zst_dangle: core::ptr::NonNull<F> = core::ptr::NonNull::dangling();
    // SAFETY: While the pointer is dangling, it is a dangling pointer to a ZST, based on the
    // assertion above. The type is also inhabited, by the caller's assertion. This means
    // we can materialize it.
    unsafe { zst_dangle.as_ref() }
}
