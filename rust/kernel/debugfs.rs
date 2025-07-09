// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Google LLC.

//! DebugFS Abstraction
//!
//! C header: [`include/linux/debugfs.h`](srctree/include/linux/debugfs.h)

#[cfg(CONFIG_DEBUG_FS)]
use crate::prelude::GFP_KERNEL;
use crate::str::CStr;
#[cfg(CONFIG_DEBUG_FS)]
use crate::sync::Arc;
use core::fmt::Display;

#[cfg(CONFIG_DEBUG_FS)]
mod display_file;
#[cfg(CONFIG_DEBUG_FS)]
mod entry;
#[cfg(CONFIG_DEBUG_FS)]
use entry::Entry;

/// Owning handle to a DebugFS directory.
///
/// This directory will be cleaned up when the handle and all child directory/file handles have
/// been dropped.
// We hold a reference to our parent if it exists to prevent the dentry we point to from being
// cleaned up when our parent is removed.
pub struct Dir(#[cfg(CONFIG_DEBUG_FS)] Option<Arc<Entry>>);

impl Dir {
    /// Create a new directory in DebugFS. If `parent` is [`None`], it will be created at the root.
    #[cfg(CONFIG_DEBUG_FS)]
    fn create(name: &CStr, parent: Option<&Dir>) -> Self {
        let parent_ptr = match parent {
            // If the parent couldn't be allocated, just early-return
            Some(Dir(None)) => return Self(None),
            Some(Dir(Some(entry))) => entry.as_ptr(),
            None => core::ptr::null_mut(),
        };
        // SAFETY:
        // * `name` argument points to a NUL-terminated string that lives across the call, by
        //   invariants of `&CStr`.
        // * If `parent` is `None`, `parent_ptr` is null to mean create at root.
        // * If `parent` is `Some`, `parent_ptr` is a live dentry debugfs pointer.
        let dir = unsafe { bindings::debugfs_create_dir(name.as_char_ptr(), parent_ptr) };

        Self(
            // If Arc creation fails, the `Entry` will be dropped, so the directory will be cleaned
            // up.
            Arc::new(
                // SAFETY: `debugfs_create_dir` either returns an error code or a legal `dentry`
                // pointer, and the parent is the same one passed to `debugfs_create_dir`
                unsafe { Entry::new(dir, parent.and_then(|dir| dir.0.clone())) },
                GFP_KERNEL,
            )
            .ok(),
        )
    }

    #[cfg(not(CONFIG_DEBUG_FS))]
    fn create(_name: &CStr, _parent: Option<&Dir>) -> Self {
        Self()
    }

    #[cfg(CONFIG_DEBUG_FS)]
    fn create_file<T: Display + Sized + Sync>(&self, name: &CStr, data: &'static T) -> File {
        let Some(parent) = &self.0 else {
            return File {
                _entry: Entry::empty(),
            };
        };
        // SAFETY:
        // * `name` is a NUL-terminated C string, living across the call, by `CStr` invariant.
        // * `parent` is a live `dentry` since we have a reference to it.
        // * `vtable` is all stock `seq_file` implementations except for `open`.
        //   `open`'s only requirement beyond what is provided to all open functions is that the
        //   inode's data pointer must point to a `T` that will outlive it, which we know because
        //   we have a static reference.
        let ptr = unsafe {
            bindings::debugfs_create_file_full(
                name.as_char_ptr(),
                0o444,
                parent.as_ptr(),
                data as *const _ as *mut _,
                core::ptr::null(),
                &<T as display_file::DisplayFile>::VTABLE,
            )
        };

        // SAFETY: `debugfs_create_file_full` either returns an error code or a legal
        // dentry pointer, so `Entry::new` is safe to call here.
        let entry = unsafe { Entry::new(ptr, Some(parent.clone())) };

        File { _entry: entry }
    }

    #[cfg(not(CONFIG_DEBUG_FS))]
    fn create_file<T: Display + Sized + Sync>(&self, _name: &CStr, _data: &'static T) -> File {
        File {}
    }

    /// Create a DebugFS subdirectory.
    ///
    /// Subdirectory handles cannot outlive the directory handle they were created from.
    ///
    /// # Examples
    ///
    /// ```
    /// # use kernel::c_str;
    /// # use kernel::debugfs::Dir;
    /// let parent = Dir::new(c_str!("parent"));
    /// let child = parent.subdir(c_str!("child"));
    /// ```
    pub fn subdir(&self, name: &CStr) -> Self {
        Dir::create(name, Some(self))
    }

    /// Create a file in a DebugFS directory with the provided name, and contents from invoking
    /// [`Display::fmt`] on the provided reference.
    ///
    /// # Examples
    ///
    /// ```
    /// # use kernel::c_str;
    /// # use kernel::debugfs::Dir;
    /// let dir = Dir::new(c_str!("my_debugfs_dir"));
    /// dir.display_file(c_str!("foo"), &200);
    /// // "my_debugfs_dir/foo" now contains the number 200.
    /// ```
    pub fn display_file<T: Display + Sized + Sync>(&self, name: &CStr, data: &'static T) -> File {
        self.create_file(name, data)
    }

    /// Create a new directory in DebugFS at the root.
    ///
    /// # Examples
    ///
    /// ```
    /// # use kernel::c_str;
    /// # use kernel::debugfs::Dir;
    /// let debugfs = Dir::new(c_str!("parent"));
    /// ```
    pub fn new(name: &CStr) -> Self {
        Dir::create(name, None)
    }
}

/// Handle to a DebugFS file.
pub struct File {
    #[cfg(CONFIG_DEBUG_FS)]
    _entry: Entry,
}
