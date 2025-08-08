// SPDX-License-Identifier: GPL-2.0

//! Safety related APIs.

/// Checks that preconditions of an unsafe function are followed.
///
/// The check is enabled at runtime if debug assertions (`CONFIG_RUST_DEBUG_ASSERTIONS`)
/// are enabled. Otherwise, this macro is no-op.
///
/// # Examples
///
/// ```no_run
/// # use kernel::unsafe_precondition_assert;
/// # use kernel::cpu::{nr_cpu_ids, CpuId};
/// /// Creates a [`CpuId`] from the given `id` without bound checks.
/// ///
/// /// # Safety
/// ///
/// /// The caller must ensure that `id` is a valid CPU ID (i.e, `0 <= id < nr_cpu_ids()`).
/// unsafe fn new_cpu_id_unchecked(id: i32) -> CpuId {
///     let max_cpus = nr_cpu_ids();
///
///     unsafe_precondition_assert!(id >= 0, "id ({}) must be positive", id);
///
///     unsafe_precondition_assert!(
///         id < max_cpus, "id ({}) must be less than total CPUs ({})", id, max_cpus
///     );
///
///     CpuId(id)
/// }
/// ```
///
/// # Panics
///
/// Panics if the expression is evaluated to `false` at runtime.
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
