// SPDX-License-Identifier: GPL-2.0

//! Scatterlist
//!
//! C header: [`include/linux/scatterlist.h`](srctree/include/linux/scatterlist.h)

use core::borrow::{Borrow, BorrowMut};

use crate::{
    bindings,
    device::{Bound, Device},
    dma::DmaDataDirection,
    error::{Error, Result},
    page::Page,
    types::{ARef, Opaque},
};

/// A single scatter-gather entry, representing a span of pages in the device's DMA address space.
///
/// This interface is accessible only via the `SGTable` iterators. When using the API safely, certain
/// methods are only available depending on a specific state of operation of the scatter-gather table,
/// i.e. setting page entries is done internally only during construction while retrieving the DMA address
/// is only possible when the `SGTable` is already mapped for DMA via a device.
///
/// # Invariants
///
/// The `scatterlist` pointer is valid for the lifetime of an SGEntry instance.
#[repr(transparent)]
pub struct SGEntry(Opaque<bindings::scatterlist>);

impl SGEntry {
    /// Convert a raw `struct scatterlist *` to a `&'a SGEntry`.
    ///
    /// This is meant as a helper for other kernel subsystems and not to be used by device drivers directly.
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
    /// This is meant as a helper for other kernel subsystems and not to be used by device drivers directly.
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

    /// Internal constructor helper to set this entry to point at a given page. Not to be used directly.
    fn set_page(&mut self, page: &Page, length: u32, offset: u32) {
        let c: *mut bindings::scatterlist = self.0.get();
        // SAFETY: according to the `SGEntry` invariant, the scatterlist pointer is valid.
        // `Page` invariant also ensure the pointer is valid.
        unsafe { bindings::sg_set_page(c, page.as_ptr(), length, offset) };
    }
}

/// Trait implemented by all mapping states.
pub trait MappingState {}

/// Trait implemented by all mapping states representing the fact that a `struct sg_table` is
/// mapped (and thus its DMA addresses are valid).
pub trait MappedState: MappingState {}

/// Represents the fact that a `struct sg_table` is not DMA-mapped.
pub struct Unmapped;
impl MappingState for Unmapped {}

/// Represents the fact that a `struct sg_table` is DMA-mapped by an external entity.
pub struct BorrowedMapping;
impl MappingState for BorrowedMapping {}
impl MappedState for BorrowedMapping {}

/// A managed DMA mapping of a `struct sg_table` to a given device.
///
/// The mapping is cleared when this object is dropped.
///
/// # Invariants
///
/// - The `scatterlist` pointer is valid for the lifetime of a `ManagedMapping` instance.
/// - The `Device` instance is within a [`kernel::device::Bound`] context.
pub struct ManagedMapping {
    dev: ARef<Device>,
    dir: DmaDataDirection,
    // This works because the `sgl` member of `struct sg_table` never moves, and the fact we can
    // build this implies that we have an exclusive reference to the `sg_table`, thus it cannot be
    // modified by anyone else.
    sgl: *mut bindings::scatterlist,
    orig_nents: ffi::c_uint,
}

/// SAFETY: An `ManagedMapping` object is an immutable interface and should be safe to `Send` across threads.
unsafe impl Send for ManagedMapping {}
impl MappingState for ManagedMapping {}
impl MappedState for ManagedMapping {}

impl Drop for ManagedMapping {
    fn drop(&mut self) {
        // SAFETY: Invariants on `Device<Bound>` and `Self` ensures that the `self.dev` and `self.sgl`
        // are valid.
        unsafe {
            bindings::dma_unmap_sg_attrs(
                self.dev.as_raw(),
                self.sgl,
                self.orig_nents as i32,
                self.dir as i32,
                0,
            )
        };
    }
}

/// A scatter-gather table of DMA address spans.
///
/// This structure represents the Rust abstraction for a C `struct sg_table`. This implementation
/// abstracts the usage of an already existing C `struct sg_table` within Rust code that we get
/// passed from the C side.
pub struct SGTable<T: Borrow<bindings::sg_table>, M: MappingState> {
    /// Mapping state of the underlying `struct sg_table`.
    ///
    /// This defines which methods of `SGTable` are available.
    ///
    /// Declared first so it is dropped before `table`, so we remove the mapping before freeing the
    /// SG table if the latter is owned.
    _mapping: M,

    /// Something that can borrow the underlying `struct sg_table`.
    table: T,
}

impl<T> SGTable<T, Unmapped>
where
    T: Borrow<bindings::sg_table>,
{
    /// Create a new unmapped `SGTable` from an already-existing `struct sg_table`.
    ///
    /// # Safety
    ///
    /// Callers must ensure that the `struct sg_table` borrowed by `r` is initialized, valid for
    /// the lifetime of the returned reference, and is not mapped.
    pub unsafe fn new_unmapped(r: T) -> Self {
        Self {
            table: r,
            _mapping: Unmapped,
        }
    }
}

impl<T> SGTable<T, BorrowedMapping>
where
    T: Borrow<bindings::sg_table>,
{
    /// Create a new mapped `SGTable` from an already-existing `struct sg_table`.
    ///
    /// # Safety
    ///
    /// Callers must ensure that the `struct sg_table` borrowed by `r` is initialized, valid for
    /// the lifetime of the returned reference, and is DMA-mapped.
    pub unsafe fn new_mapped(r: T) -> Self {
        Self {
            table: r,
            _mapping: BorrowedMapping,
        }
    }
}

impl<T, M> SGTable<T, M>
where
    T: Borrow<bindings::sg_table>,
    M: MappedState,
{
    /// Returns an immutable iterator over the scatter-gather table.
    pub fn iter(&self) -> SGTableIter<'_> {
        SGTableIter {
            // SAFETY: dereferenced pointer is valid due to the type invariants on `SGTable`.
            pos: Some(unsafe { SGEntry::as_ref(self.table.borrow().sgl) }),
        }
    }
}

/// Provides a list of pages that can be used to build a `SGTable`.
pub trait SGTablePages {
    /// Returns an iterator to the pages providing the backing memory of `self`.
    ///
    /// Implementers should return an iterator which provides information regarding each page entry to
    /// build the `SGTable`. The first element in the tuple is a reference to the Page, the second element
    /// as the offset into the page, and the third as the length of data. The fields correspond to the
    /// first three fields of the C `struct scatterlist`.
    fn iter<'a>(&'a self) -> impl Iterator<Item = (&'a Page, usize, usize)>;

    /// Returns the number of pages in the list.
    fn entries(&self) -> usize;
}

/// An iterator through `SGTable` entries.
pub struct SGTableIter<'a> {
    pos: Option<&'a SGEntry>,
}

impl<'a> Iterator for SGTableIter<'a> {
    type Item = &'a SGEntry;

    fn next(&mut self) -> Option<Self::Item> {
        let entry = self.pos;
        // SAFETY: `sg` is an immutable reference and is equivalent to `scatterlist` via its type
        // invariants, so its safe to use with sg_next.
        let next = unsafe { bindings::sg_next(self.pos?.as_raw()) };

        // SAFETY: `sg_next` returns either a valid pointer to a `scatterlist`, or null if we
        // are at the end of the scatterlist.
        self.pos = (!next.is_null()).then(|| unsafe { SGEntry::as_ref(next) });
        entry
    }
}

impl<'a, T, M> IntoIterator for &'a SGTable<T, M>
where
    T: Borrow<bindings::sg_table>,
    M: MappedState,
{
    type Item = &'a SGEntry;
    type IntoIter = SGTableIter<'a>;

    fn into_iter(self) -> Self::IntoIter {
        self.iter()
    }
}

impl<T> SGTable<T, Unmapped>
where
    T: BorrowMut<bindings::sg_table>,
{
    /// Map this scatter-gather table describing a buffer for DMA by the `Device`.
    ///
    /// To prevent the table from being mapped more than once, this call consumes `self` and transfers
    /// ownership of resources to the new `SGTable<_, ManagedMapping>` object.
    pub fn dma_map(
        mut self,
        dev: &Device<Bound>,
        dir: DmaDataDirection,
    ) -> Result<SGTable<T, ManagedMapping>> {
        // SAFETY: Invariants on `Device<Bound>` and `SGTable` ensures that the pointers are valid.
        let ret = unsafe {
            bindings::dma_map_sgtable(
                dev.as_raw(),
                self.table.borrow_mut(),
                dir as i32,
                bindings::DMA_ATTR_NO_WARN as usize,
            )
        };
        if ret != 0 {
            return Err(Error::from_errno(ret));
        }

        let sgl = self.table.borrow_mut().sgl;
        let orig_nents = self.table.borrow().orig_nents;

        Ok(SGTable {
            table: self.table,
            // INVARIANT:
            // - `sgl` is valid by the type invariant of `OwnedSgt`.
            // - `dev` is a reference to Device<Bound>.
            _mapping: ManagedMapping {
                dev: dev.into(),
                dir,
                sgl,
                orig_nents,
            },
        })
    }
}

/// An owned `struct sg_table`, which lifetime is tied to this object.
///
/// # Invariants
///
/// The `sg_table` is valid and initialized for the lifetime of an `OwnedSgt` instance.
pub struct OwnedSgt<P: SGTablePages> {
    sgt: bindings::sg_table,
    /// Used to keep the memory pointed to by `sgt` alive.
    _pages: P,
}

/// SAFETY: An `OwnedSgt` object is constructed internally by `SGTable` and no interface is exposed to
/// the user to modify its state after construction, except [`SGTable::dma_map`] which transfers
/// ownership of the object, hence should be safe to `Send` across threads.
unsafe impl<P: SGTablePages> Send for OwnedSgt<P> {}

impl<P> Drop for OwnedSgt<P>
where
    P: SGTablePages,
{
    fn drop(&mut self) {
        // SAFETY: Invariant on `OwnedSgt` ensures that the sg_table is valid.
        unsafe { bindings::sg_free_table(&mut self.sgt) };
    }
}

impl<P> Borrow<bindings::sg_table> for OwnedSgt<P>
where
    P: SGTablePages,
{
    fn borrow(&self) -> &bindings::sg_table {
        &self.sgt
    }
}

// To allow mapping the state!
impl<P> BorrowMut<bindings::sg_table> for OwnedSgt<P>
where
    P: SGTablePages,
{
    fn borrow_mut(&mut self) -> &mut bindings::sg_table {
        &mut self.sgt
    }
}

impl<P: SGTablePages> SGTable<OwnedSgt<P>, Unmapped> {
    /// Allocate and build a new `SGTable` from an existing list of `pages`. This method moves the
    /// ownership of `pages` to the table.
    ///
    /// To build a scatter-gather table, provide the `pages` object which must implement the
    /// `SGTablePages` trait.
    ///
    ///# Examples
    ///
    /// ```
    /// use kernel::{device::Device, scatterlist::*, page::*, prelude::*};
    ///
    /// struct PagesArray(KVec<Page>);
    /// impl SGTablePages for PagesArray {
    ///     fn iter<'a>(&'a self) -> impl Iterator<Item = (&'a Page, usize, usize)> {
    ///         self.0.iter().map(|page| (page, kernel::page::PAGE_SIZE, 0))
    ///     }
    ///
    ///     fn entries(&self) -> usize {
    ///         self.0.len()
    ///     }
    /// }
    ///
    /// let mut pages = KVec::new();
    /// let _ = pages.push(Page::alloc_page(GFP_KERNEL)?, GFP_KERNEL);
    /// let _ = pages.push(Page::alloc_page(GFP_KERNEL)?, GFP_KERNEL);
    /// let sgt = SGTable::new_owned(PagesArray(pages), GFP_KERNEL)?;
    /// # Ok::<(), Error>(())
    /// ```
    pub fn new_owned(pages: P, flags: kernel::alloc::Flags) -> Result<Self> {
        // SAFETY: `sgt` is not a reference.
        let mut sgt: bindings::sg_table = unsafe { core::mem::zeroed() };

        // SAFETY: The sgt pointer is from the Opaque-wrapped `sg_table` object hence is valid.
        let ret =
            unsafe { bindings::sg_alloc_table(&mut sgt, pages.entries() as u32, flags.as_raw()) };
        if ret != 0 {
            return Err(Error::from_errno(ret));
        }
        // SAFETY: We just successfully allocated `sgt`, hence the pointer is valid and have sole access to
        // it at this point.
        let sgentries = unsafe { core::slice::from_raw_parts_mut(sgt.sgl, pages.entries()) };
        for (entry, page) in sgentries
            .iter_mut()
            .map(|e|
                 // SAFETY: `SGEntry::as_mut` is called on the pointer only once, which is valid and non-NULL
                 // while inside the closure.
                 unsafe { SGEntry::as_mut(e) })
            .zip(pages.iter())
        {
            entry.set_page(page.0, page.1 as u32, page.2 as u32)
        }

        Ok(Self {
            // INVARIANT: We just successfully allocated and built the table from the page entries.
            table: OwnedSgt { sgt, _pages: pages },
            _mapping: Unmapped,
        })
    }
}
