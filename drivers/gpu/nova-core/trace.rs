// SPDX-License-Identifier: GPL-2.0

//! Nova tracepoint helpers.

use kernel::{
    ffi::c_char,
    fmt::{
        self,
        Write, //
    },
    str::{
        CStr,
        CStrExt,
        Formatter, //
    }, //
};

const MESSAGE_MAX: usize = 512;

// To add another formatted Nova Core trace event:
//
// 1. Define it from `nova_core_trace_class` in `trace.h`.
// 2. Declare its Rust entry point with `declare_nova_core_trace!` below.
// 3. Add a public frontend macro that passes its event name to
//    `nova_core_trace_impl!`, following the examples at the end of this file.
pub(crate) mod raw {
    use super::c_char;

    macro_rules! declare_nova_core_trace {
        ($event:ident) => {
            kernel::tracepoint::declare_trace! {
                /// # Safety
                ///
                /// `dev` must point to a valid NUL-terminated string, and
                /// `message` must point to `message_len` readable bytes for
                /// this call.
                pub(crate) unsafe fn $event(
                    dev: *const c_char,
                    message: *const c_char,
                    message_len: usize,
                );
            }
        };
    }

    declare_nova_core_trace!(nova_core_trace_driver);
    declare_nova_core_trace!(nova_core_trace_fsp);
    declare_nova_core_trace!(nova_core_trace_gsp);
    declare_nova_core_trace!(nova_core_trace_vgpu);
}

/// Formats and emits a Nova Core text trace event.
///
/// # Safety
///
/// `trace` must synchronously consume `dev`, `message`, and `message_len`
/// according to the `nova_core_trace_class` event prototype.
pub(crate) unsafe fn nova_core_trace_fmt(
    dev: &CStr,
    args: fmt::Arguments<'_>,
    trace: unsafe fn(*const c_char, *const c_char, usize),
) {
    let mut message = [0u8; MESSAGE_MAX];
    let message_len = {
        let mut formatter = Formatter::new(&mut message);

        let _ = formatter.write_fmt(args);
        formatter.bytes_written().min(MESSAGE_MAX)
    };

    // SAFETY: The caller guarantees that `trace` synchronously consumes its
    // arguments. The device name is NUL-terminated, and `message` contains
    // `message_len` initialized bytes.
    unsafe {
        trace(
            dev.as_char_ptr(),
            message.as_ptr().cast::<c_char>(),
            message_len,
        )
    }
}

macro_rules! nova_core_trace_impl {
    ($event:ident, $dev:expr, $($arg:tt)*) => {{
        #[cfg(CONFIG_TRACEPOINTS)]
        let should_trace = {
            // SAFETY: `$event` names a real C tracepoint static key.
            unsafe {
                kernel::macros::paste! {
                    kernel::jump_label::static_branch_unlikely!(
                        kernel::bindings::[<__tracepoint_ $event>],
                        kernel::bindings::tracepoint,
                        key
                    )
                }
            }
        };

        #[cfg(not(CONFIG_TRACEPOINTS))]
        let should_trace = false;

        if should_trace {
            match ($dev, kernel::prelude::fmt!($($arg)*)) {
                (dev, args) => {
                    // SAFETY: `$event` has the event-class prototype required
                    // by `nova_core_trace_fmt`.
                    unsafe {
                        $crate::trace::nova_core_trace_fmt(
                            dev.as_ref().name(),
                            args,
                            $crate::trace::raw::$event,
                        )
                    }
                }
            }
        }
    }};
}

// Frontend macros expand in their caller's module and invoke this helper by
// path, so the re-export must be visible from the parent module.
pub(super) use nova_core_trace_impl;

macro_rules! nova_core_trace_driver {
    ($dev:expr, $($arg:tt)*) => {
        $crate::trace::nova_core_trace_impl!(nova_core_trace_driver, $dev, $($arg)*)
    };
}

macro_rules! nova_core_trace_fsp {
    ($dev:expr, $($arg:tt)*) => {
        $crate::trace::nova_core_trace_impl!(nova_core_trace_fsp, $dev, $($arg)*)
    };
}

macro_rules! nova_core_trace_gsp {
    ($dev:expr, $($arg:tt)*) => {
        $crate::trace::nova_core_trace_impl!(nova_core_trace_gsp, $dev, $($arg)*)
    };
}

#[expect(unused_macros)]
macro_rules! nova_core_trace_vgpu {
    ($dev:expr, $($arg:tt)*) => {
        $crate::trace::nova_core_trace_impl!(nova_core_trace_vgpu, $dev, $($arg)*)
    };
}

pub(crate) use nova_core_trace_driver;
pub(crate) use nova_core_trace_fsp;
pub(crate) use nova_core_trace_gsp;
#[expect(unused_imports)]
pub(crate) use nova_core_trace_vgpu;
