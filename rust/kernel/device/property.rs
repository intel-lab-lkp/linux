// SPDX-License-Identifier: GPL-2.0

//! Unified device property interface.
//!
//! C header: [`include/linux/property.h`](srctree/include/linux/property.h)

use core::{mem::MaybeUninit, ptr};

use crate::{
    alloc::KVec,
    bindings,
    error::{to_result, Result},
    prelude::*,
    str::{CStr, CString},
    types::Opaque,
};

/// A reference-counted fwnode_handle.
///
/// This structure represents the Rust abstraction for a
/// C `struct fwnode_handle`. This implementation abstracts the usage of an
/// already existing C `struct fwnode_handle` within Rust code that we get
/// passed from the C side.
///
/// # Invariants
///
/// A `FwNode` instance represents a valid `struct fwnode_handle` created by the
/// C portion of the kernel.
///
/// Instances of this type are always reference-counted, that is, a call to
/// `fwnode_handle_get` ensures that the allocation remains valid at least until
/// the matching call to `fwnode_handle_put`.
#[repr(transparent)]
pub struct FwNode(Opaque<bindings::fwnode_handle>);

impl FwNode {
    /// Obtain the raw `struct fwnode_handle *`.
    pub(crate) fn as_raw(&self) -> *mut bindings::fwnode_handle {
        self.0.get()
    }

    /// Returns an object that implements [`Display`](core::fmt::Display) for
    /// printing the name of a node.
    pub fn display_name(&self) -> impl core::fmt::Display + '_ {
        struct FwNodeDisplayName<'a>(&'a FwNode);

        impl core::fmt::Display for FwNodeDisplayName<'_> {
            fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
                // SAFETY: self is valid by its type invariant
                let name = unsafe { bindings::fwnode_get_name(self.0.as_raw()) };
                if name.is_null() {
                    return Ok(());
                }
                // SAFETY: fwnode_get_name returns null or a valid C string and
                // name is not null
                let name = unsafe { CStr::from_char_ptr(name) };
                write!(f, "{name}")
            }
        }

        FwNodeDisplayName(self)
    }

    /// Returns an object that implements [`Display`](core::fmt::Display) for
    /// printing the full path of a node.
    pub fn display_path(&self) -> impl core::fmt::Display + '_ {
        struct FwNodeDisplayPath<'a>(&'a FwNode);

        impl core::fmt::Display for FwNodeDisplayPath<'_> {
            fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
                // The logic here is the same as the one in lib/vsprintf.c
                // (fwnode_full_name_string).

                // SAFETY: `self.0.as_raw()` is valid by its type invariant
                let num_parents = unsafe { bindings::fwnode_count_parents(self.0.as_raw()) };

                for depth in (0..=num_parents).rev() {
                    let fwnode = if depth == 0 {
                        self.0.as_raw()
                    } else {
                        // SAFETY: `self.0.as_raw()` is valid
                        unsafe { bindings::fwnode_get_nth_parent(self.0.as_raw(), depth) }
                    };

                    // SAFETY: fwnode is valid, it is either `self.0.as_raw()` or
                    // the return value of `bindings::fwnode_get_nth_parent` which
                    // returns a valid pointer to a fwnode_handle if the provided
                    // depth is within the valid range, which we know to be true.
                    let prefix = unsafe { bindings::fwnode_get_name_prefix(fwnode) };
                    if !prefix.is_null() {
                        // SAFETY: fwnode_get_name_prefix returns null or a
                        // valid C string
                        let prefix = unsafe { CStr::from_char_ptr(prefix) };
                        write!(f, "{prefix}")?;
                    }
                    write!(f, "{}", self.0.display_name())?;

                    if depth != 0 {
                        // SAFETY: `fwnode` is valid, because `depth` is
                        // a valid depth of a parent of `self.0.as_raw()`.
                        // `fwnode_get_nth_parent` increments the refcount and
                        // we are responsible to decrement it.
                        unsafe { bindings::fwnode_handle_put(fwnode) }
                    }
                }

                Ok(())
            }
        }

        FwNodeDisplayPath(self)
    }

    /// Checks if property is present or not.
    pub fn property_present(&self, name: &CStr) -> bool {
        // SAFETY: By the invariant of `CStr`, `name` is null-terminated.
        unsafe { bindings::fwnode_property_present(self.as_raw().cast_const(), name.as_char_ptr()) }
    }

    /// Returns firmware property `name` boolean value
    pub fn property_read_bool(&self, name: &CStr) -> bool {
        // SAFETY: `name` is non-null and null-terminated. `self.as_raw()` is valid
        // because `self` is valid.
        unsafe { bindings::fwnode_property_read_bool(self.as_raw(), name.as_char_ptr()) }
    }

    /// Returns the index of matching string `match_str` for firmware string property `name`
    pub fn property_match_string(&self, name: &CStr, match_str: &CStr) -> Result<usize> {
        // SAFETY: `name` and `match_str` are non-null and null-terminated. `self.as_raw` is
        // valid because `self` is valid.
        let ret = unsafe {
            bindings::fwnode_property_match_string(
                self.as_raw(),
                name.as_char_ptr(),
                match_str.as_char_ptr(),
            )
        };
        to_result(ret)?;
        Ok(ret as usize)
    }

    /// Returns firmware property `name` integer array values in a KVec
    pub fn property_read_array_vec<'fwnode, 'name, T: PropertyInt>(
        &'fwnode self,
        name: &'name CStr,
        len: usize,
    ) -> Result<PropertyGuard<'fwnode, 'name, KVec<T>>> {
        let mut val: KVec<T> = KVec::with_capacity(len, GFP_KERNEL)?;

        // SAFETY: `val.as_mut_ptr()` is valid because `KVec::with_capacity`
        // didn't return an error and it has at least space for `len` number
        // of elements.
        let err = unsafe { read_array_out_param::<T>(self, name, val.as_mut_ptr(), len) };
        let res = if err < 0 {
            Err(Error::from_errno(err))
        } else {
            // SAFETY: fwnode_property_read_int_array() writes exactly `len`
            // entries on success
            unsafe { val.set_len(len) }
            Ok(val)
        };
        Ok(PropertyGuard {
            inner: res,
            fwnode: self,
            name,
        })
    }

    /// Returns integer array length for firmware property `name`
    pub fn property_count_elem<T: PropertyInt>(&self, name: &CStr) -> Result<usize> {
        // SAFETY: `out_param` is allowed to be null because `len` is zero.
        let ret = unsafe { read_array_out_param::<T>(self, name, ptr::null_mut(), 0) };
        to_result(ret)?;
        Ok(ret as usize)
    }

    /// Returns the value of firmware property `name`.
    ///
    /// This method is generic over the type of value to read. Informally,
    /// the types that can be read are booleans, strings, unsigned integers and
    /// arrays of unsigned integers.
    ///
    /// Reading a `KVec` of integers is done with the separate
    /// method [`Self::property_read_array_vec`], because it takes an
    /// additional `len` argument.
    ///
    /// When reading a boolean, this method never fails. A missing property
    /// is interpreted as `false`, whereas a present property is interpreted
    /// as `true`.
    ///
    /// For more precise documentation about what types can be read, see
    /// the [implementors of Property][Property#implementors] and [its
    /// implementations on foreign types][Property#foreign-impls].
    ///
    /// # Examples
    ///
    /// ```
    /// # use kernel::{c_str, device::{Device, property::FwNode}, str::CString};
    /// fn examples(dev: &Device) -> Result {
    ///     let fwnode = dev.fwnode().ok_or(ENOENT)?;
    ///     let b: u32 = fwnode.property_read(c_str!("some-number")).required_by(dev)?;
    ///     if let Some(s) = fwnode.property_read::<CString>(c_str!("some-str")).optional() {
    ///         // ...
    ///     }
    ///     Ok(())
    /// }
    /// ```
    pub fn property_read<'fwnode, 'name, T: Property>(
        &'fwnode self,
        name: &'name CStr,
    ) -> PropertyGuard<'fwnode, 'name, T> {
        PropertyGuard {
            inner: T::read_from_fwnode_property(self, name),
            fwnode: self,
            name,
        }
    }
}

// SAFETY: Instances of `FwNode` are always reference-counted.
unsafe impl crate::types::AlwaysRefCounted for FwNode {
    fn inc_ref(&self) {
        // SAFETY: The existence of a shared reference guarantees that the refcount is non-zero.
        unsafe { bindings::fwnode_handle_get(self.as_raw()) };
    }

    unsafe fn dec_ref(obj: ptr::NonNull<Self>) {
        // SAFETY: The safety requirements guarantee that the refcount is non-zero.
        unsafe { bindings::fwnode_handle_put(obj.cast().as_ptr()) }
    }
}

/// Implemented for several types that can be read as properties.
///
/// Informally, this is implemented for strings, integers and arrays of
/// integers. It's used to make [`FwNode::property_read`] generic over the
/// type of property being read. There are also two dedicated methods to read
/// other types, because they require more specialized function signatures:
/// - [`property_read_bool`](Device::property_read_bool)
/// - [`property_read_array_vec`](Device::property_read_array_vec)
pub trait Property: Sized {
    /// Used to make [`FwNode::property_read`] generic.
    fn read_from_fwnode_property(fwnode: &FwNode, name: &CStr) -> Result<Self>;
}

impl Property for CString {
    fn read_from_fwnode_property(fwnode: &FwNode, name: &CStr) -> Result<Self> {
        let mut str: *mut u8 = ptr::null_mut();
        let pstr: *mut _ = &mut str;

        // SAFETY: `name` is non-null and null-terminated. `fwnode.as_raw` is
        // valid because `fwnode` is valid.
        let ret = unsafe {
            bindings::fwnode_property_read_string(fwnode.as_raw(), name.as_char_ptr(), pstr.cast())
        };
        to_result(ret)?;

        // SAFETY: `pstr` contains a non-null ptr on success
        let str = unsafe { CStr::from_char_ptr(*pstr) };
        Ok(str.try_into()?)
    }
}
/// Implemented for all integers that can be read as properties.
///
/// This helper trait is needed on top of the existing [`Property`]
/// trait to associate the integer types of various sizes with their
/// corresponding `fwnode_property_read_*_array` functions.
pub trait PropertyInt: Copy {
    /// # Safety
    ///
    /// Callers must uphold the same safety invariants as for the various
    /// `fwnode_property_read_*_array` functions.
    unsafe fn read_array_from_fwnode_property(
        fwnode: *const bindings::fwnode_handle,
        propname: *const ffi::c_char,
        val: *mut Self,
        nval: usize,
    ) -> ffi::c_int;
}
// This macro generates implementations of the traits `Property` and
// `PropertyInt` for integers of various sizes. Its input is a list
// of pairs separated by commas. The first element of the pair is the
// type of the integer, the second one is the name of its corresponding
// `fwnode_property_read_*_array` function.
macro_rules! impl_property_for_int {
    ($($int:ty: $f:ident),* $(,)?) => { $(
        impl PropertyInt for $int {
            unsafe fn read_array_from_fwnode_property(
                fwnode: *const bindings::fwnode_handle,
                propname: *const ffi::c_char,
                val: *mut Self,
                nval: usize,
            ) -> ffi::c_int {
                // SAFETY: The safety invariants on the trait require
                // callers to uphold the invariants of the functions
                // this macro is called with.
                unsafe {
                    bindings::$f(fwnode, propname, val.cast(), nval)
                }
            }
        }
    )* };
}
impl_property_for_int! {
    u8: fwnode_property_read_u8_array,
    u16: fwnode_property_read_u16_array,
    u32: fwnode_property_read_u32_array,
    u64: fwnode_property_read_u64_array,
    i8: fwnode_property_read_u8_array,
    i16: fwnode_property_read_u16_array,
    i32: fwnode_property_read_u32_array,
    i64: fwnode_property_read_u64_array,
}
/// # Safety
///
/// Callers must ensure that if `len` is non-zero, `out_param` must be
/// valid and point to memory that has enough space to hold at least
/// `len` number of elements.
unsafe fn read_array_out_param<T: PropertyInt>(
    fwnode: &FwNode,
    name: &CStr,
    out_param: *mut T,
    len: usize,
) -> ffi::c_int {
    // SAFETY: `name` is non-null and null-terminated.
    // `fwnode.as_raw` is valid because `fwnode` is valid.
    // `out_param` is valid and has enough space for at least
    // `len` number of elements as per the safety requirement.
    unsafe {
        T::read_array_from_fwnode_property(fwnode.as_raw(), name.as_char_ptr(), out_param, len)
    }
}
impl<T: PropertyInt, const N: usize> Property for [T; N] {
    fn read_from_fwnode_property(fwnode: &FwNode, name: &CStr) -> Result<Self> {
        let mut val: [MaybeUninit<T>; N] = [const { MaybeUninit::uninit() }; N];

        // SAFETY: `val.as_mut_ptr()` is valid and points to enough space for
        // `N` elements. Casting from `*mut MaybeUninit<T>` to `*mut T` is safe
        // because `MaybeUninit<T>` has the same memory layout as `T`.
        let ret = unsafe { read_array_out_param::<T>(fwnode, name, val.as_mut_ptr().cast(), N) };
        to_result(ret)?;

        // SAFETY: `val` is always initialized when
        // fwnode_property_read_<T>_array is successful.
        Ok(val.map(|v| unsafe { v.assume_init() }))
    }
}
impl<T: PropertyInt> Property for T {
    fn read_from_fwnode_property(fwnode: &FwNode, name: &CStr) -> Result<Self> {
        let val: [_; 1] = <[T; 1] as Property>::read_from_fwnode_property(fwnode, name)?;
        Ok(val[0])
    }
}

/// A helper for reading device properties.
///
/// Use [`Self::required_by`] if a missing property is considered a bug and
/// [`Self::optional`] otherwise.
///
/// For convenience, [`Self::or`] and [`Self::or_default`] are provided.
pub struct PropertyGuard<'fwnode, 'name, T> {
    /// The result of reading the property.
    inner: Result<T>,
    /// The fwnode of the property, used for logging in the "required" case.
    fwnode: &'fwnode FwNode,
    /// The name of the property, used for logging in the "required" case.
    name: &'name CStr,
}

impl<T> PropertyGuard<'_, '_, T> {
    /// Access the property, indicating it is required.
    ///
    /// If the property is not present, the error is automatically logged. If a
    /// missing property is not an error, use [`Self::optional`] instead. The
    /// device is required to associate the log with it.
    pub fn required_by(self, dev: &super::Device) -> Result<T> {
        if self.inner.is_err() {
            dev_err!(
                dev,
                "{}: property '{}' is missing\n",
                self.fwnode.display_path(),
                self.name
            );
        }
        self.inner
    }

    /// Access the property, indicating it is optional.
    ///
    /// In contrast to [`Self::required_by`], no error message is logged if
    /// the property is not present.
    pub fn optional(self) -> Option<T> {
        self.inner.ok()
    }

    /// Access the property or the specified default value.
    ///
    /// Do not pass a sentinel value as default to detect a missing property.
    /// Use [`Self::required_by`] or [`Self::optional`] instead.
    pub fn or(self, default: T) -> T {
        self.inner.unwrap_or(default)
    }
}

impl<T: Default> PropertyGuard<'_, '_, T> {
    /// Access the property or a default value.
    ///
    /// Use [`Self::or`] to specify a custom default value.
    pub fn or_default(self) -> T {
        self.inner.unwrap_or_default()
    }
}
