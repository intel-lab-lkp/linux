// SPDX-License-Identifier: GPL-2.0

//! Additional (and temporary) `usize` helpers.

/// Extension trait providing a portable version of [`usize::is_multiple_of`].
///
/// `usize::is_multiple_of` was stabilized in Rust 1.87.0. This extension trait
/// provides the same functionality for kernels built with older toolchains.
///
/// This trait can be removed once the MSRV passes 1.87.
///
/// [`usize::is_multiple_of`]: https://doc.rust-lang.org/std/primitive.usize.html#method.is_multiple_of
#[cfg(not(CONFIG_RUSTC_HAS_USIZE_IS_MULTIPLE_OF))]
pub trait UsizeExt {
    /// Returns `true` if `self` is a multiple of `rhs`.
    ///
    /// This is a portable layer on top of [`usize::is_multiple_of`]; see its documentation for
    /// details.
    ///
    /// [`usize::is_multiple_of`]: https://doc.rust-lang.org/std/primitive.usize.html#method.is_multiple_of
    fn is_multiple_of(self, rhs: usize) -> bool;
}

#[cfg(not(CONFIG_RUSTC_HAS_USIZE_IS_MULTIPLE_OF))]
impl UsizeExt for usize {
    #[inline]
    fn is_multiple_of(self, rhs: usize) -> bool {
        self % rhs == 0
    }
}
