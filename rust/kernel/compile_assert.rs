// SPDX-License-Identifier: GPL-2.0

//! Compile-time asserts.

/// Asserts that the given type is [`Sync`]. This check is done at compile time and does nothing
/// at runtime.
///
/// Note that this is only intended to avoid regressions and for sanity checks.
///
/// # Examples
/// ```
/// # use kernel::compile_assert::assert_sync;
/// # use kernel::types::NotThreadSafe;
///
///
/// // Do the assertion in a const block to make sure it won't be executed at runtime.
/// const _:() = {
///     assert_sync::<i32>(); // Succeeds because `i32` is Sync
///     // assert_sync::<NotThreadSafe>(); // Fails because `NotThreadSafe` is not `Sync`.
/// };
///
/// ```
#[inline(always)]
pub const fn assert_sync<T: ?Sized + Sync>() {}
