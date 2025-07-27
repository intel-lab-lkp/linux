// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2025 Furiosa AI.

//! IoUring command and submission queue entry abstractions.
//!
//! C headers: [`include/linux/io_uring/cmd.h`](srctree/include/linux/io_uring/cmd.h) and
//! [`include/linux/io_uring/io_uring.h`](srctree/include/linux/io_uring/io_uring.h)

use core::{mem::MaybeUninit, pin::Pin, ptr::addr_of_mut};

use crate::{fs::File, types::Opaque};

/// A Rust abstraction for the Linux kernel's `io_uring_cmd` structure.
///
/// This structure is a safe, opaque wrapper around the raw C `io_uring_cmd`
/// binding from the Linux kernel. It represents a command structure used
/// in io_uring operations within the kernel.
///
/// # Type Safety
///
/// The `#[repr(transparent)]` attribute ensures that this wrapper has
/// the same memory layout as the underlying `io_uring_cmd` structure,
/// allowing it to be safely transmuted between the two representations.
///
/// # Fields
///
/// * `inner` - An opaque wrapper containing the actual `io_uring_cmd` data.
///             The `Opaque` type prevents direct access to the internal
///             structure fields, ensuring memory safety and encapsulation.
///
/// # Usage
///
/// This type is used internally by the io_uring subsystem to manage
/// asynchronous I/O commands. It is typically accessed through a pinned
/// mutable reference: `Pin<&mut IoUringCmd>`. The pinning ensures that
/// the structure remains at a fixed memory location, which is required
/// for safe interaction with the kernel's io_uring infrastructure.
///
/// Users typically receive this type as an argument in the `file_operations::uring_cmd()`
/// callback function, where they can access and manipulate the io_uring command
/// data for custom file operations.
///
/// This type should not be constructed or manipulated directly by
/// kernel module developers.
#[repr(transparent)]
pub struct IoUringCmd {
    inner: Opaque<bindings::io_uring_cmd>,
}

impl IoUringCmd {
    /// Returns the cmd_op with associated with the io_uring_cmd.
    #[inline]
    pub fn cmd_op(&self) -> u32 {
        // SAFETY: The call guarantees that `self.inner` is not dangling and stays valid
        unsafe { (*self.inner.get()).cmd_op }
    }

    /// Returns the flags with associated with the io_uring_cmd.
    #[inline]
    pub fn flags(&self) -> u32 {
        // SAFETY: The call guarantees that `self.inner` is not dangling and stays valid
        unsafe { (*self.inner.get()).flags }
    }

    /// Returns the ref pdu for free use.
    #[inline]
    pub fn pdu(&mut self) -> &mut MaybeUninit<[u8; 32]> {
        // SAFETY: The call guarantees that `self.inner` is not dangling and stays valid
        let inner = unsafe { &mut *self.inner.get() };
        let ptr = addr_of_mut!(inner.pdu) as *mut MaybeUninit<[u8; 32]>;

        // SAFETY: The call guarantees that `self.inner` is not dangling and stays valid
        unsafe { &mut *ptr }
    }

    /// Constructs a new `IoUringCmd` from a raw `io_uring_cmd`
    ///
    /// # Safety
    ///
    /// The caller must guarantee that:
    /// - The pointer `ptr` is not null and points to a valid `bindings::io_uring_cmd`.
    /// - The memory pointed to by `ptr` remains valid for the duration of the returned reference's lifetime `'a`.
    /// - The memory will not be moved or freed while the returned `Pin<&mut IoUringCmd>` is alive.
    #[inline]
    pub unsafe fn from_raw<'a>(ptr: *mut bindings::io_uring_cmd) -> Pin<&'a mut IoUringCmd> {
        // SAFETY: The caller guarantees that the pointer is not dangling and stays valid for the
        // duration of 'a. The cast is okay because `IoUringCmd` is `repr(transparent)` and has the
        // same memory layout as `bindings::io_uring_cmd`. The returned `Pin` ensures that the object
        // cannot be moved, which is required because the kernel may hold pointers to this memory
        // location and moving it would invalidate those pointers.
        unsafe { Pin::new_unchecked(&mut *ptr.cast()) }
    }

    /// Returns the file that referenced by uring cmd self.
    #[inline]
    pub fn file(&self) -> &File {
        // SAFETY: The call guarantees that the `self.inner` is not dangling and stays valid
        let file = unsafe { (*self.inner.get()).file };
        // SAFETY: The call guarantees that `file` points valid file.
        unsafe { File::from_raw_file(file) }
    }

    /// Returns a reference to the uring cmd's SQE.
    #[inline]
    pub fn sqe(&self) -> &IoUringSqe {
        // SAFETY: The call guarantees that the `self.inner` is not dangling and stays valid
        let sqe = unsafe { (*self.inner.get()).sqe };
        // SAFETY: The call guarantees that the `sqe` points valid io_uring_sqe.
        unsafe { IoUringSqe::from_raw(sqe) }
    }

    /// Called by consumers of io_uring_cmd, if they originally returned -EIOCBQUEUED upon receiving the command
    #[inline]
    pub fn done(self: Pin<&mut IoUringCmd>, ret: isize, res2: u64, issue_flags: u32) {
        // SAFETY: The call guarantees that `self.inner` is not dangling and stays valid
        unsafe {
            bindings::io_uring_cmd_done(self.inner.get(), ret, res2, issue_flags);
        }
    }
}

/// A Rust abstraction for the Linux kernel's `io_uring_sqe` structure.
///
/// This structure is a safe, opaque wrapper around the raw C `io_uring_sqe`
/// binding from the Linux kernel. It represents a Submission Queue Entry
/// used in io_uring operations within the kernel.
///
/// # Type Safety
///
/// The `#[repr(transparent)]` attribute ensures that this wrapper has
/// the same memory layout as the underlying `io_uring_sqe` structure,
/// allowing it to be safely transmuted between the two representations.
///
/// # Fields
///
/// * `inner` - An opaque wrapper containing the actual `io_uring_sqe` data.
///             The `Opaque` type prevents direct access to the internal
///             structure fields, ensuring memory safety and encapsulation.
///
/// # Usage
///
/// This type represents a submission queue entry that describes an I/O
/// operation to be executed by the io_uring subsystem. It contains
/// information such as the operation type, file descriptor, buffer
/// pointers, and other operation-specific data.
///
/// Users can obtain this type from `IoUringCmd::sqe()` method, which
/// extracts the submission queue entry associated with a command.
///
/// This type should not be constructed or manipulated directly by
/// kernel module developers.
#[repr(transparent)]
pub struct IoUringSqe {
    inner: Opaque<bindings::io_uring_sqe>,
}

impl<'a> IoUringSqe {
    /// Returns the cmd_data with associated with the io_uring_sqe.
    /// This function returns 16 byte array. We don't support IORING_SETUP_SQE128 for now.
    pub fn cmd_data(&'a self) -> &'a [Opaque<u8>] {
        // SAFETY: The call guarantees that `self.inner` is not dangling and stays valid
        let cmd = unsafe { (*self.inner.get()).__bindgen_anon_6.cmd.as_ref() };

        // SAFETY: The call guarantees that `cmd` is not dangling and stays valid
        unsafe { core::slice::from_raw_parts(cmd.as_ptr() as *const Opaque<u8>, 16) }
    }

    /// Constructs a new `IoUringSqe` from a raw `io_uring_sqe`
    ///
    /// # Safety
    ///
    /// The caller must guarantee that:
    /// - The pointer `ptr` is not null and points to a valid `bindings::io_uring_sqe`.
    /// - The memory pointed to by `ptr` remains valid for the duration of the returned reference's lifetime `'a`.
    #[inline]
    pub unsafe fn from_raw(ptr: *const bindings::io_uring_sqe) -> &'a IoUringSqe {
        // SAFETY: The caller guarantees that the pointer is not dangling and stays valid for the
        // duration of 'a. The cast is okay because `IoUringSqe` is `repr(transparent)` and has the
        // same memory layout as `bindings::io_uring_sqe`.
        unsafe { &*ptr.cast() }
    }
}
