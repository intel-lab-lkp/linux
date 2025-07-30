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
/// /// The caller must ensure that interpreting the bytes of `[T; N]` as `[U; N]` is valid.
/// ///
/// /// This requires:
/// /// - `T` and `U` must have same size.
/// /// - The bit pattern of `T` must be valid for `U`.
/// /// - The alignment of `T` must be at least as strict as `U`.
/// unsafe fn transmute_array<const N: usize, T: Copy, U: Copy>(input: [T; N]) -> [U; N] {
///     unsafe_precondition_assert!(
///         core::mem::size_of::<T>() == core::mem::size_of::<U>(),
///         "src and dst must have the same size"
///     );
///
///     unsafe_precondition_assert!(
///         core::mem::align_of::<T>() >= core::mem::align_of::<U>(),
///         "src alignment must be compatible with dst alignment"
///     );
///
///     unsafe { core::mem::transmute_copy(&input) }
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
