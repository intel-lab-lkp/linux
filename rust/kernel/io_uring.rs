// SPDX-License-Identifier: GPL-2.0
// SPDX-FileCopyrightText: (C) 2025 Furiosa AI

//! Abstractions for io-uring.
//!
//! This module provides types for implements io-uring interface for char device.
//!
//!
//! C headers: [`include/linux/io_uring/cmd.h`](srctree/include/linux/io_uring/cmd.h) and
//! [`include/linux/io_uring/io_uring.h`](srctree/include/linux/io_uring/io_uring.h)

use core::{mem::MaybeUninit, pin::Pin};

use crate::error::from_result;
use crate::transmute::{AsBytes, FromBytes};
use crate::{fs::File, types::Opaque};

use crate::prelude::*;

/// io-uring opcode
pub mod opcode {
    /// opcode for uring cmd
    pub const URING_CMD: u32 = bindings::io_uring_op_IORING_OP_URING_CMD;
}

/// A Rust abstraction for the Linux kernel's `io_uring_cmd` structure.
///
/// This structure is a safe, opaque wrapper around the raw C `io_uring_cmd`
/// binding from the Linux kernel. It represents a command structure used
/// in io_uring operations within the kernel.
/// This type is used internally by the io_uring subsystem to manage
/// asynchronous I/O commands.
///
/// This type should not be constructed or manipulated directly by
/// kernel module developers.
///
/// # INVARIANT
/// - `self.inner` always points to a valid, live `bindings::io_uring_cmd`.
#[repr(transparent)]
pub struct IoUringCmd {
    /// An opaque wrapper containing the actual `io_uring_cmd` data.
    inner: Opaque<bindings::io_uring_cmd>,
}

impl IoUringCmd {
    /// Returns the cmd_op with associated with the `io_uring_cmd`.
    #[inline]
    pub fn cmd_op(&self) -> u32 {
        // SAFETY: `self.inner` is guaranteed by the type invariant to point
        // to a live `io_uring_cmd`, so dereferencing is safe.
        unsafe { (*self.inner.get()).cmd_op }
    }

    /// Returns the flags with associated with the `io_uring_cmd`.
    #[inline]
    pub fn flags(&self) -> u32 {
        // SAFETY: `self.inner` is guaranteed by the type invariant to point
        // to a live `io_uring_cmd`, so dereferencing is safe.
        unsafe { (*self.inner.get()).flags }
    }

    /// Reads protocol data unit as `T` that impl `FromBytes` from uring cmd
    ///
    /// Fails with [`EFAULT`] if size of `T` is bigger than pdu size.
    #[inline]
    pub fn read_pdu<T: FromBytes>(&self) -> Result<T> {
        // SAFETY: `self.inner` is guaranteed by the type invariant to point
        // to a live `io_uring_cmd`, so dereferencing is safe.
        let inner = unsafe { &mut *self.inner.get() };

        let len = size_of::<T>();
        if len > inner.pdu.len() {
            return Err(EFAULT);
        }

        let mut out: MaybeUninit<T> = MaybeUninit::uninit();
        let ptr = &raw mut inner.pdu as *const c_void;

        // SAFETY:
        // * The `ptr` is valid pointer from `self.inner` that is guaranteed by type invariant.
        // * The `out` is valid pointer that points `T` which impls `FromBytes` and checked
        //   size of `T` is smaller than pdu size.
        unsafe {
            core::ptr::copy_nonoverlapping(ptr, out.as_mut_ptr().cast::<c_void>(), len);
        }

        // SAFETY: The read above has initialized all bytes in `out`, and since `T` implements
        // `FromBytes`, any bit-pattern is a valid value for this type.
        Ok(unsafe { out.assume_init() })
    }

    /// Writes the provided `value` to `pdu` in uring_cmd `self`
    ///
    /// Fails with [`EFAULT`] if size of `T` is bigger than pdu size.
    #[inline]
    pub fn write_pdu<T: AsBytes>(&mut self, value: &T) -> Result<()> {
        // SAFETY: `self.inner` is guaranteed by the type invariant to point
        // to a live `io_uring_cmd`, so dereferencing is safe.
        let inner = unsafe { &mut *self.inner.get() };

        let len = size_of::<T>();
        if len > inner.pdu.len() {
            return Err(EFAULT);
        }

        let src = (value as *const T).cast::<c_void>();
        let dst = &raw mut inner.pdu as *mut c_void;

        // SAFETY:
        // * The `src` is points valid memory that is guaranteed by `T` impls `AsBytes`
        // * The `dst` is valid. It's from `self.inner` that is guaranteed by type invariant.
        // * It's safe to copy because size of `T` is no more than len of pdu.
        unsafe {
            core::ptr::copy_nonoverlapping(src, dst, len);
        }

        Ok(())
    }

    /// Constructs a new [`IoUringCmd`] from a raw `io_uring_cmd`
    ///
    /// # Safety
    ///
    /// The caller must guarantee that:
    /// - `ptr` is non-null, properly aligned, and points to a valid
    ///   `bindings::io_uring_cmd`.
    /// - The pointed-to memory remains initialized and valid for the entire
    ///   lifetime `'a` of the returned reference.
    /// - While the returned `Pin<&'a mut IoUringCmd>` is alive, the underlying
    ///   object is **not moved** (pinning requirement).
    /// - **Aliasing rules:** the returned `&mut` has **exclusive** access to the same
    ///   object for its entire lifetime:
    ///   - No other `&mut` **or** `&` references to the same `io_uring_cmd` may be
    ///     alive at the same time.
    ///   - There must be no concurrent reads/writes through raw pointers, FFI, or
    ///     other kernel paths to the same object during this lifetime.
    ///   - If the object can be touched from other contexts (e.g. IRQ/another CPU),
    ///     the caller must provide synchronization to uphold this exclusivity.
    /// - This function relies on `IoUringCmd` being `repr(transparent)` over
    ///   `bindings::io_uring_cmd` so the cast preserves layout.
    #[inline]
    pub unsafe fn from_raw<'a>(ptr: *mut bindings::io_uring_cmd) -> Pin<&'a mut IoUringCmd> {
        // SAFETY:
        // * The caller guarantees that the pointer is not dangling and stays
        //   valid for the duration of 'a.
        // * The cast is okay because `IoUringCmd` is `repr(transparent)` and
        //   has the same memory layout as `bindings::io_uring_cmd`.
        // * The returned `Pin` ensures that the object cannot be moved, which
        //   is required because the kernel may hold pointers to this memory
        //   location and moving it would invalidate those pointers.
        unsafe { Pin::new_unchecked(&mut *ptr.cast()) }
    }

    /// Returns the file that referenced by uring cmd self.
    #[inline]
    pub fn file(&self) -> &File {
        // SAFETY: `self.inner` is guaranteed by the type invariant to point
        // to a live `io_uring_cmd`, so dereferencing is safe.
        let file = unsafe { (*self.inner.get()).file };

        // SAFETY:
        // * The `file` points valid file.
        // * refcount is positive after submission queue entry issued.
        // * There is no active fdget_pos region on the file on this thread.
        unsafe { File::from_raw_file(file) }
    }

    /// Returns an reference to the [`IoUringSqe`] associated with this command.
    #[inline]
    pub fn sqe(&self) -> &IoUringSqe {
        // SAFETY: `self.inner` is guaranteed by the type invariant to point
        // to a live `io_uring_cmd`, so dereferencing is safe.
        let sqe = unsafe { (*self.inner.get()).sqe };
        // SAFETY: The call guarantees that the `sqe` points valid io_uring_sqe.
        unsafe { IoUringSqe::from_raw(sqe) }
    }

    /// Completes an this [`IoUringCmd`] request that was previously queued.
    ///
    /// # Safety
    ///
    /// - This function must be called **only** for a command whose `uring_cmd`
    ///   handler previously returned **`-EIOCBQUEUED`** to io_uring.
    ///
    /// # Parameters
    ///
    /// - `ret`: Result to return to userspace.
    /// - `res2`: Extra for big completion queue entry `IORING_SETUP_CQE32`.
    /// - `issue_flags`: Flags associated with this request, typically the same
    ///   as those passed to the `uring_cmd` handler.
    #[inline]
    pub fn done(self: Pin<&mut IoUringCmd>, ret: Result<i32>, res2: u64, issue_flags: u32) {
        let ret = from_result(|| ret) as isize;
        // SAFETY: The call guarantees that `self.inner` is not dangling and stays valid
        unsafe {
            bindings::io_uring_cmd_done(self.inner.get(), ret, res2, issue_flags);
        }
    }
}

/// A Rust abstraction for the Linux kernel's `io_uring_sqe` structure.
///
/// This structure is a safe, opaque wrapper around the raw C [`io_uring_sqe`](srctree/include/uapi/linux/io_uring.h)
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
/// Users can obtain this type from [`IoUringCmd::sqe()`] method, which
/// extracts the submission queue entry associated with a command.
///
/// This type should not be constructed or manipulated directly by
/// kernel module developers.
///
/// # INVARIANT
/// - `self.inner` always points to a valid, live `bindings::io_uring_sqe`.
#[repr(transparent)]
pub struct IoUringSqe {
    inner: Opaque<bindings::io_uring_sqe>,
}

impl IoUringSqe {
    /// Reads and interprets the `cmd` field of an `bindings::io_uring_sqe` as a value of type `T`.
    ///
    /// # Safety & Invariants
    /// - Construction of `T` is delegated to `FromBytes`, which guarantees that `T` has no
    ///   invalid bit patterns and can be safely reconstructed from raw bytes.
    /// - **Limitation:** This implementation does not support `IORING_SETUP_SQE128` (larger SQE entries).
    ///   Only the standard `io_uring_sqe` layout is handled here.
    ///
    /// # Errors
    /// * Returns `EINVAL` if the `self` does not hold a `opcode::URING_CMD`.
    /// * Returns `EFAULT` if the command buffer is smaller than the requested type `T`.
    ///
    /// # Returns
    /// * On success, returns a `T` deserialized from the `cmd`.
    /// * On failure, returns an appropriate error as described above.
    pub fn cmd_data<T: FromBytes>(&self) -> Result<T> {
        // SAFETY: `self.inner` guaranteed by the type invariant to point
        // to a live `io_uring_sqe`, so dereferencing is safe.
        let sqe = unsafe { &*self.inner.get() };

        if u32::from(sqe.opcode) != opcode::URING_CMD {
            return Err(EINVAL);
        }

        // SAFETY: Accessing the `sqe.cmd` union field is safe because we've
        // verified that `sqe.opcode == IORING_OP_URING_CMD`, which guarantees
        // that this union variant is initialized and valid.
        let cmd = unsafe { sqe.__bindgen_anon_6.cmd.as_ref() };
        let cmd_len = size_of_val(&sqe.__bindgen_anon_6.bindgen_union_field);

        if cmd_len < size_of::<T>() {
            return Err(EFAULT);
        }

        let cmd_ptr = cmd.as_ptr() as *mut T;

        // SAFETY: `cmd_ptr` is valid from `self.inner` which is guaranteed by
        // type variant. And also it points to initialized `T` from userspace.
        let ret = unsafe { core::ptr::read_unaligned(cmd_ptr) };

        Ok(ret)
    }

    /// Constructs a new `IoUringSqe` from a raw `io_uring_sqe`.
    ///
    /// # Safety
    ///
    /// The caller must guarantee that:
    /// - `ptr` is non-null, properly aligned, and points to a valid initialized
    ///   `bindings::io_uring_sqe`.
    /// - The pointed-to memory remains valid (not freed or repurposed) for the
    ///   entire lifetime `'a` of the returned reference.
    /// - **Aliasing rules (for `&T`):** while the returned `&'a IoUringSqe` is
    ///   alive, there must be **no mutable access** to the same object through any
    ///   path (no `&mut`, no raw-pointer writes, no FFI/IRQ/other-CPU writers).
    ///   Multiple `&` is fine **only if all of them are read-only** for the entire
    ///   overlapping lifetime.
    /// - This relies on `IoUringSqe` being `repr(transparent)` over
    ///   `bindings::io_uring_sqe`, so the cast preserves layout.
    #[inline]
    pub unsafe fn from_raw<'a>(ptr: *const bindings::io_uring_sqe) -> &'a IoUringSqe {
        // SAFETY: The caller guarantees that the pointer is not dangling and stays valid for the
        // duration of 'a. The cast is okay because `IoUringSqe` is `repr(transparent)` and has the
        // same memory layout as `bindings::io_uring_sqe`.
        unsafe { &*ptr.cast() }
    }
}
