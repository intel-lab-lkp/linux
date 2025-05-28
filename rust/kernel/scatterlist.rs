// SPDX-License-Identifier: GPL-2.0

//! Scatterlist
//!
//! C header: [`include/linux/scatterlist.h`](srctree/include/linux/scatterlist.h)

use crate::{
    bindings,
    device::{Bound, Device},
    dma::DmaDataDirection,
    error::{Error, Result},
    page::Page,
    types::{ARef, Opaque},
};
use core::marker::PhantomData;
use core::ops::{Deref, DerefMut};

/// Marker trait for the mapping state of the `SGTable`
pub trait MapState: private::Sealed {}

/// The [`Unmapped`] state of the `SGTable` is the table's initial state. While in this state, the pages of
/// the `SGTable` can be built by the CPU.
pub struct Unmapped;

/// The [`Initialized`] state of the `SGTable` means that the table's span of pages has already been built.
pub struct Initialized;

/// The [`Mapped`] state of the `SGTable` means that it is now mapped via DMA. While in this state
/// modification of the pages by the CPU is disallowed. This state will expose an interface to query
/// the DMA address of the entries.
pub struct Mapped;

mod private {
    pub trait Sealed {}

    impl Sealed for super::Mapped {}
    impl Sealed for super::Initialized {}
    impl Sealed for super::Unmapped {}
}

impl MapState for Unmapped {}
impl MapState for Initialized {}
impl MapState for Mapped {}

/// A single scatter-gather entry, representing a span of pages in the device's DMA address space.
///
/// # Invariants
///
/// The `scatterlist` pointer is valid for the lifetime of an SGEntry instance.
#[repr(transparent)]
pub struct SGEntry<T: MapState = Unmapped>(Opaque<bindings::scatterlist>, PhantomData<T>);

impl<T: MapState> SGEntry<T> {
    /// Convert a raw `struct scatterlist *` to a `&'a SGEntry`.
    ///
    /// # Safety
    ///
    /// Callers must ensure that the `struct scatterlist` pointed to by `ptr` is valid for the lifetime
    /// of the returned reference.
    pub(crate) unsafe fn as_ref<'a>(ptr: *mut bindings::scatterlist) -> &'a Self {
        // SAFETY: The pointer is valid and guaranteed by the safety requirements of the function.
        unsafe { &*ptr.cast() }
    }

    /// Convert a raw `struct scatterlist *` to a `&'a mut SGEntry`.
    ///
    /// # Safety
    ///
    /// See safety requirements of [`SGEntry::as_ref`]. In addition, callers must ensure that only
    /// a single mutable reference can be taken from the same raw pointer, i.e. for the lifetime of the
    /// returned reference, no other call to this function on the same `struct scatterlist *` should
    /// be permitted.
    pub(crate) unsafe fn as_mut<'a>(ptr: *mut bindings::scatterlist) -> &'a mut Self {
        // SAFETY: The pointer is valid and guaranteed by the safety requirements of the function.
        unsafe { &mut *ptr.cast() }
    }

    /// Obtain the raw `struct scatterlist *`.
    pub(crate) fn as_raw(&self) -> *mut bindings::scatterlist {
        self.0.get()
    }
}

impl SGEntry<Mapped> {
    /// Returns the DMA address of this SG entry.
    pub fn dma_address(&self) -> bindings::dma_addr_t {
        // SAFETY: By the type invariant of `SGEntry`, ptr is valid.
        unsafe { bindings::sg_dma_address(self.0.get()) }
    }

    /// Returns the length of this SG entry.
    pub fn dma_len(&self) -> u32 {
        // SAFETY: By the type invariant of `SGEntry`, ptr is valid.
        unsafe { bindings::sg_dma_len(self.0.get()) }
    }
}

impl SGEntry<Unmapped> {
    /// Set this entry to point at a given page.
    pub fn set_page(&mut self, page: &Page, length: u32, offset: u32) {
        let c: *mut bindings::scatterlist = self.0.get();
        // SAFETY: according to the `SGEntry` invariant, the scatterlist pointer is valid.
        // `Page` invariant also ensures the pointer is valid.
        unsafe { bindings::sg_set_page(c, page.as_ptr(), length, offset) };
    }
}

/// A scatter-gather table of DMA address spans.
///
/// This structure represents the Rust abstraction for a C `struct sg_table`. This implementation
/// is able to abstract the usage of an already existing C `struct sg_table`. A new table can be
/// allocated by calling [`SGTable::alloc_table`].
///
/// # Invariants
///
/// The `sg_table` pointer is valid for the lifetime of an SGTable instance.
#[repr(transparent)]
pub struct SGTable<T: MapState = Unmapped>(Opaque<bindings::sg_table>, PhantomData<T>);

impl<T: MapState> SGTable<T> {
    /// Convert a raw `struct sg_table *` to a `&'a SGTable`.
    ///
    /// # Safety
    ///
    /// Callers must ensure that the `struct sg_table` pointed to by `ptr` is valid for the lifetime
    /// of the returned reference.
    #[allow(unused)]
    pub(crate) unsafe fn as_ref<'a>(ptr: *mut bindings::sg_table) -> &'a Self {
        // SAFETY: Guaranteed by the safety requirements of the function.
        unsafe { &*ptr.cast() }
    }

    /// Convert a raw `struct sg_table *` to a `&'a mut SGTable`.
    ///
    /// # Safety
    ///
    /// See safety requirements of [`SGTable::as_ref`]. In addition, callers must ensure that only
    /// a single mutable reference can be taken from the same raw pointer, i.e. for the lifetime of the
    /// returned reference, no other call to this function on the same `struct sg_table *` should
    /// be permitted.
    #[allow(unused)]
    pub(crate) unsafe fn as_mut<'a>(ptr: *mut bindings::sg_table) -> &'a mut Self {
        // SAFETY: Guaranteed by the safety requirements of the function.
        unsafe { &mut *ptr.cast() }
    }

    /// Obtain the raw `struct sg_table *`.
    pub(crate) fn as_raw(&self) -> *mut bindings::sg_table {
        self.0.get()
    }

    fn take_sgt(&mut self) -> Opaque<bindings::sg_table> {
        let sgt: bindings::sg_table = Default::default();
        let sgt: Opaque<bindings::sg_table> = Opaque::new(sgt);
        core::mem::replace(&mut self.0, sgt)
    }
}

impl SGTable<Unmapped> {
    /// Allocate and construct a new scatter-gather table.
    pub fn alloc_table(nents: usize, flags: kernel::alloc::Flags) -> Result<Self> {
        let sgt: Opaque<bindings::sg_table> = Opaque::uninit();

        // SAFETY: The sgt pointer is from the Opaque-wrapped `sg_table` object hence is valid.
        let ret = unsafe { bindings::sg_alloc_table(sgt.get(), nents as _, flags.as_raw()) };
        if ret != 0 {
            return Err(Error::from_errno(ret));
        }
        Ok(Self(sgt, PhantomData))
    }

    /// The scatter-gather table page initializer.
    ///
    /// Runs a piece of code that initializes the pages of the scatter-gather table. This call transitions
    /// to and returns a `SGTable<Initialized>` object which can then be later mapped via DMA.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::{device::Device, scatterlist::*, page::*};
    ///
    /// let sgt = SGTable::alloc_table(4, GFP_KERNEL)?;
    /// let sgt = sgt.init(|iter| {
    ///     for sg in iter {
    ///         sg.set_page(&Page::alloc_page(GFP_KERNEL)?, PAGE_SIZE as u32, 0);
    ///     }
    ///     Ok(())
    /// })?;
    /// # Ok::<(), Error>(())
    /// ```
    pub fn init(
        mut self,
        f: impl FnOnce(SGTableIterMut<'_>) -> Result,
    ) -> Result<SGTable<Initialized>> {
        f(self.iter())?;
        let sgt = self.take_sgt();
        core::mem::forget(self);
        Ok(SGTable(sgt, PhantomData))
    }

    fn iter(&mut self) -> SGTableIterMut<'_> {
        SGTableIterMut {
            // SAFETY: dereferenced pointer is valid due to the type invariants on `SGTable`. This call
            // is in a private function which is allowed to be called only within the state transition
            // function [`SGTable<Unmapped>::init`] ensuring that the mutable reference can only be
            // obtained once for this object.
            pos: Some(unsafe { SGEntry::<Unmapped>::as_mut((*self.0.get()).sgl) }),
        }
    }
}

impl SGTable<Initialized> {
    /// Map this scatter-gather table describing a buffer for DMA by the `Device`.
    ///
    /// This call transitions to and returns a `DeviceSGTable` object.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::{device::{Bound, Device}, scatterlist::*};
    ///
    /// # fn test(dev: &Device<Bound>, sgt: SGTable<Initialized>) -> Result {
    /// let sgt = sgt.dma_map(dev, kernel::dma::DmaDataDirection::DmaToDevice)?;
    /// for sg in sgt.iter() {
    ///     let _addr = sg.dma_address();
    ///     let _len = sg.dma_len();
    /// }
    /// # Ok::<(), Error>(()) }
    /// ```
    pub fn dma_map(mut self, dev: &Device<Bound>, dir: DmaDataDirection) -> Result<DeviceSGTable> {
        // SAFETY: Invariants on `Device` and `SGTable` ensures that the pointers are valid.
        let ret = unsafe {
            bindings::dma_map_sgtable(
                dev.as_raw(),
                self.as_raw(),
                dir as _,
                bindings::DMA_ATTR_NO_WARN as _,
            )
        };
        if ret != 0 {
            return Err(Error::from_errno(ret));
        }
        let sgt = self.take_sgt();
        core::mem::forget(self);
        Ok(DeviceSGTable {
            sg: SGTable(sgt, PhantomData),
            dir,
            dev: dev.into(),
        })
    }
}

impl SGTable<Mapped> {
    /// Returns an immutable iterator over the scather-gather table that is mapped for DMA.
    pub fn iter(&self) -> SGTableIter<'_> {
        SGTableIter {
            // SAFETY: dereferenced pointer is valid due to the type invariants on `SGTable`.
            pos: Some(unsafe { SGEntry::<Mapped>::as_ref((*self.0.get()).sgl) }),
        }
    }
}

/// A scatter-gather table that is mapped for DMA operation.
pub struct DeviceSGTable {
    sg: SGTable<Mapped>,
    dir: DmaDataDirection,
    dev: ARef<Device>,
}

impl Drop for DeviceSGTable {
    fn drop(&mut self) {
        // SAFETY: Invariants on `Device<Bound>` and `SGTable` ensures that the `self.dev` and `self.sg`
        // pointers are valid.
        unsafe {
            bindings::dma_unmap_sgtable(self.dev.as_raw(), self.sg.as_raw(), self.dir as _, 0)
        };
    }
}

// TODO: Implement these as macros for objects that want to derive from `SGTable`.
impl Deref for DeviceSGTable {
    type Target = SGTable<Mapped>;

    fn deref(&self) -> &Self::Target {
        &self.sg
    }
}

impl DerefMut for DeviceSGTable {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.sg
    }
}

/// SAFETY: A `SGTable<Mapped>` is an immutable interface and should be safe to `Send` across threads.
unsafe impl Send for SGTable<Mapped> {}

/// A mutable iterator through `SGTable` entries.
pub struct SGTableIterMut<'a> {
    pos: Option<&'a mut SGEntry<Unmapped>>,
}

impl<'a> IntoIterator for &'a mut SGTable<Unmapped> {
    type Item = &'a mut SGEntry<Unmapped>;
    type IntoIter = SGTableIterMut<'a>;

    fn into_iter(self) -> Self::IntoIter {
        self.iter()
    }
}

impl<'a> Iterator for SGTableIterMut<'a> {
    type Item = &'a mut SGEntry<Unmapped>;

    fn next(&mut self) -> Option<Self::Item> {
        self.pos.take().map(|entry| {
            let sg = entry.as_raw();
            // SAFETY: `sg` is guaranteed to be valid and non-NULL while inside this closure.
            let next = unsafe { bindings::sg_next(sg) };
            self.pos = (!next.is_null()).then(||
                                              // SAFETY: `SGEntry::as_mut` is called on `next` only once,
                                              // which is valid and non-NULL
                                              // inside the closure.
                                              unsafe { SGEntry::as_mut(next) });
            // SAFETY: `SGEntry::as_mut` is called on `sg` only once, which is valid and non-NULL
            // inside the closure.
            unsafe { SGEntry::as_mut(sg) }
        })
    }
}

/// An iterator through `SGTable<Mapped>` entries.
pub struct SGTableIter<'a> {
    pos: Option<&'a SGEntry<Mapped>>,
}

impl<'a> IntoIterator for &'a SGTable<Mapped> {
    type Item = &'a SGEntry<Mapped>;
    type IntoIter = SGTableIter<'a>;

    fn into_iter(self) -> Self::IntoIter {
        self.iter()
    }
}

impl<'a> Iterator for SGTableIter<'a> {
    type Item = &'a SGEntry<Mapped>;

    fn next(&mut self) -> Option<Self::Item> {
        self.pos.map(|entry| {
            let sg = entry.as_raw();
            // SAFETY: `sg` is always guaranteed to be valid and non-NULL while inside this closure.
            let next = unsafe { bindings::sg_next(sg) };
            self.pos = (!next.is_null()).then(||
                                              // SAFETY: `next` is always valid and non-NULL inside
                                              // this closure.
                                              unsafe { SGEntry::as_ref(next) });
            // SAFETY: `sg` is always guaranteed to be valid and non-NULL while inside this closure.
            unsafe { SGEntry::as_ref(sg) }
        })
    }
}

impl<T: MapState> Drop for SGTable<T> {
    fn drop(&mut self) {
        // SAFETY: Invariant on `SGTable` ensures that the sg_table is valid.
        unsafe { bindings::sg_free_table(self.as_raw()) };
    }
}
