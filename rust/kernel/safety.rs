// SPDX-License-Identifier: GPL-2.0

//! Safety related APIs.

/// Checks that preconditions of an unsafe function are followed.
///
/// The check is enabled at runtime if debug assertions (`CONFIG_RUST_DEBUG_ASSERTIONS`)
/// are enabled. Otherwise, this macro is no-op.
///
/// # Examples
///
/// ```
/// /// # Safety
/// ///
/// /// - `buf` must be non-null.
/// /// - `buf` must be 16-byte aligned.
/// /// - `len` must be multiple of [`PAGE_SIZE`].
/// unsafe fn foo(buf: *const u8, len: usize) {
///     unsafe_precondition_assert!(!buf.is_null(), "buf must not be null");
///     unsafe_precondition_assert!((buf as usize) % 16 == 0, "buf must be 16-byte aligned");
///     unsafe_precondition_assert!(
///         len % PAGE_SIZE == 0,
///         "len ({}) must be multiple of PAGE_SIZE ({})",
///         len,
///         PAGE_SIZE
///     );
///     // ...
/// }
/// ```
///
/// # Panics
///
/// Panics if the expression is evaluated to `false` at runtime.
///
#[macro_export]
macro_rules! unsafe_precondition_assert {
    ($cond:expr $(,)?) => {
        $crate::unsafe_precondition_assert!(@inner $cond, ::core::stringify!($cond))
    };

    ($cond:expr, $($arg:tt)+) => {
        $crate::unsafe_precondition_assert!(@inner $cond, ::core::format_args!($($arg)+))
    };

    (@inner $cond:expr, $msg:expr) => {
        ::core::debug_assert!($cond, "unsafe precondition(s) violated: {}", $msg) };
}
