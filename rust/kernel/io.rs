// SPDX-License-Identifier: GPL-2.0

//! Memory-mapped IO.
//!
//! C header: [`include/asm-generic/io.h`](srctree/include/asm-generic/io.h)

use crate::error::{code::EINVAL, Result};
use crate::{bindings, build_assert};
use io_backend::*;

/// `io_backend` is private and implements the config specific logic for
/// `IoAccess::from_raw_cookie`.
#[cfg(all(CONFIG_X86, CONFIG_GENERIC_IOMAP))]
mod io_backend {
    // if on x86, generic_iomap is enabled so copy the logic
    // from IO_COND in `lib/iomap.c`

    // values copied from `lib/iomap.c`
    const PIO_OFFSET: usize = 0x10000;
    const PIO_RESERVED: usize = 0x40000;

    #[inline]
    pub(super) fn is_mmio(addr: usize) -> bool {
        addr >= PIO_RESERVED
    }

    #[inline]
    pub(super) fn is_portio(addr: usize) -> bool {
        !is_mmio(addr) && addr > PIO_OFFSET
    }
}
#[cfg(not(CONFIG_GENERIC_IOMAP))]
mod io_backend {
    // for everyone who does not use generic iomap
    // except for alpha and parisc, neither of which has a rust compiler,
    // ioread/iowrite is defined in `include/asm-generic/io.h`.
    //
    // for these ioread/iowrite, maps to read/write.
    // so allow any io to be converted  because they use the same backend
    #[inline]
    pub(super) fn is_mmio(_addr: usize) -> bool {
        true
    }

    #[inline]
    pub(super) fn is_portio(_addr: usize) -> bool {
        false
    }
}

/// Private macro to define the [`IoAccess`] functions.
macro_rules! define_io_access_function {
    (@read_derived $(#[$attr:meta])* $name_unchecked:ident, $vis:vis $name:ident, $try_vis:vis $try_name:ident, $type_name:ty) => {
    /// Read data from a given offset known at compile time.
    ///
    /// Bound checks are perfomed on compile time, hence if the offset is not known at compile
    /// time, the build will fail.
    $(#[$attr])*
    #[inline]
    $vis fn $name(&self, offset: usize) -> $type_name {
        build_assert!(offset_valid::<$type_name>(offset, SIZE));

        // SAFETY: offset checked to be valid above.
        unsafe { self.$name_unchecked(offset) }
    }

    /// Read data from a given offset.
    ///
    /// Bound checks are performed at runtime, it fails if the offset (plus type size) is
    /// out of bounds.
    $(#[$attr])*
    #[inline]
    $try_vis fn $try_name(&self, offset: usize) -> Result<$type_name> {
        if !(offset_valid::<$type_name>(offset, self.maxsize())) {
            return Err(EINVAL);
        }

        // SAFETY: offset checked to be valid above.
        Ok(unsafe { self.$name_unchecked(offset) })
    }
    };
    (@read $(#[$attr:meta])* $name_unchecked:ident, $name:ident, $try_name:ident, $type_name:ty) => {
    /// Read data from a given offset without doing any bound checks.
    /// The offset is relative to the base address of Self.
    ///
    /// # Safety
    ///
    /// The offset has to be valid for self.
    $(#[$attr])*
    unsafe fn $name_unchecked(&self, offset: usize) -> $type_name;

    define_io_access_function!(@read_derived $(#[$attr])* $name_unchecked, $name, $try_name, $type_name);
    };
    (@read $($(#[$attr:meta])* $name_unchecked:ident, $name:ident, $try_name:ident, $type_name:ty;)+) => {
    $(
        define_io_access_function!(@read $(#[$attr])* $name_unchecked, $name, $try_name, $type_name);
    )*
    };
    (@write_derived $(#[$attr:meta])* $name_unchecked:ident, $vis:vis $name:ident, $try_vis:vis $try_name:ident, $type_name:ty) => {
    /// Write data to a given offset known at compile time.
    /// Bound checks are performed at compile time, hence if the offset is not known at compile
    /// time, the build will fail.
    $(#[$attr])*
    #[inline]
    $vis fn $name(&self, value: $type_name, offset: usize) {
        build_assert!(offset_valid::<$type_name>(offset, SIZE));

        // SAFETY: offset checked to be valid above.
        unsafe { self.$name_unchecked(value, offset) }
    }

    /// Write data to a given offset.
    ///
    /// Bound checks are performed at runtime, it fails if the offset (plus the type size) is
    /// out of bounds.
    $(#[$attr])*
        #[inline]
    $try_vis fn $try_name(&self, value: $type_name, offset: usize) -> Result {
        if !(offset_valid::<$type_name>(offset, self.maxsize())) {
            return Err(EINVAL);
        }

        // SAFETY: offset checked to be valid above.
        Ok(unsafe { self.$name_unchecked(value, offset) })
    }
    };
    (@write $(#[$attr:meta])* $name_unchecked:ident, $name:ident, $try_name:ident, $type_name:ty) => {
    /// Write data to a given offset without doing any bound checks.
    /// The offset is relative to the base address of self.
    ///
    /// # Safety
    ///
    /// The offset has to be valid for Self.
    $(#[$attr])*
    unsafe fn $name_unchecked(&self, value: $type_name, offset: usize);

    define_io_access_function!(@write_derived $(#[$attr])* $name_unchecked, $name, $try_name, $type_name);
    };
    (@write $($(#[$attr:meta])* $name_unchecked:ident, $name:ident, $try_name:ident, $type_name:ty;)+) => {
    $(
        define_io_access_function!(@write $(#[$attr])* $name_unchecked, $name, $try_name, $type_name);
    )*
    };
}

/// Private macro to generate accessor functions that call the correct C functions given as `fn_c`.
///
/// This takes either `@read` or `@write` to generate a single read or write accessor function.
///
/// This also can take a list of read write pairs to generate both at the same time.
macro_rules! impl_accessor_fn {
    (@read $(#[$attr:meta])* $vis:vis $fn_rust:ident, $fn_c:ident, $type_name:ty) => {
    $(#[$attr])*
    $vis unsafe fn $fn_rust(&self, offset: usize) -> $type_name {
        // SAFETY: by the safety requirement of the function `self.addr() + offset` is valid to read
        // TODO: once MSRV is >= 1.79.0 replace `+` with `unchecked_add`
        unsafe { bindings::$fn_c((self.addr() + offset) as _) as _ }
    }
    };
    (@write $(#[$attr:meta])* $vis:vis $fn_rust:ident, $fn_c:ident, $type_name:ty) => {
    $(#[$attr])*
    $vis unsafe fn $fn_rust(&self, value: $type_name, offset: usize) {
        // SAFETY:
        // by the safety requirement of the function `self.addr() + offset` is valid to write
        // TODO: once MSRV is >= 1.79.0 replace `+` with `unchecked_add`
        unsafe { bindings::$fn_c(value, (self.addr() + offset) as _) as _ }
    }
    };
    (
    $(
        $(#[$attr:meta])*
        $vis_read:vis $fn_rust_read:ident, $fn_c_read:ident,
        $vis_write:vis $fn_rust_write:ident, $fn_c_write:ident,
        $type_name:ty $(;)?
    )+
    ) => {
    $(
        impl_accessor_fn!(@read $(#[$attr])* $vis_read $fn_rust_read, $fn_c_read, $type_name);
        impl_accessor_fn!(@write $(#[$attr])* $vis_write $fn_rust_write, $fn_c_write, $type_name);
    )+
    };
}

/// Check if the offset is valid to still support the type U in the given size
const fn offset_valid<U>(offset: usize, size: usize) -> bool {
    let type_size = core::mem::size_of::<U>();
    if let Some(end) = offset.checked_add(type_size) {
        end <= size && offset % type_size == 0
    } else {
        false
    }
}

/// Io Access functions.
///
/// # Safety
///
/// `SIZE` and `maxsize()` has to always be valid to add to the base address.
pub unsafe trait IoAccess<const SIZE: usize = 0> {
    /// Returns the maximum size of the accessed IO area.
    fn maxsize(&self) -> usize;

    /// Returns the base address of the accessed IO area.
    fn addr(&self) -> usize;

    /// Attempts to create a `Self` from a [`IoRaw`].
    ///
    /// # Safety
    /// `raw` should be a io cookie that can be accessed by the C `ioread`/`iowrite` functions
    unsafe fn from_raw_cookie(raw: IoRaw<SIZE>) -> Result<Self>
    where
        Self: Sized;

    define_io_access_function!(@read
        read8_unchecked, read8, try_read8, u8;
        read16_unchecked, read16, try_read16, u16;
        read32_unchecked, read32, try_read32, u32;
    );

    define_io_access_function!(@write
        write8_unchecked, write8, try_write8, u8;
        write16_unchecked, write16, try_write16, u16;
        write32_unchecked, write32, try_write32, u32;
    );
}

/// Extending trait of [`IoAccess`] offering 64 bit functions.
#[cfg(CONFIG_64BIT)]
pub trait IoAccess64<const SIZE: usize = 0>: IoAccess<SIZE> {
    define_io_access_function!(@read read64_unchecked, read64, try_read64, u64);
    define_io_access_function!(@write write64_unchecked, write64, try_write64, u64);
}

/// Io Relaxed Access functions.
///
/// Similar to [`IoAccess`] but using relaxed memory boundries.
pub trait IoAccessRelaxed<const SIZE: usize = 0>: IoAccess<SIZE> {
    define_io_access_function!(@read
        read8_relaxed_unchecked, read8_relaxed, try_read8_relaxed, u8;
        read16_relaxed_unchecked, read16_relaxed, try_read16_relaxed, u16;
        read32_relaxed_unchecked, read32_relaxed, try_read32_relaxed, u32;
    );

    define_io_access_function!(@write
        write8_relaxed_unchecked, write8_relaxed, try_write8_relaxed, u8;
        write16_relaxed_unchecked, write16_relaxed, try_write16_relaxed, u16;
        write32_relaxed_unchecked, write32_relaxed, try_write32_relaxed, u32;
    );
}

/// Extending trait of [`IoAccessRelaxed`] offering 64 bit functions.
#[cfg(CONFIG_64BIT)]
pub trait IoAccess64Relaxed<const SIZE: usize = 0>: IoAccess<SIZE> {
    define_io_access_function!(@read
        read64_relaxed_unchecked, read64_relaxed, try_read64_relaxed, u64;
    );

    define_io_access_function!(@write
        write64_relaxed_unchecked, write64_relaxed, try_write64_relaxed, u64;
    );
}

/// Raw representation of an MMIO region.
///
/// By itself, the existence of an instance of this structure does not provide any guarantees that
/// the represented MMIO region does exist or is properly mapped.
///
/// Instead, the bus specific MMIO implementation must convert this raw representation into an `Io`
/// instance providing the actual memory accessors. Only by the conversion into an `Io` structure
/// any guarantees are given.
pub struct IoRaw<const SIZE: usize = 0> {
    addr: usize,
    maxsize: usize,
}

impl<const SIZE: usize> IoRaw<SIZE> {
    /// Returns a new `IoRaw` instance on success, an error otherwise.
    ///
    /// # Examples
    ///
    /// Const generic size 0, only allowing runtime checks:
    /// ```
    /// use kernel::io::IoRaw;
    ///
    /// let raw: IoRaw<0> = IoRaw::new(0xDEADBEEFC0DE, 8).unwrap();
    /// # assert_eq!(raw.addr(), 0xDEADBEEFC0DE);
    /// # assert_eq!(raw.maxsize(), 8);
    /// ```
    ///
    /// Const generic size equals maxsize:
    /// ```
    /// use kernel::io::IoRaw;
    ///
    /// let raw: IoRaw<8> = IoRaw::new(0xDEADBEEFC0DE, 8).unwrap();
    /// # assert_eq!(raw.addr(), 0xDEADBEEFC0DE);
    /// # assert_eq!(raw.maxsize(), 8);
    /// ```
    ///
    /// Const generic size bigger then maxsize:
    /// ```
    /// use kernel::io::IoRaw;
    ///
    /// IoRaw::<16>::new(0xDEADBEEFC0DE, 8).unwrap_err();
    /// ```
    pub fn new(addr: usize, maxsize: usize) -> Result<Self> {
        if maxsize < SIZE {
            return Err(EINVAL);
        }

        Ok(Self { addr, maxsize })
    }

    /// Returns the base address of the MMIO region.
    #[inline]
    pub fn addr(&self) -> usize {
        self.addr
    }

    /// Returns the maximum size of the MMIO region.
    #[inline]
    pub fn maxsize(&self) -> usize {
        self.maxsize
    }
}

impl<const SIZE: usize> core::fmt::Debug for IoRaw<SIZE> {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_struct("IoRaw")
            .field("SIZE", &SIZE)
            .field("addr", &self.addr)
            .field("maxsize", &self.maxsize)
            .finish()
    }
}

/// IO-mapped memory, starting at the base address [`addr`] and spanning [`maxsize`] bytes.
///
/// The creator (usually a subsystem / bus such as PCI) is responsible for creating the
/// mapping, performing an additional region request, etc.
///
/// # Invariants
///
/// [`addr`] is the start and [`maxsize`] the length of valid I/O mapped memory region of
/// size [`maxsize`].
///
/// [`addr`] is valid to access with the C [`read`]/[`write`] family of functions.
///
/// [`addr`]: IoAccess::addr
/// [`maxsize`]: IoAccess::maxsize
/// [`read`]: https://docs.kernel.org/driver-api/device-io.html#differences-between-i-o-access-functions
/// [`write`]: https://docs.kernel.org/driver-api/device-io.html#differences-between-i-o-access-functions
#[derive(Debug)]
#[repr(transparent)]
pub struct MMIo<const SIZE: usize = 0>(IoRaw<SIZE>);

impl<const SIZE: usize> MMIo<SIZE> {
    /// Convert a [`IoRaw`] into an [`MMIo`] instance, providing the accessors to the MMIO mapping.
    ///
    /// # Safety
    ///
    /// Callers must ensure that `addr` is the start of a valid I/O mapped memory region of
    /// size `maxsize`.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::io::{IoRaw, MMIo, IoAccess};
    ///
    /// let raw = IoRaw::<2>::new(0xDEADBEEFC0DE, 2).unwrap();
    /// // SAFETY: test, value is not actually written to.
    /// let mmio: MMIo<2> = unsafe { MMIo::from_raw(raw) };
    /// # assert_eq!(0xDEADBEEFC0DE, mmio.addr());
    /// # assert_eq!(2, mmio.maxsize());
    /// ```
    #[inline]
    pub unsafe fn from_raw(raw: IoRaw<SIZE>) -> Self {
        Self(raw)
    }

    /// Convert a ref to [`IoRaw`] into an [`MMIo`] instance, providing the accessors to the
    /// MMIo mapping.
    ///
    /// # Safety
    ///
    /// Callers must ensure that `addr` is the start of a valid I/O mapped memory region of
    /// size `maxsize`.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::io::{IoRaw, MMIo, IoAccess};
    ///
    /// let raw = IoRaw::<2>::new(0xDEADBEEFC0DE, 2).unwrap();
    /// // SAFETY: test, value is not actually written to.
    /// let mmio: &MMIo<2> = unsafe { MMIo::from_raw_ref(&raw) };
    /// # assert_eq!(raw.addr(), mmio.addr());
    /// # assert_eq!(raw.maxsize(), mmio.maxsize());
    /// ```
    #[inline]
    pub unsafe fn from_raw_ref(raw: &IoRaw<SIZE>) -> &Self {
        // SAFETY: `MMIo` is a transparent wrapper around `IoRaw`.
        unsafe { &*core::ptr::from_ref(raw).cast() }
    }
}

// SAFETY: as per invariant `raw` is valid
unsafe impl<const SIZE: usize> IoAccess<SIZE> for MMIo<SIZE> {
    #[inline]
    fn maxsize(&self) -> usize {
        self.0.maxsize()
    }

    #[inline]
    fn addr(&self) -> usize {
        self.0.addr()
    }

    unsafe fn from_raw_cookie(raw: IoRaw<SIZE>) -> Result<Self>
    where
        Self: Sized,
    {
        if is_mmio(raw.addr()) {
            // INVARIANT: `addr` is checked so it should be ok to access with read/write
            Ok(Self(raw))
        } else {
            Err(EINVAL)
        }
    }

    impl_accessor_fn!(
        read8_unchecked, readb, write8_unchecked, writeb, u8;
        read16_unchecked, readw, write16_unchecked, writew, u16;
        read32_unchecked, readl, write32_unchecked, writel, u32;
    );
}

#[cfg(CONFIG_64BIT)]
impl<const SIZE: usize> IoAccess64<SIZE> for MMIo<SIZE> {
    impl_accessor_fn!(
        read64_unchecked, readq, write64_unchecked, writeq, u64;
    );
}

impl<const SIZE: usize> IoAccessRelaxed<SIZE> for MMIo<SIZE> {
    impl_accessor_fn!(
        read8_relaxed_unchecked, readb_relaxed, write8_relaxed_unchecked, writeb_relaxed, u8;
        read16_relaxed_unchecked, readw_relaxed, write16_relaxed_unchecked, writew_relaxed, u16;
        read32_relaxed_unchecked, readl_relaxed, write32_relaxed_unchecked, writel_relaxed, u32;
    );
}

#[cfg(CONFIG_64BIT)]
impl<const SIZE: usize> IoAccess64Relaxed<SIZE> for MMIo<SIZE> {
    impl_accessor_fn!(
        read64_relaxed_unchecked, readq_relaxed, write64_relaxed_unchecked, writeq_relaxed, u64;
    );
}

/// Io that can be either PortIo or MMIo,
/// starting at the base address [`addr`] and spanning [`maxsize`] bytes.
///
/// The creator (usually a subsystem / bus such as PCI) is responsible for creating the
/// mapping, performing an additional region request, etc.
///
/// # Invariants
///
/// [`addr`] is the start and [`maxsize`] the length of a valid io region of size [`maxsize`].
///
/// [`addr`] is valid to access with the C [`ioread`]/[`iowrite`] family of functions.
///
/// [`addr`]: IoAccess::addr
/// [`maxsize`]: IoAccess::maxsize
/// [`ioread`]: https://docs.kernel.org/driver-api/device-io.html#differences-between-i-o-access-functions
/// [`iowrite`]: https://docs.kernel.org/driver-api/device-io.html#differences-between-i-o-access-functions
#[derive(Debug)]
#[repr(transparent)]
pub struct Io<const SIZE: usize = 0>(IoRaw<SIZE>);

impl<const SIZE: usize> Io<SIZE> {
    /// Convert a [`IoRaw`] into an [`Io`] instance, providing the accessors to the
    /// Io mapping.
    ///
    /// # Safety
    ///
    /// Callers must ensure that `addr` is the start of a valid I/O region of size `maxsize`.
    ///
    /// ```
    /// use kernel::io::{IoRaw, Io, IoAccess};
    ///
    /// let raw = IoRaw::<2>::new(0xDEADBEEFC0DE, 2).unwrap();
    /// // SAFETY: test, value is not actually written to.
    /// let io: Io<2> = unsafe { Io::from_raw(raw) };
    /// # assert_eq!(0xDEADBEEFC0DE, io.addr());
    /// # assert_eq!(2, io.maxsize());
    /// ```
    pub unsafe fn from_raw(raw: IoRaw<SIZE>) -> Self {
        Self(raw)
    }

    /// Convert a ref to [`IoRaw`] into an [`Io`] instance, providing the accessors to
    /// the Io mapping.
    ///
    /// # Safety
    ///
    /// Callers must ensure that `addr` is the start of a valid I/O mapped memory region of
    /// size `maxsize`.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::io::{IoRaw, Io, IoAccess};
    ///
    /// let raw = IoRaw::<2>::new(0xDEADBEEFC0DE, 2).unwrap();
    /// // SAFETY: test, value is not actually written to.
    /// let io: &Io<2> = unsafe { Io::from_raw_ref(&raw) };
    /// # assert_eq!(raw.addr(), io.addr());
    /// # assert_eq!(raw.maxsize(), io.maxsize());
    /// ```
    #[inline]
    pub unsafe fn from_raw_ref(raw: &IoRaw<SIZE>) -> &Self {
        // SAFETY: `Io` is a transparent wrapper around `IoRaw`.
        unsafe { &*core::ptr::from_ref(raw).cast() }
    }
}

// SAFETY: as per invariant `raw` is valid
unsafe impl<const SIZE: usize> IoAccess<SIZE> for Io<SIZE> {
    #[inline]
    fn addr(&self) -> usize {
        self.0.addr()
    }

    #[inline]
    fn maxsize(&self) -> usize {
        self.0.maxsize()
    }

    unsafe fn from_raw_cookie(raw: IoRaw<SIZE>) -> Result<Self>
    where
        Self: Sized,
    {
        if is_mmio(raw.addr()) || is_portio(raw.addr()) {
            // INVARIANT: `addr` is not touched so it should be able to be read with ioread/iowrite
            Ok(Self(raw))
        } else {
            Err(EINVAL)
        }
    }

    impl_accessor_fn!(
        read8_unchecked, ioread8, write8_unchecked, iowrite8, u8;
        read16_unchecked, ioread16, write16_unchecked, iowrite16, u16;
        read32_unchecked, ioread32, write32_unchecked, iowrite32, u32;
    );
}
