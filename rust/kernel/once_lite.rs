// SPDX-License-Identifier: GPL-2.0

//! Support for calling a function exactly once.
//!
//! C header: [`include/linux/once_lite.h`](srctree/include/linux/once_lite.h)

use crate::sync::atomic::{Atomic, AtomicType, Relaxed};

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
    pub fn call_once<F>(&self, f: F) -> bool
    where
        F: FnOnce(),
    {
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
/// kernel::do_once_lite!(|| {
///     kernel::pr_info!("This will be printed only once\n");
/// });
/// ```
#[macro_export]
macro_rules! do_once_lite {
    ($e:expr) => {{
        #[link_section = ".data..once"]
        static ONCE: $crate::once_lite::OnceLite = $crate::once_lite::OnceLite::new();
        ONCE.call_once($e)
    }};
}
