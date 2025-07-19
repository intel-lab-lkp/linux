// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2025 Furiosa AI.

//! Files and file descriptors.
//!
//! C headers: [`include/linux/io_uring/cmd.h`](srctree/include/linux/io_uring/cmd.h) and
//! [`include/linux/file.h`](srctree/include/linux/file.h)

use core::mem::MaybeUninit;

use crate::{fs::File, types::Opaque};

pub mod flags {
    pub const COMPLETE_DEFER: i32 = bindings::io_uring_cmd_flags_IO_URING_F_COMPLETE_DEFER;
    pub const UNLOCKED: i32 = bindings::io_uring_cmd_flags_IO_URING_F_UNLOCKED;

    pub const MULTISHOT: i32 = bindings::io_uring_cmd_flags_IO_URING_F_MULTISHOT;
    pub const IOWQ: i32 = bindings::io_uring_cmd_flags_IO_URING_F_IOWQ;
    pub const NONBLOCK: i32 = bindings::io_uring_cmd_flags_IO_URING_F_NONBLOCK;

    pub const SQE128: i32 = bindings::io_uring_cmd_flags_IO_URING_F_SQE128;
    pub const CQE32: i32 = bindings::io_uring_cmd_flags_IO_URING_F_CQE32;
    pub const IOPOLL: i32 = bindings::io_uring_cmd_flags_IO_URING_F_IOPOLL;

    pub const CANCEL: i32 = bindings::io_uring_cmd_flags_IO_URING_F_CANCEL;
    pub const COMPAT: i32 = bindings::io_uring_cmd_flags_IO_URING_F_COMPAT;
    pub const TASK_DEAD: i32 = bindings::io_uring_cmd_flags_IO_URING_F_TASK_DEAD;
}

#[repr(transparent)]
pub struct IoUringCmd {
    inner: Opaque<bindings::io_uring_cmd>,
}

impl IoUringCmd {
    /// Returns the cmd_op with associated with the io_uring_cmd.
    #[inline]
    pub fn cmd_op(&self) -> u32 {
        // SAFETY: The call guarantees that the pointer is not dangling and stays valid
        unsafe { (*self.inner.get()).cmd_op }
    }

    /// Returns the flags with associated with the io_uring_cmd.
    #[inline]
    pub fn flags(&self) -> u32 {
        // SAFETY: The call guarantees that the pointer is not dangling and stays valid
        unsafe { (*self.inner.get()).flags }
    }

    /// Returns the ref pdu for free use.
    #[inline]
    pub fn pdu(&mut self) -> MaybeUninit<&mut [u8; 32]> {
        // SAFETY: The call guarantees that the pointer is not dangling and stays valid
        unsafe { MaybeUninit::new(&mut (*self.inner.get()).pdu) }
    }

    /// Constructs a new `struct io_uring_cmd` wrapper from a file descriptor.
    #[inline]
    pub unsafe fn from_raw<'a>(ptr: *const bindings::io_uring_cmd) -> &'a IoUringCmd {
        // SAFETY: The caller guarantees that the pointer is not dangling and stays valid for the
        // duration of 'a. The cast is okay because `File` is `repr(transparent)`.
        unsafe { &*ptr.cast() }
    }

    // Returns the file that referenced by uring cmd self.
    #[inline]
    pub fn file<'a>(&'a self) -> &'a File {
        // SAFETY: The call guarantees that the pointer is not dangling and stays valid
        let file = unsafe { (*self.inner.get()).file };
        unsafe { File::from_raw_file(file) }
    }

    // Returns the sqe  that referenced by uring cmd self.
    #[inline]
    pub fn sqe(&self) -> &IoUringSqe {
        // SAFETY: The call guarantees that the pointer is not dangling and stays valid
        let ptr = unsafe { (*self.inner.get()).sqe };
        unsafe { IoUringSqe::from_raw(ptr) }
    }

    // Called by consumers of io_uring_cmd, if they originally returned -EIOCBQUEUED upon receiving the command
    #[inline]
    pub fn done(self, ret: isize, res2: u64, issue_flags: u32) {
        // SAFETY: The call guarantees that the pointer is not dangling and stays valid
        unsafe {
            bindings::io_uring_cmd_done(self.inner.get(), ret, res2, issue_flags);
        }
    }
}

#[repr(transparent)]
pub struct IoUringSqe {
    inner: Opaque<bindings::io_uring_sqe>,
}

impl<'a> IoUringSqe {
    pub fn cmd_data(&'a self) -> &'a [Opaque<u8>] {
        // SAFETY: The call guarantees that the pointer is not dangling and stays valid
        unsafe {
            let cmd = (*self.inner.get()).__bindgen_anon_6.cmd.as_ref();
            core::slice::from_raw_parts(cmd.as_ptr() as *const Opaque<u8>, 8)
        }
    }

    #[inline]
    pub unsafe fn from_raw(ptr: *const bindings::io_uring_sqe) -> &'a IoUringSqe {
        // SAFETY: The caller guarantees that the pointer is not dangling and stays valid for the
        // duration of 'a. The cast is okay because `File` is `repr(transparent)`.
        //
        // INVARIANT: The caller guarantees that there are no problematic `fdget_pos` calls.
        unsafe { &*ptr.cast() }
    }
}
