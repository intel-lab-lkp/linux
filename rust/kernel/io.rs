// SPDX-License-Identifier: GPL-2.0

//! Memory-mapped IO.
//!
//! C header: [`include/asm-generic/io.h`](srctree/include/asm-generic/io.h)

use core::{
    marker::PhantomData,
    mem::MaybeUninit, //
};

use crate::{
    bindings,
    prelude::*,
    ptr::{
        Alignment,
        KnownSize, //
    },
    transmute::{
        AsBytes,
        FromBytes, //
    },
    types::Either, //
};

pub mod mem;
pub mod poll;
pub mod register;
pub mod resource;

pub use crate::register;
pub use resource::Resource;

use register::LocatedRegister;

/// Physical address type.
///
/// This is a type alias to either `u32` or `u64` depending on the config option
/// `CONFIG_PHYS_ADDR_T_64BIT`, and it can be a u64 even on 32-bit architectures.
pub type PhysAddr = bindings::phys_addr_t;

/// Resource Size type.
///
/// This is a type alias to either `u32` or `u64` depending on the config option
/// `CONFIG_PHYS_ADDR_T_64BIT`, and it can be a u64 even on 32-bit architectures.
pub type ResourceSize = bindings::resource_size_t;

/// Untyped I/O region.
///
/// This type can be used when an I/O region without known type information has a compile-time known
/// minimum size (and a runtime known actual size).
///
/// This must be naturally aligned to `usize`.
///
/// # Invariants
///
/// Size of the region is at least as large as the `SIZE` generic parameter.
#[repr(C)]
#[cfg_attr(CONFIG_64BIT, repr(align(8)))]
#[cfg_attr(not(CONFIG_64BIT), repr(align(4)))]
pub struct Region<const SIZE: usize = 0> {
    inner: [u8],
}

impl<const SIZE: usize> Region<SIZE> {
    /// Create a raw mutable pointer from given base address and size.
    ///
    /// `size` should be at least as large as the minimum size `SIZE` to uphold the type invariant.
    ///
    /// Just like other methods on raw pointers, it is not unsafe to create a raw pointer
    /// that does not uphold the type invariants. However such pointers are not valid.
    #[inline]
    pub fn ptr_from_raw_parts_mut(base: *mut u8, size: usize) -> *mut Self {
        core::ptr::slice_from_raw_parts_mut(base, size) as *mut Region<SIZE>
    }

    /// Create a raw mutable pointer from given base address and size.
    ///
    /// The alignment of `base` is checked, and `size` is checked against the minimum size specified
    /// via const generics.
    #[inline]
    pub fn ptr_try_from_raw_parts_mut(base: *mut u8, size: usize) -> Result<*mut Self> {
        if size < SIZE || base.align_offset(size_of::<usize>()) != 0 {
            return Err(EINVAL);
        }

        Ok(Self::ptr_from_raw_parts_mut(base, size))
    }
}

impl<const SIZE: usize> KnownSize for Region<SIZE> {
    const MIN_SIZE: usize = SIZE;
    const MIN_ALIGN: Alignment = Alignment::new::<{ size_of::<usize>() }>();

    #[inline(always)]
    fn size(p: *const Self) -> usize {
        (p as *const [u8]).len()
    }
}

// SAFETY: I/O regions can compose of arbitrary bytes.
unsafe impl<const SIZE: usize> kernel::transmute::FromBytes for Region<SIZE> {}
// SAFETY: Values read from I/O are always treated as initialized.
unsafe impl<const SIZE: usize> kernel::transmute::AsBytes for Region<SIZE> {}

/// Raw representation of an MMIO region.
///
/// `MmioRaw<T>` is equivalent to `T __iomem *` in C.
///
/// By itself, the existence of an instance of this structure does not provide any guarantees that
/// the represented MMIO region does exist or is properly mapped.
///
/// Instead, the bus specific MMIO implementation must convert this raw representation into an
/// `Mmio` instance providing the actual memory accessors. Only by the conversion into an `Mmio`
/// structure any guarantees are given.
pub struct MmioRaw<T: ?Sized> {
    /// Pointer is in I/O address space.
    ///
    /// The provenance does not matter, only the address and metadata do.
    ptr: *mut T,
}

impl<T: ?Sized> Copy for MmioRaw<T> {}
impl<T: ?Sized> Clone for MmioRaw<T> {
    #[inline]
    fn clone(&self) -> Self {
        *self
    }
}

// SAFETY: `MmioRaw` is just an address, so is thread-safe.
unsafe impl<T: ?Sized> Send for MmioRaw<T> {}
// SAFETY: `MmioRaw` is just an address, so is thread-safe.
unsafe impl<T: ?Sized> Sync for MmioRaw<T> {}

impl<T> MmioRaw<T> {
    /// Create a `MmioRaw` from address.
    #[inline]
    pub fn new(addr: usize) -> Self {
        Self {
            ptr: core::ptr::without_provenance_mut(addr),
        }
    }
}

impl<const SIZE: usize> MmioRaw<Region<SIZE>> {
    /// Create a `MmioRaw` representing a I/O region with given size.
    ///
    /// The size is checked against the minimum size specified via const generics.
    #[inline]
    pub fn new_region(addr: usize, size: usize) -> Result<Self> {
        Ok(Self {
            ptr: Region::ptr_try_from_raw_parts_mut(core::ptr::without_provenance_mut(addr), size)?,
        })
    }
}

impl<T: ?Sized + KnownSize> MmioRaw<T> {
    /// Returns the base address of the MMIO region.
    #[inline]
    pub fn addr(&self) -> usize {
        self.ptr.addr()
    }

    /// Returns the size of the MMIO region.
    #[inline]
    pub fn size(&self) -> usize {
        KnownSize::size(self.ptr)
    }
}

/// Checks whether an access of type `U` at the given `base` and the given `offset`
/// is valid within this region.
///
/// The `base` is used for alignment checking only. This can be set to 0 to skip the check.
#[inline]
const fn offset_valid<U>(base: usize, offset: usize, size: usize) -> bool {
    let type_size = core::mem::size_of::<U>();
    if let Some(end) = offset.checked_add(type_size) {
        end <= size && (base.wrapping_add(offset) % type_size == 0)
    } else {
        false
    }
}

/// I/O backends.
///
/// This is an abstract representation to be implemented by arbitrary I/O
/// backends (e.g. MMIO, PCI config space, etc.).
///
/// The base trait only defines the projection operations; which I/O methods are available depends
/// on which [`IoCapable<T>`] traits are implemented for the type. For example, for MMIO regions,
/// all widths (u8, u16, u32, and u64 on 64-bit systems) are typically supported. For PCI
/// configuration space, u8, u16, and u32 are supported but u64 is not.
///
/// This trait is separate from the `Io` trait as multiple different I/O types may share the same
/// operation.
pub trait IoBackend {
    /// View type for this I/O backend.
    type View<'a, T: ?Sized + KnownSize>: IoBase<'a, Backend = Self, Target = T>;

    /// Convert a `view` to a raw pointer for projection.
    fn as_ptr<'a, T: ?Sized + KnownSize>(view: Self::View<'a, T>) -> *mut T;

    /// Project `view` to its subregion indicated by `ptr`.
    ///
    /// If input `view` is valid, returned view must also be valid.
    ///
    /// # Safety
    ///
    /// `ptr` must be a projection of `Self::as_ptr(view)`.
    unsafe fn project_view<'a, T: ?Sized + KnownSize, U: ?Sized + KnownSize>(
        view: Self::View<'a, T>,
        ptr: *mut U,
    ) -> Self::View<'a, U>;
}

/// Trait indicating that an I/O backend supports operations of a certain type and providing an
/// implementation for these operations.
///
/// Different I/O backends can implement this trait to expose only the operations they support.
///
/// For example, a PCI configuration space may implement `IoCapable<u8>`, `IoCapable<u16>`,
/// and `IoCapable<u32>`, but not `IoCapable<u64>`, while an MMIO region on a 64-bit
/// system might implement all four.
pub trait IoCapable<T>: IoBackend {
    /// Performs an I/O read of type `T` at `view` and returns the result.
    fn io_read<'a>(view: Self::View<'a, T>) -> T;

    /// Performs an I/O write of `value` at `view`.
    fn io_write<'a>(view: Self::View<'a, T>, value: T);
}

/// Trait indicating that an I/O backend supports memory copy operations.
///
/// # Safety
///
/// If [`Self::is_mapped`] is overridden, it must be correct per documentation.
pub unsafe trait IoCopyable: IoBackend {
    /// Whether the pointers for this I/O backend are in the CPU address space, and are coherently
    /// mapped.
    ///
    /// When this returns true, `Self::as_ptr(view)` must return a valid and aligned pointer. The
    /// pointer may be accessed with byte-wise atomic memory copy or volatile read/write.
    ///
    /// This is not an associated constants to support backends where the view may be conditionally
    /// mapped. This method should be marked as `#[inline(always)]` if it always returns true, so
    /// `build_assert!()` in `copy_{from,to}io` can see it.
    #[inline]
    fn is_mapped<T: ?Sized + KnownSize>(_view: Self::View<'_, T>) -> bool {
        false
    }

    /// Copy contents of `view` to `buffer`.
    ///
    /// # Safety
    ///
    /// - `buffer` is valid for volatile write for `view.size()` bytes.
    #[inline]
    unsafe fn copy_from_io(view: Self::View<'_, [u8]>, buffer: *mut u8) {
        build_assert!(Self::is_mapped(view));

        let ptr = Self::as_ptr(view);

        // Use `bindings::memcpy` instead of copy_nonoverlapping for volatile.
        // SAFETY:
        // - `is_mapped` guarantees `ptr` is in CPU address space and valid for read.
        // - `buffer` is valid for write for `view.size()` bytes which is equal to `ptr.len()`.
        unsafe { bindings::memcpy(buffer.cast(), ptr.cast(), ptr.len()) };
    }

    /// Copy `size` bytes from `buffer` to `address`.
    ///
    /// # Safety
    ///
    /// - `buffer` is valid for volatile read for `view.size()` bytes.
    #[inline]
    unsafe fn copy_to_io(view: Self::View<'_, [u8]>, buffer: *const u8) {
        build_assert!(Self::is_mapped(view));

        let ptr = Self::as_ptr(view);

        // Use `bindings::memcpy` instead of copy_nonoverlapping for volatile.
        // SAFETY:
        // - `is_mapped` guarantees `ptr` is in CPU address space and valid for write.
        // - `buffer` is valid for read for `view.size()` bytes which is equal to `ptr.len()`.
        unsafe { bindings::memcpy(ptr.cast(), buffer.cast(), ptr.len()) };
    }
}

/// Describes a given I/O location: its offset, width, and type to convert the raw value from and
/// into.
///
/// This trait is the key abstraction allowing [`Io::read`], [`Io::write`], and [`Io::update`] (and
/// their fallible [`try_read`](Io::try_read), [`try_write`](Io::try_write) and
/// [`try_update`](Io::try_update) counterparts) to work uniformly with both raw [`usize`] offsets
/// (for primitive types like [`u32`]) and typed ones (like those generated by the [`register!`]
/// macro).
///
/// An `IoLoc<Base, T>` carries the following pieces of information:
///
/// - The valid `Base` to operate on. For most registers, this should be [`Region`].
/// - The offset to access (returned by [`IoLoc::offset`]),
/// - The width of the access (determined by [`IoLoc::IoType`]),
/// - The type `T` in which the raw data is returned or provided.
///
/// `T` and `IoLoc::IoType` may differ: for instance, a typed register has `T` = the register type
/// with its bitfields, and `IoType` = its backing primitive (e.g. `u32`).
pub trait IoLoc<Base: ?Sized, T> {
    /// Size ([`u8`], [`u16`], etc) of the I/O performed on the returned [`offset`](IoLoc::offset).
    type IoType: Into<T> + From<T>;

    /// Consumes `self` and returns the offset of this location.
    fn offset(self) -> usize;
}

/// Implements [`IoLoc<Region<SIZE>, $ty>`] for [`usize`], allowing [`usize`] to be used as a
/// parameter of [`Io::read`] and [`Io::write`].
macro_rules! impl_usize_ioloc {
    ($($ty:ty),*) => {
        $(
            impl<const SIZE: usize> IoLoc<Region<SIZE>, $ty> for usize {
                type IoType = $ty;

                #[inline(always)]
                fn offset(self) -> usize {
                    self
                }
            }
        )*
    }
}

// Provide the ability to read any primitive type from a [`usize`].
impl_usize_ioloc!(u8, u16, u32, u64);

/// Types implementing this trait (e.g. MMIO BARs or PCI config regions)
/// can perform I/O operations on regions of memory.
///
/// This trait defines which backend shall be used for I/O operations and provides a method to
/// convert into [`IoBackend::View`]. Users should use the [`Io`] trait which provides the actual
/// methods to perform I/O operations.
pub trait IoBase<'a>: Copy {
    /// Type that defines all I/O operations.
    type Backend: IoBackend;

    /// Type of this I/O region. For untyped regions, [`Region`] can be used.
    type Target: ?Sized + KnownSize;

    /// Return a view that covers the full region.
    fn as_view(self) -> <Self::Backend as IoBackend>::View<'a, Self::Target>;
}

/// Extension trait to provide I/O operation methods to types that implement [`IoBase`].
///
/// This trait provides:
/// - Helper methods for offset validation and address calculation
/// - Fallible (runtime checked) accessors for different data widths
///
/// Which I/O methods are available depends on the associated [`IoBackend`] implementation.
pub trait Io<'a>: IoBase<'a> {
    /// Returns the size of this I/O region.
    #[inline]
    fn size(self) -> usize {
        KnownSize::size(Self::Backend::as_ptr(self.as_view()))
    }

    /// Returns the length of the slice in number of elements.
    #[inline]
    fn len<T>(self) -> usize
    where
        Self: Io<'a, Target = [T]>,
    {
        Self::Backend::as_ptr(self.as_view()).len()
    }

    /// Returns `true` if the slice has a length of 0.
    #[inline]
    fn is_empty<T>(self) -> bool
    where
        Self: Io<'a, Target = [T]>,
    {
        self.len() == 0
    }

    /// Try to convert into a different typed I/O view.
    ///
    /// The target type must be of same or smaller size to current type, and the current view must
    /// be properly aligned for the target type.
    ///
    /// # Examples
    ///
    /// ```no_run
    /// use kernel::io::{
    ///     io_project,
    ///     Mmio,
    ///     Io,
    ///     Region,
    /// };
    /// struct MyStruct { field: u32, }
    ///
    /// // SAFETY: All bit patterns are acceptable values for `MyStruct`.
    /// unsafe impl kernel::transmute::FromBytes for MyStruct {};
    /// // SAFETY: Instances of `MyStruct` have no uninitialized portions.
    /// unsafe impl kernel::transmute::AsBytes for MyStruct {};
    ///
    /// # fn test(mmio: &Mmio<'_, Region>) -> Result {
    /// // let mmio: Mmio<Region>;
    /// let whole: Mmio<'_, MyStruct> = mmio.try_cast()?;
    /// # Ok::<(), Error>(()) }
    /// ```
    #[inline]
    fn try_cast<U>(self) -> Result<<Self::Backend as IoBackend>::View<'a, U>>
    where
        Self::Target: FromBytes + AsBytes,
        U: FromBytes + AsBytes,
    {
        let view = self.as_view();
        let ptr = Self::Backend::as_ptr(view);

        if size_of::<U>() > KnownSize::size(ptr) {
            return Err(EINVAL);
        }

        if ptr.addr() % align_of::<U>() != 0 {
            return Err(EINVAL);
        }

        // SAFETY: We have checked bounds and alignment, so this is a valid projection.
        Ok(unsafe { Self::Backend::project_view(view, ptr.cast()) })
    }

    /// Read a value from I/O.
    ///
    /// This only works for primitives supported by the I/O backend.
    ///
    /// # Examples
    ///
    /// ```no_run
    /// # use kernel::io::*;
    /// # fn test_read_val(mmio: Mmio<'_, u32>) {
    /// // let mmio: Mmio<'_, u32>;
    /// let val: u32 = mmio.read_val();
    /// # }
    /// ```
    #[inline]
    fn read_val(self) -> Self::Target
    where
        Self::Backend: IoCapable<Self::Target>,
        Self::Target: Sized,
    {
        Self::Backend::io_read(self.as_view())
    }

    /// Write a value to I/O.
    ///
    /// This only works for primitives supported by the I/O backend.
    ///
    /// # Examples
    ///
    /// ```no_run
    /// # use kernel::io::*;
    /// # fn test_write_val(mmio: Mmio<'_, u32>) {
    /// // let mmio: Mmio<'_, u32>;
    /// mmio.write_val(1u32);
    /// # }
    /// ```
    #[inline]
    fn write_val(self, value: Self::Target)
    where
        Self::Backend: IoCapable<Self::Target>,
        Self::Target: Sized,
    {
        Self::Backend::io_write(self.as_view(), value)
    }

    /// Copy-read from I/O memory.
    ///
    /// There is no atomicity guarantee.
    ///
    /// # Examples
    ///
    /// ```no_run
    /// # use kernel::io::*;
    /// # fn test_copy_read(mmio: Mmio<'_, [u8; 6]>) {
    /// // let mmio: Mmio<'_, [u8; 6]>;
    /// let val: [u8; 6] = mmio.copy_read();
    /// # }
    /// ```
    #[inline]
    fn copy_read(self) -> Self::Target
    where
        Self::Backend: IoCopyable,
        Self::Target: Sized + FromBytes,
    {
        let view = self.as_view();

        // Optimized path if I/O backend is CPU mapped.
        if Self::Backend::is_mapped(view) {
            let ptr = Self::Backend::as_ptr(view);
            // SAFETY:
            // - `is_mapped` guarantees `ptr` is valid for read in CPU address space.
            // - Using read_volatile() here so that race with hardware is well-defined.
            // - Using read_volatile() here is not sound if it races with other CPU per Rust
            //   rules, but this is allowed per LKMM.
            return unsafe { ptr.read_volatile() };
        }

        // Project `self` to `[u8]`.
        let ptr = Self::Backend::as_ptr(view);
        // SAFETY: This is a identity projection.
        let slice_view = unsafe {
            Self::Backend::project_view(
                view,
                core::ptr::slice_from_raw_parts_mut::<u8>(ptr.cast(), size_of::<Self::Target>()),
            )
        };

        let mut buf = MaybeUninit::<Self::Target>::uninit();
        // SAFETY: `buf.as_mut_ptr()` is valid for write for `size_of::<T>()` bytes.
        unsafe { Self::Backend::copy_from_io(slice_view, buf.as_mut_ptr().cast()) };
        // SAFETY: T: FromBytes` guarantee that all bit patterns are valid.
        unsafe { buf.assume_init() }
    }

    /// Copy-write to I/O memory.
    ///
    /// There is no atomicity guarantee.
    ///
    /// # Examples
    ///
    /// ```no_run
    /// # use kernel::io::*;
    /// # fn test_copy_write(mmio: Mmio<'_, [u8; 6]>) {
    /// // let mmio: Mmio<'_, [u8; 6]>;
    /// mmio.copy_write([0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF]);
    /// # }
    /// ```
    #[inline]
    fn copy_write(self, value: Self::Target)
    where
        Self::Backend: IoCopyable,
        Self::Target: Sized + AsBytes,
    {
        let view = self.as_view();

        // Optimized path if I/O backend is CPU mapped.
        if Self::Backend::is_mapped(view) {
            let ptr = Self::Backend::as_ptr(view);
            // SAFETY:
            // - `is_mapped` guarantees `ptr` is valid for write in CPU address space.
            // - Using write_volatile() here so that race with hardware is well-defined.
            // - Using write_volatile() here is not sound if it races with other CPU per Rust
            //   rules, but this is allowed per LKMM.
            unsafe { ptr.write_volatile(value) };
            return;
        }

        // Project `self` to `[u8]`.
        let ptr = Self::Backend::as_ptr(view);
        // SAFETY: This is a identity projection.
        let slice_view = unsafe {
            Self::Backend::project_view(
                view,
                core::ptr::slice_from_raw_parts_mut::<u8>(ptr.cast(), size_of::<Self::Target>()),
            )
        };

        // SAFETY: `&raw const value` is valid for read for `size_of::<T>()` bytes.
        unsafe { Self::Backend::copy_to_io(slice_view, (&raw const value).cast()) };
        core::mem::forget(value);
    }

    /// Copy bytes from slice to I/O memory.
    ///
    /// The length of `self` must be the same as `data`, similar to [`[u8]::copy_from_slice`].
    ///
    /// # Examples
    ///
    /// ```no_run
    /// # use kernel::io::*;
    /// # fn test_copy_write(mmio: Mmio<'_, [u8]>) {
    /// // let mmio: Mmio<'_, [u8]>;
    /// mmio.copy_from_slice(&[0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF]);
    /// # }
    /// ```
    #[inline]
    fn copy_from_slice(self, data: &[u8])
    where
        Self::Backend: IoCopyable,
        Self: Io<'a, Target = [u8]>,
    {
        assert_eq!(self.len(), data.len());

        // SAFETY: `data.as_ptr()` is valid for read for `self.size()` bytes.
        unsafe {
            Self::Backend::copy_to_io(self.as_view(), data.as_ptr());
        }
    }

    /// Copy bytes from I/O memory to slice.
    ///
    /// The length of `self` must be the same as `data`, similar to [`[u8]::copy_from_slice`].
    ///
    /// # Examples
    ///
    /// ```no_run
    /// # use kernel::io::*;
    /// # fn test_copy_write(mmio: Mmio<'_, [u8]>) {
    /// // let mmio: Mmio<'_, [u8]>;
    /// let mut buf = [0; 6];
    /// mmio.copy_to_slice(&mut buf);
    /// # }
    /// ```
    #[inline]
    fn copy_to_slice(self, data: &mut [u8])
    where
        Self::Backend: IoCopyable,
        Self: Io<'a, Target = [u8]>,
    {
        assert_eq!(self.len(), data.len());

        // SAFETY: `data.as_ptr()` is valid for write for `self.size()` bytes.
        unsafe {
            Self::Backend::copy_from_io(self.as_view(), data.as_mut_ptr());
        }
    }

    /// Copy bytes from `data` I/O slice to the `self`.
    ///
    /// The length of `self` must be the same as `data`, similar to [`[u8]::copy_from_slice`].
    ///
    /// # Examples
    ///
    /// ```no_run
    /// # use kernel::io::*;
    /// # fn test_copy_write(dst: Mmio<'_, [u8]>, src: Mmio<'_, [u8]>) {
    /// // let dst: Mmio<'_, [u8]>;
    /// // let src: Mmio<'_, [u8]>;
    /// dst.copy_from_io_slice(src);
    /// # }
    /// ```
    fn copy_from_io_slice<'b, T>(self, data: T)
    where
        Self::Backend: IoCopyable,
        Self: Io<'a, Target = [u8]>,
        T: Io<'b, Target = [u8], Backend: IoCopyable>,
    {
        fn copy_from_io_slice_via_buffer<
            'a,
            'b,
            T: Io<'a, Target = [u8], Backend: IoCopyable>,
            U: Io<'b, Target = [u8], Backend: IoCopyable>,
        >(
            dest: T,
            src: U,
        ) {
            let mut buf = MaybeUninit::<[u8; 256]>::uninit();

            let mut offset = 0;
            let mut len = dest.len();

            while len != 0 {
                let copy_len = core::cmp::min(len, 256);

                // SAFETY: `buf.as_mut_ptr()` is valid for write for `copy_len` bytes as `copy_len
                // <= 256`.
                unsafe {
                    U::Backend::copy_from_io(
                        io_project!(src, [panic: offset..][panic: ..copy_len]),
                        buf.as_mut_ptr().cast(),
                    )
                };

                // SAFETY: `buf.as_ptr()` is valid for read for `copy_len` bytes as `copy_len <=
                // 256`.
                unsafe {
                    T::Backend::copy_to_io(
                        io_project!(dest, [panic: offset..][panic: ..copy_len]),
                        buf.as_ptr().cast(),
                    )
                };

                offset += copy_len;
                len -= copy_len;
            }
        }

        assert_eq!(self.len(), data.len());

        let dst_view = self.as_view();
        let src_view = data.as_view();

        if T::Backend::is_mapped(src_view) {
            // SAFETY: `T::Backend::as_ptr(src_view)` is valid for read for `data.len()`
            // bytes.
            unsafe {
                Self::Backend::copy_to_io(self.as_view(), T::Backend::as_ptr(src_view).cast())
            }
        } else if Self::Backend::is_mapped(dst_view) {
            // SAFETY: `Self::Backend::as_ptr(dst_view)` is valid for write for `data.len()`
            // bytes.
            unsafe {
                T::Backend::copy_from_io(data.as_view(), Self::Backend::as_ptr(dst_view).cast())
            }
        } else {
            copy_from_io_slice_via_buffer(dst_view, src_view)
        }
    }

    /// Copy bytes from `self` to the `data` I/O slice.
    ///
    /// The length of `self` must be the same as `data`, similar to [`[u8]::copy_from_slice`].
    ///
    /// # Examples
    ///
    /// ```no_run
    /// # use kernel::io::*;
    /// # fn test_copy_write(dst: Mmio<'_, [u8]>, src: Mmio<'_, [u8]>) {
    /// // let dst: Mmio<'_, [u8]>;
    /// // let src: Mmio<'_, [u8]>;
    /// src.copy_to_io_slice(dst);
    /// # }
    /// ```
    #[inline]
    fn copy_to_io_slice<'b, T>(self, data: T)
    where
        Self::Backend: IoCopyable,
        Self: Io<'a, Target = [u8]>,
        T: Io<'b, Target = [u8], Backend: IoCopyable>,
    {
        data.copy_from_io_slice(self)
    }

    /// Returns a view for a given `offset`, performing compile-time bound checks.
    // Always inline to optimize out error path of `build_assert`.
    #[inline(always)]
    fn io_addr_assert<U>(self, offset: usize) -> <Self::Backend as IoBackend>::View<'a, U> {
        // We cannot check alignment with `offset_valid` using `ptr.addr()`. So set 0 for it and
        // ensure alignment by checking that the alignment of `U` is smaller or equal to the
        // alignment of `Self::Target`.
        const_assert!(Alignment::of::<U>().as_usize() <= Self::Target::MIN_ALIGN.as_usize());
        build_assert!(offset_valid::<U>(0, offset, Self::Target::MIN_SIZE));

        let view = self.as_view();
        let ptr = Self::Backend::as_ptr(view);
        let projected_ptr = ptr.cast::<U>().wrapping_byte_add(offset);
        // SAFETY: `offset_valid` checks for size and alignment and therefore `projected_ptr` is a
        // valid projection.
        unsafe { Self::Backend::project_view(view, projected_ptr) }
    }

    /// Returns a view for a given `offset`, performing runtime bound checks.
    #[inline]
    fn io_addr<U>(self, offset: usize) -> Result<<Self::Backend as IoBackend>::View<'a, U>> {
        let view = self.as_view();
        let ptr = Self::Backend::as_ptr(view);

        if !offset_valid::<U>(ptr.addr(), offset, KnownSize::size(ptr)) {
            return Err(EINVAL);
        }

        let projected_ptr = ptr.cast::<U>().wrapping_byte_add(offset);
        // SAFETY: `offset_valid` checks for size and alignment and therefore `projected_ptr` is a
        // valid projection.
        Ok(unsafe { Self::Backend::project_view(view, projected_ptr) })
    }

    /// Fallible 8-bit read with runtime bounds check.
    #[inline(always)]
    fn try_read8(self, offset: usize) -> Result<u8>
    where
        usize: IoLoc<Self::Target, u8, IoType = u8>,
        Self::Backend: IoCapable<u8>,
    {
        self.try_read(offset)
    }

    /// Fallible 16-bit read with runtime bounds check.
    #[inline(always)]
    fn try_read16(self, offset: usize) -> Result<u16>
    where
        usize: IoLoc<Self::Target, u16, IoType = u16>,
        Self::Backend: IoCapable<u16>,
    {
        self.try_read(offset)
    }

    /// Fallible 32-bit read with runtime bounds check.
    #[inline(always)]
    fn try_read32(self, offset: usize) -> Result<u32>
    where
        usize: IoLoc<Self::Target, u32, IoType = u32>,
        Self::Backend: IoCapable<u32>,
    {
        self.try_read(offset)
    }

    /// Fallible 64-bit read with runtime bounds check.
    #[inline(always)]
    fn try_read64(self, offset: usize) -> Result<u64>
    where
        usize: IoLoc<Self::Target, u64, IoType = u64>,
        Self::Backend: IoCapable<u64>,
    {
        self.try_read(offset)
    }

    /// Fallible 8-bit write with runtime bounds check.
    #[inline(always)]
    fn try_write8(self, value: u8, offset: usize) -> Result
    where
        usize: IoLoc<Self::Target, u8, IoType = u8>,
        Self::Backend: IoCapable<u8>,
    {
        self.try_write(offset, value)
    }

    /// Fallible 16-bit write with runtime bounds check.
    #[inline(always)]
    fn try_write16(self, value: u16, offset: usize) -> Result
    where
        usize: IoLoc<Self::Target, u16, IoType = u16>,
        Self::Backend: IoCapable<u16>,
    {
        self.try_write(offset, value)
    }

    /// Fallible 32-bit write with runtime bounds check.
    #[inline(always)]
    fn try_write32(self, value: u32, offset: usize) -> Result
    where
        usize: IoLoc<Self::Target, u32, IoType = u32>,
        Self::Backend: IoCapable<u32>,
    {
        self.try_write(offset, value)
    }

    /// Fallible 64-bit write with runtime bounds check.
    #[inline(always)]
    fn try_write64(self, value: u64, offset: usize) -> Result
    where
        usize: IoLoc<Self::Target, u64, IoType = u64>,
        Self::Backend: IoCapable<u64>,
    {
        self.try_write(offset, value)
    }

    /// Infallible 8-bit read with compile-time bounds check.
    #[inline(always)]
    fn read8(self, offset: usize) -> u8
    where
        usize: IoLoc<Self::Target, u8, IoType = u8>,
        Self::Backend: IoCapable<u8>,
    {
        self.read(offset)
    }

    /// Infallible 16-bit read with compile-time bounds check.
    #[inline(always)]
    fn read16(self, offset: usize) -> u16
    where
        usize: IoLoc<Self::Target, u16, IoType = u16>,
        Self::Backend: IoCapable<u16>,
    {
        self.read(offset)
    }

    /// Infallible 32-bit read with compile-time bounds check.
    #[inline(always)]
    fn read32(self, offset: usize) -> u32
    where
        usize: IoLoc<Self::Target, u32, IoType = u32>,
        Self::Backend: IoCapable<u32>,
    {
        self.read(offset)
    }

    /// Infallible 64-bit read with compile-time bounds check.
    #[inline(always)]
    fn read64(self, offset: usize) -> u64
    where
        usize: IoLoc<Self::Target, u64, IoType = u64>,
        Self::Backend: IoCapable<u64>,
    {
        self.read(offset)
    }

    /// Infallible 8-bit write with compile-time bounds check.
    #[inline(always)]
    fn write8(self, value: u8, offset: usize)
    where
        usize: IoLoc<Self::Target, u8, IoType = u8>,
        Self::Backend: IoCapable<u8>,
    {
        self.write(offset, value)
    }

    /// Infallible 16-bit write with compile-time bounds check.
    #[inline(always)]
    fn write16(self, value: u16, offset: usize)
    where
        usize: IoLoc<Self::Target, u16, IoType = u16>,
        Self::Backend: IoCapable<u16>,
    {
        self.write(offset, value)
    }

    /// Infallible 32-bit write with compile-time bounds check.
    #[inline(always)]
    fn write32(self, value: u32, offset: usize)
    where
        usize: IoLoc<Self::Target, u32, IoType = u32>,
        Self::Backend: IoCapable<u32>,
    {
        self.write(offset, value)
    }

    /// Infallible 64-bit write with compile-time bounds check.
    #[inline(always)]
    fn write64(self, value: u64, offset: usize)
    where
        usize: IoLoc<Self::Target, u64, IoType = u64>,
        Self::Backend: IoCapable<u64>,
    {
        self.write(offset, value)
    }

    /// Generic fallible read with runtime bounds check.
    ///
    /// # Examples
    ///
    /// Read a primitive type from an I/O address:
    ///
    /// ```no_run
    /// use kernel::io::{
    ///     Io,
    ///     Mmio,
    ///     Region,
    /// };
    ///
    /// fn do_reads(io: Mmio<'_, Region>) -> Result {
    ///     // 32-bit read from address `0x10`.
    ///     let v: u32 = io.try_read(0x10)?;
    ///
    ///     // 8-bit read from address `0xfff`.
    ///     let v: u8 = io.try_read(0xfff)?;
    ///
    ///     Ok(())
    /// }
    /// ```
    #[inline(always)]
    fn try_read<T, L>(self, location: L) -> Result<T>
    where
        L: IoLoc<Self::Target, T>,
        Self::Backend: IoCapable<L::IoType>,
    {
        let view = self.io_addr::<L::IoType>(location.offset())?;
        Ok(Self::Backend::io_read(view).into())
    }

    /// Generic fallible write with runtime bounds check.
    ///
    /// # Examples
    ///
    /// Write a primitive type to an I/O address:
    ///
    /// ```no_run
    /// use kernel::io::{
    ///     Io,
    ///     Mmio,
    ///     Region,
    /// };
    ///
    /// fn do_writes(io: Mmio<'_, Region>) -> Result {
    ///     // 32-bit write of value `1` at address `0x10`.
    ///     io.try_write(0x10, 1u32)?;
    ///
    ///     // 8-bit write of value `0xff` at address `0xfff`.
    ///     io.try_write(0xfff, 0xffu8)?;
    ///
    ///     Ok(())
    /// }
    /// ```
    #[inline(always)]
    fn try_write<T, L>(self, location: L, value: T) -> Result
    where
        L: IoLoc<Self::Target, T>,
        Self::Backend: IoCapable<L::IoType>,
    {
        let view = self.io_addr::<L::IoType>(location.offset())?;
        let io_value = value.into();
        Self::Backend::io_write(view, io_value);
        Ok(())
    }

    /// Generic fallible write of a fully-located register value.
    ///
    /// # Examples
    ///
    /// Tuples carrying a location and a value can be used with this method:
    ///
    /// ```no_run
    /// use kernel::io::{
    ///     register,
    ///     Io,
    ///     Mmio,
    ///     Region,
    /// };
    ///
    /// register! {
    ///     VERSION(u32) @ 0x100 {
    ///         15:8 major;
    ///         7:0  minor;
    ///     }
    /// }
    ///
    /// impl VERSION {
    ///     fn new(major: u8, minor: u8) -> Self {
    ///         VERSION::zeroed().with_major(major).with_minor(minor)
    ///     }
    /// }
    ///
    /// fn do_write_reg(io: Mmio<'_, Region>) -> Result {
    ///
    ///     io.try_write_reg(VERSION::new(1, 0))
    /// }
    /// ```
    #[inline(always)]
    fn try_write_reg<T, L, V>(self, value: V) -> Result
    where
        L: IoLoc<Self::Target, T>,
        V: LocatedRegister<Self::Target, Location = L, Value = T>,
        Self::Backend: IoCapable<L::IoType>,
    {
        let (location, value) = value.into_io_op();

        self.try_write(location, value)
    }

    /// Generic fallible update with runtime bounds check.
    ///
    /// Note: this does not perform any synchronization. The caller is responsible for ensuring
    /// exclusive access if required.
    ///
    /// # Examples
    ///
    /// Read the u32 value at address `0x10`, increment it, and store the updated value back:
    ///
    /// ```no_run
    /// use kernel::io::{
    ///     Io,
    ///     Mmio,
    ///     Region,
    /// };
    ///
    /// fn do_update(io: Mmio<'_, Region<0x1000>>) -> Result {
    ///     io.try_update(0x10, |v: u32| {
    ///         v + 1
    ///     })
    /// }
    /// ```
    #[inline(always)]
    fn try_update<T, L, F>(self, location: L, f: F) -> Result
    where
        L: IoLoc<Self::Target, T>,
        Self::Backend: IoCapable<L::IoType>,
        F: FnOnce(T) -> T,
    {
        let view = self.io_addr::<L::IoType>(location.offset())?;

        let value: T = Self::Backend::io_read(view).into();
        let io_value = f(value).into();
        Self::Backend::io_write(view, io_value);

        Ok(())
    }

    /// Generic infallible read with compile-time bounds check.
    ///
    /// # Examples
    ///
    /// Read a primitive type from an I/O address:
    ///
    /// ```no_run
    /// use kernel::io::{
    ///     Io,
    ///     Mmio,
    ///     Region,
    /// };
    ///
    /// fn do_reads(io: Mmio<'_, Region<0x1000>>) {
    ///     // 32-bit read from address `0x10`.
    ///     let v: u32 = io.read(0x10);
    ///
    ///     // 8-bit read from the top of the I/O space.
    ///     let v: u8 = io.read(0xfff);
    /// }
    /// ```
    #[inline(always)]
    fn read<T, L>(self, location: L) -> T
    where
        L: IoLoc<Self::Target, T>,
        Self::Backend: IoCapable<L::IoType>,
    {
        let view = self.io_addr_assert::<L::IoType>(location.offset());
        Self::Backend::io_read(view).into()
    }

    /// Generic infallible write with compile-time bounds check.
    ///
    /// # Examples
    ///
    /// Write a primitive type to an I/O address:
    ///
    /// ```no_run
    /// use kernel::io::{
    ///     Io,
    ///     Mmio,
    ///     Region,
    /// };
    ///
    /// fn do_writes(io: Mmio<'_, Region<0x1000>>) {
    ///     // 32-bit write of value `1` at address `0x10`.
    ///     io.write(0x10, 1u32);
    ///
    ///     // 8-bit write of value `0xff` at the top of the I/O space.
    ///     io.write(0xfff, 0xffu8);
    /// }
    /// ```
    #[inline(always)]
    fn write<T, L>(self, location: L, value: T)
    where
        L: IoLoc<Self::Target, T>,
        Self::Backend: IoCapable<L::IoType>,
    {
        let view = self.io_addr_assert::<L::IoType>(location.offset());
        let io_value = value.into();
        Self::Backend::io_write(view, io_value);
    }

    /// Generic infallible write of a fully-located register value.
    ///
    /// # Examples
    ///
    /// Tuples carrying a location and a value can be used with this method:
    ///
    /// ```no_run
    /// use kernel::io::{
    ///     register,
    ///     Io,
    ///     Mmio,
    ///     Region,
    /// };
    ///
    /// register! {
    ///     VERSION(u32) @ 0x100 {
    ///         15:8 major;
    ///         7:0  minor;
    ///     }
    /// }
    ///
    /// impl VERSION {
    ///     fn new(major: u8, minor: u8) -> Self {
    ///         VERSION::zeroed().with_major(major).with_minor(minor)
    ///     }
    /// }
    ///
    /// fn do_write_reg(io: Mmio<'_, Region<0x1000>>) {
    ///     io.write_reg(VERSION::new(1, 0));
    /// }
    /// ```
    #[inline(always)]
    fn write_reg<T, L, V>(self, value: V)
    where
        L: IoLoc<Self::Target, T>,
        V: LocatedRegister<Self::Target, Location = L, Value = T>,
        Self::Backend: IoCapable<L::IoType>,
    {
        let (location, value) = value.into_io_op();

        self.write(location, value)
    }

    /// Generic infallible update with compile-time bounds check.
    ///
    /// Note: this does not perform any synchronization. The caller is responsible for ensuring
    /// exclusive access if required.
    ///
    /// # Examples
    ///
    /// Read the u32 value at address `0x10`, increment it, and store the updated value back:
    ///
    /// ```no_run
    /// use kernel::io::{
    ///     Io,
    ///     Mmio,
    ///     Region,
    /// };
    ///
    /// fn do_update(io: Mmio<'_, Region<0x1000>>) {
    ///     io.update(0x10, |v: u32| {
    ///         v + 1
    ///     })
    /// }
    /// ```
    #[inline(always)]
    fn update<T, L, F>(self, location: L, f: F)
    where
        L: IoLoc<Self::Target, T>,
        Self::Backend: IoCapable<L::IoType>,
        F: FnOnce(T) -> T,
    {
        let view = self.io_addr_assert::<L::IoType>(location.offset());
        let value: T = Self::Backend::io_read(view).into();
        let io_value = f(value).into();
        Self::Backend::io_write(view, io_value);
    }
}

impl<'a, T: IoBase<'a>> Io<'a> for T {}

/// A view of memory-mapped I/O region.
///
/// # Invariant
///
/// `ptr` points to a valid and aligned memory-mapped I/O region for the duration lifetime `'a`.
pub struct Mmio<'a, T: ?Sized> {
    ptr: *mut T,
    phantom: PhantomData<&'a ()>,
}

impl<T: ?Sized> Copy for Mmio<'_, T> {}
impl<T: ?Sized> Clone for Mmio<'_, T> {
    #[inline]
    fn clone(&self) -> Self {
        *self
    }
}

impl<'a, T: ?Sized> Mmio<'a, T> {
    /// Create a `Mmio`, providing the accessors to the MMIO mapping.
    ///
    /// # Safety
    ///
    /// `raw` represents an valid and aligned memory-mapped I/O region while `'a` is alive.
    #[inline]
    pub unsafe fn from_raw(raw: MmioRaw<T>) -> Self {
        // INVARIANT: Per safety requirement.
        Self {
            ptr: raw.ptr,
            phantom: PhantomData,
        }
    }
}

// SAFETY: `Mmio<'_, T>` is conceptually `&T` but in I/O memory.
unsafe impl<T: ?Sized + Sync> Send for Mmio<'_, T> {}

// SAFETY: `Mmio<'_, T>` is conceptually `&T` but in I/O memory.
unsafe impl<T: ?Sized + Sync> Sync for Mmio<'_, T> {}

impl<'a, T: ?Sized + KnownSize> IoBase<'a> for Mmio<'a, T> {
    type Backend = MmioBackend;
    type Target = T;

    #[inline]
    fn as_view(self) -> Mmio<'a, T> {
        self
    }
}

/// I/O Backend for memory-mapped I/O.
pub struct MmioBackend;

impl IoBackend for MmioBackend {
    type View<'a, T: ?Sized + KnownSize> = Mmio<'a, T>;

    #[inline]
    fn as_ptr<'a, T: ?Sized + KnownSize>(view: Self::View<'a, T>) -> *mut T {
        view.ptr
    }

    #[inline]
    unsafe fn project_view<'a, T: ?Sized + KnownSize, U: ?Sized + KnownSize>(
        _view: Self::View<'a, T>,
        ptr: *mut U,
    ) -> Self::View<'a, U> {
        // INVARIANT: Per safety requirement, `ptr` is projection from `view`, so it is also a valid
        // memory-mapped I/O region.
        Mmio {
            ptr,
            phantom: PhantomData,
        }
    }
}

/// Implements [`IoCapable`] on `$backend` for `$ty` using `$read_fn` and `$write_fn`.
macro_rules! impl_mmio_io_capable {
    ($backend: ident, $ty:ty, $read_fn:ident, $write_fn:ident) => {
        impl IoCapable<$ty> for $backend {
            #[inline]
            fn io_read(view: <$backend as IoBackend>::View<'_, $ty>) -> $ty {
                // SAFETY: By the type invariant, `view.ptr` is a valid address for MMIO operations.
                unsafe { bindings::$read_fn(view.ptr.cast_const().cast()) }
            }

            #[inline]
            fn io_write(view: <$backend as IoBackend>::View<'_, $ty>, value: $ty) {
                // SAFETY: By the type invariant, `view.ptr` is a valid address for MMIO operations.
                unsafe { bindings::$write_fn(value, view.ptr.cast()) }
            }
        }
    };
}

// MMIO regions support 8, 16, and 32-bit accesses.
impl_mmio_io_capable!(MmioBackend, u8, readb, writeb);
impl_mmio_io_capable!(MmioBackend, u16, readw, writew);
impl_mmio_io_capable!(MmioBackend, u32, readl, writel);
// MMIO regions on 64-bit systems also support 64-bit accesses.
#[cfg(CONFIG_64BIT)]
impl_mmio_io_capable!(MmioBackend, u64, readq, writeq);

// SAFETY: `is_mapped` is not overridden.
unsafe impl IoCopyable for MmioBackend {
    #[inline]
    unsafe fn copy_from_io(view: Self::View<'_, [u8]>, buffer: *mut u8) {
        // SAFETY:
        // - `view.ptr` is valid MMIO memory for `view.size()` bytes.
        // - `buffer` is valid for write for `view.size()` bytes.
        unsafe {
            bindings::memcpy_fromio(buffer.cast(), view.ptr.cast(), view.size());
        }
    }

    #[inline]
    unsafe fn copy_to_io(view: Self::View<'_, [u8]>, buffer: *const u8) {
        // SAFETY:
        // - `view.ptr` is valid MMIO memory for `view.size()` bytes.
        // - `buffer` is valid for read for `view.size()` bytes.
        unsafe {
            bindings::memcpy_toio(view.ptr.cast(), buffer.cast(), view.size());
        }
    }
}

/// [`Mmio`] but using relaxed accessors.
///
/// This type provides an implementation of [`Io`] that uses relaxed I/O MMIO operands instead of
/// the regular ones.
///
/// See [`Mmio::relaxed`] for a usage example.
///
/// # Invariant
///
/// `ptr` points to a valid and aligned memory-mapped I/O region for the duration lifetime `'a`.
pub struct RelaxedMmio<'a, T: ?Sized> {
    ptr: *mut T,
    phantom: PhantomData<&'a ()>,
}

impl<T: ?Sized> Copy for RelaxedMmio<'_, T> {}
impl<T: ?Sized> Clone for RelaxedMmio<'_, T> {
    #[inline]
    fn clone(&self) -> Self {
        *self
    }
}

/// I/O Backend for memory-mapped I/O, with relaxed access semantics.
pub struct RelaxedMmioBackend;

impl IoBackend for RelaxedMmioBackend {
    type View<'a, T: ?Sized + KnownSize> = RelaxedMmio<'a, T>;

    #[inline]
    fn as_ptr<'a, T: ?Sized + KnownSize>(view: Self::View<'a, T>) -> *mut T {
        view.ptr
    }

    #[inline]
    unsafe fn project_view<'a, T: ?Sized + KnownSize, U: ?Sized + KnownSize>(
        _view: Self::View<'a, T>,
        ptr: *mut U,
    ) -> Self::View<'a, U> {
        // INVARIANT: Per safety requirement, `ptr` is projection from `view`, so it is also a valid
        // memory-mapped I/O region.
        RelaxedMmio {
            ptr,
            phantom: PhantomData,
        }
    }
}

impl<'a, T: ?Sized + KnownSize> IoBase<'a> for RelaxedMmio<'a, T> {
    type Backend = RelaxedMmioBackend;
    type Target = T;

    #[inline]
    fn as_view(self) -> RelaxedMmio<'a, T> {
        self
    }
}

impl<'a, T: ?Sized> Mmio<'a, T> {
    /// Returns a [`RelaxedMmio`] reference that performs relaxed I/O operations.
    ///
    /// Relaxed accessors do not provide ordering guarantees with respect to DMA or memory accesses
    /// and can be used when such ordering is not required.
    ///
    /// # Examples
    ///
    /// ```no_run
    /// use kernel::io::{
    ///     Io,
    ///     Mmio,
    ///     Region,
    ///     RelaxedMmio,
    /// };
    ///
    /// fn do_io(io: Mmio<'_, Region<0x100>>) {
    ///     // The access is performed using `readl_relaxed` instead of `readl`.
    ///     let v = io.relaxed().read32(0x10);
    /// }
    ///
    /// ```
    pub fn relaxed(self) -> RelaxedMmio<'a, T> {
        // INVARIANT: `RelaxedMmio` has the same invariant as `Mmio`.
        RelaxedMmio {
            ptr: self.ptr,
            phantom: PhantomData,
        }
    }
}

// MMIO regions support 8, 16, and 32-bit accesses.
impl_mmio_io_capable!(RelaxedMmioBackend, u8, readb_relaxed, writeb_relaxed);
impl_mmio_io_capable!(RelaxedMmioBackend, u16, readw_relaxed, writew_relaxed);
impl_mmio_io_capable!(RelaxedMmioBackend, u32, readl_relaxed, writel_relaxed);
// MMIO regions on 64-bit systems also support 64-bit accesses.
#[cfg(CONFIG_64BIT)]
impl_mmio_io_capable!(RelaxedMmioBackend, u64, readq_relaxed, writeq_relaxed);

/// I/O Backend for system memory.
pub struct SysMemBackend;

impl IoBackend for SysMemBackend {
    type View<'a, T: ?Sized + KnownSize> = SysMem<'a, T>;

    #[inline]
    fn as_ptr<'a, T: ?Sized + KnownSize>(view: Self::View<'a, T>) -> *mut T {
        view.ptr
    }

    #[inline]
    unsafe fn project_view<'a, T: ?Sized + KnownSize, U: ?Sized + KnownSize>(
        _view: Self::View<'a, T>,
        ptr: *mut U,
    ) -> Self::View<'a, U> {
        // INVARIANT: Per safety requirement, `ptr` is projection from `view`, so it is also a valid
        // kernel accessible memory region.
        SysMem {
            ptr,
            phantom: PhantomData,
        }
    }
}

/// Implements [`IoCapable`] on `SysMemBackend` for `$ty` using `read_volatile` and
/// `write_volatile`.
macro_rules! impl_sysmem_io_capable {
    ($ty:ty) => {
        impl IoCapable<$ty> for SysMemBackend {
            fn io_read(view: SysMem<'_, $ty>) -> $ty {
                // SAFETY:
                // - Per type invariant, `ptr` is valid and aligned.
                // - Using read_volatile() here so that race with hardware is well-defined.
                // - Using read_volatile() here is not sound if it races with other CPU per Rust
                //   rules, but this is allowed per LKMM.
                // - The macro is only used on primitives so all bit patterns are valid.
                unsafe { view.ptr.read_volatile() }
            }

            fn io_write(view: SysMem<'_, $ty>, value: $ty) {
                // SAFETY:
                // - Per type invariant, `ptr` is valid and aligned.
                // - Using write_volatile() here so that race with hardware is well-defined.
                // - Using write_volatile() here is not sound if it races with other CPU per Rust
                //   rules, but this is allowed per LKMM.
                unsafe { view.ptr.write_volatile(value) }
            }
        }
    };
}

impl_sysmem_io_capable!(u8);
impl_sysmem_io_capable!(u16);
impl_sysmem_io_capable!(u32);
#[cfg(CONFIG_64BIT)]
impl_sysmem_io_capable!(u64);

// SAFETY: `SysMem::as_ptr` is mapped to the CPU address space.
unsafe impl IoCopyable for SysMemBackend {
    #[inline(always)]
    fn is_mapped<T: ?Sized + KnownSize>(_view: Self::View<'_, T>) -> bool {
        true
    }
}

/// System memory region.
///
/// Provides `Io` trait implementation for kernel virtual address ranges,
/// using volatile read/write to safely access shared memory that may be
/// concurrently accessed by external hardware.
///
/// # Invariants
///
/// `self.ptr.addr() .. self.ptr.addr() + KnownSize::size(self.ptr)` is valid and aligned kernel
/// accessible memory region for the lifetime `'a`.
pub struct SysMem<'a, T: ?Sized> {
    ptr: *mut T,
    phantom: PhantomData<&'a ()>,
}

impl<T: ?Sized> Copy for SysMem<'_, T> {}
impl<T: ?Sized> Clone for SysMem<'_, T> {
    #[inline]
    fn clone(&self) -> Self {
        *self
    }
}

impl<'a, T: ?Sized> SysMem<'a, T> {
    /// Create a `SysMem` from a raw pointer.
    ///
    /// # Safety
    ///
    /// `ptr.addr() .. ptr.addr() + KnownSize::size(ptr)` must be valid and aligned kernel
    /// accessible memory region for the lifetime `'a`.
    #[inline]
    pub unsafe fn new(ptr: *mut T) -> Self {
        // INVARIANT: Per safety requirement.
        Self {
            ptr,
            phantom: PhantomData,
        }
    }

    /// Obtain the raw pointer to the memory.
    #[inline]
    pub fn as_ptr(self) -> *mut T {
        self.ptr
    }
}

impl<'a, T: ?Sized + KnownSize> IoBase<'a> for SysMem<'a, T> {
    type Backend = SysMemBackend;
    type Target = T;

    #[inline]
    fn as_view(self) -> <Self::Backend as IoBackend>::View<'a, Self::Target> {
        self
    }
}

// This helper turns associated functions to methods so it can be invoked in macro.
// Used by `io_project!()` only.
#[doc(hidden)]
#[derive(Clone, Copy)]
pub struct ProjectHelper<T>(pub T);

impl<'a, T> ProjectHelper<T>
where
    T: Io<'a, Backend: IoBackend<View<'a, T::Target> = T>>,
{
    // These helper methods must not have symbols present in binary to avoid confusion.
    #[inline(always)]
    pub fn as_ptr(self) -> *mut T::Target {
        T::Backend::as_ptr(self.0)
    }

    /// # Safety
    ///
    /// Same as `IoBackend::project_view`
    #[inline(always)]
    pub unsafe fn project_view<U: ?Sized + KnownSize>(
        self,
        ptr: *mut U,
    ) -> <T::Backend as IoBackend>::View<'a, U> {
        // SAFETY: Per safety requirement.
        unsafe { T::Backend::project_view::<T::Target, _>(self.0, ptr) }
    }
}

impl<'a, T: ?Sized + KnownSize, L: IoBase<'a, Target = T>, R: IoBase<'a, Target = T>> IoBase<'a>
    for Either<L, R>
{
    type Backend = Either<L::Backend, R::Backend>;
    type Target = T;

    #[inline]
    fn as_view(self) -> <Self::Backend as IoBackend>::View<'a, Self::Target> {
        match self {
            Either::Left(l) => Either::Left(l.as_view()),
            Either::Right(r) => Either::Right(r.as_view()),
        }
    }
}

impl<L: IoBackend, R: IoBackend> IoBackend for Either<L, R> {
    type View<'a, T: ?Sized + KnownSize> = Either<L::View<'a, T>, R::View<'a, T>>;

    #[inline]
    fn as_ptr<'a, T: ?Sized + KnownSize>(view: Self::View<'a, T>) -> *mut T {
        match view {
            Either::Left(l) => L::as_ptr(l),
            Either::Right(r) => R::as_ptr(r),
        }
    }

    #[inline]
    unsafe fn project_view<'a, T: ?Sized + KnownSize, U: ?Sized + KnownSize>(
        view: Self::View<'a, T>,
        ptr: *mut U,
    ) -> Self::View<'a, U> {
        match view {
            // SAFETY: Per safety requirement.
            Either::Left(l) => Either::Left(unsafe { L::project_view(l, ptr) }),
            // SAFETY: Per safety requirement.
            Either::Right(r) => Either::Right(unsafe { R::project_view(r, ptr) }),
        }
    }
}

impl<T, L: IoCapable<T>, R: IoCapable<T>> IoCapable<T> for Either<L, R> {
    #[inline]
    fn io_read(view: Self::View<'_, T>) -> T {
        match view {
            Either::Left(l) => L::io_read(l),
            Either::Right(r) => R::io_read(r),
        }
    }

    #[inline]
    fn io_write<'a>(view: Self::View<'a, T>, value: T) {
        match view {
            Either::Left(l) => L::io_write(l, value),
            Either::Right(r) => R::io_write(r, value),
        }
    }
}

// SAFETY: Per safety guarantee of `L` and `R`'s `IoCopyable` impl, `is_mapped` is correctly
// implemented.
unsafe impl<L: IoCopyable, R: IoCopyable> IoCopyable for Either<L, R> {
    #[inline]
    fn is_mapped<T: ?Sized + KnownSize>(view: Self::View<'_, T>) -> bool {
        match view {
            Either::Left(l) => L::is_mapped(l),
            Either::Right(r) => R::is_mapped(r),
        }
    }

    #[inline]
    unsafe fn copy_from_io(view: Self::View<'_, [u8]>, buffer: *mut u8) {
        match view {
            // SAFETY: Per safety requirement.
            Either::Left(l) => unsafe { L::copy_from_io(l, buffer) },
            // SAFETY: Per safety requirement.
            Either::Right(r) => unsafe { R::copy_from_io(r, buffer) },
        }
    }

    #[inline]
    unsafe fn copy_to_io(view: Self::View<'_, [u8]>, buffer: *const u8) {
        match view {
            // SAFETY: Per safety requirement.
            Either::Left(l) => unsafe { L::copy_to_io(l, buffer) },
            // SAFETY: Per safety requirement.
            Either::Right(r) => unsafe { R::copy_to_io(r, buffer) },
        }
    }
}

/// Project an I/O type to a subview of it.
///
/// The syntax is of form `io_project!(io, proj)` where `io` is an expression to a type that
/// implements [`Io`] and `proj` is a [projection specification](kernel::ptr::project!).
///
/// In addition to projecting from [`Io`], you may also project from a [`View`] of an [`Io`].
///
/// # Examples
///
/// ```
/// use kernel::io::{
///     io_project,
///     Mmio,
/// };
/// struct MyStruct { field: u32, }
///
/// // SAFETY: All bit patterns are acceptable values for `MyStruct`.
/// unsafe impl kernel::transmute::FromBytes for MyStruct {};
/// // SAFETY: Instances of `MyStruct` have no uninitialized portions.
/// unsafe impl kernel::transmute::AsBytes for MyStruct {};
///
/// # fn test(mmio: Mmio<'_, [MyStruct]>) -> Result {
/// // let mmio: Mmio<[MyStruct]>;
/// let field: Mmio<'_, u32> = io_project!(mmio, [try: 1].field);
/// let whole: Mmio<'_, MyStruct> = io_project!(mmio, [try: 2]);
/// let nested: Mmio<'_, u32> = io_project!(whole, .field);
/// # Ok::<(), Error>(()) }
/// ```
#[macro_export]
#[doc(hidden)]
macro_rules! io_project {
    ($io:expr, $($proj:tt)*) => {{
        #[allow(unused)]
        use $crate::io::IoBase as _;
        let view = $crate::io::ProjectHelper($io.as_view());
        let ptr = $crate::ptr::project!(
            mut view.as_ptr(), $($proj)*
        );
        #[allow(unused_unsafe)]
        // SAFETY: `ptr` is a projection.
        unsafe { view.project_view(ptr) }
    }};
}
#[doc(inline)]
pub use crate::io_project;

/// Read from I/O memory.
///
/// The syntax is of form `io_read!(io, proj)` where `io` is an expression to a type that
/// implements [`Io`] and `proj` is a [projection specification](kernel::ptr::project!).
///
/// # Examples
///
/// ```
/// struct MyStruct { field: u32, }
///
/// // SAFETY: All bit patterns are acceptable values for `MyStruct`.
/// unsafe impl kernel::transmute::FromBytes for MyStruct{};
/// // SAFETY: Instances of `MyStruct` have no uninitialized portions.
/// unsafe impl kernel::transmute::AsBytes for MyStruct{};
///
/// # fn test(mmio: kernel::io::Mmio<'_, [MyStruct]>) -> Result {
/// // let mmio: Mmio<'_, [MyStruct]>;
/// let field: u32 = kernel::io::io_read!(mmio, [try: 2].field);
/// # Ok::<(), Error>(()) }
/// ```
#[macro_export]
#[doc(hidden)]
macro_rules! io_read {
    ($io:expr, $($proj:tt)*) => {
        $crate::io::Io::read_val($crate::io_project!($io, $($proj)*))
    };
}
#[doc(inline)]
pub use crate::io_read;

/// Writes to I/O memory.
///
/// The syntax is of form `io_write!(io, proj, val)` where `io` is an expression to a type that
/// implements [`Io`] and `proj` is a [projection specification](kernel::ptr::project!),
/// and `val` is the value to be written to the projected location.
///
/// # Examples
///
/// ```
/// struct MyStruct { field: u32, }
///
/// // SAFETY: All bit patterns are acceptable values for `MyStruct`.
/// unsafe impl kernel::transmute::FromBytes for MyStruct{};
/// // SAFETY: Instances of `MyStruct` have no uninitialized portions.
/// unsafe impl kernel::transmute::AsBytes for MyStruct{};
///
/// # fn test(mmio: kernel::io::Mmio<'_, [MyStruct]>) -> Result {
/// // let mmio: Mmio<'_, [MyStruct]>;
/// kernel::io::io_write!(mmio, [try: 2].field, 10);
/// # Ok::<(), Error>(()) }
/// ```
#[macro_export]
#[doc(hidden)]
macro_rules! io_write {
    (@parse [$io:expr] [$($proj:tt)*] [, $val:expr]) => {
        $crate::io::Io::write_val($crate::io_project!($io, $($proj)*), $val)
    };
    (@parse [$io:expr] [$($proj:tt)*] [.$field:tt $($rest:tt)*]) => {
        $crate::io_write!(@parse [$io] [$($proj)* .$field] [$($rest)*])
    };
    (@parse [$io:expr] [$($proj:tt)*] [[$flavor:ident: $index:expr] $($rest:tt)*]) => {
        $crate::io_write!(@parse [$io] [$($proj)* [$flavor: $index]] [$($rest)*])
    };
    ($io:expr, $($rest:tt)*) => {
        $crate::io_write!(@parse [$io] [] [$($rest)*])
    };
}
#[doc(inline)]
pub use crate::io_write;
