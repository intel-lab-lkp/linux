// SPDX-License-Identifier: GPL-2.0

//! This module contains the kernel APIs for verifying invariants
//! required by the unsafe code.

/// Checks that preconditions of an unsafe code are followed.
///
/// The check is enabled at runtime if debug assertions (`CONFIG_RUST_DEBUG_ASSERTIONS`)
/// are enabled. In release builds, this macro is no-op.
///
/// # Examples
///
/// ```
/// // SAFETY: The caller ensures the size and alignment
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
///     core::mem::transmute_copy(&input)
/// }
/// ```
///
/// # Panics
///
/// This will invoke the [`panic!`] macro if the provided expression cannot be evaluated
/// to true at runtime.
#[macro_export]
macro_rules! unsafe_precondition_assert {
    ($($arg:tt)*) => {
        if cfg!(debug_assertions) {
            crate::pr_err!("unsafe precondition(s) violated");
            ::core::assert!($($arg)*);
        }
    };
}
