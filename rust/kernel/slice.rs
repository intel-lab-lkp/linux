// SPDX-License-Identifier: GPL-2.0

//! Additional (and temporary) slice helpers.

/// Extension trait providing a portable version of [`as_flattened`] and
/// [`as_flattened_mut`].
///
/// In Rust 1.80, the previously unstable `slice::flatten` family of methods
/// have been stabilized and renamed from `flatten` to `as_flattened`.
///
/// This creates an issue for as long as the MSRV is < 1.80, as the same functionality is provided
/// by different methods depending on the compiler version.
///
/// This extension trait solves this by abstracting `as_flatten` and calling the correct  method
/// depending on the Rust version.
///
/// This trait can be removed once the MSRV passes 1.80.
///
/// [`as_flattened`]: slice::as_flattened
/// [`as_flattened_mut`]: slice::as_flattened_mut
pub trait AsFlattened<T> {
    /// Takes an `&[[T; N]]` and flattens it to a `&[T]`.
    ///
    /// This is an portable layer on top of [`as_flattened`]; see its documentation for details.
    ///
    /// [`as_flattened`]: slice::as_flattened
    fn as_flattened_slice(&self) -> &[T];

    /// Takes an `&mut [[T; N]]` and flattens it to a `&mut [T]`.
    ///
    /// This is an portable layer on top of [`as_flattened_mut`]; see its documentation for details.
    ///
    /// [`as_flattened_mut`]: slice::as_flattened_mut
    fn as_flattened_slice_mut(&mut self) -> &mut [T];
}

impl<T, const N: usize> AsFlattened<T> for [[T; N]] {
    #[allow(clippy::incompatible_msrv)]
    fn as_flattened_slice(&self) -> &[T] {
        #[cfg(not(CONFIG_RUSTC_HAS_SLICE_AS_FLATTENED))]
        {
            self.flatten()
        }

        #[cfg(CONFIG_RUSTC_HAS_SLICE_AS_FLATTENED)]
        {
            self.as_flattened()
        }
    }

    #[allow(clippy::incompatible_msrv)]
    fn as_flattened_slice_mut(&mut self) -> &mut [T] {
        #[cfg(not(CONFIG_RUSTC_HAS_SLICE_AS_FLATTENED))]
        {
            self.flatten_mut()
        }

        #[cfg(CONFIG_RUSTC_HAS_SLICE_AS_FLATTENED)]
        {
            self.as_flattened_mut()
        }
    }
}
