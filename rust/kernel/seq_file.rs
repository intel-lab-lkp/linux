// SPDX-License-Identifier: GPL-2.0

//! Seq file bindings.
//!
//! C header: [`include/linux/seq_file.h`](srctree/include/linux/seq_file.h)

use crate::{bindings, ffi, fmt, str::CStr, str::CStrExt as _, types::NotThreadSafe, types::Opaque};

/// A utility for generating the contents of a seq file.
#[repr(transparent)]
pub struct SeqFile {
    inner: Opaque<bindings::seq_file>,
    _not_send: NotThreadSafe,
}

/// The prefix type for [`SeqFile::hex_dump`].
pub enum HexDumpPrefix {
    /// No prefix.
    None,
    /// Prefix with the memory address.
    Address,
    /// Prefix with the offset within the buffer.
    Offset,
}

impl HexDumpPrefix {
    fn as_c_int(self) -> ffi::c_int {
        match self {
            Self::None => bindings::DUMP_PREFIX_NONE as ffi::c_int,
            Self::Address => bindings::DUMP_PREFIX_ADDRESS as ffi::c_int,
            Self::Offset => bindings::DUMP_PREFIX_OFFSET as ffi::c_int,
        }
    }
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
        // SAFETY: Passing a void pointer to `Arguments` is valid for `%pA`.
        unsafe {
            bindings::seq_printf(
                self.inner.get(),
                c"%pA".as_char_ptr(),
                core::ptr::from_ref(&args).cast::<crate::ffi::c_void>(),
            );
        }
    }

    /// Prints a C string to the seq file.
    pub fn puts(&self, s: &CStr) {
        // SAFETY: `self.inner.get()` is valid because `&self` guarantees the
        // `SeqFile` is alive and was properly initialized via `from_raw`.
        // `s.as_char_ptr()` is valid because `CStr` is always a valid
        // null-terminated C string.
        unsafe { bindings::__seq_puts(self.inner.get(), s.as_char_ptr()) }
    }

    /// Prints a single char to the seq file.
    pub fn putc(&self, c: u8) {
        // SAFETY: `self.inner.get()` is valid because `&self`
        // guarantees `SeqFile` is alive and was properly initialized via `from_raw`
        unsafe { bindings::seq_putc(self.inner.get(), c as ffi::c_char) }
    }

    /// Writes raw bytes to the seq file.
    pub fn write(&self, data: &[u8]) {
        // SAFETY: `self.inner.get()` is valid because `&self` guarantees the
        // `SeqFile` is alive and was properly initialized via `from_raw`.
        // `data.as_ptr()` is valid and non-dangling because it comes from a
        // `&[u8]`, which guarantees the memory is valid for `data.len()` bytes
        // and will not be modified during the call due to the shared reference.
        unsafe {
            bindings::seq_write(
                self.inner.get(),
                data.as_ptr().cast::<ffi::c_void>(),
                data.len(),
            )
        };
    }

    /// Prints a hex dump of `buf` to the seq file.
    pub fn hex_dump(
        &self,
        prefix_str: &CStr,
        prefix_type: HexDumpPrefix,
        rowsize: u8,
        groupsize: u8,
        buf: &[u8],
        ascii: bool,
    ) {
        // SAFETY: `self.inner.get()` is valid because `&self` guarantees the
        // `SeqFile` is alive and was properly initialized via `from_raw`.
        // `prefix_str.as_char_ptr()` is valid because `CStr` is always a valid
        // null-terminated C string. `buf.as_ptr()` is valid and non-dangling
        // because it comes from a `&[u8]`, which guarantees the memory is valid
        // for `buf.len()` bytes and will not be modified during the call due to
        // the shared reference.
        unsafe {
            bindings::seq_hex_dump(
                self.inner.get(),
                prefix_str.as_char_ptr(),
                prefix_type.as_c_int(),
                rowsize as ffi::c_int,
                groupsize as ffi::c_int,
                buf.as_ptr().cast::<ffi::c_void>(),
                buf.len(),
                ascii,
            )
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
