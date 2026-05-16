// SPDX-License-Identifier: GPL-2.0

//! Seq file bindings.
//!
//! C header: [`include/linux/seq_file.h`](srctree/include/linux/seq_file.h)

use crate::{bindings, ffi::c_void, fmt, types::NotThreadSafe, types::Opaque};

/// A utility for generating the contents of a seq file.
#[repr(transparent)]
pub struct SeqFile {
    inner: Opaque<bindings::seq_file>,
    _not_send: NotThreadSafe,
}

impl SeqFile {
    /// Creates a new [`SeqFile`] from a raw pointer.
    ///
    /// # Safety
    ///
    /// The caller must ensure that for the duration of `'a` the following is satisfied:
    /// * The pointer points at a valid `struct seq_file`.
    /// * The `struct seq_file` is not accessed from any other thread.
    pub unsafe fn from_raw<'a>(ptr: *mut bindings::seq_file) -> &'a SeqFile {
        // SAFETY: The caller ensures that the reference is valid for 'a. There's no way to trigger
        // a data race by using the `&SeqFile` since this is the only thread accessing the seq_file.
        //
        // CAST: The layout of `struct seq_file` and `SeqFile` is compatible.
        unsafe { &*ptr.cast() }
    }

    /// Used by the [`seq_print`] macro.
    #[inline]
    pub fn call_printf(&self, args: fmt::Arguments<'_>) {
        let mut this = self;
        let _ = fmt::Write::write_fmt(&mut this, args);
    }
}

impl fmt::Write for &SeqFile {
    #[inline]
    fn write_str(&mut self, s: &str) -> fmt::Result {
        // SAFETY: `self` is a valid reference, ensuring `self.inner.get()` is a valid pointer
        // to `struct seq_file`. `s` is a valid string slice, guaranteeing `s.as_ptr()` is
        // readable for `s.len()` bytes. `seq_write` handles bounds checking and does not
        // require a null-terminated string.
        //
        // CAST: `s.as_ptr()` (a `*const u8`) is cast to `*const c_void` because `seq_write`
        // only reads the buffer via `memcpy` and does not care about the underlying type.

        let res =
            unsafe { bindings::seq_write(self.inner.get(), s.as_ptr().cast::<c_void>(), s.len()) };

        if res < 0 {
            Err(fmt::Error)
        } else {
            Ok(())
        }
    }
}

/// Write to a [`SeqFile`] with the ordinary Rust formatting syntax.
#[macro_export]
macro_rules! seq_print {
    ($m:expr, $($arg:tt)+) => (
        $m.call_printf($crate::prelude::fmt!($($arg)+))
    );
}
pub use seq_print;
