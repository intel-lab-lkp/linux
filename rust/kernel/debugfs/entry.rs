// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Google LLC.

use crate::sync::Arc;

/// Owning handle to a DebugFS entry.
///
/// # Invariants
///
/// The wrapped pointer will always be `NULL`, an error, or an owned DebugFS `dentry`.
pub(crate) struct Entry {
    entry: *mut bindings::dentry,
    _parent: Option<Arc<Entry>>,
}

// SAFETY: [`Entry`] is just a `dentry` under the hood, which the API promises can be transferred
// between threads.
unsafe impl Send for Entry {}

// SAFETY: All the native functions we re-export use interior locking, and the contents of the
// struct are opaque to Rust.
unsafe impl Sync for Entry {}

impl Entry {
    /// Constructs a new DebugFS [`Entry`] from the underlying pointer.
    ///
    /// # Safety
    ///
    /// The pointer must either be an error code, `NULL`, or represent a transfer of ownership of a
    /// live DebugFS directory. If this is a child directory or file, `'a` must be less than the
    /// lifetime of the parent directory.
    ///
    /// If the dentry has a parent, it must be provided as the parent argument.
    pub(crate) unsafe fn new(entry: *mut bindings::dentry, parent: Option<Arc<Entry>>) -> Self {
        Self {
            entry,
            _parent: parent,
        }
    }

    /// Constructs a placeholder DebugFS [`Entry`].
    pub(crate) fn empty() -> Self {
        Self {
            entry: core::ptr::null_mut(),
            _parent: None,
        }
    }

    /// Returns the pointer representation of the DebugFS directory.
    ///
    /// # Guarantees
    ///
    /// Due to the type invariant, the value returned from this function will always be an error
    /// code, NULL, or a live DebugFS directory.
    pub(crate) fn as_ptr(&self) -> *mut bindings::dentry {
        self.entry
    }
}

impl Drop for Entry {
    fn drop(&mut self) {
        // SAFETY: `debugfs_remove` can take `NULL`, error values, and legal DebugFS dentries.
        // `as_ptr` guarantees that the pointer is of this form.
        unsafe { bindings::debugfs_remove(self.as_ptr()) }
    }
}
