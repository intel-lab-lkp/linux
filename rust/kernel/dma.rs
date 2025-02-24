// SPDX-License-Identifier: GPL-2.0

//! Direct memory access (DMA).
//!
//! C header: [`include/linux/dma-mapping.h`](srctree/include/linux/dma-mapping.h)

use crate::{
    bindings, build_assert,
    device::Device,
    error::code::*,
    error::Result,
    transmute::{AsBytes, FromBytes},
    types::ARef,
};

/// Inform the kernel about the device's DMA addressing capabilities. This will set the mask for
/// both streaming and coherent APIs together.
pub fn dma_set_mask_and_coherent(dev: &Device, mask: u64) -> i32 {
    // SAFETY: device pointer is guaranteed as valid by invariant on `Device`.
    unsafe { bindings::dma_set_mask_and_coherent(dev.as_raw(), mask) }
}

/// Same as `dma_set_mask_and_coherent`, but set the mask only for streaming mappings.
pub fn dma_set_mask(dev: &Device, mask: u64) -> i32 {
    // SAFETY: device pointer is guaranteed as valid by invariant on `Device`.
    unsafe { bindings::dma_set_mask(dev.as_raw(), mask) }
}

/// Possible attributes associated with a DMA mapping.
///
/// They can be combined with the operators `|`, `&`, and `!`.
///
/// Values can be used from the [`attrs`] module.
#[derive(Clone, Copy, PartialEq)]
#[repr(transparent)]
pub struct Attrs(u32);

impl Attrs {
    /// Get the raw representation of this attribute.
    pub(crate) fn as_raw(self) -> crate::ffi::c_ulong {
        self.0 as _
    }

    /// Check whether `flags` is contained in `self`.
    pub fn contains(self, flags: Attrs) -> bool {
        (self & flags) == flags
    }
}

impl core::ops::BitOr for Attrs {
    type Output = Self;
    fn bitor(self, rhs: Self) -> Self::Output {
        Self(self.0 | rhs.0)
    }
}

impl core::ops::BitAnd for Attrs {
    type Output = Self;
    fn bitand(self, rhs: Self) -> Self::Output {
        Self(self.0 & rhs.0)
    }
}

impl core::ops::Not for Attrs {
    type Output = Self;
    fn not(self) -> Self::Output {
        Self(!self.0)
    }
}

/// DMA mapping attrributes.
pub mod attrs {
    use super::Attrs;

    /// Specifies that reads and writes to the mapping may be weakly ordered, that is that reads
    /// and writes may pass each other.
    pub const DMA_ATTR_WEAK_ORDERING: Attrs = Attrs(bindings::DMA_ATTR_WEAK_ORDERING);

    /// Specifies that writes to the mapping may be buffered to improve performance.
    pub const DMA_ATTR_WRITE_COMBINE: Attrs = Attrs(bindings::DMA_ATTR_WRITE_COMBINE);

    /// Lets the platform to avoid creating a kernel virtual mapping for the allocated buffer.
    pub const DMA_ATTR_NO_KERNEL_MAPPING: Attrs = Attrs(bindings::DMA_ATTR_NO_KERNEL_MAPPING);

    /// Allows platform code to skip synchronization of the CPU cache for the given buffer assuming
    /// that it has been already transferred to 'device' domain.
    pub const DMA_ATTR_SKIP_CPU_SYNC: Attrs = Attrs(bindings::DMA_ATTR_SKIP_CPU_SYNC);

    /// Forces contiguous allocation of the buffer in physical memory.
    pub const DMA_ATTR_FORCE_CONTIGUOUS: Attrs = Attrs(bindings::DMA_ATTR_FORCE_CONTIGUOUS);

    /// This is a hint to the DMA-mapping subsystem that it's probably not worth the time to try
    /// to allocate memory to in a way that gives better TLB efficiency.
    pub const DMA_ATTR_ALLOC_SINGLE_PAGES: Attrs = Attrs(bindings::DMA_ATTR_ALLOC_SINGLE_PAGES);

    /// This tells the DMA-mapping subsystem to suppress allocation failure reports (similarly to
    /// __GFP_NOWARN).
    pub const DMA_ATTR_NO_WARN: Attrs = Attrs(bindings::DMA_ATTR_NO_WARN);

    /// Used to indicate that the buffer is fully accessible at an elevated privilege level (and
    /// ideally inaccessible or at least read-only at lesser-privileged levels).
    pub const DMA_ATTR_PRIVILEGED: Attrs = Attrs(bindings::DMA_ATTR_PRIVILEGED);
}

/// An abstraction of the `dma_alloc_coherent` API.
///
/// This is an abstraction around the `dma_alloc_coherent` API which is used to allocate and map
/// large consistent DMA regions.
///
/// A [`CoherentAllocation`] instance contains a pointer to the allocated region (in the
/// processor's virtual address space) and the device address which can be given to the device
/// as the DMA address base of the region. The region is released once [`CoherentAllocation`]
/// is dropped.
///
/// # Invariants
///
/// For the lifetime of an instance of [`CoherentAllocation`], the cpu address is a valid pointer
/// to an allocated region of consistent memory and we hold a reference to the device.
pub struct CoherentAllocation<T: AsBytes + FromBytes> {
    dev: ARef<Device>,
    dma_handle: bindings::dma_addr_t,
    count: usize,
    cpu_addr: *mut T,
    dma_attrs: Attrs,
}

impl<T: AsBytes + FromBytes> CoherentAllocation<T> {
    /// Allocates a region of `size_of::<T> * count` of consistent memory.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::device::Device;
    /// use kernel::dma::{attrs::*, CoherentAllocation};
    ///
    /// # fn test(dev: &Device) -> Result {
    /// let c: CoherentAllocation<u64> = CoherentAllocation::alloc_attrs(dev.into(), 4, GFP_KERNEL,
    ///                                                                  DMA_ATTR_NO_WARN)?;
    /// # Ok::<(), Error>(()) }
    /// ```
    pub fn alloc_attrs(
        dev: ARef<Device>,
        count: usize,
        gfp_flags: kernel::alloc::Flags,
        dma_attrs: Attrs,
    ) -> Result<CoherentAllocation<T>> {
        build_assert!(
            core::mem::size_of::<T>() > 0,
            "It doesn't make sense for the allocated type to be a ZST"
        );

        let size = count
            .checked_mul(core::mem::size_of::<T>())
            .ok_or(EOVERFLOW)?;
        let mut dma_handle = 0;
        // SAFETY: device pointer is guaranteed as valid by invariant on `Device`.
        // We ensure that we catch the failure on this function and throw an ENOMEM
        let ret = unsafe {
            bindings::dma_alloc_attrs(
                dev.as_raw(),
                size,
                &mut dma_handle,
                gfp_flags.as_raw(),
                dma_attrs.as_raw(),
            )
        };
        if ret.is_null() {
            return Err(ENOMEM);
        }
        // INVARIANT: We just successfully allocated a coherent region which is accessible for
        // `count` elements, hence the cpu address is valid. We also hold a refcounted reference
        // to the device.
        Ok(Self {
            dev,
            dma_handle,
            count,
            cpu_addr: ret as *mut T,
            dma_attrs,
        })
    }

    /// Performs the same functionality as `alloc_attrs`, except the `dma_attrs` is 0 by default.
    pub fn alloc_coherent(
        dev: ARef<Device>,
        count: usize,
        gfp_flags: kernel::alloc::Flags,
    ) -> Result<CoherentAllocation<T>> {
        CoherentAllocation::alloc_attrs(dev, count, gfp_flags, Attrs(0))
    }

    /// Create a duplicate of the `CoherentAllocation` object but prevent it from being dropped.
    pub fn skip_drop(self) -> CoherentAllocation<T> {
        let me = core::mem::ManuallyDrop::new(self);
        Self {
            // SAFETY: The refcount of `dev` will not be decremented because this doesn't actually
            // duplicafe `ARef` and the use of `ManuallyDrop` forgets the originals.
            dev: unsafe { core::ptr::read(&me.dev) },
            dma_handle: me.dma_handle,
            count: me.count,
            cpu_addr: me.cpu_addr,
            dma_attrs: me.dma_attrs,
        }
    }

    /// Returns the base address to the allocated region in the CPU's virtual address space.
    pub fn start_ptr(&self) -> *const T {
        self.cpu_addr
    }

    /// Returns the base address to the allocated region in the CPU's virtual address space as
    /// a mutable pointer.
    pub fn start_ptr_mut(&mut self) -> *mut T {
        self.cpu_addr
    }

    /// Returns a DMA handle which may given to the device as the DMA address base of
    /// the region.
    pub fn dma_handle(&self) -> bindings::dma_addr_t {
        self.dma_handle
    }

    /// Returns the data from the region starting from `offset` as a slice.
    /// `offset` and `count` are in units of `T`, not the number of bytes.
    ///
    /// Due to the safety requirements of slice, the caller should consider that the region could
    /// be modified by the device at anytime (see the safety block below). For ringbuffer type of
    /// r/w access or use-cases where the pointer to the live data is needed, `start_ptr()` or
    /// `start_ptr_mut()` could be used instead.
    ///
    /// # Safety
    ///
    /// Callers must ensure that no hardware operations that involve the buffer are currently
    /// taking place while the returned slice is live.
    pub unsafe fn as_slice(&self, offset: usize, count: usize) -> Result<&[T]> {
        let end = offset.checked_add(count).ok_or(EOVERFLOW)?;
        if end >= self.count {
            return Err(EINVAL);
        }
        // SAFETY:
        // - The pointer is valid due to type invariant on `CoherentAllocation`,
        // we've just checked that the range and index is within bounds. The immutability of the
        // of data is also guaranteed by the safety requirements of the function.
        // - `offset` can't overflow since it is smaller than `self.count` and we've checked
        // that `self.count` won't overflow early in the constructor.
        Ok(unsafe { core::slice::from_raw_parts(self.cpu_addr.add(offset), count) })
    }

    /// Performs the same functionality as `as_slice`, except that a mutable slice is returned.
    /// See that method for documentation and safety requirements.
    ///
    /// # Safety
    ///
    /// It is the callers responsibility to avoid separate read and write accesses to the region
    /// while the returned slice is live.
    pub unsafe fn as_slice_mut(&self, offset: usize, count: usize) -> Result<&mut [T]> {
        let end = offset.checked_add(count).ok_or(EOVERFLOW)?;
        if end >= self.count {
            return Err(EINVAL);
        }
        // SAFETY:
        // - The pointer is valid due to type invariant on `CoherentAllocation`,
        // we've just checked that the range and index is within bounds. The immutability of the
        // of data is also guaranteed by the safety requirements of the function.
        // - `offset` can't overflow since it is smaller than `self.count` and we've checked
        // that `self.count` won't overflow early in the constructor.
        Ok(unsafe { core::slice::from_raw_parts_mut(self.cpu_addr.add(offset), count) })
    }

    /// Writes data to the region starting from `offset`. `offset` is in units of `T`, not the
    /// number of bytes.
    ///
    /// # Examples
    ///
    /// ```
    /// # fn test(alloc: &mut kernel::dma::CoherentAllocation<u8>) -> Result {
    /// let somedata: [u8; 4] = [0xf; 4];
    /// let buf: &[u8] = &somedata;
    /// alloc.write(buf, 0)?;
    /// # Ok::<(), Error>(()) }
    /// ```
    pub fn write(&self, src: &[T], offset: usize) -> Result {
        let end = offset.checked_add(src.len()).ok_or(EOVERFLOW)?;
        if end >= self.count {
            return Err(EINVAL);
        }
        // SAFETY:
        // - The pointer is valid due to type invariant on `CoherentAllocation`
        // and we've just checked that the range and index is within bounds.
        // - `offset` can't overflow since it is smaller than `self.count` and we've checked
        // that `self.count` won't overflow early in the constructor.
        unsafe {
            core::ptr::copy_nonoverlapping(src.as_ptr(), self.cpu_addr.add(offset), src.len())
        };
        Ok(())
    }

    /// Retrieve a single entry from the region with bounds checking. `offset` is in units of `T`,
    /// not the number of bytes.
    pub fn item_from_index(&self, offset: usize) -> Result<*mut T> {
        if offset >= self.count {
            return Err(EINVAL);
        }
        // SAFETY:
        // - The pointer is valid due to type invariant on `CoherentAllocation`
        // and we've just checked that the range and index is within bounds.
        // - `offset` can't overflow since it is smaller than `self.count` and we've checked
        // that `self.count` won't overflow early in the constructor.
        Ok(unsafe { &mut *self.cpu_addr.add(offset) })
    }

    /// Reads the value of `field` and ensures that its type is `FromBytes`
    ///
    /// # Safety:
    ///
    /// This must be called from the `dma_read` macro which ensures that the `field` pointer is
    /// validated beforehand.
    ///
    /// Public but hidden since it should only be used from `dma_read` macro.
    #[doc(hidden)]
    pub unsafe fn field_read<F: FromBytes>(&self, field: *const F) -> F {
        // SAFETY: By the safety requirements field is valid
        unsafe { field.read() }
    }

    /// Writes a value to `field` and ensures that its type is `AsBytes`
    ///
    /// # Safety:
    ///
    /// This must be called from the `dma_write` macro which ensures that the `field` pointer is
    /// validated beforehand.
    ///
    /// Public but hidden since it should only be used from `dma_write` macro.
    #[doc(hidden)]
    pub unsafe fn field_write<F: AsBytes>(&self, field: *mut F, val: F) {
        // SAFETY: By the safety requirements field is valid
        unsafe { field.write(val) }
    }
}

/// Reads a field of an item from an allocated region of structs.
/// # Examples
///
/// ```
/// struct MyStruct { field: u32, }
/// // SAFETY: All bit patterns are acceptable values for MyStruct.
/// unsafe impl kernel::transmute::FromBytes for MyStruct{};
/// // SAFETY: Instances of MyStruct have no uninitialized portions.
/// unsafe impl kernel::transmute::AsBytes for MyStruct{};
///
/// # fn test(alloc: &kernel::dma::CoherentAllocation<MyStruct>) -> Result {
/// let whole = kernel::dma_read!(alloc[2]);
/// let field = kernel::dma_read!(alloc[1].field);
/// # Ok::<(), Error>(()) }
/// ```
#[macro_export]
macro_rules! dma_read {
    ($dma:ident [ $idx:expr ] $($field:tt)* ) => {{
        let item = $dma.item_from_index($idx)?;
        // SAFETY: `item_from_index` ensures that `item` is always a valid pointer and can be
        // dereferenced. The compiler also further validates the expression on whether `field`
        // is a member of `item` when expanded by the macro.
        unsafe {
            let ptr_field = ::core::ptr::addr_of!((*item) $($field)*);
            $dma.field_read(ptr_field)
        }
    }};
}

/// Writes to a field of an item from an allocated region of structs.
/// # Examples
///
/// ```
/// struct MyStruct { member: u32, }
/// // SAFETY: All bit patterns are acceptable values for MyStruct.
/// unsafe impl kernel::transmute::FromBytes for MyStruct{};
/// // SAFETY: Instances of MyStruct have no uninitialized portions.
/// unsafe impl kernel::transmute::AsBytes for MyStruct{};
///
/// # fn test(alloc: &mut kernel::dma::CoherentAllocation<MyStruct>) -> Result {
/// kernel::dma_write!(alloc[2].member = 0xf);
/// kernel::dma_write!(alloc[1] = MyStruct { member: 0xf });
/// # Ok::<(), Error>(()) }
/// ```
#[macro_export]
macro_rules! dma_write {
    ($dma:ident [ $idx:expr ] $($field:tt)*) => {{
        kernel::dma_write!($dma, $idx, $($field)*);
    }};
    ($dma:ident, $idx: expr, = $val:expr) => {
        let item = $dma.item_from_index($idx)?;
        // SAFETY: `item_from_index` ensures that `item` is always a valid item.
        unsafe { $dma.field_write(item, $val) }
    };
    ($dma:ident, $idx: expr, $(.$field:ident)* = $val:expr) => {
        let item = $dma.item_from_index($idx)?;
        // SAFETY: `item_from_index` ensures that `item` is always a valid pointer and can be
        // dereferenced. The compiler also further validates the expression on whether `field`
        // is a member of `item` when expanded by the macro.
        unsafe {
            let ptr_field = ::core::ptr::addr_of_mut!((*item) $(.$field)*);
            $dma.field_write(ptr_field, $val)
        }
    };
}

impl<T: AsBytes + FromBytes> Drop for CoherentAllocation<T> {
    fn drop(&mut self) {
        let size = self.count * core::mem::size_of::<T>();
        // SAFETY: the device, cpu address, and the dma handle is valid due to the
        // type invariants on `CoherentAllocation`.
        unsafe {
            bindings::dma_free_attrs(
                self.dev.as_raw(),
                size,
                self.cpu_addr as _,
                self.dma_handle,
                self.dma_attrs.as_raw(),
            )
        }
    }
}
