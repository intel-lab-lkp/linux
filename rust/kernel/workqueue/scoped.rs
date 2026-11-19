// SPDX-License-Identifier: GPL-2.0

//! Scoped work items.
//!
//! Provides [`ScopedWork`] for work items that may borrow data with some non-`'static` lifetime.
//!
//! Unlike [`Work`]-based items, ownership of a [`ScopedWork`] is not transferred to
//! the workqueue. Instead, its destructor calls `cancel_work_sync()`, which guarantees that the
//! work function is not running (and will not run) once the destructor returns. This is what
//! allows the inner data to carry non-`'static` lifetimes.
//!
//! Creating a [`ScopedWork`] is `unsafe`, because the above guarantee only holds if the destructor
//! actually runs: the value must not be forgotten, and must be dropped before the bound [`Queue`]
//! or any lifetime in the inner data expires. Once created, a [`ScopedWork`] can be enqueued onto
//! its bound workqueue using the safe [`ScopedWork::enqueue`].
//!
//! # Examples
//!
//! ```
//! use kernel::workqueue::{
//!     new_scoped_work,
//!     Queue,
//!     ScopedWork,
//!     ScopedWorkItem,
//! };
//!
//! struct MyWork<'a> {
//!     value: &'a u32,
//! }
//!
//! impl ScopedWorkItem for MyWork<'_> {
//!     fn run(work: &ScopedWork<'_, Self>) {
//!         pr_info!("value = {}\n", work.value);
//!     }
//! }
//!
//! let queue = Queue::new_unbound().build(c"my-wq")?;
//! let value = 42;
//!
//! // Define `my_work` outside the `unsafe` block.
//! let my_work = MyWork { value: &value };
//! let work = KBox::pin_init(
//!     // SAFETY: `work` is not forgotten and is dropped before `queue` and `value`.
//!     unsafe { new_scoped_work!(&queue, "MyWork", my_work) },
//!     GFP_KERNEL,
//! )?;
//!
//! work.enqueue();
//!
//! // Dropping `work` calls `cancel_work_sync()`, ensuring the work item has finished or been
//! // cancelled before `value` and `queue` are dropped.
//! drop(work);
//! # Ok::<(), Error>(())
//! ```

use crate::{
    bindings,
    container_of,
    prelude::*,
    sync::LockClassKey,
    types::Opaque, //
};

use super::Queue;
#[cfg(doc)]
use super::Work;

use pin_init::Wrapper;

use core::{
    marker::PhantomData,
    ops::Deref,
    ptr::NonNull, //
};

/// Trait for types that can be used as scoped work items.
///
/// Implementers define the work function that executes when the item is dequeued by a workqueue
/// thread. Because `&Self` is shared with the workqueue worker thread while the work item is
/// executing, implementers must be [`Sync`].
///
/// The callback receives a reference to the containing [`ScopedWork`], which provides access to
/// the inner data via [`Deref`] and can be re-enqueued from within the work function using
/// [`ScopedWork::enqueue`].
pub trait ScopedWorkItem: Sized + Sync {
    /// Called when the work item is executed.
    fn run(work: &ScopedWork<'_, Self>);
}

/// The payload of a [`ScopedWork`].
///
/// # Invariants
///
/// - `queue` points to the valid [`Queue`] bound to the enclosing [`ScopedWork`].
/// - `work` is an initialized `work_struct` whose work function is [`ScopedWork::run`] and is only
///   ever enqueued onto `queue`.
/// - An `Inner<T>` only ever exists inside a [`ScopedWork<'_, T>`].
#[pin_data]
struct Inner<T: ScopedWorkItem> {
    queue: NonNull<Queue>,
    #[pin]
    work: Opaque<bindings::work_struct>,
    #[pin]
    data: T,
}

/// A scoped work item that cancels synchronously on drop.
///
/// `ScopedWork<'wq, T>` is bound to a single [`Queue`] with lifetime `'wq` and contains a
/// `work_struct` and the user data `T`. Its destructor calls `cancel_work_sync()`, guaranteeing
/// the work function is not running when the data is dropped.
///
/// This allows `T` to carry non-`'static` lifetimes.
///
/// Construct via [`new_scoped_work!`], which returns an `impl PinInit` suitable for embedding
/// in-place inside other pinned structs. Because the soundness of [`ScopedWork::enqueue`] relies
/// on the destructor running before the memory is reused or `'wq` or any lifetime in `T` expires,
/// [`ScopedWork::new`] is `unsafe`.
///
/// # Examples
///
/// Self-re-enqueueing from within the work function:
///
/// ```
/// use kernel::sync::atomic::{Atomic, Relaxed};
/// use kernel::sync::Completion;
/// use kernel::workqueue::{new_scoped_work, Queue, ScopedWork, ScopedWorkItem};
///
/// #[pin_data]
/// struct RequeueWork<'a> {
///     counter: &'a Atomic<u32>,
///     #[pin]
///     done: Completion,
/// }
///
/// impl ScopedWorkItem for RequeueWork<'_> {
///     fn run(work: &ScopedWork<'_, Self>) {
///         if work.counter.fetch_add(1, Relaxed) < 2 {
///             work.enqueue();
///         } else {
///             work.done.complete_all();
///         }
///     }
/// }
///
/// let queue = Queue::new_ordered().build(c"requeue_wq")?;
/// let counter = Atomic::new(0u32);
/// let counter_ref = &counter;
///
/// // Define `requeue` outside the `unsafe` block.
/// let requeue = pin_init!(RequeueWork {
///     counter: counter_ref,
///     done <- Completion::new(),
/// });
/// let work = KBox::pin_init(
///     // SAFETY: `work` is not forgotten and is dropped before `queue` and `counter`.
///     unsafe { new_scoped_work!(&queue, "RequeueWork", requeue) },
///     GFP_KERNEL,
/// )?;
///
/// work.enqueue();
/// work.done.wait_for_completion();
/// drop(work);
///
/// assert_eq!(counter.load(Relaxed), 3);
/// # Ok::<(), Error>(())
/// ```
#[pin_data(PinnedDrop)]
#[repr(transparent)]
pub struct ScopedWork<'wq, T: ScopedWorkItem> {
    /// The whole payload is wrapped in [`Opaque`] on purpose: while the destructor waits in
    /// `cancel_work_sync()`, a worker thread may still be executing [`ScopedWorkItem::run`] with a
    /// shared reference to this value (and e.g. calling [`ScopedWork::enqueue`]), concurrently with
    /// the `&mut Self` held by `drop`. [`Opaque`] opts out of the uniqueness guarantee of that
    /// `&mut`. Wrapping individual fields would not be enough, since the `&mut Self` would still
    /// claim exclusive access to `queue` and the padding bytes.
    ///
    /// # Invariants
    ///
    /// - `inner` is always initialized.
    /// - The `queue` field inside `inner` points to the [`Queue`] bound to this `ScopedWork`,
    ///   which remains valid for `'wq`.
    #[pin]
    inner: Opaque<Inner<T>>,
    _wq: PhantomData<&'wq Queue>,
}

// SAFETY: A `&ScopedWork<'_, T>` only hands out `&T` (via `Deref`, where `T: Sync` via
// `ScopedWorkItem`), `&Queue` (via `queue()`, where `Queue: Sync`), and accesses the
// `work_struct` only through the thread-safe C workqueue API.
unsafe impl<T: ScopedWorkItem> Sync for ScopedWork<'_, T> {}

// SAFETY: `Queue: Sync`, and the `work_struct` is only ever accessed through the
// thread-safe C workqueue API. Moving `ScopedWork` transfers ownership of `T` (so `T` may
// be dropped on the destination thread), which requires `T: Send`, while a worker thread
// may concurrently access `&T` in `ScopedWork::run`, which is sound because
// `T: ScopedWorkItem` implies `T: Sync`.
unsafe impl<T: ScopedWorkItem + Send> Send for ScopedWork<'_, T> {}

impl<T: ScopedWorkItem> Deref for ScopedWork<'_, T> {
    type Target = T;

    #[inline]
    fn deref(&self) -> &T {
        // SAFETY: `inner` is always initialized (type invariant), and `data` is only ever accessed
        // through shared references.
        unsafe { &(*self.inner.get()).data }
    }
}

impl<'wq, T: ScopedWorkItem> ScopedWork<'wq, T> {
    /// Creates a pin-initializer for a new scoped work item bound to `queue`.
    ///
    /// Use [`new_scoped_work!`] to automatically provide the lock class key.
    ///
    /// # Safety
    ///
    /// The caller must ensure that the destructor of the initialized `ScopedWork<'wq, T>` runs
    /// before the memory it lives in is reused and before `'wq` or any lifetime in `T` expires.
    /// In particular, the value must not be leaked, e.g. via [`core::mem::forget`] or by leaking
    /// the allocation that contains it.
    #[inline]
    pub unsafe fn new<E>(
        queue: &'wq Queue,
        name: &'static CStr,
        key: Pin<&'static LockClassKey>,
        init: impl PinInit<T, E>,
    ) -> impl PinInit<Self, E> {
        // INVARIANT: `inner` is initialized with a legal value of type `Inner<T>`, whose `queue`
        // points to `queue: &'wq Queue` (valid for `'wq`).
        try_pin_init!(Self {
            // INVARIANT:
            // - `queue` points to the valid `Queue` bound to this `ScopedWork`.
            // - `work` is initialized via `init_work_with_key` with `Self::run` as the work
            //   function and is only ever enqueued onto `queue`.
            // - This `Inner<T>` is constructed directly inside `ScopedWork<'wq, T>`.
            inner <- Opaque::pin_init(try_pin_init!(Inner::<T> {
                queue: NonNull::from(queue),
                // Initialize `data` first so that `init_work_with_key` is not called if `init`
                // fails.
                data <- init,
                work <- Opaque::try_ffi_init(|slot| {
                    // SAFETY: `slot` is a valid pointer to uninitialized memory for a
                    // `work_struct`, and `Self::run` is a valid work function for a `work_struct`
                    // embedded in an `Inner<T>` that is embedded in a `ScopedWork<'wq, T>`.
                    unsafe {
                        bindings::init_work_with_key(
                            slot,
                            Some(Self::run),
                            name.as_char_ptr(),
                            key.as_ptr(),
                        )
                    };
                    Ok::<(), E>(())
                }),
            }? E)),
            _wq: PhantomData,
        }? E)
    }

    /// Returns the workqueue this work item is bound to.
    #[inline]
    pub fn queue(&self) -> &'wq Queue {
        // SAFETY: `inner` is always initialized (type invariant), `queue` is never mutated
        // after initialization, and the safety contract of `ScopedWork::new` ensures `'wq` has
        // not expired while `self` is alive.
        unsafe { (*self.inner.get()).queue.as_ref() }
    }

    /// Enqueues this work item on its bound workqueue.
    ///
    /// Returns `false` if the work item is already pending on the workqueue.
    ///
    /// The work item will be submitted using `WORK_CPU_UNBOUND`.
    #[inline]
    pub fn enqueue(&self) -> bool {
        // SAFETY:
        // - `self.queue().as_raw()` is a valid `workqueue_struct` pointer because `'wq` has
        //   not expired (by the safety contract of `ScopedWork::new`).
        // - `self.raw_work()` is a valid `work_struct` initialized with `Self::run` (type
        //   invariant).
        // - Because `self` is only ever enqueued onto `self.queue()` and never migrated across
        //   workqueues, `cancel_work_sync()` in `PinnedDrop::drop` is guaranteed to wait for
        //   any running execution of `Self::run` to complete before `drop` returns; and
        //   `ScopedWork::new` guarantees `drop` runs before the backing memory is reused or
        //   `'wq` or any lifetime in `T` expires.
        // - `T: ScopedWorkItem` guarantees `T: Sync`, so sharing `&T` with the worker thread is
        //   sound.
        unsafe {
            bindings::queue_work_on(
                bindings::wq_misc_consts_WORK_CPU_UNBOUND as ffi::c_int,
                self.queue().as_raw(),
                self.raw_work(),
            )
        }
    }

    /// Returns a raw pointer to the `work_struct`.
    #[inline]
    fn raw_work(&self) -> *mut bindings::work_struct {
        // SAFETY: `inner` is always initialized (type invariant).
        unsafe { Opaque::cast_into(&raw const (*self.inner.get()).work) }
    }

    /// The work function registered with the C workqueue.
    ///
    /// # Safety
    ///
    /// `ptr` must point to the `work` field of the `Inner<T>` of a live `ScopedWork<'wq, T>`.
    unsafe extern "C" fn run(ptr: *mut bindings::work_struct) {
        // SAFETY: `Opaque<T>` is `#[repr(transparent)]`, and by the safety requirements `ptr`
        // points to the `work` field of an `Inner<T>`.
        let inner =
            unsafe { container_of!(ptr.cast::<Opaque<bindings::work_struct>>(), Inner<T>, work) };

        // SAFETY: `Inner<T>` only ever exists inside a `ScopedWork<'wq, T>` (type invariant), and
        // both `Opaque` and `ScopedWork` are `#[repr(transparent)]`, so `inner` is also a valid
        // pointer to the containing `ScopedWork<'wq, T>`. By the safety requirements it is live;
        // its destructor calls `cancel_work_sync()` before the memory is released, so it stays
        // live for the duration of this call. Moreover, `Self: Sync` (from `T: ScopedWorkItem` and
        // `Queue: Sync`), so passing `&Self` on the worker thread is sound.
        T::run(unsafe { &*inner.cast::<Self>() });
    }
}

#[pinned_drop]
impl<T: ScopedWorkItem> PinnedDrop for ScopedWork<'_, T> {
    #[inline]
    fn drop(self: Pin<&mut Self>) {
        // SAFETY: `raw_work()` returns a valid `work_struct`. Because `work` is only ever
        // enqueued onto its single bound `queue` (type invariant), `cancel_work_sync()` is
        // guaranteed to wait for any running execution of `Self::run` to complete before returning.
        unsafe { bindings::cancel_work_sync(self.raw_work()) };

        // SAFETY: `raw_work()` returns a valid `work_struct` that has been cancelled and will not
        // be used again.
        unsafe { bindings::destroy_work_on_stack(self.raw_work()) };

        // SAFETY: `inner` is initialized (type invariant), no longer accessed by the work
        // function, and never accessed again after this point. `Opaque` does not drop its
        // contents, so we have to do it here.
        unsafe { core::ptr::drop_in_place(self.inner.get()) };
    }
}

/// Creates a [`ScopedWork`] pin-initializer with a new lock class.
///
/// This macro expands to a call to [`ScopedWork::new`], which is `unsafe`, so the invocation must
/// be wrapped in an `unsafe` block.
///
/// # Safety
///
/// The caller must uphold the safety requirements of [`ScopedWork::new`]: the destructor of the
/// initialized `ScopedWork` must run before the memory it lives in is reused and before the bound
/// [`Queue`]'s lifetime or any lifetime in the inner type expires.
///
/// # Examples
///
/// ```
/// use kernel::workqueue::{
///     new_scoped_work,
///     OwnedQueue,
///     Queue,
///     ScopedWork,
///     ScopedWorkItem,
/// };
///
/// struct MyWork {
///     value: u32,
/// }
///
/// impl ScopedWorkItem for MyWork {
///     fn run(work: &ScopedWork<'_, Self>) {
///         pr_info!("value = {}\n", work.value);
///     }
/// }
///
/// #[pin_data]
/// struct MyData {
///     #[pin]
///     work: ScopedWork<'wq, MyWork>,
///     wq: OwnedQueue,
/// }
///
/// fn init_data() -> impl PinInit<MyData, Error> {
///     // Define `my_work` outside the `unsafe` block.
///     let my_work = MyWork { value: 7 };
///     try_pin_init!(MyData {
///         wq: Queue::new_unbound().build(c"my-wq")?,
///         // SAFETY: `MyData` is only ever pin-initialized, and `work` is declared before `wq`,
///         // so `work` is dropped before `wq`.
///         work <- unsafe { new_scoped_work!(wq, "MyWork", my_work) },
///     })
/// }
/// # pin_init::stack_try_pin_init!(let data =? init_data());
/// # data.work().enqueue();
/// # Ok::<(), Error>(())
/// ```
#[macro_export]
macro_rules! new_scoped_work {
    ($queue:expr, $name:literal, $init:expr $(,)?) => {
        $crate::workqueue::ScopedWork::new(
            $queue,
            $crate::c_str!($name),
            $crate::static_lock_class!(),
            $init,
        )
    };
}
pub use new_scoped_work;
