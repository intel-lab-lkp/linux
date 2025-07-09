// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Google LLC.

//! DebugFS Abstraction
//!
//! C header: [`include/linux/debugfs.h`](srctree/include/linux/debugfs.h)

use crate::prelude::*;
use crate::str::CStr;
#[cfg(CONFIG_DEBUG_FS)]
use crate::sync::Arc;
use core::fmt;
use core::fmt::Display;
use core::marker::PhantomPinned;
use core::ops::Deref;

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
    /// Creates a DebugFS file which will own the data produced by the initializer provided in
    /// `data`.
    ///
    /// # Safety
    ///
    /// The provided vtable must be appropriate for implementing a seq_file if provided
    /// with a private data pointer which provides shared access to a `T`.
    unsafe fn create_file<'a, T: Sync, E, TI: PinInit<T, E>>(
        &self,
        name: &'a CStr,
        data: TI,
        vtable: &'static bindings::file_operations,
    ) -> impl PinInit<File<T>, E> + use<'_, 'a, T, E, TI> {
        try_pin_init! {
            File {
                _entry: Entry::empty(),
                data <- data,
                _pin: PhantomPinned,
            } ? E
        }
        .pin_chain(|file| {
            let Some(parent) = &self.0 else {
                return Ok(());
            };

            // SAFETY:
            // * `name` is a NUL-terminated C string, living across the call, by `CStr` invariant.
            // * `parent` is a live `dentry` since we have a reference to it.
            // * Since the file owns the `T` and it is pinned, we can safely assume the pointer
            //   lives and is valid as long as we are.
            // * Since the `Entry` will live in the `File`, it will be dropped before the pointer
            //   is invalidated. Dropping the `Entry` will remove the DebugFS file and avoid
            //   further access.
            let ptr = unsafe {
                bindings::debugfs_create_file_full(
                    name.as_char_ptr(),
                    0o444,
                    parent.as_ptr(),
                    &file.data as *const _ as *mut c_void,
                    core::ptr::null(),
                    vtable,
                )
            };

            // SAFETY: `debugfs_create_file_full` either returns an error code or a legal
            // dentry pointer, so `Entry::new` is safe to call here.
            *file.entry_mut() = unsafe { Entry::new(ptr, Some(parent.clone())) };

            Ok(())
        })
    }

    #[cfg(not(CONFIG_DEBUG_FS))]
    /// Creates a DebugFS file which will own the data produced by the initializer provided in
    /// `data`.
    ///
    /// # Safety
    ///
    /// As DebugFS is disabled, this is actually entirely safe. It is marked unsafe for code
    /// compatibility with the DebugFS-enabled variant.
    unsafe fn create_file<'a, T: Sync, E, TI: PinInit<T, E>>(
        &self,
        _name: &'a CStr,
        data: TI,
        _vtable: (),
    ) -> impl PinInit<File<T>, E> + use<'_, 'a, T, E, TI> {
        try_pin_init! {
            File {
                data <- data,
                _pin: PhantomPinned,
            } ? E
        }
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
    ///
    /// ```
    /// # use kernel::c_str;
    /// # use kernel::debugfs::Dir;
    /// # use kernel::prelude::*;
    /// let val = KBox::new(300, GFP_KERNEL)?;
    /// let dir = Dir::new(c_str!("my_debugfs_dir"));
    /// dir.display_file(c_str!("foo"), val);
    /// // "my_debugfs_dir/foo" now contains the number 300.
    /// # Ok::<(), Error>(())
    /// ```
    pub fn display_file<'b, T: Display + Send + Sync, E, TI: PinInit<T, E>>(
        &self,
        name: &'b CStr,
        data: TI,
    ) -> impl PinInit<File<T>, E> + use<'_, 'b, T, E, TI> {
        #[cfg(CONFIG_DEBUG_FS)]
        let vtable = &<T as display_file::DisplayFile>::VTABLE;
        #[cfg(not(CONFIG_DEBUG_FS))]
        let vtable = ();

        // SAFETY: `vtable` is all stock `seq_file` implementations except for `open`.
        // `open`'s only requirement beyond what is provided to all open functions is that the
        // inode's data pointer must point to a `T` that will outlive it, which is provided by
        // `create_file`'s safety requirements.
        unsafe { self.create_file(name, data, vtable) }
    }

    /// Create a file in a DebugFS directory with the provided name, and contents from invoking `f`
    /// on the provided reference.
    ///
    /// `f` must be a function item or a non-capturing closure, or this will fail to compile.
    ///
    /// # Examples
    ///
    /// ```
    /// # use core::sync::atomic::{AtomicU32, Ordering};
    /// # use kernel::c_str;
    /// # use kernel::debugfs::Dir;
    /// let dir = Dir::new(c_str!("foo"));
    /// static MY_ATOMIC: AtomicU32 = AtomicU32::new(3);
    /// let file = dir.fmt_file(c_str!("bar"), &MY_ATOMIC, &|val, f| {
    ///   let out = val.load(Ordering::Relaxed);
    ///   writeln!(f, "{out:#010x}")
    /// });
    /// MY_ATOMIC.store(10, Ordering::Relaxed);
    /// ```
    pub fn fmt_file<
        'b,
        T: Send + Sync,
        E,
        TI: PinInit<T, E>,
        F: Fn(&T, &mut fmt::Formatter<'_>) -> fmt::Result + Send + Sync,
    >(
        &self,
        name: &'b CStr,
        data: TI,
        _f: &'static F,
    ) -> impl PinInit<File<T>, E> + use<'_, 'b, T, TI, E, F> {
        #[cfg(CONFIG_DEBUG_FS)]
        let vtable = &<display_file::FormatAdapter<T, F> as display_file::DisplayFile>::VTABLE;
        #[cfg(not(CONFIG_DEBUG_FS))]
        let vtable = ();

        // SAFETY: `vtable` is all stock `seq_file` implementations except for `open`.
        // `open`'s only requirement beyond what is provided to all open functions is that the
        // inode's data pointer must point to a `FormatAdapter<T, F>` that will outlive it.
        // `create_file`'s safety requirements provide the lifetime aspect of this, but we are
        // using a private `T` pointer. This is legal because:
        // 1. `FormatAdapter<T, F>` is a `#[repr(transparent)]` wrapper around `T`, so the
        //    implicit transmute is legal.
        // 2. The invariant in `FormatAdapter` that `F` is inhabited is upheld because we have
        //    `_f`, so constructing a `FormatAdapter<T, F> is legal.
        unsafe { self.create_file(name, data, vtable) }
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
#[pin_data]
pub struct File<T> {
    // This order is load-bearing for drops - `_entry` must be dropped before `data`.
    #[cfg(CONFIG_DEBUG_FS)]
    _entry: Entry,
    #[pin]
    data: T,
    // Even if `T` is `Unpin`, we still can't allow it to be moved.
    #[pin]
    _pin: PhantomPinned,
}

#[cfg(CONFIG_DEBUG_FS)]
impl<T> File<T> {
    fn entry_mut(self: Pin<&mut Self>) -> &mut Entry {
        // SAFETY: _entry is not structurally pinned
        unsafe { &mut Pin::into_inner_unchecked(self)._entry }
    }
}

impl<T> Deref for File<T> {
    type Target = T;
    fn deref(&self) -> &T {
        &self.data
    }
}
