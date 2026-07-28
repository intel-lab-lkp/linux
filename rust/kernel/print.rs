// SPDX-License-Identifier: GPL-2.0

//! Printing facilities.
//!
//! C header: [`include/linux/printk.h`](srctree/include/linux/printk.h)
//!
//! Reference: <https://docs.kernel.org/core-api/printk-basics.html>

use crate::{
    ffi::{c_char, c_void},
    fmt,
    prelude::*,
    str::RawFormatter,
    sync::atomic::{
        Atomic,
        AtomicType,
        Relaxed, //
    },
};

// Called from `vsprintf` with format specifier `%pA`.
#[expect(clippy::missing_safety_doc)]
#[export]
unsafe extern "C" fn rust_fmt_argument(
    buf: *mut c_char,
    end: *mut c_char,
    ptr: *const c_void,
) -> *mut c_char {
    use fmt::Write;
    // SAFETY: The C contract guarantees that `buf` is valid if it's less than `end`.
    let mut w = unsafe { RawFormatter::from_ptrs(buf.cast(), end.cast()) };
    // SAFETY: TODO.
    let _ = w.write_fmt(unsafe { *ptr.cast::<fmt::Arguments<'_>>() });
    w.pos().cast()
}

/// Format levels.
///
/// Public but hidden since it should only be used from public macros.
#[doc(hidden)]
pub mod level {
    use core::ffi::CStr;

    pub trait Level {
        const FORMAT_STRING: &'static CStr;
    }

    /// Generates a fixed format string for the kernel's [`_printk`].
    ///
    /// The format string is always the same for a given level, i.e. for a
    /// given `prefix`, which are the kernel's `KERN_*` constants.
    ///
    /// [`_printk`]: srctree/include/linux/printk.h
    const fn generate(prefix: &[u8; 3]) -> [u8; 10] {
        // Ensure the `KERN_*` macros are what we expect.
        assert!(prefix.len() == 3);
        assert!(prefix[0] == b'\x01');
        assert!(prefix[1] >= b'0' && prefix[1] <= b'7');

        let mut fmt = *b"\0\0%s: %pA\0";
        fmt[0] = prefix[0];
        fmt[1] = prefix[1];
        fmt
    }

    // #[rustfmt::skip] // Rustfmt formats the macro awkwardly.
    macro_rules! define_level {
        ($lvl:ident) => {
            pub struct $lvl;

            impl Level for $lvl {
                const FORMAT_STRING: &'static CStr = match CStr::from_bytes_with_nul(&generate(
                    macros::paste!(bindings::[<KERN_ $lvl>]),
                )) {
                    Ok(v) => v,
                    Err(_) => unreachable!(),
                };
            }
        };
    }

    define_level!(EMERG);
    define_level!(ALERT);
    define_level!(CRIT);
    define_level!(ERR);
    define_level!(WARNING);
    define_level!(NOTICE);
    define_level!(INFO);
    define_level!(DEBUG);
}

/// Prints a message via the kernel's [`_printk`].
///
/// Public but hidden since it should only be used from public macros.
///
/// [`_printk`]: srctree/include/linux/_printk.h
#[doc(hidden)]
#[cfg_attr(not(CONFIG_PRINTK), allow(unused_variables))]
pub fn call_printk<Lvl: level::Level>(module_name: &CStr, args: fmt::Arguments<'_>) {
    // `_printk` does not seem to fail in any path.
    #[cfg(CONFIG_PRINTK)]
    // SAFETY: TODO.
    unsafe {
        bindings::_printk(
            Lvl::FORMAT_STRING.as_char_ptr(),
            module_name.as_char_ptr(),
            core::ptr::from_ref(&args).cast::<c_void>(),
        );
    }
}

/// Prints a message via the kernel's [`_printk`] for the `CONT` level.
///
/// Public but hidden since it should only be used from public macros.
///
/// [`_printk`]: srctree/include/linux/printk.h
#[doc(hidden)]
#[cfg_attr(not(CONFIG_PRINTK), allow(unused_variables))]
pub fn call_printk_cont(args: fmt::Arguments<'_>) {
    const CONT_FMT: [u8; 6] = {
        // Ensure the `KERN_*` macros are what we expect (2 byte + 1 byte nul-termination).
        assert!(bindings::KERN_CONT.len() == 3);

        let mut fmt = *b"\0\0%pA\0";
        fmt[0] = bindings::KERN_CONT[0];
        fmt[1] = bindings::KERN_CONT[1];
        fmt
    };

    // `_printk` does not seem to fail in any path.
    //
    // SAFETY: The format string is fixed.
    #[cfg(CONFIG_PRINTK)]
    unsafe {
        bindings::_printk(
            CONT_FMT.as_ptr(),
            core::ptr::from_ref(&args).cast::<c_void>(),
        );
    }
}

/// Performs formatting and forwards the string to [`call_printk`].
///
/// Public but hidden since it should only be used from public macros.
#[doc(hidden)]
#[cfg(not(testlib))]
#[macro_export]
#[expect(clippy::crate_in_macro_def)]
macro_rules! print_macro (
    ($level:ident, $($arg:tt)+) => (
        match $crate::prelude::fmt!($($arg)+) {
            args => {
                $crate::print::call_printk::<$crate::print::level::$level>(
                    crate::__LOG_PREFIX,
                    args,
                );
            }
        }
    );
);

/// Stub for doctests
#[cfg(testlib)]
#[macro_export]
macro_rules! print_macro (
    ($format_string:path, $($arg:tt)+) => (
        ()
    );
);

// We could use a macro to generate these macros. However, doing so ends
// up being a bit ugly: it requires the dollar token trick to escape `$` as
// well as playing with the `doc` attribute. Furthermore, they cannot be easily
// imported in the prelude due to [1]. So, for the moment, we just write them
// manually, like in the C side; while keeping most of the logic in another
// macro, i.e. [`print_macro`].
//
// [1]: https://github.com/rust-lang/rust/issues/52234

/// Prints an emergency-level message (level 0).
///
/// Use this level if the system is unusable.
///
/// Equivalent to the kernel's [`pr_emerg`] macro.
///
/// Mimics the interface of [`std::print!`]. See [`core::fmt`] and
/// [`std::format!`] for information about the formatting syntax.
///
/// [`pr_emerg`]: https://docs.kernel.org/core-api/printk-basics.html#c.pr_emerg
/// [`std::print!`]: https://doc.rust-lang.org/std/macro.print.html
/// [`std::format!`]: https://doc.rust-lang.org/std/macro.format.html
///
/// # Examples
///
/// ```
/// pr_emerg!("hello {}\n", "there");
/// ```
#[macro_export]
macro_rules! pr_emerg (
    ($($arg:tt)*) => (
        $crate::print_macro!(EMERG, $($arg)*)
    )
);

/// Prints an alert-level message (level 1).
///
/// Use this level if action must be taken immediately.
///
/// Equivalent to the kernel's [`pr_alert`] macro.
///
/// Mimics the interface of [`std::print!`]. See [`core::fmt`] and
/// [`std::format!`] for information about the formatting syntax.
///
/// [`pr_alert`]: https://docs.kernel.org/core-api/printk-basics.html#c.pr_alert
/// [`std::print!`]: https://doc.rust-lang.org/std/macro.print.html
/// [`std::format!`]: https://doc.rust-lang.org/std/macro.format.html
///
/// # Examples
///
/// ```
/// pr_alert!("hello {}\n", "there");
/// ```
#[macro_export]
macro_rules! pr_alert (
    ($($arg:tt)*) => (
        $crate::print_macro!(ALERT, $($arg)*)
    )
);

/// Prints a critical-level message (level 2).
///
/// Use this level for critical conditions.
///
/// Equivalent to the kernel's [`pr_crit`] macro.
///
/// Mimics the interface of [`std::print!`]. See [`core::fmt`] and
/// [`std::format!`] for information about the formatting syntax.
///
/// [`pr_crit`]: https://docs.kernel.org/core-api/printk-basics.html#c.pr_crit
/// [`std::print!`]: https://doc.rust-lang.org/std/macro.print.html
/// [`std::format!`]: https://doc.rust-lang.org/std/macro.format.html
///
/// # Examples
///
/// ```
/// pr_crit!("hello {}\n", "there");
/// ```
#[macro_export]
macro_rules! pr_crit (
    ($($arg:tt)*) => (
        $crate::print_macro!(CRIT, $($arg)*)
    )
);

/// Prints an error-level message (level 3).
///
/// Use this level for error conditions.
///
/// Equivalent to the kernel's [`pr_err`] macro.
///
/// Mimics the interface of [`std::print!`]. See [`core::fmt`] and
/// [`std::format!`] for information about the formatting syntax.
///
/// [`pr_err`]: https://docs.kernel.org/core-api/printk-basics.html#c.pr_err
/// [`std::print!`]: https://doc.rust-lang.org/std/macro.print.html
/// [`std::format!`]: https://doc.rust-lang.org/std/macro.format.html
///
/// # Examples
///
/// ```
/// pr_err!("hello {}\n", "there");
/// ```
#[macro_export]
macro_rules! pr_err (
    ($($arg:tt)*) => (
        $crate::print_macro!(ERR, $($arg)*)
    )
);

/// Prints a warning-level message (level 4).
///
/// Use this level for warning conditions.
///
/// Equivalent to the kernel's [`pr_warn`] macro.
///
/// Mimics the interface of [`std::print!`]. See [`core::fmt`] and
/// [`std::format!`] for information about the formatting syntax.
///
/// [`pr_warn`]: https://docs.kernel.org/core-api/printk-basics.html#c.pr_warn
/// [`std::print!`]: https://doc.rust-lang.org/std/macro.print.html
/// [`std::format!`]: https://doc.rust-lang.org/std/macro.format.html
///
/// # Examples
///
/// ```
/// pr_warn!("hello {}\n", "there");
/// ```
#[macro_export]
macro_rules! pr_warn (
    ($($arg:tt)*) => (
        $crate::print_macro!(WARNING, $($arg)*)
    )
);

/// Prints a notice-level message (level 5).
///
/// Use this level for normal but significant conditions.
///
/// Equivalent to the kernel's [`pr_notice`] macro.
///
/// Mimics the interface of [`std::print!`]. See [`core::fmt`] and
/// [`std::format!`] for information about the formatting syntax.
///
/// [`pr_notice`]: https://docs.kernel.org/core-api/printk-basics.html#c.pr_notice
/// [`std::print!`]: https://doc.rust-lang.org/std/macro.print.html
/// [`std::format!`]: https://doc.rust-lang.org/std/macro.format.html
///
/// # Examples
///
/// ```
/// pr_notice!("hello {}\n", "there");
/// ```
#[macro_export]
macro_rules! pr_notice (
    ($($arg:tt)*) => (
        $crate::print_macro!(NOTICE, $($arg)*)
    )
);

/// Prints an info-level message (level 6).
///
/// Use this level for informational messages.
///
/// Equivalent to the kernel's [`pr_info`] macro.
///
/// Mimics the interface of [`std::print!`]. See [`core::fmt`] and
/// [`std::format!`] for information about the formatting syntax.
///
/// [`pr_info`]: https://docs.kernel.org/core-api/printk-basics.html#c.pr_info
/// [`std::print!`]: https://doc.rust-lang.org/std/macro.print.html
/// [`std::format!`]: https://doc.rust-lang.org/std/macro.format.html
///
/// # Examples
///
/// ```
/// pr_info!("hello {}\n", "there");
/// ```
#[macro_export]
#[doc(alias = "print")]
macro_rules! pr_info (
    ($($arg:tt)*) => (
        $crate::print_macro!(INFO, $($arg)*)
    )
);

/// Prints a debug-level message (level 7).
///
/// Use this level for debug messages.
///
/// Equivalent to the kernel's [`pr_debug`] macro, except that it doesn't support dynamic debug
/// yet.
///
/// Mimics the interface of [`std::print!`]. See [`core::fmt`] and
/// [`std::format!`] for information about the formatting syntax.
///
/// [`pr_debug`]: https://docs.kernel.org/core-api/printk-basics.html#c.pr_debug
/// [`std::print!`]: https://doc.rust-lang.org/std/macro.print.html
/// [`std::format!`]: https://doc.rust-lang.org/std/macro.format.html
///
/// # Examples
///
/// ```
/// pr_debug!("hello {}\n", "there");
/// ```
#[macro_export]
#[doc(alias = "print")]
macro_rules! pr_debug (
    ($($arg:tt)*) => (
        if cfg!(debug_assertions) {
            $crate::print_macro!(DEBUG, $($arg)*)
        }
    )
);

/// Continues a previous log message in the same line.
///
/// Use only when continuing a previous `pr_*!` macro (e.g. [`pr_info!`]).
///
/// Equivalent to the kernel's [`pr_cont`] macro.
///
/// Mimics the interface of [`std::print!`]. See [`core::fmt`] and
/// [`std::format!`] for information about the formatting syntax.
///
/// [`pr_info!`]: crate::pr_info!
/// [`pr_cont`]: https://docs.kernel.org/core-api/printk-basics.html#c.pr_cont
/// [`std::print!`]: https://doc.rust-lang.org/std/macro.print.html
/// [`std::format!`]: https://doc.rust-lang.org/std/macro.format.html
///
/// # Examples
///
/// ```
/// # use kernel::pr_cont;
/// pr_info!("hello");
/// pr_cont!(" {}\n", "there");
/// ```
#[macro_export]
macro_rules! pr_cont (
    ($($arg:tt)*) => (
        $crate::print::call_printk_cont(
            $crate::prelude::fmt!($($arg)+),
        )
    )
);

/// A lightweight `call_once` primitive.
///
/// This structure provides the Rust equivalent of the kernel's `DO_ONCE_LITE` macro.
/// While it would be possible to implement the feature entirely as a Rust macro,
/// the functionality that can be implemented as regular functions has been
/// extracted and implemented as the `OnceLite` struct for better code maintainability.
pub struct OnceLite(Atomic<State>);

#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
enum State {
    Incomplete = 0,
    Complete = 1,
}

// SAFETY: `State` and `i32` has the same size and alignment, and it's round-trip
// transmutable to `i32`.
unsafe impl AtomicType for State {
    type Repr = i32;
}

impl OnceLite {
    /// Creates a new [`OnceLite`] in the incomplete state.
    #[inline(always)]
    #[allow(clippy::new_without_default)]
    pub const fn new() -> Self {
        OnceLite(Atomic::new(State::Incomplete))
    }

    /// Calls the provided function exactly once.
    ///
    /// There is no other synchronization between two `call_once()`s
    /// except that only one will execute `f`, in other words, callers
    /// should not use a failed `call_once()` as a proof that another
    /// `call_once()` has already finished and the effect is observable
    /// to this thread.
    pub fn call_once<F>(&self, f: F) -> bool
    where
        F: FnOnce(),
    {
        // Avoid expensive cmpxchg if already completed.
        // ORDERING: `Relaxed` is used here since no synchronization is required.
        let old = self.0.load(Relaxed);
        if old == State::Complete {
            return false;
        }

        // ORDERING: `Relaxed` is used here since no synchronization is required.
        let old = self.0.xchg(State::Complete, Relaxed);
        if old == State::Complete {
            return false;
        }

        f();
        true
    }
}

/// Run the given function exactly once.
///
/// This is equivalent to the kernel's `DO_ONCE_LITE` macro.
///
/// # Examples
///
/// ```
/// kernel::do_once_lite! {
///     kernel::pr_info!("This will be printed only once\n");
/// };
/// ```
#[macro_export]
macro_rules! do_once_lite {
    { $($e:tt)* } => {{
        #[link_section = ".data..once"]
        static ONCE: $crate::print::OnceLite = $crate::print::OnceLite::new();
        ONCE.call_once(|| { $($e)* });
    }};
}

/// Prints an emergency-level message (level 0) only once.
///
/// Equivalent to the kernel's `pr_emerg_once` macro.
#[macro_export]
macro_rules! pr_emerg_once (
    ($($arg:tt)*) => (
        $crate::do_once_lite! { $crate::pr_emerg!($($arg)*) }
    )
);

/// Prints an alert-level message (level 1) only once.
///
/// Equivalent to the kernel's `pr_alert_once` macro.
#[macro_export]
macro_rules! pr_alert_once (
    ($($arg:tt)*) => (
        $crate::do_once_lite! { $crate::pr_alert!($($arg)*) }
    )
);

/// Prints a critical-level message (level 2) only once.
///
/// Equivalent to the kernel's `pr_crit_once` macro.
#[macro_export]
macro_rules! pr_crit_once (
    ($($arg:tt)*) => (
        $crate::do_once_lite! { $crate::pr_crit!($($arg)*) }
    )
);

/// Prints an error-level message (level 3) only once.
///
/// Equivalent to the kernel's `pr_err_once` macro.
#[macro_export]
macro_rules! pr_err_once (
    ($($arg:tt)*) => (
        $crate::do_once_lite! { $crate::pr_err!($($arg)*) }
    )
);

/// Prints a warning-level message (level 4) only once.
///
/// Equivalent to the kernel's `pr_warn_once` macro.
#[macro_export]
macro_rules! pr_warn_once (
    ($($arg:tt)*) => (
        $crate::do_once_lite! { $crate::pr_warn!($($arg)*) }
    )
);

/// Prints a notice-level message (level 5) only once.
///
/// Equivalent to the kernel's `pr_notice_once` macro.
#[macro_export]
macro_rules! pr_notice_once (
    ($($arg:tt)*) => (
        $crate::do_once_lite! { $crate::pr_notice!($($arg)*) }
    )
);

/// Prints an info-level message (level 6) only once.
///
/// Equivalent to the kernel's `pr_info_once` macro.
#[macro_export]
macro_rules! pr_info_once (
    ($($arg:tt)*) => (
        $crate::do_once_lite! { $crate::pr_info!($($arg)*) }
    )
);
