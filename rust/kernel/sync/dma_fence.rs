// SPDX-License-Identifier: GPL-2.0

//! DmaFence support.
//!
//! Reference: <https://docs.kernel.org/driver-api/dma-buf.html#c.dma_fence>
//!
//! C header: [`include/linux/dma-fence.h`](srctree/include/linux/dma-fence.h)

use crate::{
    bindings,
    prelude::*,
    str::CStr,
    types::ForeignOwnable,
    types::{ARef, AlwaysRefCounted, Opaque},
};

use core::{
    ptr::{drop_in_place, NonNull},
    sync::atomic::{AtomicU64, Ordering},
};

use kernel::sync::{Arc, ArcBorrow};

/// The C dma_fence backend functions ask for certain name parameters at times. In order to avoid
/// storing those names in [`DmaFence`] (there can be millions of fences at the same time, wasting
/// much memory), the driver must provide associated constants which the C backend will access,
/// ultimately.
pub trait DmaFenceNames {
    /// The driver's name.
    const DRIVER_NAME: &CStr;
    /// The name of the timeline the fence is associated with.
    const TIMELINE_NAME: &CStr;
}

/// Defines the callback function the dma-fence backend will call once the fence gets signalled.
pub trait DmaFenceCbFunc {
    /// The callback function. `cb` is a container of the data which the driver passed in
    /// [`DmaFence::register_callback`].
    fn callback(cb: Pin<KBox<DmaFenceCb<Self>>>)
    where
        Self: Sized;
}

/// Container for driver data which the driver gets back in its callback once the fence gets
/// signalled.
#[pin_data]
pub struct DmaFenceCb<T: DmaFenceCbFunc> {
    /// C struct needed for the backend.
    #[pin]
    inner: Opaque<bindings::dma_fence_cb>,
    /// Driver data.
    #[pin]
    pub data: T,
}

impl<T: DmaFenceCbFunc + 'static> DmaFenceCb<T> {
    fn new(data: impl PinInit<T>) -> Result<Pin<KBox<Self>>> {
        let cb = try_pin_init!(Self {
            inner: Opaque::zeroed(), // This gets initialized by the C backend.
            data <- data,
        });

        KBox::pin_init(cb, GFP_KERNEL)
    }

    /// Callback for the C dma_fence backend.
    ///
    /// # Safety
    /// All data used and cast in this function was validly created by
    /// [`DmaFence::register_callback`] and isn't modified by the C backend until this callback
    /// here has run.
    unsafe extern "C" fn callback(
        _fence_ptr: *mut bindings::dma_fence,
        cb_ptr: *mut bindings::dma_fence_cb,
    ) {
        let cb_ptr = Opaque::cast_from(cb_ptr);

        // SAFETY: The constructor guarantees that `cb_ptr` is always `inner` of a DmaFenceCb.
        let cb_ptr = unsafe { crate::container_of!(cb_ptr, Self, inner) }.cast_mut() as *mut c_void;
        // SAFETY: `cp_ptr` is the heap memory of a Pin<Kbox<Self>> because it was created by
        // invoking ForeignOwnable::into_foreign() on such an instance.
        let cb = unsafe { <Pin<KBox<Self>> as ForeignOwnable>::from_foreign(cb_ptr) };

        // Pass ownership back over to the driver.
        T::callback(cb);
    }
}

/// A dma-fence context. A fence context takes care of associating related fences with each other,
/// providing each with raising sequence numbers and a common identifier.
#[pin_data]
pub struct DmaFenceCtx {
    /// An opaque spinlock. Only ever passed to the C backend, never used by Rust.
    #[pin]
    lock: Opaque<bindings::spinlock_t>,
    /// The fence context number.
    nr: u64,
    /// The sequence number for the next fence created.
    seqno: AtomicU64,
}

impl DmaFenceCtx {
    /// Create a new `DmaFenceCtx`.
    pub fn new() -> Result<Arc<Self>> {
        let ctx = pin_init!(Self {
            // Feed in a non-Rust spinlock for now, since the Rust side never needs the lock.
            lock <- Opaque::ffi_init(|slot: *mut bindings::spinlock| {
                // SAFETY: `slot` is a valid pointer to an uninitialized `struct spinlock_t`.
                unsafe { bindings::spin_lock_init(slot) };
            }),
            // SAFETY: `dma_fence_context_alloc()` merely works on a global atomic. Parameter `1`
            // is the number of contexts we want to allocate.
            nr: unsafe { bindings::dma_fence_context_alloc(1) },
            seqno: AtomicU64::new(0),
        });

        Arc::pin_init(ctx, GFP_KERNEL)
    }

    fn get_new_fence_seqno(&self) -> u64 {
        self.seqno.fetch_add(1, Ordering::Relaxed)
    }
}

impl ArcBorrow<'_, DmaFenceCtx> {
    /// Create a new fence, consuming `data`.
    ///
    /// The fence will increment the refcount of the fence context associated with this
    /// [`DmaFenceCtx`].
    pub fn new_fence<T: DmaFenceNames>(
        &mut self,
        data: impl PinInit<T>,
    ) -> Result<ARef<DmaFence<T>>> {
        let fctx: Arc<DmaFenceCtx> = (*self).into();
        let seqno: u64 = fctx.get_new_fence_seqno();

        // TODO: Should we reset seqno in case of failure?
        // Pass `fctx` by value so that the fence will hold a reference to the DmaFenceCtx as long
        // as it lives.
        DmaFence::new(fctx, data, &self.lock, self.nr, seqno)
    }
}

/// A synchronization primitive mainly for GPU drivers.
///
/// DmaFences are always reference counted. The typical use case is that one side registers
/// callbacks on the fence which will perform a certain action (such as queueing work) once the
/// other side signals the fence.
///
/// # Examples
///
/// ```
/// use kernel::c_str;
/// use kernel::sync::{Arc, ArcBorrow, DmaFence, DmaFenceCtx, DmaFenceNames, DmaFenceCb, DmaFenceCbFunc};
/// use core::sync::atomic::{self, AtomicBool};
///
/// static mut CHECKER: AtomicBool = AtomicBool::new(false);
///
/// struct CallbackData {
///     i: u32,
/// }
///
/// impl CallbackData {
///     fn new() -> Self {
///         Self { i: 9 }
///     }
/// }
///
/// impl DmaFenceCbFunc for CallbackData {
///     fn callback(cb: Pin<KBox<DmaFenceCb<Self>>>) where Self: Sized {
///         assert_eq!(cb.data.i, 9);
///         // SAFETY: Just to have an easy way for testing. This cannot race with the checker
///         // because the fence signalling callbacks are executed synchronously.
///         unsafe { CHECKER.store(true, atomic::Ordering::Relaxed); }
///     }
/// }
///
/// struct DriverData {
///     i: u32,
/// }
///
/// impl DriverData {
///     fn new() -> Self {
///         Self { i: 5 }
///     }
/// }
///
/// impl DmaFenceNames for DriverData {
///     const DRIVER_NAME: &CStr = c_str!("DUMMY_DRIVER");
///     const TIMELINE_NAME: &CStr = c_str!("DUMMY_TIMELINE");
/// }
///
/// let data = DriverData::new();
/// let fctx = DmaFenceCtx::new()?;
///
/// let mut fence = fctx.as_arc_borrow().new_fence(data)?;
///
/// let cb_data = CallbackData::new();
/// fence.register_callback(cb_data);
/// // fence.begin_signalling();
/// fence.signal()?;
/// // Now check wehether the callback was actually executed.
/// // SAFETY: `fence.signal()` above works sequentially. We just check here whether the signalling
/// // actually did set the boolean correctly.
/// unsafe { assert_eq!(CHECKER.load(atomic::Ordering::Relaxed), true); }
///
/// Ok::<(), Error>(())
/// ```
#[pin_data]
pub struct DmaFence<T> {
    /// The actual dma_fence passed to C.
    #[pin]
    inner: Opaque<bindings::dma_fence>,
    /// User data.
    #[pin]
    data: T,
    /// Marks whether the fence is currently in the signalling critical section.
    signalling: bool,
    /// A boolean needed for the C backend's lockdep guard.
    signalling_cookie: bool,
    /// A reference to the associated [`DmaFenceCtx`] so that it cannot be dropped while there are
    /// still fences around.
    fctx: Arc<DmaFenceCtx>,
}

// SAFETY: `DmaFence` is safe to be sent to any task.
unsafe impl<T> Send for DmaFence<T> {}

// SAFETY: `DmaFence` is safe to be accessed concurrently.
unsafe impl<T> Sync for DmaFence<T> {}

// SAFETY: These implement the C backends refcounting methods which are proven to work correctly.
unsafe impl<T: DmaFenceNames> AlwaysRefCounted for DmaFence<T> {
    fn inc_ref(&self) {
        // SAFETY: `self.as_raw()` is a pointer to a valid `struct dma_fence`.
        unsafe { bindings::dma_fence_get(self.as_raw()) }
    }

    /// # Safety
    ///
    /// `ptr`must be a valid pointer to a [`DmaFence`].
    unsafe fn dec_ref(ptr: NonNull<Self>) {
        // SAFETY: `ptr` is never a NULL pointer; and when `dec_ref()` is called
        // the fence is by definition still valid.
        let fence = unsafe { (*ptr.as_ptr()).inner.get() };

        // SAFETY: Valid because `fence` was created validly above.
        unsafe { bindings::dma_fence_put(fence) }
    }
}

impl<T: DmaFenceNames> DmaFence<T> {
    // TODO: There could be a subtle potential problem here? The LLVM compiler backend can create
    // several versions of this constant. Their content would be identical, but their addresses
    // different.
    const OPS: bindings::dma_fence_ops = Self::ops_create();

    /// Create an initializer for a new [`DmaFence`].
    fn new(
        fctx: Arc<DmaFenceCtx>,
        data: impl PinInit<T>, // TODO: The driver data should implement PinInit<T, Error>
        lock: &Opaque<bindings::spinlock_t>,
        context: u64,
        seqno: u64,
    ) -> Result<ARef<Self>> {
        let fence = pin_init!(Self {
            inner <- Opaque::ffi_init(|slot: *mut bindings::dma_fence| {
                let lock_ptr = &raw const (*lock);
                // SAFETY: `slot` is a valid pointer to an uninitialized `struct dma_fence`.
                // `lock_ptr` is a pointer to the spinlock of the fence context, which is shared
                // among all the fences. This can't become a UAF because each fence takes a
                // reference of the fence context.
                unsafe { bindings::dma_fence_init(slot, &Self::OPS, Opaque::cast_into(lock_ptr), context, seqno) };
            }),
            data <- data,
            signalling: false,
            signalling_cookie: false,
            fctx: fctx,
        });

        let b = KBox::pin_init(fence, GFP_KERNEL)?;

        // SAFETY: We don't move the contents of `b` anywhere here. After unwrapping it, ARef will
        // take care of preventing memory moves.
        let rawptr = KBox::into_raw(unsafe { Pin::into_inner_unchecked(b) });

        // SAFETY: `rawptr` was created validly above.
        let aref = unsafe { ARef::from_raw(NonNull::new_unchecked(rawptr)) };

        Ok(aref)
    }

    /// Mark the beginning of a DmaFence signalling critical section. Should be called once a fence
    /// gets published.
    ///
    /// The signalling critical section is marked as finished automatically once the fence signals.
    pub fn begin_signalling(&mut self) {
        // FIXME: this needs to be mutable, obviously, but we can't borrow mutably. *sigh*
        self.signalling = true;
        // TODO: Should we warn if a user tries to do this several times for the same
        // fence? And should we ignore the request if the fence is already signalled?

        // SAFETY: `dma_fence_begin_signalling()` works on global lockdep data and calling it is
        // always safe.
        self.signalling_cookie = unsafe { bindings::dma_fence_begin_signalling() };
    }

    const fn ops_create() -> bindings::dma_fence_ops {
        // SAFETY: Zeroing out memory on the stack is always safe.
        let mut ops: bindings::dma_fence_ops = unsafe { core::mem::zeroed() };

        ops.get_driver_name = Some(Self::get_driver_name);
        ops.get_timeline_name = Some(Self::get_timeline_name);
        ops.release = Some(Self::release);

        ops
    }

    extern "C" fn get_driver_name(_ptr: *mut bindings::dma_fence) -> *const c_char {
        T::DRIVER_NAME.as_ptr()
    }

    extern "C" fn get_timeline_name(_ptr: *mut bindings::dma_fence) -> *const c_char {
        T::TIMELINE_NAME.as_ptr()
    }

    /// The release function called by the C backend once the refcount drops to 0. We use this to
    /// drop the Rust dma-fence, too. Since [`DmaFence`] implements [`AlwaysRefCounted`], this is
    /// perfectly safe and a convenient way to concile the two release mechanisms of C and Rust.
    unsafe extern "C" fn release(ptr: *mut bindings::dma_fence) {
        let ptr = Opaque::cast_from(ptr);

        // SAFETY: The constructor guarantees that `ptr` is always the inner fence of a DmaFence.
        let fence = unsafe { crate::container_of!(ptr, Self, inner) }.cast_mut();

        // SAFETY: See above. Also, the release callback will only be called once, when the
        // refcount drops to 0, and when that happens the fence is by definition still valid.
        unsafe { drop_in_place(fence) };
    }

    /// Signal the fence. This will invoke all registered callbacks.
    pub fn signal(&self) -> Result {
        // SAFETY: `self` is refcounted.
        let ret = unsafe { bindings::dma_fence_signal(self.as_raw()) };
        if ret != 0 {
            return Err(Error::from_errno(ret));
        }

        if self.signalling {
            // SAFETY: `dma_fence_end_signalling()` works on global lockdep data. The only
            // parameter is a boolean passed by value.
            unsafe { bindings::dma_fence_end_signalling(self.signalling_cookie) };
        }

        Ok(())
    }

    /// Register a callback on a [`DmaFence`]. The callback will be invoked in the fence's
    /// signalling path, i.e., critical section.
    ///
    /// Consumes `data`. `data` is passed back to the implemented callback function when the fence
    /// gets signalled.
    pub fn register_callback<U: DmaFenceCbFunc + 'static>(&self, data: impl PinInit<U>) -> Result {
        let cb = DmaFenceCb::new(data)?;
        let ptr = cb.into_foreign() as *mut DmaFenceCb<U>;
        // SAFETY: `ptr` was created validly directly above.
        let inner_cb = unsafe { (*ptr).inner.get() };

        // SAFETY: `self.as_raw()` is valid because `self` is refcounted, `inner_cb` was created
        // validly above and was turned into a ForeignOwnable, so it won't be dropped. `callback`
        // has static life time.
        let ret = unsafe {
            bindings::dma_fence_add_callback(
                self.as_raw(),
                inner_cb,
                Some(DmaFenceCb::<U>::callback),
            )
        };
        if ret != 0 {
            return Err(Error::from_errno(ret));
        }
        Ok(())
    }

    fn as_raw(&self) -> *mut bindings::dma_fence {
        self.inner.get()
    }
}
