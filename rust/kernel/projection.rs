// SPDX-License-Identifier: GPL-2.0

//! Infrastructure for handling projections.

use core::{
    mem::MaybeUninit,
    ops::Deref, //
};

use crate::{
    build_error,
    error::{
        code::ERANGE,
        Error, //
    }, //
};

/// Error raised when a projection is attempted on array or slices out of bounds.
pub struct OutOfBound;

impl From<OutOfBound> for Error {
    #[inline(always)]
    fn from(_: OutOfBound) -> Self {
        ERANGE
    }
}

/// A helper trait to perform index projection.
///
/// This is similar to `core::slice::SliceIndex`, but operate on raw pointers safely and fallibly.
///
/// # Safety
///
/// `get` must return a pointer in bounds of the provided pointer.
#[doc(hidden)]
pub unsafe trait ProjectIndex<T: ?Sized>: Sized {
    type Output: ?Sized;

    /// Returns an index-projected pointer, if in bounds.
    fn get(self, slice: *mut T) -> Option<*mut Self::Output>;

    /// Returns an index-projected pointer; fail the build if it cannot be proved to be in bounds.
    #[inline(always)]
    fn index(self, slice: *mut T) -> *mut Self::Output {
        Self::get(self, slice).unwrap_or_else(|| build_error!())
    }
}

// Forward array impl to slice impl.
// SAFETY: `get` returned pointers are in bounds.
unsafe impl<T, I, const N: usize> ProjectIndex<[T; N]> for I
where
    I: ProjectIndex<[T]>,
{
    type Output = <I as ProjectIndex<[T]>>::Output;

    #[inline(always)]
    fn get(self, slice: *mut [T; N]) -> Option<*mut Self::Output> {
        <I as ProjectIndex<[T]>>::get(self, slice)
    }

    #[inline(always)]
    fn index(self, slice: *mut [T; N]) -> *mut Self::Output {
        <I as ProjectIndex<[T]>>::index(self, slice)
    }
}

// SAFETY: `get` returned pointers are in bounds.
unsafe impl<T> ProjectIndex<[T]> for usize {
    type Output = T;

    #[inline(always)]
    fn get(self, slice: *mut [T]) -> Option<*mut T> {
        if self > slice.len() {
            None
        } else {
            Some(slice.cast::<T>().wrapping_add(self))
        }
    }
}

// SAFETY: `get` returned pointers are in bounds.
unsafe impl<T> ProjectIndex<[T]> for core::ops::Range<usize> {
    type Output = [T];

    #[inline(always)]
    fn get(self, slice: *mut [T]) -> Option<*mut [T]> {
        let new_len = self.end.checked_sub(self.start)?;
        if self.end > slice.len() {
            return None;
        }
        Some(core::ptr::slice_from_raw_parts_mut(
            slice.cast::<T>().wrapping_add(self.start),
            new_len,
        ))
    }
}

// SAFETY: `get` returned pointers are in bounds.
unsafe impl<T> ProjectIndex<[T]> for core::ops::RangeTo<usize> {
    type Output = [T];

    #[inline(always)]
    fn get(self, slice: *mut [T]) -> Option<*mut [T]> {
        (0..self.end).get(slice)
    }
}

// SAFETY: `get` returned pointers are in bounds.
unsafe impl<T> ProjectIndex<[T]> for core::ops::RangeFrom<usize> {
    type Output = [T];

    #[inline(always)]
    fn get(self, slice: *mut [T]) -> Option<*mut [T]> {
        (self.start..slice.len()).get(slice)
    }
}

// SAFETY: `get` returned pointers are in bounds.
unsafe impl<T> ProjectIndex<[T]> for core::ops::RangeFull {
    type Output = [T];

    #[inline(always)]
    fn get(self, slice: *mut [T]) -> Option<*mut [T]> {
        Some(slice)
    }
}

/// A helper trait to perform field projection.
///
/// This trait has a `DEREF` generic parameter so it can be implemented twice for types that
/// implement `Deref`. This will cause an ambiguity error and thus block `Deref` types being used
/// as base of projection, as they can inject unsoundness.
///
/// # Safety
///
/// `proj` should invoke `f` with valid allocation, as documentation described.
#[doc(hidden)]
pub unsafe trait ProjectField<const DEREF: bool> {
    /// Project a pointer to a type to a pointer of a field.
    ///
    /// `f` is always invoked with valid allocation so it can safely obtain raw pointers to fields
    /// using `&raw mut`.
    ///
    /// This is needed because `base` might not point to a valid allocation, while `&raw mut`
    /// requires pointers to be in bounds of a valid allocation.
    ///
    /// # Safety
    ///
    /// `f` must returns a pointer in bounds of the provided pointer.
    unsafe fn proj<F>(base: *mut Self, f: impl FnOnce(*mut Self) -> *mut F) -> *mut F;
}

// SAFETY: `proj` invokes `f` with valid allocation.
unsafe impl<T> ProjectField<false> for T {
    #[inline(always)]
    unsafe fn proj<F>(base: *mut Self, f: impl FnOnce(*mut Self) -> *mut F) -> *mut F {
        // Create a valid allocation to start projection, as `base` is not necessarily so.
        let mut place = MaybeUninit::uninit();
        let place_base = place.as_mut_ptr();
        let field = f(place_base);
        // SAFETY: `field` is in bounds from `base` per safety requirement.
        let offset = unsafe { field.byte_offset_from(place_base) };
        base.wrapping_byte_offset(offset).cast()
    }
}

// SAFETY: vacuously satisfied.
unsafe impl<T: Deref> ProjectField<true> for T {
    #[inline(always)]
    unsafe fn proj<F>(_: *mut Self, _: impl FnOnce(*mut Self) -> *mut F) -> *mut F {
        build_error!("this function is a guard against `Deref` impl and is never invoked");
    }
}

/// Create a projection from a raw pointer.
///
/// Supported projections include field projections and index projections.
/// It is not allowed to project into types that implement custom `Deref` or `Index`.
///
/// The macro has basic syntax of `kernel::project_pointer!(ptr, projection)`, where `ptr` is an
/// expression that evaluates to a raw pointer which serves as the base of projection. `projection`
/// can be a projection expression of form `.field` (normally identifer, or numeral in case of
/// tuple structs) or of form `[index]`.
///
/// If mutable pointer is needed, the macro input can be prefixed with `mut` keyword, i.e.
/// `kernel::project_pointer!(mut ptr, projection)`. By default, a const pointer is created.
///
/// `project_pointer!` macro can perform both fallible indexing and build-time checked indexing.
/// `[index]` form performs build-time bounds checking; if compiler fails to prove `[index]` is in
/// bounds, compilation will fail. `[index]?` can be used to perform runtime bounds checking;
/// `OutOfBound` error is raised via `?` if the index is out of bounds.
///
/// # Examples
///
/// Field projections are performed with `.field_name`:
/// ```
/// struct MyStruct { field: u32, }
/// let ptr: *const MyStruct = core::ptr::dangling();
/// let field_ptr: *const u32 = kernel::project_pointer!(ptr, .field);
///
/// struct MyTupleStruct(u32, u32);
/// let ptr: *const MyTupleStruct = core::ptr::dangling();
/// let field_ptr: *const u32 = kernel::project_pointer!(ptr, .1);
/// ```
///
/// Index projections are performed with `[index]`:
/// ```
/// let ptr: *const [u8; 32] = core::ptr::dangling();
/// let field_ptr: *const u8 = kernel::project_pointer!(ptr, [1]);
/// // This will fail the build.
/// // kernel::project_pointer!(ptr, [128]);
/// // This will raise an `OutOfBound` error (which is convertable to `ERANGE`).
/// // kernel::project_pointer!(ptr, [128]?);
/// ```
///
/// If you need to match on the error instead of propagate, put the invocation inside a closure:
/// ```
/// let ptr: *const [u8; 32] = core::ptr::dangling();
/// let field_ptr: Result<*const u8> = (|| -> Result<_> {
///     Ok(kernel::project_pointer!(ptr, [128]?))
/// })();
/// assert!(field_ptr.is_err());
/// ```
///
/// For mutable pointers, put `mut` as the first token in macro invocation.
/// ```
/// let ptr: *mut [(u8, u16); 32] = core::ptr::dangling_mut();
/// let field_ptr: *mut u16 = kernel::project_pointer!(mut ptr, [1].1);
/// ```
#[macro_export]
macro_rules! project_pointer {
    (@gen $ptr:ident, ) => {};
    // Field projection. `$field` needs to be `tt` to support tuple index like `.0`.
    (@gen $ptr:ident, .$field:tt $($rest:tt)*) => {
        // SAFETY: the provided closure always return in bounds pointer.
        let $ptr = unsafe {
            $crate::projection::ProjectField::proj($ptr, #[inline(always)] |ptr| {
                // SAFETY: `$field` is in bounds, and no implicit `Deref` is possible (if the
                // type implements `Deref`, Rust cannot infer the generic parameter `DEREF`).
                &raw mut (*ptr).$field
            })
        };
        $crate::project_pointer!(@gen $ptr, $($rest)*)
    };
    // Fallible index projection.
    (@gen $ptr:ident, [$index:expr]? $($rest:tt)*) => {
        let $ptr = $crate::projection::ProjectIndex::get($index, $ptr)
            .ok_or($crate::projection::OutOfBound)?;
        $crate::project_pointer!(@gen $ptr, $($rest)*)
    };
    // Build-time checked index projection.
    (@gen $ptr:ident, [$index:expr] $($rest:tt)*) => {
        let $ptr = $crate::projection::ProjectIndex::index($index, $ptr);
        $crate::project_pointer!(@gen $ptr, $($rest)*)
    };
    (mut $ptr:expr, $($proj:tt)*) => {{
        let ptr = $ptr;
        $crate::project_pointer!(@gen ptr, $($proj)*);
        ptr
    }};
    ($ptr:expr, $($proj:tt)*) => {{
        let ptr = $ptr.cast_mut();
        // We currently always project using mutable pointer, as it is not decided whether `&raw
        // const` allows the resulting pointer to be mutated (see documentation of `addr_of!`).
        $crate::project_pointer!(@gen ptr, $($proj)*);
        ptr.cast_const()
    }};
}
