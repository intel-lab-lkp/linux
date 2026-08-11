// SPDX-License-Identifier: GPL-2.0

//! Abstractions for sysfs device attributes.
//!
//! C header: [`include/linux/sysfs.h`](srctree/include/linux/sysfs.h)
//! C header: [`include/linux/device.h`](srctree/include/linux/device.h)
//!
//! A sysfs attribute is a file in a device's sysfs directory. Reading the file
//! invokes the attribute's `show` callback, writing it invokes `store`.
//!
//! The types here are built to live in `static`s: they are constructed in const
//! context, handed to the C side as raw pointers, and never moved or mutated
//! from Rust afterwards. That is what makes the [`Send`]/[`Sync`] impls below
//! sound, and it is why every constructor is a `const fn`.
//!
//! The usual flow is:
//!
//! 1. One [`DeviceAttribute`] per file, each parameterised by a distinct `ID`
//!    so that a single type can implement [`AttributeOperations`] several times.
//! 2. An [`AttributeList`] collecting the raw attribute pointers, NULL
//!    terminated as the C side expects.
//! 3. An [`AttributeGroup`] wrapping that list, and an [`AttributeGroups`]
//!    wrapping the group, which is what gets stored in `driver->dev_groups`.
//!
//! The [`attribute_list!`] macro builds all four layers.
//!
//! # Registration window
//!
//! Both trampolines recover the driver-private data from `dev->driver_data`,
//! which is only meaningful while a driver is bound. The groups built here must
//! therefore be installed through `driver->dev_groups`, which the driver core
//! creates after a successful `probe` and removes before `remove` runs. Hanging
//! them off `device_type::groups` or `dev->groups` instead would expose the
//! files from `device_add` onwards, i.e. before any driver has set the private
//! data, and the first read would dereference NULL.

use core::{
    marker::PhantomData,
    mem::MaybeUninit,
    pin::Pin,
    ptr, //
};

use crate::{
    bindings,
    device::{
        Bound,
        Device, //
    },
    error::Result,
    ffi::CStr,
    macros::vtable,
    page::PAGE_SIZE,
    types::Opaque, //
};

/// A single sysfs attribute of a device, i.e. one file in the device's sysfs
/// directory.
///
/// # Type parameters
///
/// * `ID` distinguishes attributes backed by the same operations type. Because
///   the operations are supplied by a trait implemented *on* that type, a device
///   with several attributes needs several impls, and `ID` is what keeps them
///   apart.
/// * `D` is the driver-private data type the trampolines will recover from the
///   device, tied to `O` through `AttributeOperations::Data`.
/// * `O` supplies the `show`/`store` implementations.
///
/// # Invariants
///
/// `attribute` contains an initialised [`bindings::device_attribute`] whose
/// `show`/`store` function pointers, when non-NULL, are the trampolines
/// [`Self::show`] and [`Self::store`] of *this* type. It is never mutated after
/// construction.
#[repr(transparent)]
pub struct DeviceAttribute<const ID: u64, D, O: AttributeOperations<ID, Data = D>> {
    attribute: Opaque<bindings::device_attribute>,
    _phantom: PhantomData<O>,
}

impl<const ID: u64, D, O> DeviceAttribute<ID, D, O>
where
    O: AttributeOperations<ID, Data = D>,
{
    /// Trampoline invoked by the sysfs core when userspace reads the file
    /// backing this attribute.
    ///
    /// Returns the number of bytes written into `page`, or a negative errno.
    ///
    /// # Safety
    ///
    /// * `dev` must point to a valid `struct device` which stays valid for the
    ///   duration of this call.
    /// * `dev` must have a driver bound for the duration of this call, and its
    ///   private data must be a live `O::Data` installed by that driver.
    /// * `page` must point to a buffer that is valid for writes of at least
    ///   [`PAGE_SIZE`] bytes and that is not aliased for the duration of this
    ///   call.
    /// * `item` must point to the `device_attribute` this trampoline was
    ///   installed on, i.e. the one owned by a `Self`.
    ///
    /// The sysfs core upholds the first and third requirements: it passes the
    /// device the attribute was registered against and a freshly allocated page
    /// that it hands out to exactly one reader at a time, and kernfs holds an
    /// active reference across the call so the group cannot be torn down
    /// underneath it. The second is upheld by registering the group through
    /// `driver->dev_groups`; see the module docs.
    unsafe extern "C" fn show(
        dev: *mut bindings::device,
        // Unused: the operations are selected statically through `O`, so we
        // never need to look at the attribute we were called on.
        _item: *mut bindings::device_attribute,
        page: *mut kernel::ffi::c_char,
    ) -> isize {
        // SAFETY: By the function safety requirements, `dev` points to a valid
        // `struct device` that outlives this call, and therefore outlives the
        // reference derived from it, and it has a driver bound for that whole
        // period, which is what the `Bound` context asserts.
        let dev = unsafe { Device::<Bound>::from_raw(dev) };

        // SAFETY: By the function safety requirements, the private data of
        // `dev` is a live `O::Data` installed by the bound driver, so it is
        // sound to view it as such. The driver core does not clear the private
        // data until after these files are gone, so the borrow cannot outlive
        // the allocation.
        let data = unsafe { dev.drvdata_unchecked::<O::Data>() };

        // SAFETY: By the function safety requirements, `page` is valid for
        // writes of `PAGE_SIZE` bytes and is not aliased for the duration of
        // this call, so creating a unique reference to it is sound. `c_char`
        // and `u8` have the same size and alignment (1), so the cast to
        // `[u8; PAGE_SIZE]` preserves layout and cannot introduce a misaligned
        // access.
        let page = unsafe { &mut *(page.cast::<[u8; PAGE_SIZE]>()) };

        match O::show(data, dev, page) {
            // Clamped so that a buggy implementation reporting more than a page
            // cannot be reinterpreted as a negative value, i.e. silently turned
            // into an errno, by the cast.
            Ok(size) => size.min(PAGE_SIZE) as isize,
            Err(err) => err.to_errno() as isize,
        }
    }

    /// Trampoline invoked by the sysfs core when userspace writes to the file
    /// backing this attribute.
    ///
    /// Returns the number of bytes consumed, or a negative errno. Returning
    /// fewer bytes than `size` makes userspace retry with the remainder, so
    /// implementations that consumed the whole buffer must return `size`.
    ///
    /// # Safety
    ///
    /// * `dev` must point to a valid `struct device` which stays valid for the
    ///   duration of this call.
    /// * `dev` must have a driver bound for the duration of this call, and its
    ///   private data must be a live `O::Data` installed by that driver.
    /// * `page` must point to a buffer that is valid for reads of at least
    ///   `size` bytes and that is not mutated for the duration of this call.
    /// * `item` must point to the `device_attribute` this trampoline was
    ///   installed on, i.e. the one owned by a `Self`.
    unsafe extern "C" fn store(
        dev: *mut bindings::device,
        // Unused, see `show` above.
        _item: *mut bindings::device_attribute,
        page: *const kernel::ffi::c_char,
        size: usize,
    ) -> isize {
        // SAFETY: By the function safety requirements, `dev` points to a valid
        // `struct device` that outlives the derived reference, and it has a
        // driver bound for that whole period, which is what the `Bound` context
        // asserts.
        let dev = unsafe { Device::<Bound>::from_raw(dev) };

        // SAFETY: By the function safety requirements, the private data of
        // `dev` is a live `O::Data` installed by the bound driver, so it is
        // sound to view it as such. See `show` above.
        let data = unsafe { dev.drvdata_unchecked::<O::Data>() };

        match O::store(
            data,
            dev,
            // SAFETY: By the function safety requirements, `page` is valid for
            // reads of `size` bytes and is not mutated while this call runs, so
            // it may be viewed as a shared slice. `c_char` and `u8` share size
            // and alignment, and `size` bytes cannot exceed `isize::MAX`
            // because the buffer is a single kernel page.
            unsafe { core::slice::from_raw_parts(page.cast(), size) },
        ) {
            // Clamped so that a buggy implementation reporting more than a page
            // cannot be reinterpreted as a negative value, i.e. silently turned
            // into an errno, by the cast.
            Ok(size) => size.min(PAGE_SIZE) as isize,
            Err(err) => err.to_errno() as isize,
        }
    }

    /// Creates a new attribute, which will appear as a file named `name` in the
    /// device's sysfs directory.
    ///
    /// The file's mode is derived from which of [`AttributeOperations::show`]
    /// and [`AttributeOperations::store`] `O` actually implements, so an
    /// attribute is never readable without a `show` nor writable without a
    /// `store`.
    ///
    /// `name` is `'static` because the C side stores the pointer and
    /// dereferences it for as long as the attribute is registered.
    pub const fn new(name: &'static CStr) -> Self {
        Self {
            attribute: Opaque::new(bindings::device_attribute {
                attr: bindings::attribute {
                    // The pointer stays valid for as long as the attribute is
                    // registered because `name` is `'static`.
                    name: crate::str::as_char_ptr_in_const_context(name),
                    // `S_IRUSR` (0o400) if readable, `S_IWUSR` (0o200) if
                    // writable. Only the owner gets access; drivers that want
                    // wider permissions need a different constructor.
                    mode: if O::HAS_SHOW { 0o400 } else { 0 }
                        | if O::HAS_STORE { 0o200 } else { 0 },
                },
                // Both bindgen anonymous unions hold a single `Option<fn>`
                // field, so initialising that one field initialises the whole
                // union; there is no padding to leave uninitialised.
                __bindgen_anon_1: bindings::device_attribute__bindgen_ty_1 {
                    show: if O::HAS_SHOW { Some(Self::show) } else { None },
                },
                __bindgen_anon_2: bindings::device_attribute__bindgen_ty_2 {
                    store: if O::HAS_STORE {
                        Some(Self::store)
                    } else {
                        None
                    },
                },
            }),
            _phantom: PhantomData,
        }
    }

    /// Returns a raw pointer to the embedded `struct attribute`, for building an
    /// [`AttributeList`].
    ///
    /// The returned pointer inherits the lifetime of `&self`; callers must not
    /// hand it to the C side unless `self` lives in a `static`.
    pub const fn as_raw_attribute(&self) -> *const bindings::attribute {
        // SAFETY: `Opaque::get` returns a valid pointer to the initialised
        // `device_attribute` (type invariant), and `attr` is the first field of
        // that struct, so projecting to it is in bounds. No reference to the
        // `device_attribute` is created, so concurrent writes from the C side
        // cannot be aliasing violations.
        unsafe { (&raw const (*self.attribute.get()).attr) }
    }
}

// SAFETY: The only operation on a `DeviceAttribute` is `as_raw_attribute`,
// which hands out a raw pointer and performs no access. The wrapped
// `device_attribute` is never mutated from Rust after construction, so there is
// no Rust-side state that needs synchronisation and `&Self` may be shared
// across threads.
unsafe impl<const ID: u64, D, O: AttributeOperations<ID, Data = D>> Sync
    for DeviceAttribute<ID, D, O>
{
}

// SAFETY: A `DeviceAttribute` owns no thread-affine state (it is a plain struct
// of a name pointer, a mode and two function pointers), so ownership can be
// transferred to another thread.
unsafe impl<const ID: u64, D, O: AttributeOperations<ID, Data = D>> Send
    for DeviceAttribute<ID, D, O>
{
}

/// Operations backing a single sysfs attribute.
///
/// Implemented on an operations type, once per attribute; the `ID` parameter is
/// what allows several impls on the same type. The driver's private data is
/// named separately by [`Self::Data`], so the two may but need not be the same
/// type. Both methods are optional: [`vtable`] generates the
/// `HAS_SHOW`/`HAS_STORE` constants that [`DeviceAttribute::new`] uses to decide
/// which C callbacks and which mode bits to install, so a method that is not
/// implemented is never called.
#[vtable]
pub trait AttributeOperations<const ID: u64> {
    /// The driver-private data type this attribute belongs to.
    ///
    /// Recovered from `dev->driver_data` by the trampolines, so it must be the
    /// type the bound driver actually stored there.
    ///
    /// The `Driver` trait that uses this needs to enforce this data type restriction.
    type Data: Sync;

    /// Called when userspace reads the attribute's file.
    ///
    /// Writes the textual representation of the value into `buf` and returns the
    /// number of bytes written, which must not exceed [`PAGE_SIZE`]. The
    /// contents of `buf` on entry are unspecified.
    fn show(
        _data: Pin<&Self::Data>,
        _dev: &Device<Bound>,
        _buf: &mut [u8; PAGE_SIZE],
    ) -> Result<usize> {
        // Unreachable: `HAS_SHOW` is `false` for this impl, so
        // `DeviceAttribute::new` leaves the C `show` pointer NULL. Reaching
        // here means the vtable was built inconsistently, hence a build error
        // rather than a runtime one.
        kernel::build_error!(kernel::error::VTABLE_DEFAULT_ERROR)
    }

    /// Called when userspace writes to the attribute's file.
    ///
    /// `buf` holds the bytes written by userspace and is not NUL terminated.
    /// Returns the number of bytes consumed; return `buf.len()` to signal that
    /// the whole write was accepted.
    fn store(_data: Pin<&Self::Data>, _dev: &Device<Bound>, _buf: &[u8]) -> Result<usize> {
        // Unreachable: `HAS_STORE` is `false` for this impl, so
        // `DeviceAttribute::new` leaves the C `store` pointer NULL. Reaching
        // here means the vtable was built inconsistently, hence a build error
        // rather than a runtime one.
        kernel::build_error!(kernel::error::VTABLE_DEFAULT_ERROR)
    }
}

/// A NULL terminated array of `N` attribute pointers, as consumed by
/// `struct attribute_group::attrs`.
///
/// `N` counts the terminator, so a group of two attributes uses
/// `AttributeList<3, _>`. `D` records the private data type every attribute in
/// the list expects, which is what stops a list built for one driver from being
/// wrapped in a group and attached to another.
///
/// # Invariants
///
/// The last element is NULL. Every other element points to a
/// [`bindings::attribute`] that is valid for as long as this list is reachable
/// from the C side, and belongs to a `DeviceAttribute` whose `Data` is `D`.
#[repr(transparent)]
pub struct AttributeList<const N: usize, D>([*const bindings::attribute; N], PhantomData<D>);

impl<const N: usize, D> AttributeList<N, D> {
    /// Creates a new attribute list from raw attribute pointers.
    ///
    /// # Safety
    ///
    /// * `list` must be NULL terminated, i.e. `list[N - 1]` must be NULL.
    /// * Every other element must point to a [`bindings::attribute`] that stays
    ///   valid for as long as the list is registered. In practice each must
    ///   come from [`DeviceAttribute::as_raw_attribute`] on a `static`.
    /// * Each of those attributes must belong to a `DeviceAttribute` whose
    ///   `Data` is `D`, since its trampoline will read the device's private data
    ///   as a `D`.
    pub const unsafe fn new(list: [*const bindings::attribute; N]) -> Self {
        // INVARIANT: The safety requirements guarantee NULL termination, the
        // validity of every other element, and that each belongs to a
        // `DeviceAttribute` over `D`.
        Self(list, PhantomData)
    }
}

// SAFETY: An `AttributeList` is an immutable array of raw pointers with no
// interior mutability and no operations beyond construction, so sharing `&Self`
// across threads cannot cause a data race. The pointees are `DeviceAttribute`s,
// which are themselves `Sync`.
unsafe impl<const N: usize, D> Sync for AttributeList<N, D> {}

// SAFETY: An `AttributeList` owns no thread-affine state, so ownership can be
// transferred between threads.
unsafe impl<const N: usize, D> Send for AttributeList<N, D> {}

/// A group of sysfs attributes, i.e. `struct attribute_group`.
///
/// A group with no name (as built below) places its attributes directly in the
/// device's sysfs directory; a named group would place them in a subdirectory.
/// `D` is carried over from the list so the private data type stays visible up
/// to the point of registration.
///
/// # Invariants
///
/// The wrapped `attribute_group` is initialised, its `attrs_const` field points
/// to a NULL terminated attribute array that outlives it, and all other fields
/// are zero.
#[repr(transparent)]
pub struct AttributeGroup<D>(Opaque<bindings::attribute_group>, PhantomData<D>);

impl<D> AttributeGroup<D> {
    /// Creates an unnamed group containing every attribute in `attributes`.
    pub const fn from_attribute_list<const N: usize>(
        attributes: &'static AttributeList<N, D>,
    ) -> Self {
        // INVARIANT: `attributes` is `'static`, so the array outlives the
        // group, and it is NULL terminated by `AttributeList`'s invariant.
        AttributeGroup(
            Opaque::new(bindings::attribute_group {
                __bindgen_anon_2: bindings::attribute_group__bindgen_ty_2 {
                    // `AttributeList` is `repr(transparent)` over
                    // `[*const attribute; N]`, so a pointer to the list is a
                    // pointer to its first element, i.e. a
                    // `*const *const attribute`.
                    attrs_const: ptr::from_ref(attributes).cast(),
                },
                // SAFETY: Every remaining field of `attribute_group` is a
                // pointer, an `Option<fn>`, a `umode_t` or a union of those, and
                // the all-zero bit pattern is valid for each of them: NULL name,
                // no `is_visible`/`is_bin_visible` callbacks, no bin attributes.
                // This is also what the C side expects from a statically
                // declared group.
                ..unsafe { MaybeUninit::zeroed().assume_init() }
            }),
            PhantomData,
        )
    }
}

// SAFETY: An `AttributeGroup` is never mutated from Rust after construction and
// exposes no operations, so there is no Rust-side state requiring
// synchronisation; the C side does its own locking on the group.
unsafe impl<D> Sync for AttributeGroup<D> {}

// SAFETY: An `AttributeGroup` owns no thread-affine state, so ownership can be
// transferred between threads.
unsafe impl<D> Send for AttributeGroup<D> {}

/// A NULL terminated array of attribute group pointers, as consumed by
/// `struct driver::dev_groups`.
///
/// Only a single group is supported; the array is fixed at two entries, the
/// group and the terminator.
///
/// # Invariants
///
/// Element 0 points to a valid [`bindings::attribute_group`] that outlives this
/// value, and element 1 is NULL.
#[repr(transparent)]
pub struct AttributeGroups<D>([*const bindings::attribute_group; 2], PhantomData<D>);

impl<D> AttributeGroups<D> {
    /// Creates a group array holding the single group `attribute_group`.
    pub const fn new(attribute_group: &'static AttributeGroup<D>) -> Self {
        // INVARIANT: `attribute_group` is a `'static` reference, so the pointer
        // stays valid forever, and the second element is the NULL terminator.
        //
        // The cast is layout-preserving: `AttributeGroup` is
        // `repr(transparent)` over `Opaque<attribute_group>` plus a zero-sized
        // `PhantomData`, and `Opaque` is itself `repr(transparent)` over the
        // underlying `attribute_group`.
        Self(
            [ptr::from_ref(attribute_group).cast(), ptr::null()],
            PhantomData,
        )
    }

    /// Returns a pointer to the group array, for storing in `dev_groups`.
    ///
    /// Only `driver->dev_groups` is a valid destination; see the module docs for
    /// why the earlier-created group fields are not.
    pub(crate) fn as_ptr(&self) -> *mut *const bindings::attribute_group {
        // This should be `*const *const attribute_group`, but the kernel's
        // `dev_groups` field requires `*mut *const attribute_group`, not sure
        // why.
        //
        // The C side only reads through the pointer, so handing out a `*mut`
        // derived from `&self` is fine as long as callers never write through
        // it. That is why this is `pub(crate)` rather than `pub`.
        self.0.as_ptr().cast_mut()
    }
}

// SAFETY: An `AttributeGroups` is an immutable array of raw pointers with no
// operations that access the pointees, so `&Self` may be shared across threads.
// The groups it points to are themselves `Sync`.
unsafe impl<D> Sync for AttributeGroups<D> {}

// SAFETY: An `AttributeGroups` owns no thread-affine state, so ownership can be
// transferred between threads.
unsafe impl<D> Send for AttributeGroups<D> {}

/// Builds the full attribute plumbing for a driver and evaluates to a
/// `&'static AttributeGroups` suitable for `driver->dev_groups`.
///
/// All three arguments are labelled. `data:` is the driver-private data type the
/// trampolines will recover from the device, `ops:` is the type implementing the
/// attribute operations, and `attributes:` is a comma separated list of
/// identifiers, one per sysfs file. Each identifier must name a `u64` constant,
/// which is used as the `ID` type parameter, and `ops:` must implement
/// `AttributeOperations<ID, Data = $data>` for each of them. The sysfs file name
/// is the lowercased identifier.
///
/// Everything the macro creates is a `static`, so the pointers handed to C
/// remain valid for the lifetime of the module.
///
/// # Examples
///
/// ```ignore
/// const POWER: u64 = 0;
/// const MODE: u64 = 1;
///
/// #[vtable]
/// impl AttributeOperations<POWER> for SampleDriver {
///     type Data = Self;
///
///     fn show(
///         _data: Pin<&SampleDriver>,
///         _dev: &Device<Bound>,
///         buf: &mut [u8; PAGE_SIZE],
///     ) -> Result<usize> {
///         buf[0] = b'h';
///         Ok(1)
///     }
///
///     fn store(
///         _data: Pin<&SampleDriver>,
///         _dev: &Device<Bound>,
///         buf: &[u8; PAGE_SIZE],
///     ) -> Result<usize> {
///         // use the data stored in buf
///         Ok(buf.len())
///     }
/// }
///
/// #[vtable]
/// impl AttributeOperations<MODE> for SampleDriver {
///     type Data = Self;
///
///     fn show(
///         _data: Pin<&SampleDriver>,
///         _dev: &Device<Bound>,
///         buf: &mut [u8; PAGE_SIZE],
///     ) -> Result<usize> {
///         buf[0] = b'1';
///         Ok(1)
///     }
/// }
///
/// let groups = kernel::attribute_list!(
///     data: SampleDriver,
///     ops: SampleDriver,
///     attributes: POWER, MODE,
/// );
/// ```
#[macro_export]
macro_rules! attribute_list {
    (
        data: $data: ty,
        ops: $ops: ty,
        attributes: $($attributes: ident),+
        $(,)?
    ) => {
        {
            use $crate::{
                c_str,
                sysfs::{
                    AttributeGroup,
                    AttributeGroups,
                    AttributeList,
                    DeviceAttribute,
                }
            };

            use core::ptr;

            // One `static` per attribute, named `<IDENT>_ATTR`. It must be a
            // `static` (not a `let`) so that the pointer taken below outlives
            // this block and can be handed to the C side.
            $(
                $crate::macros::paste!{
                    static [< $attributes:upper _ATTR >]: DeviceAttribute<$attributes, $data, $ops>
                        = DeviceAttribute::new(c_str!(stringify!([< $attributes:lower >])));
                }
            )+

            // One slot per attribute plus one for the NULL terminator. The
            // `let _ = $attributes;` is just a way to expand each repetition to
            // `+ 1` while keeping the metavariable in the expansion.
            const LEN: usize = 1 $( + { let _ = $attributes; 1})+;

            // SAFETY: The array holds exactly one pointer per attribute,
            // obtained from `as_raw_attribute` on a `static` (so valid for
            // 'static), followed by the required NULL terminator. Every
            // attribute was declared above with `$ops` as its operations type,
            // whose `Data` is `$data`, which matches the list's `D`.
            static ATTRIBUTE_LIST: AttributeList::<LEN, $data> = unsafe { AttributeList::new([
                    $(
                        $crate::macros::paste!{
                            [< $attributes:upper _ATTR >].as_raw_attribute()
                        },
                    )*
                    ptr::null()
                ]
            ) };

            // Wrap the list in a group, and the group in the NULL terminated
            // group array that `dev_groups` expects.
            static ATTRIBUTE_GROUP: AttributeGroup<$data>
                = AttributeGroup::from_attribute_list(&ATTRIBUTE_LIST);
            static ATTRIBUTE_GROUPS: AttributeGroups<$data>
                = AttributeGroups::new(&ATTRIBUTE_GROUP);

            // Evaluates to a `&'static AttributeGroups`; `ATTRIBUTE_GROUPS` is
            // a `static`, so the borrow outlives the block.
            &ATTRIBUTE_GROUPS
        }
    };
}
