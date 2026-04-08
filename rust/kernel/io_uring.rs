// SPDX-License-Identifier: GPL-2.0
// SPDX-FileCopyrightText: (C) 2025 Furiosa AI

//! Abstractions for io-uring.
//!
//! This module provides abstractions for the io-uring interface for character devices.
//!
//!
//! C headers: [`include/linux/io_uring/cmd.h`](srctree/include/linux/io_uring/cmd.h) and
//! [`include/linux/io_uring/io_uring.h`](srctree/include/linux/io_uring/io_uring.h)

use core::ptr::NonNull;

use crate::error::from_result;
use crate::transmute::{AsBytes, FromBytes};
use crate::{fs::File, types::Opaque};

use crate::prelude::*;

/// Size in bytes of the protocol data unit (PDU) embedded in `io_uring_cmd`.
///
/// Matches the size of the `pdu` field in `struct io_uring_cmd` as defined in
/// `include/linux/io_uring/cmd.h`.
pub(crate) const PDU_SIZE: usize = 32;

/// Opcode of an [`IoUringSqe`].
///
/// Each submission queue entry in io_uring specifies an operation
/// to perform, such as read, write, or a driver-specific `URING_CMD`.
#[repr(transparent)]
#[derive(PartialEq)]
pub struct Opcode(u8);

impl Opcode {
    /// Driver-specific passthrough command.
    pub const URING_CMD: Self = Self(bindings::io_uring_op_IORING_OP_URING_CMD as u8);
}

/// A fresh `io_uring_cmd` received from the driver callback.
///
/// Represents a submission received from userspace via `IORING_OP_URING_CMD`.
/// A driver obtains this from the `uring_cmd` callback in [`crate::miscdevice::MiscDevice`].
///
/// The driver must either complete the command synchronously by calling
/// [`Self::complete`], or queue it for asynchronous completion by calling
/// [`Self::queue`], which yields a [`QueuedIoUringCmd`] handle.
///
/// # Invariants
///
/// `self.inner` is non-null, properly aligned, and points to a valid, live
/// `bindings::io_uring_cmd` for the duration of the driver callback.
pub struct IoUringCmd {
    inner: NonNull<bindings::io_uring_cmd>,
}

// SAFETY: `io_uring_cmd` is a kernel-allocated structure.  The kernel
// guarantees that it remains alive until the driver either returns a
// non-`EIOCBQUEUED` result or calls `io_uring_cmd_done32()`.  Moving the
// pointer to another thread is safe: the kernel object is not tied to any
// particular CPU or task context.
unsafe impl Send for IoUringCmd {}

// SAFETY: All `&self` methods on `IoUringCmd` only read from the underlying
// `io_uring_cmd` (cmd_op, flags, sqe, file).  `write_pdu` takes `&mut self`,
// so the borrow checker prevents concurrent mutable access.  Sharing
// `&IoUringCmd` across threads is therefore safe.
unsafe impl Sync for IoUringCmd {}

/// An [`IoUringCmd`] that has been queued for asynchronous completion.
///
/// The only way to obtain a `QueuedIoUringCmd` is through [`IoUringCmd::queue`],
/// which ensures the command was properly handed off to the async path before
/// [`UringCmdAction::Queued`] is returned to the vtable.
///
/// Call [`Self::done`] exactly once to post the completion to userspace.
///
/// # Invariants
///
/// `self.inner` is non-null, properly aligned, and points to a valid, live
/// `bindings::io_uring_cmd` until [`Self::done`] is called.
pub struct QueuedIoUringCmd {
    inner: NonNull<bindings::io_uring_cmd>,
}

// SAFETY: Same reasoning as for `IoUringCmd`. After `queue()`, the handle is
// intentionally moved to a different context (e.g. a workqueue) to call
// `done()` later.
unsafe impl Send for QueuedIoUringCmd {}

// SAFETY: All `&self` methods on `QueuedIoUringCmd` only read from the
// underlying `io_uring_cmd`.
unsafe impl Sync for QueuedIoUringCmd {}

/// Proof that a `uring_cmd` request completed synchronously.
pub struct CompleteAction {
    ret: i32,
}

impl CompleteAction {
    /// Returns the userspace result for this synchronous completion.
    #[inline]
    pub fn ret(&self) -> i32 {
        self.ret
    }
}

/// Proof that a `uring_cmd` request was queued for asynchronous completion.
///
/// This type has a private field and can only be constructed inside this module,
/// so it can only be obtained through [`IoUringCmd::queue`].
pub struct QueuedAction {
    _private: (),
}

/// Completion mode for `uring_cmd`.
pub enum UringCmdAction {
    /// Request is completed synchronously and returns this result to userspace.
    Complete(CompleteAction),
    /// Request is queued for asynchronous completion.
    ///
    /// This variant can only be constructed by calling [`IoUringCmd::queue`],
    /// which enforces that the caller holds a [`QueuedIoUringCmd`] handle and
    /// will eventually call [`QueuedIoUringCmd::done`].
    Queued(QueuedAction),
}

impl IoUringCmd {
    /// Returns the `cmd_op` associated with this command.
    #[inline]
    pub fn cmd_op(&self) -> u32 {
        // SAFETY: `self.inner` is guaranteed by the type invariant to point
        // to a live `io_uring_cmd`, so dereferencing is safe.
        unsafe { (*self.as_raw()).cmd_op }
    }

    /// Returns the flags field of this command.
    ///
    /// The returned value is `io_uring_cmd.flags`, which is a combination of:
    /// - User-set flags from `sqe->uring_cmd_flags` (bits 0–1):
    ///   `IORING_URING_CMD_FIXED`, `IORING_URING_CMD_MULTISHOT`.
    /// - Kernel-set flags (bits 30–31):
    ///   `IORING_URING_CMD_CANCELABLE`, `IORING_URING_CMD_REISSUE`.
    ///
    /// Note: this is **not** the `issue_flags` parameter passed to the
    /// `uring_cmd` callback, which carries `IO_URING_F_*` flags such as
    /// `IO_URING_F_NONBLOCK`.
    #[inline]
    pub fn flags(&self) -> u32 {
        // SAFETY: `self.inner` is guaranteed by the type invariant to point
        // to a live `io_uring_cmd`, so dereferencing is safe.
        unsafe { (*self.as_raw()).flags }
    }

    /// Reads the protocol data unit (PDU) as a value of type `T`.
    ///
    /// # Errors
    ///
    /// Returns [`EINVAL`] if `size_of::<T>()` exceeds the PDU size.
    #[inline]
    pub fn read_pdu<T: FromBytes>(&self) -> Result<T> {
        // SAFETY: `self.inner` is guaranteed by the type invariant to point
        // to a live `io_uring_cmd`, so dereferencing is safe.
        let inner = unsafe { &*self.inner.as_ref() };

        if size_of::<T>() > inner.pdu.len() {
            return Err(EINVAL);
        }

        let ptr = inner.pdu.as_ptr() as *const T;

        // SAFETY: `ptr` is a valid pointer derived from `self.inner`, which
        // is guaranteed by the type invariant. `size_of::<T>()` bytes are
        // available in the PDU (checked above). `read_unaligned` is used
        // because the PDU is a byte array and may not satisfy `T`'s alignment.
        // `T: FromBytes` guarantees that every bit-pattern is a valid value.
        Ok(unsafe { core::ptr::read_unaligned(ptr) })
    }

    /// Writes `value` to the PDU of this command.
    ///
    /// # Errors
    ///
    /// Returns [`EINVAL`] if `size_of::<T>()` exceeds the PDU size.
    #[inline]
    pub fn write_pdu<T: AsBytes>(&mut self, value: &T) -> Result<()> {
        // SAFETY: `self.inner` is guaranteed by the type invariant to point
        // to a live `io_uring_cmd`, so dereferencing is safe.
        let inner = unsafe { self.inner.as_mut() };

        let len = size_of::<T>();
        if len > inner.pdu.len() {
            return Err(EINVAL);
        }

        let src = (value as *const T).cast::<u8>();
        let dst = &raw mut inner.pdu as *mut u8;

        // SAFETY:
        // * `src` points to valid memory because `T: AsBytes`.
        // * `dst` is valid and derived from `self.inner`, which is guaranteed
        //   by the type invariant.
        // * The byte count does not exceed the PDU length (checked above).
        unsafe {
            core::ptr::copy_nonoverlapping(src, dst, len);
        }

        Ok(())
    }

    /// Constructs an [`IoUringCmd`] from a raw pointer.
    ///
    /// # Safety
    ///
    /// The caller must guarantee that:
    /// - `ptr` is non-null, properly aligned, and points to a valid, initialised
    ///   `bindings::io_uring_cmd`.
    /// - The pointed-to object remains alive until the driver either returns a
    ///   non-`EIOCBQUEUED` value or calls [`QueuedIoUringCmd::done`].
    /// - No other mutable reference to the same object exists for the duration
    ///   of the returned handle's lifetime.
    #[inline]
    pub(crate) unsafe fn from_raw(ptr: *mut bindings::io_uring_cmd) -> Result<Self> {
        let Some(inner) = NonNull::new(ptr) else {
            return Err(EINVAL);
        };

        Ok(Self { inner })
    }

    /// Returns a raw pointer to the underlying `io_uring_cmd`.
    #[inline]
    fn as_raw(&self) -> *mut bindings::io_uring_cmd {
        self.inner.as_ptr()
    }

    /// Returns the file associated with this command.
    ///
    /// The returned reference is valid for the lifetime of `&self`.  The kernel
    /// holds a reference to the file for the entire lifetime of the enclosing
    /// `io_kiocb`, so this is safe to call at any point while `IoUringCmd` is
    /// alive.
    #[inline]
    pub fn file(&self) -> &File {
        // SAFETY: `self.inner` is guaranteed by the type invariant to point
        // to a live `io_uring_cmd`, so dereferencing is safe.
        let file = unsafe { (*self.as_raw()).file };

        // SAFETY:
        // * The `io_kiocb` holds a reference to the file for its entire
        //   lifetime, so `file` is valid and has a positive refcount.
        // * There is no active fdget_pos region on the file on this thread.
        unsafe { File::from_raw_file(file) }
    }

    /// Returns a reference to the [`IoUringSqe`] associated with this command.
    #[inline]
    pub fn sqe(&self) -> &IoUringSqe {
        // SAFETY: `self.inner` is guaranteed by the type invariant to point
        // to a live `io_uring_cmd`, so dereferencing is safe.
        let sqe = unsafe { self.inner.as_ref().sqe };
        // SAFETY: `sqe` is a valid pointer set by the io_uring core during
        // submission queue entry preparation and remains valid for the lifetime
        // of the `io_uring_cmd`.
        unsafe { IoUringSqe::from_raw(sqe) }
    }

    /// Marks this command as completed synchronously with the provided return value.
    ///
    /// The vtable will return `ret` directly to the io_uring core, which posts
    /// the completion queue entry.  No further action is needed from the driver.
    #[inline]
    pub fn complete(self, ret: i32) -> UringCmdAction {
        UringCmdAction::Complete(CompleteAction { ret })
    }

    /// Queues this command for asynchronous completion.
    ///
    /// Returns a [`UringCmdAction::Queued`] token to return from the driver
    /// callback and a [`QueuedIoUringCmd`] handle that must be used to call
    /// [`QueuedIoUringCmd::done`] at a later point.
    ///
    /// Because [`QueuedAction`] has a private field, [`UringCmdAction::Queued`]
    /// can **only** be constructed through this method.  This prevents a driver
    /// from accidentally returning `Queued` after already completing the command
    /// via `done()`.
    #[inline]
    pub fn queue(self) -> (UringCmdAction, QueuedIoUringCmd) {
        let queued = QueuedIoUringCmd { inner: self.inner };
        (UringCmdAction::Queued(QueuedAction { _private: () }), queued)
    }
}

impl QueuedIoUringCmd {
    /// Returns the `cmd_op` associated with this command.
    #[inline]
    pub fn cmd_op(&self) -> u32 {
        // SAFETY: `self.inner` is guaranteed by the type invariant to point
        // to a live `io_uring_cmd`, so dereferencing is safe.
        unsafe { (*self.inner.as_ptr()).cmd_op }
    }

    /// Returns the file associated with this command.
    ///
    /// See [`IoUringCmd::file`] for safety details.
    #[inline]
    pub fn file(&self) -> &File {
        // SAFETY: Same as `IoUringCmd::file`.
        let file = unsafe { (*self.inner.as_ptr()).file };
        // SAFETY: The `io_kiocb` holds a reference to the file for its entire
        // lifetime, so `file` is valid and has a positive refcount.
        unsafe { File::from_raw_file(file) }
    }

    /// Reads the PDU as a value of type `T`.
    ///
    /// See [`IoUringCmd::read_pdu`] for details and error conditions.
    #[inline]
    pub fn read_pdu<T: FromBytes>(&self) -> Result<T> {
        // SAFETY: `self.inner` is guaranteed by the type invariant to point
        // to a live `io_uring_cmd`, so dereferencing is safe.
        let inner = unsafe { &*self.inner.as_ref() };

        if size_of::<T>() > inner.pdu.len() {
            return Err(EINVAL);
        }

        let ptr = inner.pdu.as_ptr() as *const T;

        // SAFETY: Same as `IoUringCmd::read_pdu`.
        Ok(unsafe { core::ptr::read_unaligned(ptr) })
    }

    /// Writes `value` to the PDU of this command.
    ///
    /// See [`IoUringCmd::write_pdu`] for details and error conditions.
    #[inline]
    pub fn write_pdu<T: AsBytes>(&mut self, value: &T) -> Result<()> {
        // SAFETY: `self.inner` is guaranteed by the type invariant to point
        // to a live `io_uring_cmd`, so dereferencing is safe.
        let inner = unsafe { self.inner.as_mut() };

        let len = size_of::<T>();
        if len > inner.pdu.len() {
            return Err(EINVAL);
        }

        let src = (value as *const T).cast::<u8>();
        let dst = &raw mut inner.pdu as *mut u8;

        // SAFETY: Same as `IoUringCmd::write_pdu`.
        unsafe {
            core::ptr::copy_nonoverlapping(src, dst, len);
        }

        Ok(())
    }

    /// Posts the asynchronous completion to userspace.
    ///
    /// # Parameters
    ///
    /// - `ret`: Result to return to userspace.
    /// - `res2`: Extra result word for `IORING_SETUP_CQE32` big-CQE rings;
    ///   pass `0` if not needed.
    /// - `issue_flags`: The `issue_flags` value received by the `uring_cmd`
    ///   callback; pass it through unchanged.
    #[inline]
    pub fn done(self, ret: Result<i32>, res2: u64, issue_flags: u32) {
        let ret = from_result(|| ret);
        // SAFETY: `self.inner` is a valid `io_uring_cmd` that was previously
        // queued (returned `EIOCBQUEUED` to io_uring).  The kernel keeps the
        // `io_kiocb` alive until this call completes.
        unsafe {
            bindings::io_uring_cmd_done32(self.inner.as_ptr(), ret, res2, issue_flags);
        }
    }
}

/// A Rust abstraction for `io_uring_sqe`.
///
/// Represents a Submission Queue Entry (SQE) that describes an I/O operation
/// to be executed by the io_uring subsystem.  Obtain an instance from
/// [`IoUringCmd::sqe`].
///
/// This type should not be constructed directly by drivers.
///
/// # Invariants
///
/// `self.inner` always points to a valid, live `bindings::io_uring_sqe`.
/// The `repr(transparent)` attribute guarantees the same memory layout as the
/// underlying binding.
#[repr(transparent)]
pub struct IoUringSqe {
    inner: Opaque<bindings::io_uring_sqe>,
}

impl IoUringSqe {
    /// Returns the opcode of this SQE.
    pub fn opcode(&self) -> Opcode {
        // SAFETY: `self.inner` guaranteed by the type invariant to point
        // to a live `io_uring_sqe`, so dereferencing is safe.  Volatile
        // read is used because the SQE may reside in memory shared with
        // userspace.
        Opcode(unsafe { core::ptr::addr_of!((*self.inner.get()).opcode).read_volatile() })
    }

    /// Reads the inline `cmd` data of this SQE as a value of type `T`.
    ///
    /// Only the standard `io_uring_sqe` layout is supported
    /// (`IORING_SETUP_SQE128` is not handled here).
    ///
    /// # Errors
    ///
    /// Returns [`EINVAL`] if `size_of::<T>()` exceeds the inline command buffer.
    pub fn cmd_data<T: FromBytes>(&self) -> Result<T> {
        // SAFETY: `self.inner` guaranteed by the type invariant to point
        // to a live `io_uring_sqe`, so dereferencing is safe.
        let sqe = unsafe { &*self.inner.get() };

        // SAFETY: Accessing the `sqe.cmd` union field is safe because
        // `IoUringSqe` can only be obtained from `IoUringCmd::sqe()`, which
        // is only available inside a `uring_cmd` callback where the opcode
        // is guaranteed to be `IORING_OP_URING_CMD` by the io_uring core.
        let cmd = unsafe { sqe.__bindgen_anon_6.cmd.as_ref() };
        let cmd_len = size_of_val(&sqe.__bindgen_anon_6.bindgen_union_field);

        if cmd_len < size_of::<T>() {
            return Err(EINVAL);
        }

        let cmd_ptr = cmd.as_ptr() as *const T;

        // SAFETY: `cmd_ptr` is valid, derived from `self.inner` which is
        // guaranteed by the type invariant. `read_unaligned` is used because
        // the cmd data may not satisfy `T`'s alignment requirements.
        // `T: FromBytes` guarantees that every bit-pattern is a valid value.
        Ok(unsafe { core::ptr::read_unaligned(cmd_ptr) })
    }

    /// Constructs an [`IoUringSqe`] reference from a raw pointer.
    ///
    /// # Safety
    ///
    /// The caller must guarantee that:
    /// - `ptr` is non-null, properly aligned, and points to a valid, initialised
    ///   `bindings::io_uring_sqe`.
    /// - The pointed-to object remains valid for the entire lifetime `'a`.
    /// - No mutable access to the same object occurs while the returned
    ///   reference is alive.
    #[inline]
    pub(crate) unsafe fn from_raw<'a>(ptr: *const bindings::io_uring_sqe) -> &'a IoUringSqe {
        // SAFETY: The caller guarantees that the pointer is not dangling and
        // stays valid for the duration of 'a.  The cast is valid because
        // `IoUringSqe` is `repr(transparent)` over `bindings::io_uring_sqe`.
        unsafe { &*ptr.cast() }
    }
}
