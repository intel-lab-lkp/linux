// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Google LLC.

//! DebugFS Abstraction
//!
//! C header: [`include/linux/debugfs.h`](srctree/include/linux/debugfs.h)

// When DebugFS is disabled, many parameters are dead. Linting for this isn't helpful.
#![cfg_attr(not(CONFIG_DEBUG_FS), allow(unused_variables))]

use crate::prelude::*;
use crate::str::CStr;
#[cfg(CONFIG_DEBUG_FS)]
use crate::sync::Arc;
use core::marker::PhantomPinned;
use core::ops::Deref;

mod traits;
pub use traits::Render;

mod file_ops;
use file_ops::{FileOps, ReadFile};
#[cfg(CONFIG_DEBUG_FS)]
mod entry;
#[cfg(CONFIG_DEBUG_FS)]
use entry::Entry;

/// Owning handle to a DebugFS directory.
///
/// This directory will be removed when this handle has been dropped *and* all children have been
/// removed.
// We hold a reference to our parent if it exists in the `Entry` to prevent the dentry we point
// to from being cleaned up when our parent is removed.
//
// The `None` option indicates that the `Arc` could not be allocated, so our children would not be
// able to refer to us. In this case, we need to silently fail. All future child directories/files
// will silently fail as well.
#[derive(Clone)]
pub struct Dir(#[cfg(CONFIG_DEBUG_FS)] Option<Arc<Entry>>);

impl Dir {
    /// Create a new directory in DebugFS. If `parent` is [`None`], it will be created at the root.
    fn create(name: &CStr, parent: Option<&Dir>) -> Self {
        #[cfg(CONFIG_DEBUG_FS)]
        {
            let parent_entry = match parent {
                // If the parent couldn't be allocated, just early-return
                Some(Dir(None)) => return Self(None),
                Some(Dir(Some(entry))) => Some(entry.clone()),
                None => None,
            };
            Self(
                // If Arc creation fails, the `Entry` will be dropped, so the directory will be
                // cleaned up.
                Arc::new(Entry::dynamic_dir(name, parent_entry), GFP_KERNEL).ok(),
            )
        }
        #[cfg(not(CONFIG_DEBUG_FS))]
        Self()
    }

    /// Creates a DebugFS file which will own the data produced by the initializer provided in
    /// `data`.
    fn create_file<'a, T: Sync + 'static, E: 'a, TI: PinInit<T, E> + 'a>(
        &'a self,
        name: &'a CStr,
        data: TI,
        file_ops: &'static FileOps<T>,
    ) -> impl PinInit<File<T>, E> + 'a {
        let scope = Scope::<T>::new(data, move |data| {
            #[cfg(CONFIG_DEBUG_FS)]
            if let Some(parent) = &self.0 {
                // SAFETY: Because data derives from a scope, and our entry will be dropped before
                // the data is dropped, it is guaranteed to outlive the entry we return.
                unsafe { Entry::dynamic_file(name, parent.clone(), data, file_ops) }
            } else {
                Entry::empty()
            }
        });
        try_pin_init! {
            File {
                scope <- scope
            } ? E
        }
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

    /// Creates a subdirectory within this directory.
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

    /// Creates a read-only file in this directory.
    ///
    /// The file's contents are produced by invoking [`Render::render`] on the value initialized by
    /// `data`.
    ///
    /// # Examples
    ///
    /// ```
    /// # use kernel::c_str;
    /// # use kernel::debugfs::Dir;
    /// # use kernel::prelude::*;
    /// # let dir = Dir::new(c_str!("my_debugfs_dir"));
    /// let file = KBox::pin_init(dir.read_only_file(c_str!("foo"), 200), GFP_KERNEL)?;
    /// // "my_debugfs_dir/foo" now contains the number 200.
    /// // The file is removed when `file` is dropped.
    /// # Ok::<(), Error>(())
    /// ```
    pub fn read_only_file<'a, T: Render + Send + Sync + 'static, E: 'a, TI: PinInit<T, E> + 'a>(
        &'a self,
        name: &'a CStr,
        data: TI,
    ) -> impl PinInit<File<T>, E> + 'a {
        let file_ops = &<T as ReadFile<_>>::FILE_OPS;
        self.create_file(name, data, file_ops)
    }
}

#[pin_data]
/// Handle to a DebugFS scope, which allows a variety of DebugFS files/directories to hang off a
/// single structure.
pub struct Scope<T> {
    // This order is load-bearing for drops - `_entry` must be dropped before `data`.
    #[cfg(CONFIG_DEBUG_FS)]
    _entry: Entry,
    #[pin]
    data: T,
    // Even if `T` is `Unpin`, we still can't allow it to be moved.
    #[pin]
    _pin: PhantomPinned,
}

#[pin_data]
/// Handle to a DebugFS file, owning its backing data.
///
/// When dropped, the DebugFS file will be removed and the attached data will be dropped.
pub struct File<T> {
    #[pin]
    scope: Scope<T>,
}

#[cfg(not(CONFIG_DEBUG_FS))]
impl<T> Scope<T> {
    fn new<E, TI: PinInit<T, E>, F: for<'a> FnOnce(&'a T)>(
        data: TI,
        init: F,
    ) -> impl PinInit<Self, E> {
        try_pin_init! {
            Self {
                data <- data,
                _pin: PhantomPinned
            } ? E
        }
        .pin_chain(|scope| {
            init(&scope.data);
            Ok(())
        })
    }
}

#[cfg(CONFIG_DEBUG_FS)]
impl<T> Scope<T> {
    fn entry_mut(self: Pin<&mut Self>) -> &mut Entry {
        // SAFETY: _entry is not structurally pinned
        unsafe { &mut Pin::into_inner_unchecked(self)._entry }
    }
    fn new<'b, E: 'b, TI: PinInit<T, E> + 'b, F: for<'a> FnOnce(&'a T) -> Entry + 'b>(
        data: TI,
        init: F,
    ) -> impl PinInit<Self, E> + 'b
    where
        T: 'b,
    {
        try_pin_init! {
            Self {
                _entry: Entry::empty(),
                data <- data,
                _pin: PhantomPinned
            } ? E
        }
        .pin_chain(|scope| {
            *scope.entry_mut() = init(&scope.data);
            Ok(())
        })
    }
}

impl<T> Deref for Scope<T> {
    type Target = T;
    fn deref(&self) -> &T {
        &self.data
    }
}

impl<T> Deref for File<T> {
    type Target = T;
    fn deref(&self) -> &T {
        &self.scope
    }
}
