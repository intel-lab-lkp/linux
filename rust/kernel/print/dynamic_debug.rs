// SPDX-License-Identifier: GPL-2.0

//! Callsite metadata and printing for dynamic debug.
//!
//! C header: [`include/linux/dynamic_debug.h`](srctree/include/linux/dynamic_debug.h).
//!
//! The printing macros place descriptors in `__dyndbg`. The existing C dynamic debug core
//! discovers them at boot or module load and removes them before the module is freed.

use crate::{
    fmt,
    prelude::*,
    str::as_char_ptr_in_const_context, //
};

/// A dynamic debug callsite descriptor.
///
/// The [`crate::dynamic_debug!`] macro places descriptors at stable addresses in `__dyndbg`.
#[repr(transparent)]
pub struct Descriptor {
    /// The C descriptor, exposed for static-branch field addressing.
    pub inner: bindings::_ddebug,
}

impl Descriptor {
    /// Creates an initially disabled descriptor.
    pub const fn new(
        module: &'static CStr,
        file: &'static CStr,
        format: &'static CStr,
        line: u32,
    ) -> Self {
        // SAFETY: `_ddebug` contains raw pointers, integers and, with jump labels, a static key.
        // All accept zero; the pointers and bitfields are filled in before publication.
        let mut descriptor: bindings::_ddebug = unsafe { core::mem::zeroed() };
        descriptor.modname = as_char_ptr_in_const_context(module);
        descriptor.function = as_char_ptr_in_const_context(c"<unknown>");
        descriptor.filename = as_char_ptr_in_const_context(file);
        descriptor.format = as_char_ptr_in_const_context(format);

        // C packs `lineno:18`, `class_id:6` and `flags:8` into an unsigned int. Bindgen's setters
        // are not const, so encode the target's bitfield ordering explicitly. Truncate line numbers
        // as the C bitfield assignment does.
        let line = line & ((1 << 18) - 1);
        let class = bindings::_DPRINTK_CLASS_DFLT;
        let flags = bindings::_DPRINTK_FLAGS_NONE;
        #[cfg(target_endian = "little")]
        let bits = (line | (class << 18) | (flags << 24)).to_le_bytes();
        #[cfg(target_endian = "big")]
        let bits = ((line << 14) | (class << 8) | flags).to_be_bytes();
        descriptor._bitfield_1 = bindings::__BindgenBitfieldUnit::new(bits);

        #[cfg(CONFIG_JUMP_LABEL)]
        // An all-zero key is `STATIC_KEY_INIT_FALSE`.
        static_assert!(bindings::JUMP_TYPE_FALSE == 0);

        Self { inner: descriptor }
    }

    /// Checks the print flag when jump labels are unavailable.
    ///
    /// # Safety
    ///
    /// `descriptor` must point to an initialized descriptor valid for this call. Concurrent access
    /// to its fields must follow the C dynamic debug and jump label synchronization rules, without
    /// conflicting Rust accesses.
    #[cfg(not(CONFIG_JUMP_LABEL))]
    #[inline]
    pub unsafe fn enabled(descriptor: *mut Self) -> bool {
        // SAFETY: The pointer is valid by the caller's guarantee. The helper uses the same flag
        // access as the C dynamic debug branch, without forming a Rust reference to it.
        unsafe { bindings::ddebug_enabled(&raw mut (*descriptor).inner) }
    }

    /// Prints arguments with the crate's log prefix and the dynamic debug prefixes.
    ///
    /// # Safety
    ///
    /// `descriptor` must point to an initialized descriptor valid for this call, whose string
    /// pointers refer to valid NUL-terminated C strings. Concurrent access must follow the C
    /// dynamic debug and jump label synchronization rules, without conflicting Rust accesses.
    pub unsafe fn print(descriptor: *mut Self, prefix: &CStr, args: fmt::Arguments<'_>) {
        // SAFETY: The descriptor is valid by the caller's guarantee. The helper uses the C string
        // `prefix` and consumes `args` through `%pA` synchronously, so their borrows remain valid.
        unsafe {
            bindings::ddebug_print(
                &raw mut (*descriptor).inner,
                prefix.as_char_ptr(),
                core::ptr::from_ref(&args).cast(),
            )
        };
    }
}

/// Extracts only the format string for the callsite catalogue.
///
/// The original tokens are separately passed to `fmt!`, preserving implicit argument capture.
#[doc(hidden)]
#[macro_export]
macro_rules! dynamic_debug_format {
    ($format:expr $(, $($rest:tt)*)?) => {
        // The C catalogue ends at the first NUL; Rust formatting still uses the full string.
        match $crate::str::CStr::from_bytes_until_nul(concat!($format, "\0").as_bytes()) {
            Ok(format) => format,
            Err(_) => panic!("the format string must be NUL-terminated"),
        }
    };
}

/// Defines a callsite and evaluates a printing expression only when it is enabled.
#[doc(hidden)]
#[macro_export]
#[cfg(not(testlib))]
macro_rules! dynamic_debug {
    ($descriptor:ident, ($($arg:tt)*), $body:block) => {{
        // Keep item names out of the user's scope, including implicit format captures.
        let (descriptor, enabled) = {
            // `used(compiler)` avoids `SHF_GNU_RETAIN`, which the module loader interprets as
            // `SHF_RO_AFTER_INIT`.
            #[used(compiler)]
            #[unsafe(link_section = "__dyndbg")]
            static mut DESCRIPTOR: $crate::print::dynamic_debug::Descriptor =
                $crate::print::dynamic_debug::Descriptor::new(
                    $crate::c_str!(env!("RUST_MODNAME")),
                    $crate::c_str!(file!()),
                    $crate::dynamic_debug_format!($($arg)*),
                    line!(),
                );

            #[cfg(CONFIG_JUMP_LABEL)]
            // SAFETY: `DESCRIPTOR` contains an initialized false key at the specified field and
            // remains valid until the C core unregisters the module's jump label entries.
            let enabled = unsafe {
                $crate::jump_label::static_branch_unlikely!(
                    DESCRIPTOR,
                    $crate::print::dynamic_debug::Descriptor,
                    inner.key.dd_key_false
                )
            };

            #[cfg(not(CONFIG_JUMP_LABEL))]
            // SAFETY: The static descriptor is initialized and accessed only by the C helpers
            // and dynamic debug core, which use the C synchronization rules.
            let enabled = unsafe {
                $crate::print::dynamic_debug::Descriptor::enabled(&raw mut DESCRIPTOR)
            };

            (&raw mut DESCRIPTOR, enabled)
        };

        if enabled {
            let $descriptor = descriptor;
            $body
        }
    }};
}

/// Stub for host doctests, matching [`crate::print_macro!`].
#[cfg(testlib)]
#[macro_export]
macro_rules! dynamic_debug {
    ($descriptor:ident, ($($arg:tt)*), $body:block) => {
        ()
    };
}
