// SPDX-License-Identifier: GPL-2.0

//! Kernel page allocation and management.

use crate::{
    alloc::{AllocError, Allocator, Flags, VVec, KVec, KVVec, Vec, flags::*},
    bindings,
    error::code::*,
    error::Result,
    uaccess::UserSliceReader,
    types::{Opaque, ARef},
};
use core::ptr::{self, NonNull};

/// A bitwise shift for the page size.
pub const PAGE_SHIFT: usize = bindings::PAGE_SHIFT as usize;

/// The number of bytes in a page.
pub const PAGE_SIZE: usize = bindings::PAGE_SIZE;

/// A bitmask that gives the page containing a given address.
pub const PAGE_MASK: usize = !(PAGE_SIZE - 1);

/// Round up the given number to the next multiple of [`PAGE_SIZE`].
///
/// It is incorrect to pass an address where the next multiple of [`PAGE_SIZE`] doesn't fit in a
/// [`usize`].
pub const fn page_align(addr: usize) -> usize {
    // Parentheses around `PAGE_SIZE - 1` to avoid triggering overflow sanitizers in the wrong
    // cases.
    (addr + (PAGE_SIZE - 1)) & PAGE_MASK
}

/// A pointer to a reference-counted page.
///
/// # Invariants
///
/// The pointer is valid.
#[repr(transparent)]
pub struct Page {
    page: Opaque<bindings::page>,
}

// SAFETY: Pages have no logic that relies on them staying on a given thread, so moving them across
// threads is safe.
unsafe impl Send for Page {}

// SAFETY: Pages have no logic that relies on them not being accessed concurrently, so accessing
// them concurrently is safe.
unsafe impl Sync for Page {}

impl Page {
    /// Allocates a new page.
    ///
    /// # Examples
    ///
    /// Allocate memory for a page.
    ///
    /// ```
    /// use kernel::page::Page;
    ///
    /// # fn dox() -> Result<(), kernel::alloc::AllocError> {
    /// let page = Page::alloc_page(GFP_KERNEL)?;
    /// # Ok(()) }
    /// ```
    ///
    /// Allocate memory for a page and zero its contents.
    ///
    /// ```
    /// use kernel::page::Page;
    ///
    /// # fn dox() -> Result<(), kernel::alloc::AllocError> {
    /// let page = Page::alloc_page(GFP_KERNEL | __GFP_ZERO)?;
    /// # Ok(()) }
    /// ```
    pub fn alloc_page(flags: Flags) -> Result<ARef<Self>, AllocError> {
        // SAFETY: Depending on the value of `gfp_flags`, this call may sleep. Other than that, it
        // is always safe to call this method.
        let page = unsafe { bindings::alloc_pages(flags.as_raw(), 0) };
        if page.is_null() {
            return Err(AllocError);
        }
        // CAST: Self` is a `repr(transparent)` wrapper around `bindings::page`.
        let ptr = page.cast::<Self>();
        // INVARIANT: We just successfully allocated a page, ptr points to the new `Page` object.
        // SAFETY: According to invariant above ptr is valid.
        Ok(unsafe { ARef::from_raw(NonNull::new_unchecked(ptr)) })
    }

    /// Create a page object from a buffer which is associated with an existing C `struct page`.
    ///
    /// This function ensures it takes a page-sized buffer as represented by `PageSlice`.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::page::*;
    ///
    /// let somedata: [u8; PAGE_SIZE * 2] = [0; PAGE_SIZE * 2];
    /// let buf: &[u8] = &somedata;
    /// let pages: VVec<PageSlice> = buf.try_into()?;
    /// let page = Page::page_slice_to_page(&pages[0])?;
    /// # Ok::<(), Error>(())
    /// ```
    pub fn page_slice_to_page<'a>(page: &PageSlice) -> Result<&'a Self>
    {
        let ptr: *const core::ffi::c_void = page.0.as_ptr() as _;
        if ptr.is_null() {
            return Err(EINVAL)
        }
        // SAFETY: We've checked that `ptr` is non-null, hence it's safe to call this method.
        let page = if unsafe { bindings::is_vmalloc_addr(ptr) } {
            // SAFETY: We've checked that `ptr` is non-null and within the vmalloc range, hence
            // it's safe to call this method.
            unsafe { bindings::vmalloc_to_page(ptr) }
        // SAFETY: We've checked that `ptr` is non-null, hence it's safe to call this method.
        } else if unsafe { bindings::virt_addr_valid(ptr) } {
            // SAFETY: We've checked that `ptr` is non-null and a valid virtual address, hence
            // it's safe to call this method.
            unsafe { bindings::virt_to_page(ptr) }
        } else {
            ptr::null_mut()
        };
        if page.is_null() {
            return Err(EINVAL);
        }
        // CAST: `Self` is a `repr(transparent)` wrapper around `bindings::page`.
        // SAFETY: We just successfully retrieved an existing `bindings::page`, therefore
        // dereferencing the page pointer is valid.
        Ok(unsafe { &*page.cast() })
    }

    /// Returns a raw pointer to the page.
    pub fn as_ptr(&self) -> *mut bindings::page {
        self.page.get()
    }

    /// Runs a piece of code with this page mapped to an address.
    ///
    /// The page is unmapped when this call returns.
    ///
    /// # Using the raw pointer
    ///
    /// It is up to the caller to use the provided raw pointer correctly. The pointer is valid for
    /// `PAGE_SIZE` bytes and for the duration in which the closure is called. The pointer might
    /// only be mapped on the current thread, and when that is the case, dereferencing it on other
    /// threads is UB. Other than that, the usual rules for dereferencing a raw pointer apply: don't
    /// cause data races, the memory may be uninitialized, and so on.
    ///
    /// If multiple threads map the same page at the same time, then they may reference with
    /// different addresses. However, even if the addresses are different, the underlying memory is
    /// still the same for these purposes (e.g., it's still a data race if they both write to the
    /// same underlying byte at the same time).
    fn with_page_mapped<T>(&self, f: impl FnOnce(*mut u8) -> T) -> T {
        // SAFETY: `page` is valid due to the type invariants on `Page`.
        let mapped_addr = unsafe { bindings::kmap_local_page(self.as_ptr()) };

        let res = f(mapped_addr.cast());

        // This unmaps the page mapped above.
        //
        // SAFETY: Since this API takes the user code as a closure, it can only be used in a manner
        // where the pages are unmapped in reverse order. This is as required by `kunmap_local`.
        //
        // In other words, if this call to `kunmap_local` happens when a different page should be
        // unmapped first, then there must necessarily be a call to `kmap_local_page` other than the
        // call just above in `with_page_mapped` that made that possible. In this case, it is the
        // unsafe block that wraps that other call that is incorrect.
        unsafe { bindings::kunmap_local(mapped_addr) };

        res
    }

    /// Runs a piece of code with a raw pointer to a slice of this page, with bounds checking.
    ///
    /// If `f` is called, then it will be called with a pointer that points at `off` bytes into the
    /// page, and the pointer will be valid for at least `len` bytes. The pointer is only valid on
    /// this task, as this method uses a local mapping.
    ///
    /// If `off` and `len` refers to a region outside of this page, then this method returns
    /// [`EINVAL`] and does not call `f`.
    ///
    /// # Using the raw pointer
    ///
    /// It is up to the caller to use the provided raw pointer correctly. The pointer is valid for
    /// `len` bytes and for the duration in which the closure is called. The pointer might only be
    /// mapped on the current thread, and when that is the case, dereferencing it on other threads
    /// is UB. Other than that, the usual rules for dereferencing a raw pointer apply: don't cause
    /// data races, the memory may be uninitialized, and so on.
    ///
    /// If multiple threads map the same page at the same time, then they may reference with
    /// different addresses. However, even if the addresses are different, the underlying memory is
    /// still the same for these purposes (e.g., it's still a data race if they both write to the
    /// same underlying byte at the same time).
    fn with_pointer_into_page<T>(
        &self,
        off: usize,
        len: usize,
        f: impl FnOnce(*mut u8) -> Result<T>,
    ) -> Result<T> {
        let bounds_ok = off <= PAGE_SIZE && len <= PAGE_SIZE && (off + len) <= PAGE_SIZE;

        if bounds_ok {
            self.with_page_mapped(move |page_addr| {
                // SAFETY: The `off` integer is at most `PAGE_SIZE`, so this pointer offset will
                // result in a pointer that is in bounds or one off the end of the page.
                f(unsafe { page_addr.add(off) })
            })
        } else {
            Err(EINVAL)
        }
    }

    /// Maps the page and reads from it into the given buffer.
    ///
    /// This method will perform bounds checks on the page offset. If `offset .. offset+len` goes
    /// outside of the page, then this call returns [`EINVAL`].
    ///
    /// # Safety
    ///
    /// * Callers must ensure that `dst` is valid for writing `len` bytes.
    /// * Callers must ensure that this call does not race with a write to the same page that
    ///   overlaps with this read.
    pub unsafe fn read_raw(&self, dst: *mut u8, offset: usize, len: usize) -> Result {
        self.with_pointer_into_page(offset, len, move |src| {
            // SAFETY: If `with_pointer_into_page` calls into this closure, then
            // it has performed a bounds check and guarantees that `src` is
            // valid for `len` bytes.
            //
            // There caller guarantees that there is no data race.
            unsafe { ptr::copy_nonoverlapping(src, dst, len) };
            Ok(())
        })
    }

    /// Maps the page and writes into it from the given buffer.
    ///
    /// This method will perform bounds checks on the page offset. If `offset .. offset+len` goes
    /// outside of the page, then this call returns [`EINVAL`].
    ///
    /// # Safety
    ///
    /// * Callers must ensure that `src` is valid for reading `len` bytes.
    /// * Callers must ensure that this call does not race with a read or write to the same page
    ///   that overlaps with this write.
    pub unsafe fn write_raw(&self, src: *const u8, offset: usize, len: usize) -> Result {
        self.with_pointer_into_page(offset, len, move |dst| {
            // SAFETY: If `with_pointer_into_page` calls into this closure, then it has performed a
            // bounds check and guarantees that `dst` is valid for `len` bytes.
            //
            // There caller guarantees that there is no data race.
            unsafe { ptr::copy_nonoverlapping(src, dst, len) };
            Ok(())
        })
    }

    /// Maps the page and zeroes the given slice.
    ///
    /// This method will perform bounds checks on the page offset. If `offset .. offset+len` goes
    /// outside of the page, then this call returns [`EINVAL`].
    ///
    /// # Safety
    ///
    /// Callers must ensure that this call does not race with a read or write to the same page that
    /// overlaps with this write.
    pub unsafe fn fill_zero_raw(&self, offset: usize, len: usize) -> Result {
        self.with_pointer_into_page(offset, len, move |dst| {
            // SAFETY: If `with_pointer_into_page` calls into this closure, then it has performed a
            // bounds check and guarantees that `dst` is valid for `len` bytes.
            //
            // There caller guarantees that there is no data race.
            unsafe { ptr::write_bytes(dst, 0u8, len) };
            Ok(())
        })
    }

    /// Copies data from userspace into this page.
    ///
    /// This method will perform bounds checks on the page offset. If `offset .. offset+len` goes
    /// outside of the page, then this call returns [`EINVAL`].
    ///
    /// Like the other `UserSliceReader` methods, data races are allowed on the userspace address.
    /// However, they are not allowed on the page you are copying into.
    ///
    /// # Safety
    ///
    /// Callers must ensure that this call does not race with a read or write to the same page that
    /// overlaps with this write.
    pub unsafe fn copy_from_user_slice_raw(
        &self,
        reader: &mut UserSliceReader,
        offset: usize,
        len: usize,
    ) -> Result {
        self.with_pointer_into_page(offset, len, move |dst| {
            // SAFETY: If `with_pointer_into_page` calls into this closure, then it has performed a
            // bounds check and guarantees that `dst` is valid for `len` bytes. Furthermore, we have
            // exclusive access to the slice since the caller guarantees that there are no races.
            reader.read_raw(unsafe { core::slice::from_raw_parts_mut(dst.cast(), len) })
        })
    }
}

// SAFETY: Instances of `Page` are always reference-counted.
unsafe impl crate::types::AlwaysRefCounted for Page {
    fn inc_ref(&self) {
        // SAFETY: The existence of a shared reference guarantees that the refcount is non-zero.
        unsafe { bindings::get_page(self.as_ptr()) };
    }

    unsafe fn dec_ref(obj: ptr::NonNull<Self>) {
        // SAFETY: The safety requirements guarantee that the refcount is non-zero.
        unsafe { bindings::put_page(obj.cast().as_ptr()) }
    }
}

/// A page-aligned, page-sized object.
///
/// This is used for convenience to convert a large buffer into an array of page-sized chunks
/// allocated with the kernel's allocators which can then be used in the
/// `Page::page_slice_to_page` wrapper.
///
// FIXME: This should be `PAGE_SIZE`, but the compiler rejects everything except a literal
// integer argument for the `repr(align)` attribute.
#[repr(align(4096))]
pub struct PageSlice([u8; PAGE_SIZE]);

fn to_vec_with_allocator<A: Allocator>(val: &[u8]) -> Result<Vec<PageSlice, A>, AllocError> {
    let mut k = Vec::<PageSlice, A>::new();
    let pages = page_align(val.len()) >> PAGE_SHIFT;
    match k.reserve(pages, GFP_KERNEL) {
        Ok(()) => {
            // SAFETY: from above, the length should be equal to the vector's capacity
            unsafe { k.set_len(pages); }
            // SAFETY: src buffer sized val.len() does not overlap with dst buffer since
            // the dst buffer's size is val.len() padded up to a multiple of PAGE_SIZE.
            unsafe { ptr::copy_nonoverlapping(val.as_ptr(), k.as_mut_ptr() as *mut u8,
                                              val.len()) };
            Ok(k)
        },
        Err(_) => Err(AllocError),
    }
}

impl TryFrom<&[u8]> for VVec<PageSlice> {
    type Error = AllocError;

    fn try_from(val: &[u8]) -> Result<Self, AllocError> {
        to_vec_with_allocator(val)
    }
}

impl TryFrom<&[u8]> for KVec<PageSlice> {
    type Error = AllocError;

    fn try_from(val: &[u8]) -> Result<Self, AllocError> {
        to_vec_with_allocator(val)
    }
}

impl TryFrom<&[u8]> for KVVec<PageSlice> {
    type Error = AllocError;

    fn try_from(val: &[u8]) -> Result<Self, AllocError> {
        to_vec_with_allocator(val)
    }
}
