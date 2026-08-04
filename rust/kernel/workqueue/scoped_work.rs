// SPDX-License-Identifier: GPL-2.0

//! Scoped work items.
//!
//! Provides [`ScopedWork`] for work items whose inner data may carry non-`'static` lifetimes.
//!
//! Unlike [`Work`]-based work items, [`ScopedWork`] cancels work synchronously on drop via
//! `cancel_work_sync()`, so ownership of the data is not transferred to the workqueue.
//!
//! # Examples
//!
//! Enqueue on the system workqueue (unsafe, caller must not forget the work):
//!
//! ```
//! # use kernel::time::{Delta, delay::fsleep};
//! use kernel::workqueue::{
//!     self,
//!     new_scoped_work,
//!     ScopedWork,
//!     ScopedWorkItem,
//! };
//!
//! struct MyWork {
//!     value: u32,
//! }
//!
//! impl ScopedWorkItem for MyWork {
//!     fn run(self: Pin<&Self>) {
//!         pr_info!("value = {}\n", self.value);
//!     }
//! }
//!
//! let work = KBox::pin_init(
//!     new_scoped_work!("MyWork", MyWork { value: 42 }),
//!     GFP_KERNEL,
//! )?;
//!
//! // SAFETY: `work` is not forgotten.
//! unsafe { workqueue::system_dfl().enqueue_scoped(&*work) };
//! # // Allow the worker thread to pick up and execute the work.
//! # fsleep(Delta::from_millis(100));
//! # Ok::<(), Error>(())
//! ```
//!
//! Enqueue on a [`ScopedQueue`] using the safe path (work outlives the queue):
//!
//! ```
//! use kernel::workqueue::{
//!     new_scoped_work,
//!     ScopedQueue,
//!     ScopedWork,
//!     ScopedWorkItem,
//! };
//!
//! struct MyWork {
//!     value: u32,
//! }
//!
//! impl ScopedWorkItem for MyWork {
//!     fn run(self: Pin<&Self>) {
//!         pr_info!("value = {}\n", self.value);
//!     }
//! }
//!
//! let work = KBox::pin_init(
//!     new_scoped_work!("MyWork", MyWork { value: 42 }),
//!     GFP_KERNEL,
//! )?;
//!
//! // SAFETY: The queue is not forgotten.
//! let queue = unsafe { ScopedQueue::new(c"example_wq")? };
//!
//! // Safe since `work` outlives `queue`.
//! queue.enqueue(&*work);
//! # Ok::<(), Error>(())
//! ```

use super::{
    impl_has_work,
    HasWork,
    RawWorkItem,
    Work,
    WorkItem,
    WorkItemPointer, //
};

use crate::{
    bindings,
    prelude::*,
    sync::LockClassKey, //
};

use core::{
    ops::Deref,
    ptr::NonNull, //
};

/// Trait for types that can be used as scoped work items.
///
/// Implementers define the work function that executes when the item is dequeued by a workqueue
/// thread. The callback receives a shared pinned reference; mutation should use interior mutability
/// (e.g., [`Mutex`](crate::sync::Mutex)).
pub trait ScopedWorkItem {
    /// Called when the work item is executed.
    fn run(self: Pin<&Self>);
}

// SAFETY: The `run` callback uses the `work_struct` pointer to recover a pointer to `ScopedWork<T>`
// via `HasWork`, wraps it in `NonNull`, and calls `WorkItem::run`.
unsafe impl<T: ScopedWorkItem, const ID: u64> WorkItemPointer<ID> for NonNull<ScopedWork<T>>
where
    ScopedWork<T>: WorkItem<ID, Pointer = Self>,
    ScopedWork<T>: HasWork<ScopedWork<T>, ID>,
{
    unsafe extern "C" fn run(ptr: *mut bindings::work_struct) {
        let ptr = ptr.cast::<Work<ScopedWork<T>, ID>>();

        // SAFETY: The `work_struct` is embedded in `ScopedWork<T>` via `HasWork`.
        let ptr = unsafe { <ScopedWork<T> as HasWork<ScopedWork<T>, ID>>::work_container_of(ptr) };

        // SAFETY: `work_container_of` returns a valid, non-null pointer.
        let nn = unsafe { NonNull::new_unchecked(ptr) };

        <ScopedWork<T> as WorkItem<ID>>::run(nn);
    }
}

// Required because `WorkItemPointer<ID>: RawWorkItem<ID>` is a supertrait bound. This `__enqueue`
// is never called; the enqueue path goes through the `RawWorkItem` impl for `&ScopedWork<T>`
// instead.
//
// SAFETY: `__enqueue` is unreachable.
unsafe impl<T: ScopedWorkItem, const ID: u64> RawWorkItem<ID> for NonNull<ScopedWork<T>>
where
    ScopedWork<T>: HasWork<ScopedWork<T>, ID>,
{
    type EnqueueOutput = bool;

    unsafe fn __enqueue<F>(self, _queue_work_on: F) -> Self::EnqueueOutput
    where
        F: FnOnce(*mut bindings::work_struct) -> bool,
    {
        unreachable!()
    }
}

// SAFETY: `&ScopedWork<T>` points to a valid, pinned `ScopedWork` with a valid `work_struct`.
// The pointer remains valid until `cancel_work_sync()` completes.
unsafe impl<'a, T: ScopedWorkItem + Sync, const ID: u64> RawWorkItem<ID> for &'a ScopedWork<T>
where
    ScopedWork<T>: HasWork<ScopedWork<T>, ID>,
{
    type EnqueueOutput = bool;

    unsafe fn __enqueue<F>(self, queue_work_on: F) -> Self::EnqueueOutput
    where
        F: FnOnce(*mut bindings::work_struct) -> bool,
    {
        let self_ptr = core::ptr::from_ref(self);

        // SAFETY: `self_ptr` points to a valid `ScopedWork` with a `Work` field.
        let work_ptr = unsafe {
            <ScopedWork<T> as HasWork<ScopedWork<T>, ID>>::raw_get_work(self_ptr.cast_mut())
        };

        // SAFETY: `work_ptr` points to a valid `Work`.
        let work_ptr = unsafe { Work::raw_get(work_ptr) };

        queue_work_on(work_ptr)
    }
}

/// A scoped work item that cancels synchronously on drop.
///
/// `ScopedWork<T>` contains both a `work_struct` and the user data `T`. Its destructor calls
/// `cancel_work_sync()`, guaranteeing the work function is not running when the data is dropped.
///
/// This allows `T` to carry non-`'static` lifetimes.
///
/// Construct via [`new_scoped_work!`] which returns an `impl PinInit` suitable for embedding
/// in-place inside other pinned structs.
#[pin_data(PinnedDrop)]
pub struct ScopedWork<T: ScopedWorkItem> {
    #[pin]
    work: Work<Self>,
    #[pin]
    data: T,
}

impl_has_work! {
    impl{T: ScopedWorkItem} HasWork<ScopedWork<T>> for ScopedWork<T> { self.work }
}

impl<T: ScopedWorkItem> WorkItem for ScopedWork<T> {
    type Pointer = NonNull<Self>;

    fn run(this: NonNull<Self>) {
        // SAFETY: `this` points to a valid, pinned `ScopedWork`. `cancel_work_sync()` in
        // `PinnedDrop` prevents use-after-drop.
        let data = unsafe { Pin::new_unchecked(&(*this.as_ptr()).data) };

        T::run(data);
    }
}

impl<T: ScopedWorkItem> Deref for ScopedWork<T> {
    type Target = T;

    fn deref(&self) -> &T {
        &self.data
    }
}

impl<T: ScopedWorkItem> ScopedWork<T> {
    /// Creates a pin-initializer for a new scoped work item.
    ///
    /// Use [`new_scoped_work!`] to automatically provide the lock class key.
    pub fn new<E>(
        name: &'static CStr,
        key: Pin<&'static LockClassKey>,
        init: impl PinInit<T, E>,
    ) -> impl PinInit<Self, Error>
    where
        Error: From<E>,
    {
        try_pin_init!(Self {
            work <- Work::new(name, key),
            data <- init,
        })
    }

    /// Synchronously cancels pending work and waits for any running work function to complete.
    ///
    /// Returns `true` if the work was pending, `false` otherwise.
    pub fn cancel_work_sync(&self) -> bool {
        self.work.cancel_work_sync()
    }
}

#[pinned_drop]
impl<T: ScopedWorkItem> PinnedDrop for ScopedWork<T> {
    fn drop(self: Pin<&mut Self>) {
        self.cancel_work_sync();
    }
}

/// Creates a [`ScopedWork`] pin-initializer with a new lock class.
///
/// # Examples
///
/// ```
/// use kernel::workqueue::{
///     new_scoped_work,
///     ScopedWork,
///     ScopedWorkItem,
/// };
///
/// struct MyWork {
///     value: u32,
/// }
///
/// impl ScopedWorkItem for MyWork {
///     fn run(self: Pin<&Self>) {
///         pr_info!("value = {}\n", self.value);
///     }
/// }
///
/// #[pin_data]
/// struct MyData {
///     #[pin]
///     work: ScopedWork<MyWork>,
/// }
///
/// fn init_data() -> impl PinInit<MyData, Error> {
///     try_pin_init!(MyData {
///         work <- new_scoped_work!("MyWork", MyWork { value: 7 }),
///     })
/// }
/// ```
#[macro_export]
macro_rules! new_scoped_work {
    ($name:literal, $init:expr) => {
        $crate::workqueue::ScopedWork::new(
            $crate::c_str!($name),
            $crate::static_lock_class!(),
            $init,
        )
    };
}
pub use new_scoped_work;
