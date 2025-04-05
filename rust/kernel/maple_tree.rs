// SPDX-License-Identifier: GPL-2.0

//! Maple trees.
//!
//! C header: [`include/linux/maple_tree.h`](srctree/include/linux/maple_tree.h)
//!
//! Reference: <https://docs.kernel.org/core-api/maple_tree.html>
//!
//! # Examples
//! ```
//! # use kernel::maple_tree::*;
//! # use kernel::alloc::{KBox, flags::GFP_KERNEL};
//! let mtree = KBox::pin_init(MapleTree::new(flags::DEFAULT_TREE), GFP_KERNEL)?;
//! let mut guard = mtree.lock();
//! let entry = KBox::new(5, GFP_KERNEL)?;
//! guard.insert_range(0, 10, entry, GFP_KERNEL);
//!
//! for i in 0..=10 {
//!    assert_eq!(guard.get(i), Some(&5));
//! }
//!
//! guard.remove(2);
//!
//! for i in 0..=10 {
//!    assert_eq!(guard.get(i), None);
//! }
//!
//! # Ok::<(), Error>(())
//! ```

// TODO and missing features
// - the C version suports external locking this only supports the internal spinlock
// - this version can only have one reader because the safety requirements
//   means that we need to hold the lock
// - there is currently no api for the functionality enabled by alloc_trees
// - future work may use rcu to enable lockless operation.
// - add a way to iter through ranges
//
// SAFETY:
// we cannot derefrence any pointers without holding the spinlock because
// all returned pointers are guaranteed to have been inserted by the user
// but the pointers are not guaranteed to be still be valid
// another thread may have already removed and dropped the pointers
// so to safely deref the returned pointers the user must
// have exclusive write access to the `MapleTree`
// or rcu_read_lock() is held and updater side uses a synchronize_rcu()

use core::{ffi::c_void, marker::PhantomData, pin::Pin, ptr::NonNull};

use macros::pin_data;
use macros::pinned_drop;

use crate::error::code;
use crate::prelude::PinInit;

use crate::{
    alloc, bindings,
    error::{self, Error},
    pin_init,
    types::{ForeignOwnable, NotThreadSafe, Opaque},
};

/// A `MapleTree` is a tree like data structure that is optimized for storing
/// non-overlaping ranges and mapping them to pointers.
/// They use rcu locks and an internal spinlock to syncronise access.
///
/// Note that maple tree ranges are range inclusive.
// # Invariants
// self.root is always a valid and initialized `bindings::maple_tree`
//
// all values inserted into the tree come from `T::into_foreign`
#[pin_data(PinnedDrop)]
pub struct MapleTree<T: ForeignOwnable> {
    #[pin]
    root: Opaque<bindings::maple_tree>,
    _p: PhantomData<T>,
}

impl<T: ForeignOwnable> MapleTree<T> {
    /// creates a new `MapleTree` with the specified `flags`
    ///
    /// see [`flags`] for the list of flags and their usage
    pub fn new(flags: Flags) -> impl PinInit<Self> {
        pin_init!(
            Self{
                // SAFETY:
                // - mt is valid because of ffi_init
                // - maple_tree contains a lock which should be pinned
                root <- Opaque::ffi_init(|mt| unsafe {
                    bindings::mt_init_flags(mt, flags.as_raw())
                }),
                _p: PhantomData
            }

        )
    }

    /// helper for internal use.
    /// returns an iterator through the maple tree
    /// # Safety
    /// - the user must ensure that it has exclusive access to the tree if no locks are held
    /// - if the internal lock is held it is safe to deref the returned pointers
    /// - if the rcu lock is held the pointers will be a value that was inserted
    ///   by the user but possibly may be invalid
    unsafe fn iter_no_lock(&self) -> IterNoLock<'_, T> {
        // SAFETY:
        // self.root.get() will allways point to a valid maple_tree
        // by the invariants of MapleTree
        let ma_state = unsafe { Opaque::new(bindings::MA_STATE(self.root.get(), 0, usize::MAX)) };
        IterNoLock {
            ma_state,
            _p: PhantomData,
        }
    }

    /// locks the `Mapletree`'s internal spinlock and returns a [`Guard`].
    /// When the `Guard` is dropped, the internal spinlock is unlocked
    /// if the lock is already held by the current thread this will deadlock
    pub fn lock(&self) -> Guard<'_, T> {
        // SAFETY:
        // self.root.get() will allways point to a valid maple_tree
        // by the invariants of MapleTree
        unsafe { bindings::mtree_lock(self.root.get()) };
        Guard {
            tree: self,
            _not_send: NotThreadSafe,
        }
    }
}

#[pinned_drop]
impl<T: ForeignOwnable> PinnedDrop for MapleTree<T> {
    fn drop(self: Pin<&mut Self>) {
        // SAFETY:
        // - we have exclusive access to self because we should have
        //   exclussive access whenever drop is called
        //   if drop is called multiple times an invariant is already violated
        for i in unsafe { self.iter_no_lock() } {
            //SAFETY:
            // - we can call from_foreign because all values inserted into a MapleTree
            //   come from T::into_foreign
            // - i.as_ptr is guaranteed to not be null because of the invariant of NonNull
            // - we have exclusive access to self because we should have
            //   exclussive access whenever drop is called
            let original = unsafe { T::from_foreign(i.as_ptr()) };
            drop(original);
        }
        // SAFETY:
        // - self.root.get() will allways point to a valid maple_tree
        //   by the invariants of MapleTree
        // - we can call this without taking the spinlock because we should have
        //   exclusive access whenever drop is called
        unsafe {
            bindings::__mt_destroy(self.root.get());
        }
    }
}

// SAFETY: `MapleTree<T>` has no shared mutable state so it is `Send` iff `T` is `Send`.
// MapleTree is still pin_init so it still cannot be moved but this means that types like
// Box<MapleTree<T>> can be sent between threads
unsafe impl<T: ForeignOwnable + Send> Send for MapleTree<T> {}

// SAFETY: `MapleTree<T>` has interior mutability so it is `Sync` iff `T` is `Send`.
unsafe impl<T: ForeignOwnable + Send> Sync for MapleTree<T> {}

/// an iterator over all of the values in a [`MapleTree`].
/// this intentionally does not hold the rcu lock or the internal lock
/// the user must ensure that it exclusive access to the tree if no locks are held
/// if the internal lock is held it is safe to deref the returned pointers
/// if the rcu lock is held the pointers will be a value that was inserted
/// by the user but possibly may be invalid
struct IterNoLock<'a, T: ForeignOwnable> {
    ma_state: Opaque<bindings::ma_state>,
    _p: PhantomData<&'a MapleTree<T>>,
}

impl<'a, T: ForeignOwnable> Iterator for IterNoLock<'a, T> {
    type Item = NonNull<c_void>;
    fn next(&mut self) -> Option<Self::Item> {
        // SAFETY:
        // self.ma_state.get() will allways point to a valid ma_state by the invariants of Iter
        let ptr: *mut c_void = unsafe { bindings::mas_find(self.ma_state.get(), usize::MAX) };
        NonNull::new(ptr)
    }
}

/// A lock guard for a [`MapleTree`]'s internal spinlock
///
/// The lock is unlocked when the guard goes out of scope
// # Invariants
//
// `tree` is always a valid reference to a locked `MapleTree`
// `tree` is unlocked when the guard is dropped
pub struct Guard<'a, T: ForeignOwnable> {
    tree: &'a MapleTree<T>,
    _not_send: NotThreadSafe,
}

impl<'a, T: ForeignOwnable> Guard<'a, T> {
    /// Removes a value at the specified index.
    /// if there is no value at the index returns `None`.
    /// if there is a value at the index returns it and unmaps the entire range
    pub fn remove(&mut self, index: usize) -> Option<T> {
        // SAFETY:
        // - we can safely call MA_STATE because self.tree.root.get() will be valid
        //   by the invariants of guard
        // - we can safely call mas_erase because the lock is held and mas is valid
        // - we can call try_from_foreign because all values inserted into a MapleTree
        //   come from T::into_foreign
        unsafe {
            let mas = Opaque::new(bindings::MA_STATE(self.tree.root.get(), index, index));
            let removed = bindings::mas_erase(mas.get());
            T::try_from_foreign(removed)
        }
    }

    /// Returns a reference to the `T` at `index` if it exists,
    /// otherwise returns `None`
    pub fn get(&self, index: usize) -> Option<T::Borrowed<'_>> {
        // SAFETY:
        // self.tree.root.get() will always be valid because of the invariants of MapleTree
        let ptr = unsafe { bindings::mtree_load(self.tree.root.get(), index) };
        if ptr.is_null() {
            return None;
        }
        // SAFETY:
        // - we can safely call borrow because all values inserted into a MapleTree
        //   come from T::into_foreign
        // - ptr is not null by the check above
        Some(unsafe { T::borrow(ptr) })
    }

    /// Returns a mut reference to the `T` at `index` if it exists,
    /// otherwise returns `None`
    pub fn get_mut(&mut self, index: usize) -> Option<T::BorrowedMut<'_>> {
        // SAFETY:
        // self.tree.root.get() will always be valid because of the invariants of MapleTree
        let ptr = unsafe { bindings::mtree_load(self.tree.root.get(), index) };
        if ptr.is_null() {
            return None;
        }
        // SAFETY:
        // - we can safely call borrowmut because all values inserted into a MapleTree
        //   come from T::into_foreign
        // - ptr is not null by the check above
        // - we have exclusive ownership becauce this function takes `&mut self`
        Some(unsafe { T::borrow_mut(ptr) })
    }

    /// a convenience alias for [`Self::insert_range`] where `start == end`
    pub fn insert(&mut self, index: usize, value: T, gfp: alloc::Flags) -> Result<(), (T, Error)> {
        self.insert_range(index, index, value, gfp)
    }

    /// Maps the range `[start, end]` to `value` in the MapleTree.
    /// if `[start, end]` overlaps with any range already inserted, then `value` will
    /// not be inserted.
    ///
    /// * Returns `Ok(())` if insertion is successful
    ///
    /// * Returns `Err((value, EINVAL))` if `T::into_foreign` is `NULL`
    ///   or is in [0, 4096] with the bottom two bits set to `10` (ie 2, 6, 10 .. 4094)
    ///   or `start` > `end`.
    ///
    /// * Returns `Err((value, EEXISTS))` if there is any entry already within the range.
    ///
    /// * Returns `Err((value, ENOMEM))` if allocation failed.
    pub fn insert_range(
        &mut self,
        start: usize,
        end: usize,
        value: T,
        gfp: alloc::Flags,
    ) -> Result<(), (T, Error)> {
        let ptr = value.into_foreign();
        // a insertion of NULL will succeed even if there are values stored there (i tested this)
        // this may remove values that we do not want to remove
        // and any stored T that gets overwritten will not be dropped
        // which we do not want to happen
        // so return early if ptr is NULL
        if ptr.is_null() {
            // SAFETY:
            // ptr is from T::into_foreign so we can safely convert it back
            let original = unsafe { T::from_foreign(ptr) };
            return Err((original, code::EINVAL));
        }

        // SAFETY:
        // - we can call __mtree_insert_range because we hold the lock because of the
        //   invariants of self
        // - self.tree.root.get() will always be valid because of the invariants of MapleTree
        let errno = unsafe {
            bindings::__mtree_insert_range(self.tree.root.get(), start, end, ptr, gfp.as_raw())
        };

        let err = error::to_result(errno);
        // SAFETY:
        // - we can call from_foreign because all values inserted into a MapleTree
        //   come from T::into_foreign
        // - we have exclusive ownership of ptr because if err is an error then, ptr was
        //   not inserted into the MapleTree
        //
        err.map_err(|e| unsafe { (T::from_foreign(ptr), e) })
    }
}

impl<T: ForeignOwnable> Drop for Guard<'_, T> {
    fn drop(&mut self) {
        // SAFETY:
        // - unlock the MapleTree because the guard is being dropped
        // - self.tree.root.get() will always be valid because of the invariants of MapleTree
        unsafe { bindings::mtree_unlock(self.tree.root.get()) };
    }
}

#[derive(Clone, Copy, PartialEq)]
/// flags to be used with [`MapleTree::new`].
///
/// values can used from the [`flags`] module.
pub struct Flags(u32);

impl Flags {
    pub(crate) fn as_raw(self) -> u32 {
        self.0
    }
}

/// flags to be used with [`MapleTree::new`]
pub mod flags {
    use super::Flags;

    /// Creates a default MapleTree
    pub const DEFAULT_TREE: Flags = Flags(0);

    /// if a `MapleTree` is created with ALLOC_TREE the `MapleTree` will be a alloc tree.
    /// alloc trees have a lower branching factor but allows the user to search
    /// for a gap of a given size.
    pub const ALLOC_TREE: Flags = Flags(bindings::MT_FLAGS_ALLOC_RANGE);
}
