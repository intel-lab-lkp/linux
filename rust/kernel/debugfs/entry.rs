// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Google LLC.

use crate::debugfs::file_ops::FileOps;
use crate::ffi::c_void;
use crate::str::{CStr, CStrExt as _};
use crate::sync::Arc;
use core::marker::PhantomData;

/// Type-erased parent that keeps either an `Entry` or `LookupEntry` alive.
///
/// This allows an `Entry` to have either type of parent while maintaining proper
/// lifetime management. The parent is kept alive via `Arc` reference counting.
#[allow(dead_code)]
pub(crate) enum DynParent {
    /// Parent is an owned `Entry` (will be removed on drop).
    Entry(Arc<Entry<'static>>),
    /// Parent is a looked-up `LookupEntry` (will NOT be removed on drop).
    LookupEntry(Arc<LookupEntry>),
}

/// Owning handle to a DebugFS entry.
///
/// # Invariants
///
/// The wrapped pointer will always be `NULL`, an error, or an owned DebugFS `dentry`.
pub(crate) struct Entry<'a> {
    entry: *mut bindings::dentry,
    // If we were created with an owning parent, this is the keep-alive
    _parent: Option<DynParent>,
    // If we were created with a non-owning parent, this prevents us from outliving it
    _phantom: PhantomData<&'a ()>,
}

// SAFETY: [`Entry`] is just a `dentry` under the hood, which the API promises can be transferred
// between threads.
unsafe impl Send for Entry<'_> {}

// SAFETY: All the C functions we call on the `dentry` pointer are threadsafe.
unsafe impl Sync for Entry<'_> {}

impl Entry<'static> {
    pub(crate) fn dynamic_dir(name: &CStr, parent: Option<Arc<Self>>) -> Self {
        let parent_ptr = match &parent {
            Some(entry) => entry.as_ptr(),
            None => core::ptr::null_mut(),
        };
        // SAFETY: The invariants of this function's arguments ensure the safety of this call.
        // * `name` is a valid C string by the invariants of `&CStr`.
        // * `parent_ptr` is either `NULL` (if `parent` is `None`), or a pointer to a valid
        //   `dentry` by our invariant. `debugfs_create_dir` handles `NULL` pointers correctly.
        let entry = unsafe { bindings::debugfs_create_dir(name.as_char_ptr(), parent_ptr) };

        Entry {
            entry,
            _parent: parent.map(DynParent::Entry),
            _phantom: PhantomData,
        }
    }

    /// Creates a new directory entry with a `LookupEntry` as parent.
    ///
    /// The parent is kept alive via `Arc` reference counting. When this `Entry` is dropped,
    /// the created directory will be removed, but the parent `LookupEntry` will only have
    /// its reference count decremented.
    pub(crate) fn dynamic_dir_with_lookup_parent(name: &CStr, parent: Arc<LookupEntry>) -> Self {
        // SAFETY: The invariants of this function's arguments ensure the safety of this call.
        // * `name` is a valid C string by the invariants of `&CStr`.
        // * `parent.as_ptr()` is a pointer to a valid `dentry` by the invariants of `LookupEntry`.
        let entry = unsafe { bindings::debugfs_create_dir(name.as_char_ptr(), parent.as_ptr()) };

        Entry {
            entry,
            _parent: Some(DynParent::LookupEntry(parent)),
            _phantom: PhantomData,
        }
    }

    /// # Safety
    ///
    /// * `data` must outlive the returned `Entry`.
    pub(crate) unsafe fn dynamic_file<T>(
        name: &CStr,
        parent: Arc<Self>,
        data: &T,
        file_ops: &'static FileOps<T>,
    ) -> Self {
        // SAFETY: The invariants of this function's arguments ensure the safety of this call.
        // * `name` is a valid C string by the invariants of `&CStr`.
        // * `parent.as_ptr()` is a pointer to a valid `dentry` by invariant.
        // * The caller guarantees that `data` will outlive the returned `Entry`.
        // * The guarantees on `FileOps` assert the vtable will be compatible with the data we have
        //   provided.
        let entry = unsafe {
            bindings::debugfs_create_file_full(
                name.as_char_ptr(),
                file_ops.mode(),
                parent.as_ptr(),
                core::ptr::from_ref(data) as *mut c_void,
                core::ptr::null(),
                &**file_ops,
            )
        };

        Entry {
            entry,
            _parent: Some(DynParent::Entry(parent)),
            _phantom: PhantomData,
        }
    }
}

impl<'a> Entry<'a> {
    pub(crate) fn dir(name: &CStr, parent: Option<&'a Entry<'_>>) -> Self {
        let parent_ptr = match &parent {
            Some(entry) => entry.as_ptr(),
            None => core::ptr::null_mut(),
        };
        // SAFETY: The invariants of this function's arguments ensure the safety of this call.
        // * `name` is a valid C string by the invariants of `&CStr`.
        // * `parent_ptr` is either `NULL` (if `parent` is `None`), or a pointer to a valid
        //   `dentry` (because `parent` is a valid reference to an `Entry`). The lifetime `'a`
        //   ensures that the parent outlives this entry.
        let entry = unsafe { bindings::debugfs_create_dir(name.as_char_ptr(), parent_ptr) };

        Entry {
            entry,
            _parent: None,
            _phantom: PhantomData,
        }
    }

    pub(crate) fn file<T>(
        name: &CStr,
        parent: &'a Entry<'_>,
        data: &'a T,
        file_ops: &FileOps<T>,
    ) -> Self {
        // SAFETY: The invariants of this function's arguments ensure the safety of this call.
        // * `name` is a valid C string by the invariants of `&CStr`.
        // * `parent.as_ptr()` is a pointer to a valid `dentry` because we have `&'a Entry`.
        // * `data` is a valid pointer to `T` for lifetime `'a`.
        // * The returned `Entry` has lifetime `'a`, so it cannot outlive `parent` or `data`.
        // * The caller guarantees that `vtable` is compatible with `data`.
        // * The guarantees on `FileOps` assert the vtable will be compatible with the data we have
        //   provided.
        let entry = unsafe {
            bindings::debugfs_create_file_full(
                name.as_char_ptr(),
                file_ops.mode(),
                parent.as_ptr(),
                core::ptr::from_ref(data) as *mut c_void,
                core::ptr::null(),
                &**file_ops,
            )
        };

        Entry {
            entry,
            _parent: None,
            _phantom: PhantomData,
        }
    }
}

impl Entry<'_> {
    /// Constructs a placeholder DebugFS [`Entry`].
    pub(crate) fn empty() -> Self {
        Self {
            entry: core::ptr::null_mut(),
            _parent: None,
            _phantom: PhantomData,
        }
    }

    /// Returns the pointer representation of the DebugFS directory.
    ///
    /// # Guarantees
    ///
    /// Due to the type invariant, the value returned from this function will always be an error
    /// code, NULL, or a live DebugFS directory. If it is live, it will remain live at least as
    /// long as this entry lives.
    pub(crate) fn as_ptr(&self) -> *mut bindings::dentry {
        self.entry
    }
}

impl Drop for Entry<'_> {
    fn drop(&mut self) {
        // SAFETY: `debugfs_remove` can take `NULL`, error values, and legal DebugFS dentries.
        // `as_ptr` guarantees that the pointer is of this form.
        unsafe { bindings::debugfs_remove(self.as_ptr()) }
    }
}

/// Non-owning handle to a DebugFS entry obtained via lookup.
///
/// Unlike [`Entry`], dropping a [`LookupEntry`] does not remove the directory from the
/// filesystem. It only releases the reference obtained via `debugfs_lookup`.
///
/// # Invariants
///
/// The wrapped pointer will always be `NULL` or a valid DebugFS `dentry` obtained via
/// `debugfs_lookup`.
pub(crate) struct LookupEntry {
    entry: *mut bindings::dentry,
    // If we were created with an owning parent, this is the keep-alive
    _parent: Option<Arc<LookupEntry>>,
}

// SAFETY: [`LookupEntry`] is just a `dentry` under the hood, which the API promises can be
// transferred between threads.
unsafe impl Send for LookupEntry {}

// SAFETY: All the C functions we call on the `dentry` pointer are threadsafe.
unsafe impl Sync for LookupEntry {}

impl LookupEntry {
    /// Looks up an existing directory in DebugFS.
    ///
    /// Returns `Some(LookupEntry)` if the directory exists, `None` otherwise.
    pub(crate) fn lookup(name: &CStr, parent: Option<Arc<Self>>) -> Option<Self> {
        let parent_ptr = match &parent {
            Some(entry) => entry.as_ptr(),
            None => core::ptr::null_mut(),
        };
        // SAFETY: The invariants of this function's arguments ensure the safety of this call.
        // * `name` is a valid C string by the invariants of `&CStr`.
        // * `parent_ptr` is either `NULL` (if `parent` is `None`), or a pointer to a valid
        //   `dentry` by our invariant. `debugfs_lookup` handles `NULL` pointers correctly
        //   (searches from debugfs root).
        let entry = unsafe { bindings::debugfs_lookup(name.as_char_ptr(), parent_ptr) };

        if entry.is_null() {
            None
        } else {
            Some(LookupEntry {
                entry,
                _parent: parent,
            })
        }
    }

    /// Returns the pointer representation of the DebugFS directory.
    pub(crate) fn as_ptr(&self) -> *mut bindings::dentry {
        self.entry
    }
}

impl Drop for LookupEntry {
    fn drop(&mut self) {
        if !self.entry.is_null() {
            // SAFETY: `dput` decrements the reference count on a dentry. The pointer is valid
            // because it was obtained via `debugfs_lookup` which increments the reference count.
            unsafe { bindings::dput(self.entry) }
        }
    }
}
