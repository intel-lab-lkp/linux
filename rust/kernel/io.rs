// SPDX-License-Identifier: GPL-2.0

//! Memory-mapped IO.
//!
//! C header: [`include/asm-generic/io.h`](srctree/include/asm-generic/io.h)

use crate::{
    bindings,
    prelude::*, //
};

pub mod mem;
pub mod poll;
pub mod resource;

pub use resource::Resource;

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

/// Raw representation of an MMIO region.
///
/// By itself, the existence of an instance of this structure does not provide any guarantees that
/// the represented MMIO region does exist or is properly mapped.
///
/// Instead, the bus specific MMIO implementation must convert this raw representation into an
/// `Mmio` instance providing the actual memory accessors. Only by the conversion into an `Mmio`
/// structure any guarantees are given.
pub struct MmioRaw<const SIZE: usize = 0> {
    addr: usize,
    maxsize: usize,
}

impl<const SIZE: usize> MmioRaw<SIZE> {
    /// Returns a new `MmioRaw` instance on success, an error otherwise.
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

/// IO-mapped memory region.
///
/// The creator (usually a subsystem / bus such as PCI) is responsible for creating the
/// mapping, performing an additional region request etc.
///
/// # Invariant
///
/// `addr` is the start and `maxsize` the length of valid I/O mapped memory region of size
/// `maxsize`.
///
/// # Examples
///
/// ```no_run
/// use kernel::{
///     bindings,
///     ffi::c_void,
///     io::{
///         Io,
///         IoKnownSize,
///         Mmio,
///         MmioRaw,
///         PhysAddr,
///     },
/// };
/// use core::ops::Deref;
///
/// // See also `pci::Bar` for a real example.
/// struct IoMem<const SIZE: usize>(MmioRaw<SIZE>);
///
/// impl<const SIZE: usize> IoMem<SIZE> {
///     /// # Safety
///     ///
///     /// [`paddr`, `paddr` + `SIZE`) must be a valid MMIO region that is mappable into the CPUs
///     /// virtual address space.
///     unsafe fn new(paddr: usize) -> Result<Self>{
///         // SAFETY: By the safety requirements of this function [`paddr`, `paddr` + `SIZE`) is
///         // valid for `ioremap`.
///         let addr = unsafe { bindings::ioremap(paddr as PhysAddr, SIZE) };
///         if addr.is_null() {
///             return Err(ENOMEM);
///         }
///
///         Ok(IoMem(MmioRaw::new(addr as usize, SIZE)?))
///     }
/// }
///
/// impl<const SIZE: usize> Drop for IoMem<SIZE> {
///     fn drop(&mut self) {
///         // SAFETY: `self.0.addr()` is guaranteed to be properly mapped by `Self::new`.
///         unsafe { bindings::iounmap(self.0.addr() as *mut c_void); };
///     }
/// }
///
/// impl<const SIZE: usize> Deref for IoMem<SIZE> {
///    type Target = Mmio<SIZE>;
///
///    fn deref(&self) -> &Self::Target {
///         // SAFETY: The memory range stored in `self` has been properly mapped in `Self::new`.
///         unsafe { Mmio::from_raw(&self.0) }
///    }
/// }
///
///# fn no_run() -> Result<(), Error> {
/// // SAFETY: Invalid usage for example purposes.
/// let iomem = unsafe { IoMem::<{ core::mem::size_of::<u32>() }>::new(0xBAAAAAAD)? };
/// iomem.write32(0x42, 0x0);
/// assert!(iomem.try_write32(0x42, 0x0).is_ok());
/// assert!(iomem.try_write32(0x42, 0x4).is_err());
/// # Ok(())
/// # }
/// ```
#[repr(transparent)]
pub struct Mmio<const SIZE: usize = 0>(MmioRaw<SIZE>);

macro_rules! define_read {
    (infallible, $(#[$attr:meta])* $vis:vis $name:ident, $c_fn:ident -> $type_name:ty) => {
        /// Read IO data from a given offset known at compile time.
        ///
        /// Bound checks are performed on compile time, hence if the offset is not known at compile
        /// time, the build will fail.
        $(#[$attr])*
        #[inline]
        $vis fn $name(&self, offset: usize) -> $type_name {
            let addr = self.io_addr_assert::<$type_name>(offset);

            // SAFETY: By the type invariant `addr` is a valid address for MMIO operations.
            unsafe { bindings::$c_fn(addr as *const c_void) }
        }
    };

    (fallible, $(#[$attr:meta])* $vis:vis $try_name:ident, $c_fn:ident -> $type_name:ty) => {
        /// Read IO data from a given offset.
        ///
        /// Bound checks are performed on runtime, it fails if the offset (plus the type size) is
        /// out of bounds.
        $(#[$attr])*
        $vis fn $try_name(&self, offset: usize) -> Result<$type_name> {
            let addr = self.io_addr::<$type_name>(offset)?;

            // SAFETY: By the type invariant `addr` is a valid address for MMIO operations.
            Ok(unsafe { bindings::$c_fn(addr as *const c_void) })
        }
    };
}

macro_rules! define_write {
    (infallible, $(#[$attr:meta])* $vis:vis $name:ident, $c_fn:ident <- $type_name:ty) => {
        /// Write IO data from a given offset known at compile time.
        ///
        /// Bound checks are performed on compile time, hence if the offset is not known at compile
        /// time, the build will fail.
        $(#[$attr])*
        #[inline]
        $vis fn $name(&self, value: $type_name, offset: usize) {
            let addr = self.io_addr_assert::<$type_name>(offset);

            // SAFETY: By the type invariant `addr` is a valid address for MMIO operations.
            unsafe { bindings::$c_fn(value, addr as *mut c_void) }
        }
    };

    (fallible, $(#[$attr:meta])* $vis:vis $try_name:ident, $c_fn:ident <- $type_name:ty) => {
        /// Write IO data from a given offset.
        ///
        /// Bound checks are performed on runtime, it fails if the offset (plus the type size) is
        /// out of bounds.
        $(#[$attr])*
        $vis fn $try_name(&self, value: $type_name, offset: usize) -> Result {
            let addr = self.io_addr::<$type_name>(offset)?;

            // SAFETY: By the type invariant `addr` is a valid address for MMIO operations.
            unsafe { bindings::$c_fn(value, addr as *mut c_void) }
            Ok(())
        }
    };
}

/// Checks whether an access of type `U` at the given `offset`
/// is valid within this region.
#[inline]
const fn offset_valid<U>(offset: usize, size: usize) -> bool {
    let type_size = core::mem::size_of::<U>();
    if let Some(end) = offset.checked_add(type_size) {
        end <= size && offset % type_size == 0
    } else {
        false
    }
}

/// Represents a region of I/O space of a fixed size.
///
/// This is an abstract representation to be implemented by arbitrary I/O
/// backends (e.g. MMIO, I2C, etc.).
///
/// Provides common helpers for offset validation and address
/// calculation on top of a base address and maximum size.
pub trait IoBase {
    /// Minimum usable size of this region.
    const MIN_SIZE: usize;

    /// Returns the base address of this mapping.
    fn addr(&self) -> usize;

    /// Returns the maximum size of this mapping.
    fn maxsize(&self) -> usize;

    /// Returns the absolute I/O address for a given `offset`,
    /// performing runtime bound checks.
    #[inline]
    fn io_addr<U>(&self, offset: usize) -> Result<usize> {
        if !offset_valid::<U>(offset, self.maxsize()) {
            return Err(EINVAL);
        }

        // Probably no need to check, since the safety requirements of `Self::new` guarantee that
        // this can't overflow.
        self.addr().checked_add(offset).ok_or(EINVAL)
    }

    /// Returns the absolute I/O address for a given `offset`,
    /// performing compile-time bound checks.
    #[inline]
    fn io_addr_assert<U>(&self, offset: usize) -> usize {
        build_assert!(offset_valid::<U>(offset, Self::MIN_SIZE));

        self.addr() + offset
    }
}

/// Types implementing this trait (e.g. MMIO BARs or PCI config regions)
/// can share the same IoKnownSize accessors.
///
/// In this context, "known size" represents the static guarantee regarding
/// the minimum accessible size requested from the I/O backend.
///
/// Note that the actual size of the resource provided by the hardware
/// (e.g., the physical BAR size) might be larger than this "known size".
pub trait IoKnownSize: IoBase {
    /// Infallible 8-bit read with compile-time bounds check.
    fn read8(&self, offset: usize) -> u8;

    /// Infallible 16-bit read with compile-time bounds check.
    fn read16(&self, offset: usize) -> u16;

    /// Infallible 32-bit read with compile-time bounds check.
    fn read32(&self, offset: usize) -> u32;

    /// Infallible 8-bit write with compile-time bounds check.
    fn write8(&self, value: u8, offset: usize);

    /// Infallible 16-bit write with compile-time bounds check.
    fn write16(&self, value: u16, offset: usize);

    /// Infallible 32-bit write with compile-time bounds check.
    fn write32(&self, value: u32, offset: usize);
}

/// Types implementing this trait (e.g. MMIO BARs or PCI config regions)
/// can share the same Io accessors.
///
/// This trait defines the accessors for performing runtime-checked
/// operations on an I/O region. Note that due to the runtime checks, these
/// operations may be more expensive than those provided by
/// [`IoKnownSize`].
///
/// Implement this trait when:
/// - The I/O backend cannot guarantee a minimum accessible size for the
///   region.
/// - The I/O backend wishes to return meaningful error codes when the
///   driver accesses the region.
pub trait Io: IoBase {
    /// Fallible 8-bit read with runtime bounds check.
    fn try_read8(&self, offset: usize) -> Result<u8>;

    /// Fallible 16-bit read with runtime bounds check.
    fn try_read16(&self, offset: usize) -> Result<u16>;

    /// Fallible 32-bit read with runtime bounds check.
    fn try_read32(&self, offset: usize) -> Result<u32>;

    /// Fallible 8-bit write with runtime bounds check.
    fn try_write8(&self, value: u8, offset: usize) -> Result;

    /// Fallible 16-bit write with runtime bounds check.
    fn try_write16(&self, value: u16, offset: usize) -> Result;

    /// Fallible 32-bit write with runtime bounds check.
    fn try_write32(&self, value: u32, offset: usize) -> Result;
}

/// Represents a region of I/O space of a fixed size with 64-bit accessors.
/// Types implementing this trait can share the same IoKnownSize64
/// accessors.
#[cfg(CONFIG_64BIT)]
pub trait IoKnownSize64: IoKnownSize {
    /// Infallible 64-bit read with compile-time bounds check.
    fn read64(&self, offset: usize) -> u64;

    /// Infallible 64-bit write with compile-time bounds check.
    fn write64(&self, value: u64, offset: usize);
}

/// Types implementing this trait can share the same Io64 accessors.
#[cfg(CONFIG_64BIT)]
pub trait Io64: Io {
    /// Fallible 64-bit read with runtime bounds check.
    fn try_read64(&self, offset: usize) -> Result<u64>;

    /// Fallible 64-bit write with runtime bounds check.
    fn try_write64(&self, value: u64, offset: usize) -> Result;
}

impl<const SIZE: usize> IoBase for Mmio<SIZE> {
    const MIN_SIZE: usize = SIZE;

    /// Returns the base address of this mapping.
    #[inline]
    fn addr(&self) -> usize {
        self.0.addr()
    }

    /// Returns the maximum size of this mapping.
    #[inline]
    fn maxsize(&self) -> usize {
        self.0.maxsize()
    }
}

impl<const SIZE: usize> IoKnownSize for Mmio<SIZE> {
    define_read!(infallible, read8, readb -> u8);
    define_read!(infallible, read16, readw -> u16);
    define_read!(infallible, read32, readl -> u32);

    define_write!(infallible, write8, writeb <- u8);
    define_write!(infallible, write16, writew <- u16);
    define_write!(infallible, write32, writel <- u32);
}

impl<const SIZE: usize> Io for Mmio<SIZE> {
    define_read!(fallible, try_read8, readb -> u8);
    define_read!(fallible, try_read16, readw -> u16);
    define_read!(fallible, try_read32, readl -> u32);

    define_write!(fallible, try_write8, writeb <- u8);
    define_write!(fallible, try_write16, writew <- u16);
    define_write!(fallible, try_write32, writel <- u32);
}

#[cfg(CONFIG_64BIT)]
impl<const SIZE: usize> IoKnownSize64 for Mmio<SIZE> {
    define_read!(infallible, read64, readq -> u64);

    define_write!(infallible, write64, writeq <- u64);
}

#[cfg(CONFIG_64BIT)]
impl<const SIZE: usize> Io64 for Mmio<SIZE> {
    define_read!(fallible, try_read64, readq -> u64);

    define_write!(fallible, try_write64, writeq <- u64);
}

impl<const SIZE: usize> Mmio<SIZE> {
    /// Converts an `MmioRaw` into an `Mmio` instance, providing the accessors to the MMIO mapping.
    ///
    /// # Safety
    ///
    /// Callers must ensure that `addr` is the start of a valid I/O mapped memory region of size
    /// `maxsize`.
    pub unsafe fn from_raw(raw: &MmioRaw<SIZE>) -> &Self {
        // SAFETY: `Mmio` is a transparent wrapper around `MmioRaw`.
        unsafe { &*core::ptr::from_ref(raw).cast() }
    }

    define_read!(infallible, pub read8_relaxed, readb_relaxed -> u8);
    define_read!(infallible, pub read16_relaxed, readw_relaxed -> u16);
    define_read!(infallible, pub read32_relaxed, readl_relaxed -> u32);
    define_read!(
        infallible,
        #[cfg(CONFIG_64BIT)]
        pub read64_relaxed,
        readq_relaxed -> u64
    );

    define_read!(fallible, pub try_read8_relaxed, readb_relaxed -> u8);
    define_read!(fallible, pub try_read16_relaxed, readw_relaxed -> u16);
    define_read!(fallible, pub try_read32_relaxed, readl_relaxed -> u32);
    define_read!(
        fallible,
        #[cfg(CONFIG_64BIT)]
        pub try_read64_relaxed,
        readq_relaxed -> u64
    );

    define_write!(infallible, pub write8_relaxed, writeb_relaxed <- u8);
    define_write!(infallible, pub write16_relaxed, writew_relaxed <- u16);
    define_write!(infallible, pub write32_relaxed, writel_relaxed <- u32);
    define_write!(
        infallible,
        #[cfg(CONFIG_64BIT)]
        pub write64_relaxed,
        writeq_relaxed <- u64
    );

    define_write!(fallible, pub try_write8_relaxed, writeb_relaxed <- u8);
    define_write!(fallible, pub try_write16_relaxed, writew_relaxed <- u16);
    define_write!(fallible, pub try_write32_relaxed, writel_relaxed <- u32);
    define_write!(
        fallible,
        #[cfg(CONFIG_64BIT)]
        pub try_write64_relaxed,
        writeq_relaxed <- u64
    );
}
