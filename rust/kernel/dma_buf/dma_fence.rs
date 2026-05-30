// SPDX-License-Identifier: GPL-2.0
//
// Copyright (C) 2025, 2026 Red Hat Inc.:
//   - Philipp Stanner <pstanner@redhat.com>

//! DriverFence support.
//!
//! Reference: <https://docs.kernel.org/driver-api/dma-buf.html#c.dma_fence>
//!
//! C header: [`include/linux/dma-fence.h`](srctree/include/linux/dma-fence.h)

use crate::{
    alloc::AllocError,
    bindings,
    container_of,
    error::to_result,
    prelude::*,
    sync::rcu::RcuBox,
    types::ForeignOwnable,
    types::Opaque,
    warn_on, //
};

use pin_init::pin_init_from_closure;

use core::{
    marker::PhantomData, //
    ops::Deref,
    ptr,
    ptr::{
        drop_in_place,
        NonNull, //
    },
    sync::atomic::{
        AtomicU64,
        Ordering, //
    },
};

use bindings::ECANCELED;

use kernel::str::CString;
use kernel::sync::{
    aref::{
        ARef,
        AlwaysRefCounted, //
    },
    Arc,
    ArcBorrow, //
};

/// VTable for dma_fence backend_ops callbacks.
//
// Mandatory dma_fence backend_ops are implemented implicitly through
// [`FenceCtx`]. Additional ones shall get implemented on this trait, which then
// shall be demanded for the fence context data.
pub trait FenceCtxOps {}

/// A dma-fence context. A fence context takes care of associating related fences with each other,
/// providing each with raising sequence numbers and a common identifier.
#[pin_data(PinnedDrop)]
pub struct FenceCtx<F: Send + Sync, C: Send + Sync> {
    /// The fence context number.
    nr: u64,
    /// The sequence number for the next fence created.
    seqno: AtomicU64,
    // The name parameters live in RcuBox because they can be accessed by the
    // dma_fence backend_ops. Those accesses are guarded by the rcu_read_lock(),
    // so dropping them must be delayed by a grace period.
    /// The name of the driver this FenceCtx's fences belong to.
    driver_name: CString,
    /// The name of the timeline this FenceCtx's fences belong to.
    timeline_name: CString,
    #[pin]
    data: C,
    fence_type: PhantomData<F>,
}

#[allow(unused_unsafe)]
impl<F: Send + Sync + DriverFenceAllowedData, C: Send + Sync> FenceCtx<F, C> {
    // This can later be extended as a vtable in case other parties need support
    // for the more "exotic" callbacks.
    const OPS: bindings::dma_fence_ops = bindings::dma_fence_ops {
        get_driver_name: Some(Self::get_driver_name),
        get_timeline_name: Some(Self::get_timeline_name),
        enable_signaling: None,
        signaled: None,
        wait: None,
        release: None,
        set_deadline: None,
    };

    /// Create a new `FenceCtx`.
    pub fn new(
        driver_name: CString,
        timeline_name: CString,
        data: impl PinInit<C>,
    ) -> Result<Arc<Self>> {
        let ctx = pin_init!(Self {
            // SAFETY: `dma_fence_context_alloc()` merely works on a global atomic. Parameter `1`
            // is the number of contexts we want to allocate.
            nr: unsafe { bindings::dma_fence_context_alloc(1) },
            seqno: AtomicU64::new(0),
            driver_name,
            timeline_name,
            data <- data,
            fence_type: PhantomData,
        });

        Arc::pin_init(ctx, GFP_KERNEL)
    }

    fn get_next_fence_seqno(&self) -> u64 {
        self.seqno.fetch_add(1, Ordering::Relaxed)
    }

    /// Allocate the memory for a [`DriverFence`] and already store `data` inside.
    ///
    /// This is needed because many times, creation of a [`DriverFence`] must not
    /// fail, and allocating might deadlock in some situations.
    ///
    /// The `data` you pass here must not perform any operations that are illegal
    /// in atomic context in its [`Drop`] implementation.
    pub fn new_fence_allocation(
        self: ArcBorrow<'_, Self>,
        data: F,
    ) -> Result<DriverFenceAllocation<F, C>> {
        let fctx = Arc::<Self>::from(self);

        DriverFenceAllocation::new(fctx, data)
    }

    /// Create a new fence, consuming `data`.
    ///
    /// The fence will increment the refcount of the fence context associated with this
    /// [`FenceCtx`].
    pub fn new_fence(&self, memory: DriverFenceAllocation<F, C>) -> DriverFence<F, C> {
        let seqno: u64 = self.get_next_fence_seqno();

        // We feed the C dma_fence backend a NULL for the spinlock so that it
        // uses per-fence locks automatically.
        let null_ptr: *mut bindings::spinlock = ptr::null_mut();
        let fence_ptr = memory.as_raw();
        // SAFETY: `fence_ptr` has been created directly above. It will live
        // at least as long as `Self`. The same applies to `&Self::OPS`.
        unsafe { bindings::dma_fence_init(fence_ptr, &Self::OPS, null_ptr, self.nr, seqno) };

        // A `DriverFenceAllocation`'s purpose is to carry allocated memory, so that
        // `DriverFence`s can always be created without allocating. In this
        // method, ownership over that memory is transferred to the new
        // `DriverFence` and managed through refcounting. The C dma_fence
        // backend will ultimately free the memory once the refcount reaches 0.
        let ptr = KBox::into_raw(memory.data);
        // SAFETY: `ptr` was just created validly directly above.
        let ptr = unsafe { NonNull::new_unchecked(ptr) };

        DriverFence { data: ptr }
    }

    extern "C" fn get_driver_name(ptr: *mut bindings::dma_fence) -> *const c_char {
        // SAFETY: The C backend only invokes this callback with `ptr` pointing
        // to a valid, unsignaled `bindings::dma_fence`. All fences created
        // in this module always reside within `Fence` which always resides in
        // a `DriverFenceData`, thus satisfying the function's safety requirements.
        let fctx = unsafe { Self::from_raw_fence(ptr) };

        fctx.driver_name.as_char_ptr()
    }

    extern "C" fn get_timeline_name(ptr: *mut bindings::dma_fence) -> *const c_char {
        // SAFETY: The C backend only invokes this callback with `ptr` pointing
        // to a valid, unsignaled `bindings::dma_fence`. All fences created
        // in this module always reside within `Fence` which always resides in
        // a `DriverFenceData`, thus satisfying the function's safety requirements.
        let fctx = unsafe { Self::from_raw_fence(ptr) };

        fctx.timeline_name.as_char_ptr()
    }

    /// Create a [`FenceCtx`] from an associated [`bindings::dma_fence`].
    ///
    /// # Safety
    ///
    /// `ptr` must be a valid pointer to a dma_fence which resides within a [`Fence`],
    /// which in turn resides in a [`DriverFenceData`].
    unsafe fn from_raw_fence<'a>(ptr: *mut bindings::dma_fence) -> &'a Self {
        let opaque_fence = Opaque::cast_from(ptr);

        // SAFETY: Safe due to the function's overall safety requirements.
        let fence_ptr = unsafe { container_of!(opaque_fence, Fence, inner) };

        // DriverFenceData is repr(C) and a Fence is its first member.
        let fence_data_ptr = fence_ptr as *mut DriverFenceData<F, C>;

        // SAFETY: Safe because of the safety comment directly above.
        let fence_data = unsafe { &*fence_data_ptr };

        &fence_data.fctx
    }
}

// FenceCtx's drop() ensures that the driver cannot unload while there are still
// dma_fence callbacks running. This also prevents UAF problems with fctx.driver_name
// and fctx.timeline_name.
//
// DriverFence data gets dropped through call_rcu() in DriverFence::drop.
// This `rcu_barrier()` also serves to wait for their completion.
#[pinned_drop]
impl<F: Send + Sync, C: Send + Sync> PinnedDrop for FenceCtx<F, C> {
    fn drop(self: Pin<&mut Self>) {
        // SAFETY: `rcu_barrier()` is always safe to be called.
        unsafe { bindings::rcu_barrier() };
    }
}

/// Error type for fence callback registration.
///
/// Generic over `T` so that `AlreadySignaled` can return the callback to the
/// caller, allowing it to reclaim any resources owned by the callback (e.g.,
/// a fence handle that needs to be signaled).
#[derive(Debug)]
pub enum CallbackError<T = ()> {
    /// The fence was already signaled. The callback is returned so the caller
    /// can extract owned resources without losing them.
    AlreadySignaled(T),
    /// Some other error occurred during registration.
    Other(Error),
}

impl<T> From<CallbackError<T>> for Error {
    fn from(err: CallbackError<T>) -> Self {
        match err {
            CallbackError::AlreadySignaled(_) => ENOENT,
            CallbackError::Other(e) => e,
        }
    }
}

impl<T> From<AllocError> for CallbackError<T> {
    fn from(e: AllocError) -> Self {
        CallbackError::Other(Error::from(e))
    }
}

/// Trait for callbacks that can be registered on fences.
///
/// When the fence signals, the callback will be invoked.
///
/// # Example
///
/// ```rust
/// use kernel::dma_buf::FenceCb;
///
/// struct MyCallback {
///     // Your callback state here
/// }
///
/// impl FenceCb for MyCallback {
///     fn called(&mut self) {
///         pr_info!("Fence signaled!");
///         // Handle fence completion
///     }
/// }
/// ```
pub trait FenceCb: Send + 'static {
    /// Called when the fence is signaled.
    ///
    /// This is called from the fence signaling path, which may be in interrupt
    /// context or with locks held, which is why `self` is only borrowed, so that
    /// it cannot drop. Implementations must not sleep or perform
    /// long-running operations.
    ///
    /// An implementation likely wants to inform itself (e.g., through a work item)
    /// within this callback that the associated [`FenceCbRegistration`] can now be
    /// dropped.
    fn called(&mut self);
}

/// A callback registration on a fence.
///
/// When this object is dropped, the callback is automatically removed if it
/// hasn't been called yet.
///
/// # Invariants
///
/// If `callback` is `Some`, then `cb` is registered with the fence and the
/// callback hasn't been invoked yet. If `None`, the callback has been invoked
/// or the fence was already signaled when we tried to register.
#[pin_data(PinnedDrop)]
pub struct FenceCbRegistration<T: FenceCb + 'static> {
    #[pin]
    cb: Opaque<bindings::dma_fence_cb>,
    callback: T,
    fence: ARef<Fence>,
}

impl<T: FenceCb> FenceCbRegistration<T> {
    /// Register a callback on a fence.
    ///
    /// On success the callback is pinned in place and will fire when the fence
    /// signals. On `AlreadySignaled` the callback is returned to the caller so
    /// that owned resources can be reclaimed.
    pub fn new<'a>(fence: &'a Fence, callback: T) -> impl PinInit<Self, CallbackError<T>> + 'a
    where
        T: 'a,
    {
        // Uses `pin_init_from_closure` instead of `try_pin_init!` so that on
        // `-ENOENT` (already signaled) the callback can be read back from the
        // partially-initialized slot and returned through the error.
        //
        // SAFETY: `pin_init_from_closure` requires:
        // - On `Ok(())`: the slot is fully initialized and valid for `Drop`.
        // - On `Err(_)`: the slot is clean, i.e.: no partially-initialized fields
        //   remain, and the slot can be deallocated without dropping.
        //
        // We uphold this as follows:
        // - On success: all three fields are initialized. Ok(()) is returned.
        // - On ENOENT (already signaled): `callback` and `fence` are read back
        //   from the slot via `ptr::read`, leaving the slot clean. `cb` was
        //   initialized by `dma_fence_add_callback` (it calls
        //   `INIT_LIST_HEAD(&cb->node)` even on error), but `cb` is
        //   `Opaque<dma_fence_cb>` which has no `Drop`, so not dropping it is
        //   fine. The callback is returned through `AlreadySignaled(T)`.
        // - On other errors: same cleanup as ENOENT, error returned as
        //   `Other(e)`.
        unsafe {
            pin_init_from_closure(move |slot: *mut Self| {
                let slot_callback = &raw mut (*slot).callback;
                let slot_fence = &raw mut (*slot).fence;
                let slot_cb = &raw mut (*slot).cb;

                // Write callback and fence first — must be visible before
                // dma_fence_add_callback makes the registration live.
                core::ptr::write(slot_callback, callback);
                core::ptr::write(slot_fence, ARef::from(fence));

                let ret = to_result(bindings::dma_fence_add_callback(
                    fence.inner.get(),
                    Opaque::cast_into(slot_cb),
                    Some(Self::dma_fence_callback),
                ));

                match ret {
                    Ok(()) => Ok(()),
                    Err(e) => {
                        // Read back what we wrote to leave the slot clean.
                        let cb_back = core::ptr::read(slot_callback);
                        let _fence_back = core::ptr::read(slot_fence);

                        if e.to_errno() == ENOENT.to_errno() {
                            Err(CallbackError::AlreadySignaled(cb_back))
                        } else {
                            Err(CallbackError::Other(e))
                        }
                    }
                }
            })
        }
    }

    /// Raw dma fence callback that is called by the C code.
    ///
    /// # Safety
    ///
    /// This is only called by the dma_fence subsystem with valid pointers.
    unsafe extern "C" fn dma_fence_callback(
        _fence: *mut bindings::dma_fence,
        cb: *mut bindings::dma_fence_cb,
    ) {
        let ptr = Opaque::cast_from(cb).cast_mut();

        // SAFETY: All `cb` we can receive here have been created in such a way
        // that they are embedded into a `FenceCbRegistration`. The backend
        // ensures synchronisation so whoever holds the registration object
        // cannot drop it while this code is running. See `FenceCbRegistration::drop`.
        unsafe {
            let reg: *mut Self = container_of!(ptr, Self, cb);

            (*reg).callback.called();
        }
    }

    /// Returns a reference to the fence this callback is registered on.
    pub fn fence(self: Pin<&Self>) -> &Fence {
        &self.get_ref().fence
    }
}

#[pinned_drop]
impl<T: FenceCb> PinnedDrop for FenceCbRegistration<T> {
    fn drop(self: Pin<&mut Self>) {
        // Always call dma_fence_remove_callback, even if `callback` has already
        // been taken by `dma_fence_callback`.  This is necessary for
        // synchronization: `dma_fence_remove_callback` acquires `fence->lock`,
        // which ensures that any in-flight `dma_fence_signal` (which calls our
        // callback while holding the same lock) has completed before we free
        // the struct.
        //
        // Without this, Drop can race with a concurrent signal:
        //   CPU0 (signal, lock held): take() -> signaled(fence_ref) (in progress)
        //   CPU1 (drop): sees is_some()==false -> skips lock -> frees struct
        //   CPU0: accesses fence_ref -> use-after-free
        //
        // When the callback has already fired, the signal path detached the
        // list node via INIT_LIST_HEAD, so dma_fence_remove_callback just sees
        // an empty node and returns false — the lock acquisition is the only
        // thing that matters.
        //
        // SAFETY: The fence pointer is valid and the cb was initialized by
        // dma_fence_add_callback during construction.
        unsafe {
            bindings::dma_fence_remove_callback(self.fence.as_raw(), self.cb.get());
        }
    }
}

// SAFETY: FenceCbRegistration can be sent between threads
unsafe impl<T: FenceCb> Send for FenceCbRegistration<T> {}

// SAFETY: &FenceCbRegistration can be shared between threads if &T can.
unsafe impl<T: FenceCb> Sync for FenceCbRegistration<T> where T: Sync {}

/// The receiving counterpart of a [`DriverFence`], designed to register callbacks
/// on, check the signalled state etc. A [`Fence`] cannot be signalled.
/// A [`Fence`] is always refcounted.
pub struct Fence {
    /// The actual dma_fence passed to C.
    inner: Opaque<bindings::dma_fence>,
}

// SAFETY: Fences are literally designed to be shared between threads.
unsafe impl Send for Fence {}
// SAFETY: Fences are literally designed to be shared between threads.
unsafe impl Sync for Fence {}

impl Fence {
    /// Check whether the fence was signalled at the moment of the function call.
    pub fn is_signaled(&self) -> bool {
        // SAFETY: self is by definition still valid. The backend ensures proper
        // locking.
        unsafe { bindings::dma_fence_is_signaled(self.as_raw()) }
    }

    fn as_raw(&self) -> *mut bindings::dma_fence {
        self.inner.get()
    }

    /// Create a [`Fence`] from a raw C [`bindings::dma_fence`].
    ///
    /// # Safety
    ///
    /// `ptr` must point to an initialized fence that is embedded into a [`Fence`].
    pub unsafe fn from_raw<'a>(ptr: *mut bindings::dma_fence) -> &'a Self {
        // SAFETY: Safe as per the function's overall safety requirements.
        unsafe { &*ptr.cast() }
    }
}

// SAFETY: These implement the C backends refcounting methods which are proven to work correctly.
unsafe impl AlwaysRefCounted for Fence {
    fn inc_ref(&self) {
        // SAFETY: `self.as_raw()` is a pointer to a valid `struct dma_fence`.
        unsafe { bindings::dma_fence_get(self.as_raw()) }
    }

    /// # Safety
    ///
    /// `ptr`must be a valid pointer to a [`DriverFence`].
    unsafe fn dec_ref(ptr: NonNull<Self>) {
        // SAFETY: `ptr` is never a NULL pointer; and when `dec_ref()` is called
        // the fence is by definition still valid.
        let fence = unsafe { (*ptr.as_ptr()).inner.get() };

        // SAFETY: Valid because `fence` was created validly above.
        unsafe { bindings::dma_fence_put(fence) }
    }
}

#[repr(C)] // Necessary to guarantee that `inner` always comes first so that we can cast.
#[pin_data]
struct DriverFenceData<F: Send + Sync, C: Send + Sync> {
    #[pin]
    /// The inner fence.
    inner: Fence,
    /// Pointer to access the FenceCtx. Useful for obtaining name parameters.
    // The FenceCtx lives as long as at least all its fences, hence this is safe.
    fctx: Arc<FenceCtx<F, C>>,
    /// The API user's data. As required by [`DriverFenceAllowedData`], this either
    /// does not need drop, or must live in a [`rcu::RcuBox`]. It is essential
    /// that the data only performs operations legal in atomic context in its
    /// [`Drop`] implementation.
    #[pin]
    data: F,
}

/// A trait to enforce that all data in a [`DriverFence`] either does not need
/// drop, or lives in a [`RcuBox`].
pub trait DriverFenceAllowedData: private::Sealed {}

mod private {
    pub trait Sealed {}
}

impl<F: Copy> DriverFenceAllowedData for F {}
impl<F: Send> DriverFenceAllowedData for RcuBox<F> {}

impl<F: Copy> private::Sealed for F {}
impl<F: Send> private::Sealed for RcuBox<F> {}

/// A synchronization primitive mainly for GPU drivers.
///
/// Fences are always reference counted. The typical use case is that one side registers
/// callbacks on the fence which will perform a certain action (such as queueing work) once the
/// other side signals the fence.
///
/// # Examples
///
/// ```
/// use kernel::dma_buf::{DriverFence, FenceCtx, FenceCb, FenceCbRegistration};
/// use kernel::str::CString;
/// use kernel::sync::{
///     aref::ARef,
///     rcu::RcuBox, //
/// };
/// use core::ops::Deref;
/// use core::fmt::Display;
///
/// struct CallbackData { }
///
/// impl FenceCb for CallbackData {
///     fn called(&mut self) {
///         pr_info!("DmaFence callback executed.\n");
///     }
/// }
///
/// let driver_name = CString::try_from_fmt(fmt!("dummy_driver"))?;
/// let timeline_name = CString::try_from_fmt(fmt!("dummy_timeline"))?;
///
/// let fctx = FenceCtx::new(driver_name, timeline_name, ())?;
///
/// let fence_data = CString::try_from_fmt(fmt!("dummy_data"))?;
/// // DriverFence::data must either not need drop, or live in an RcuBox.
/// let fence_data = RcuBox::new(fence_data, GFP_KERNEL)?;
///
/// let fence_alloc = fctx.as_arc_borrow().new_fence_allocation(fence_data)?;
/// let mut fence = fctx.new_fence(fence_alloc);
///
/// let cb_data = CallbackData { };
/// let waiting_fence = ARef::from(fence.as_fence());
/// let cb_reg = FenceCbRegistration::new(&waiting_fence, cb_data);
/// let cb_reg = KBox::pin_init(cb_reg, GFP_KERNEL)?;
///
/// // DriverFence implements Deref.
/// // FIXME: unit test claims that CString does not implement Display. Why?
/// // pr_info!("Fence's inner data is: {}", fence.deref().deref());
///
/// // TODO begin_signalling
/// fence.signal(Ok(()));
/// assert_eq!(waiting_fence.is_signaled(), true);
///
/// Ok::<(), Error>(())
/// ```
pub struct DriverFence<F: Send + Sync, C: Send + Sync> {
    /// The actual content of the fence. Lives in a raw pointer so that its
    /// memory can be managed independently. Valid until both the [`DriverFence`]
    /// and all associated [`Fence`]s have disappeared.
    data: NonNull<DriverFenceData<F, C>>,
}

/// A pre-prepared DMA fence, carrying the user's data and the memory it and the
/// fence reside in. Only useful for creating a [`DriverFence`]. Splitting
/// allocation and full initialization is necessary because fences cannot be
/// allocated dynamically in some circumstances (deadlock).
pub struct DriverFenceAllocation<F: Send + Sync, C: Send + Sync> {
    /// The memory for the actual content of the fence.
    /// Handed over to a [`DriverFence`], or deallocated once the
    /// [`DriverFenceAllocation`] drops.
    data: KBox<DriverFenceData<F, C>>,
}

impl<F: Send + Sync + DriverFenceAllowedData, C: Send + Sync> DriverFenceAllocation<F, C> {
    /// Create a new allocation slot that can later be used to create a fully
    /// initialized [`DriverFence`] without the need to allocate.
    pub fn new(fctx: Arc<FenceCtx<F, C>>, data: F) -> Result<Self> {
        let fence_data = DriverFenceData {
            // `inner` remains uninitialized until a [`DriverFence`] takes over.
            inner: Fence {
                inner: Opaque::uninit(),
            },
            fctx,
            data,
        };

        // In order to support the C dma_fence callbacks, it is necessary for
        // a `Fence` and a `DriverFence` to live in the same allocation,
        // because the C backend passes a dma_fence, from which the driver most
        // likely wants to be able to access its `data` in `DriverFence`.
        //
        // Hence, we need the manage the memory manually. It will be freed by the
        // C backend automatically once the refcount within `Fence` drops to 0.
        let data = KBox::new(fence_data, GFP_KERNEL | __GFP_ZERO)?;

        Ok(Self { data })
    }

    fn as_raw(&self) -> *mut bindings::dma_fence {
        self.data.inner.inner.get()
    }
}

impl<F: Send + Sync, C: Send + Sync> DriverFence<F, C> {
    fn as_raw(&self) -> *mut bindings::dma_fence {
        // SAFETY: Valid because `self` is valid.
        let fence_data = unsafe { &mut *self.data.as_ptr() };

        fence_data.inner.inner.get()
    }

    /// Create a [`DriverFence`] from a raw pointer to a [`bindings::dma_fence`].
    ///
    /// # Safety
    ///
    /// `ptr` must be a valid pointer to a `dma_fence` that was obtained through
    /// a [`DriverFence`] with matching generic data for both fence and associated
    /// [`FenceCtx`].
    unsafe fn from_raw(ptr: *mut bindings::dma_fence) -> Self {
        let opaque_fence = Opaque::cast_from(ptr);

        // SAFETY: Safe due to the function's overall safety requirements.
        let fence_ptr = unsafe { container_of!(opaque_fence, Fence, inner) };

        // DriverFenceData is repr(C) and a Fence is its first member.
        let fence_data_ptr = fence_ptr as *mut DriverFenceData<F, C>;

        // SAFETY: `fence_data_ptr` was created validly above.
        let data = unsafe { NonNull::new_unchecked(fence_data_ptr) };

        Self { data }
    }

    /// Return the underlying [`Fence`].
    pub fn as_fence(&self) -> &Fence {
        // SAFETY: `self` is by definition still valid, and it cannot drop until
        // this new reference is gone.
        unsafe { Fence::from_raw(self.as_raw()) }
    }

    /// Signal the fence. This will invoke all registered callbacks.
    pub fn signal(self, res: Result) {
        let fence = self.as_raw();
        let mut fence_flags: usize = 0;
        let flag_ptr = &raw mut fence_flags;

        // SAFETY: Once a `DriverFence` is initialized, the inner `fence` is
        // valid and initialized. It is valid until the refcount drops
        // to 0, which can earliest happen once the `DriverFence` has been dropped.
        unsafe {
            bindings::dma_fence_lock_irqsave(fence, flag_ptr);
            if !bindings::dma_fence_is_signaled_locked(fence) {
                if let Err(err) = res {
                    bindings::dma_fence_set_error(fence, err.to_errno());
                }
                bindings::dma_fence_signal_locked(fence);
            }
            bindings::dma_fence_unlock_irqrestore(fence, flag_ptr);
        }
    }
}

// SAFETY: Fences are literally designed to be shared between threads.
unsafe impl<F: Send + Sync, C: Send + Sync> Send for DriverFence<F, C> {}

impl<F: Send + Sync, C: Send + Sync> Deref for DriverFence<F, C> {
    type Target = F;

    fn deref(&self) -> &Self::Target {
        // SAFETY: Thanks to refcounting, `data` is always valid as long as `self` is.
        let data = unsafe { &*self.data.as_ptr() };

        &data.data
    }
}

/// A borrowed [`DriverFence`]. All you can do with it is access your user data
/// and obtain a [`Fence`].
pub struct DriverFenceBorrow<F: Send + Sync, C: Send + Sync> {
    /// The actual content of the fence. Lives in a raw pointer so that its
    /// memory can be managed independently. Valid until both the [`DriverFence`]
    /// and all associated [`Fence`]s have disappeared.
    data: NonNull<DriverFenceData<F, C>>,
}

impl<F: Send + Sync, C: Send + Sync> Deref for DriverFenceBorrow<F, C> {
    type Target = F;

    fn deref(&self) -> &Self::Target {
        // SAFETY: Thanks to refcounting, `data` is always valid as long as `self` is.
        let data = unsafe { &*self.data.as_ptr() };

        &data.data
    }
}

impl<F: Send + Sync, C: Send + Sync> DriverFenceBorrow<F, C> {
    fn as_raw(&self) -> *mut bindings::dma_fence {
        // SAFETY: Valid because `self` is valid.
        let fence_data = unsafe { &mut *self.data.as_ptr() };

        fence_data.inner.inner.get()
    }

    /// Return the underlying [`Fence`].
    pub fn as_fence(&self) -> &Fence {
        // SAFETY: `self` is by definition still valid, and it cannot drop until
        // this new reference is gone.
        unsafe { Fence::from_raw(self.as_raw()) }
    }

    /// Get a [`DriverFenceBorrow`] from a raw pointer.
    ///
    /// # Safety
    ///
    /// `ptr` must point to a raw dma_fence within a [`Fence`] within a [`DriverFenceData`].
    unsafe fn from_raw(ptr: *mut bindings::dma_fence) -> Self {
        let opaque_fence = Opaque::cast_from(ptr);

        // SAFETY: Safe due to the function's overall safety requirements.
        let fence_ptr = unsafe { container_of!(opaque_fence, Fence, inner) };

        // DriverFenceData is repr(C) and a Fence is its first member.
        let fence_data_ptr = fence_ptr as *mut DriverFenceData<F, C>;

        // SAFETY: `fence_data_ptr` was created validly above.
        let data = unsafe { NonNull::new_unchecked(fence_data_ptr) };

        Self { data }
    }
}

// SAFETY: The Rust dma_fence abstractions are already designed around the inner
// C `dma_fence`, which can serve safely as the identification point when being
// owned by C. Moreover, safety is ensured by not dropping `DriverFence` and by
// only allowing operations without side effects on the Borrowed type.
unsafe impl<F: Send + Sync + 'static, C: Send + Sync + 'static> ForeignOwnable
    for DriverFence<F, C>
{
    // `DriverFence` is merely a wrapper around a raw pointer. Thus, we can just
    // use it directly.
    type Borrowed<'a> = DriverFenceBorrow<F, C>;
    type BorrowedMut<'a> = DriverFenceBorrow<F, C>;

    const FOREIGN_ALIGN: usize = core::mem::align_of::<bindings::dma_fence>();

    fn into_foreign(self) -> *mut c_void {
        let fence = self;

        let ptr = fence.as_raw();

        // DriverFence must not drop.
        core::mem::forget(fence);

        ptr.cast()
    }

    unsafe fn from_foreign(ptr: *mut c_void) -> Self {
        // SAFETY: Safe because the trait implementation only invokes this with
        // a valid `ptr`, associated to a `DriverFence` with matching generic data.
        unsafe { Self::from_raw(ptr.cast()) }
    }

    unsafe fn borrow<'a>(ptr: *mut c_void) -> Self::Borrowed<'a> {
        // SAFETY: The trait implementation ensures that `ptr` always resides
        // within a [`Fence`] within a [`DriverFenceData`].
        unsafe { DriverFenceBorrow::from_raw(ptr.cast()) }
    }

    unsafe fn borrow_mut<'a>(ptr: *mut c_void) -> Self::BorrowedMut<'a> {
        // SAFETY: The trait implementation ensures that `ptr` always resides
        // within a [`Fence`] within a [`DriverFenceData`].
        unsafe { DriverFenceBorrow::from_raw(ptr.cast()) }
    }
}

impl<F: Send + Sync, C: Send + Sync> Drop for DriverFence<F, C> {
    fn drop(&mut self) {
        let fence = self.as_raw();
        let mut fence_flags: usize = 0;
        let flag_ptr = &raw mut fence_flags;

        // SAFETY: Once a `DriverFence` is initialized, the inner `fence` is
        // valid and initialized. It is valid until the refcount drops
        // to 0, which can earliest happen once the `DriverFence` has been dropped.
        unsafe {
            bindings::dma_fence_lock_irqsave(fence, flag_ptr);
            #[allow(unused_unsafe)]
            if warn_on!(!bindings::dma_fence_is_signaled_locked(fence)) {
                bindings::dma_fence_set_error(fence, ECANCELED as i32);
                bindings::dma_fence_signal_locked(fence);
            }
            bindings::dma_fence_unlock_irqrestore(fence, flag_ptr);
        }

        // SAFETY: `self.data` is owned by the DriverFence, but could be accessed
        // through some dma_fence callbacks right now. Access is being revoked
        // above by signalling the fence. The DriverFenceAllowedData trait
        // ensures that the data either does not need drop, or if it does it
        // lives in a RcuBox which will delay dropping by one grace period, hence
        // ensuring that all readers have disappeared.
        unsafe { drop_in_place(self.data.as_ptr()) };

        // SAFETY: Once a `DriverFence` is initialized, the inner `fence` is
        // valid and initialized. It is valid until the refcount drops
        // to 0, which can earliest happen once the `DriverFence` has been dropped.
        unsafe {
            bindings::dma_fence_put(fence);
        }

        // The actual memory the data associated with a `DriverFence` lives in
        // gets freed by the C dma_fence backend once the fence's refcount reaches 0.
    }
}
