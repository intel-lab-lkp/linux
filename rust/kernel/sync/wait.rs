// SPDX-License-Identifier: GPL-2.0

//! Wait queue.
//!
//! C header: [`include/linux/wait.h`](srctree/include/linux/wait.h)

use super::LockClassKey;
use crate::{
    prelude::*,
    str::CStr,
    task::{
        self,
        TASK_INTERRUPTIBLE,
        TASK_NORMAL,
        TASK_UNINTERRUPTIBLE, //
    },
    time::Jiffies,
    types::Opaque,
};

use core::{
    pin::Pin,
    ptr, //
};

/// Creates a [`WaitQueue`] initialiser with the given name and a newly-created lock class.
#[macro_export]
macro_rules! new_waitqueue {
    ($($name:literal)?) => {
        $crate::sync::WaitQueue::new(
            $crate::optional_name!($($name)?),
            $crate::static_lock_class!(),
        )
    };
}
pub use new_waitqueue;

/// Exposes the kernel's [`struct wait_queue_head`] as a Rust wait queue.
///
/// A `WaitQueue` allows a thread to sleep until a caller-supplied condition becomes true,
/// re-checking the condition on each wake-up. This matches the C `wait_event()` family of macros.
///
/// For waiting with a lock guard (the condition variable pattern), use [`CondVar`](super::CondVar)
/// instead.
///
/// Instances of `WaitQueue` need a lock class and to be pinned. The recommended way to create such
/// instances is with the [`pin_init!`] and [`new_waitqueue!`] macros.
///
/// # Examples
///
/// ```
/// use kernel::sync::{
///     atomic::{
///         Atomic,
///         Relaxed,
///     },
///     new_waitqueue,
///     WaitQueue,
/// };
///
/// #[pin_data]
/// pub struct Example {
///     value: Atomic<i32>,
///     #[pin]
///     queue: WaitQueue,
/// }
///
/// fn wait_for_value(e: &Example, v: i32) {
///     e.queue.wait_event(|| e.value.load(Relaxed) == v);
/// }
///
/// fn set_value(e: &Example, v: i32) {
///     e.value.store(v, Relaxed);
///     e.queue.wake_up();
/// }
/// ```
///
/// [`struct wait_queue_head`]: srctree/include/linux/wait.h
#[pin_data]
pub struct WaitQueue {
    #[pin]
    wait_queue_head: Opaque<bindings::wait_queue_head>,
}

// SAFETY: `WaitQueue` only uses a `struct wait_queue_head`, which is safe to use on any thread.
unsafe impl Send for WaitQueue {}

// SAFETY: `WaitQueue` only uses a `struct wait_queue_head`, which is safe to use on multiple
// threads concurrently.
unsafe impl Sync for WaitQueue {}

impl WaitQueue {
    /// Constructs a new wait queue initialiser.
    pub fn new(name: &'static CStr, key: Pin<&'static LockClassKey>) -> impl PinInit<Self> {
        pin_init!(Self {
            // SAFETY: `slot` is valid while the closure is called and both `name` and `key` have
            // static lifetimes so they live indefinitely.
            wait_queue_head <- Opaque::ffi_init(|slot| unsafe {
                bindings::__init_waitqueue_head(slot, name.as_char_ptr(), key.as_ptr())
            }),
        })
    }

    /// Returns a raw pointer to the underlying `wait_queue_head`.
    #[inline]
    pub(super) fn as_raw(&self) -> *mut bindings::wait_queue_head {
        self.wait_queue_head.get()
    }

    /// Sleeps until the condition returns `true`.
    ///
    /// The condition is checked before each sleep and after each wake-up. The wait is
    /// uninterruptible.
    #[inline]
    pub fn wait_event<F: Fn() -> bool>(&self, condition: F) {
        self.wait_event_timeout_internal(TASK_UNINTERRUPTIBLE, &condition, Jiffies::MAX);
    }

    /// Sleeps until the condition returns `true` or a signal is received.
    ///
    /// Returns `Ok(())` when the condition is met, or `Err(WaitError::Signal)` if interrupted
    /// by a signal.
    #[inline]
    pub fn wait_event_interruptible<F: Fn() -> bool>(&self, condition: F) -> Result<(), WaitError> {
        self.wait_event_timeout_internal(TASK_INTERRUPTIBLE, &condition, Jiffies::MAX);
        if !condition() && current!().signal_pending() {
            Err(WaitError::Signal)
        } else {
            Ok(())
        }
    }

    /// Sleeps until the condition returns `true` or the timeout expires.
    ///
    /// Returns `Ok(())` when the condition is met, or `Err(WaitError::Timeout)` if the timeout
    /// elapsed first.
    #[inline]
    pub fn wait_event_timeout<F: Fn() -> bool>(
        &self,
        condition: F,
        jiffies: Jiffies,
    ) -> Result<(), WaitError> {
        let remaining = self.wait_event_timeout_internal(TASK_UNINTERRUPTIBLE, &condition, jiffies);
        if remaining == 0 && !condition() {
            Err(WaitError::Timeout)
        } else {
            Ok(())
        }
    }

    /// Sleeps until the condition returns `true`, a signal is received, or the timeout expires.
    ///
    /// Returns `Ok(())` when the condition is met, or `Err(WaitError)` on signal or timeout.
    #[inline]
    pub fn wait_event_interruptible_timeout<F: Fn() -> bool>(
        &self,
        condition: F,
        jiffies: Jiffies,
    ) -> Result<(), WaitError> {
        let remaining = self.wait_event_timeout_internal(TASK_INTERRUPTIBLE, &condition, jiffies);
        if condition() {
            Ok(())
        } else if current!().signal_pending() {
            Err(WaitError::Signal)
        } else if remaining == 0 {
            Err(WaitError::Timeout)
        } else {
            Ok(())
        }
    }

    fn wait_event_timeout_internal(
        &self,
        wait_state: c_int,
        condition: &dyn Fn() -> bool,
        jiffies: Jiffies,
    ) -> Jiffies {
        let wait = Opaque::<bindings::wait_queue_entry>::uninit();

        // SAFETY: `wait` points to valid memory.
        unsafe { bindings::init_wait(wait.get()) };

        let mut remaining = jiffies;

        loop {
            // SAFETY: Both `wait` and `wait_queue_head` point to valid memory, and `wait` was
            // initialised by `init_wait()` above.
            let ret = unsafe {
                bindings::prepare_to_wait_event(self.wait_queue_head.get(), wait.get(), wait_state)
            };

            if condition() {
                break;
            }

            if ret != 0 || remaining == 0 {
                break;
            }

            remaining = task::schedule_timeout(remaining);

            if condition() {
                break;
            }
        }

        // SAFETY: Both `wait` and `wait_queue_head` point to valid memory.
        unsafe { bindings::finish_wait(self.wait_queue_head.get(), wait.get()) };

        remaining
    }

    /// Performs a single exclusive prepare-to-wait / finish-wait cycle, calling `schedule_fn`
    /// in between.
    pub(super) fn wait_once_exclusive<F, R>(&self, wait_state: c_int, schedule_fn: F) -> R
    where
        F: FnOnce() -> R,
    {
        let wait = Opaque::<bindings::wait_queue_entry>::uninit();

        // SAFETY: `wait` points to valid memory.
        unsafe { bindings::init_wait(wait.get()) };

        // SAFETY: Both `wait` and `wait_queue_head` point to valid memory.
        unsafe {
            bindings::prepare_to_wait_exclusive(self.wait_queue_head.get(), wait.get(), wait_state)
        };

        let ret = schedule_fn();

        // SAFETY: Both `wait` and `wait_queue_head` point to valid memory.
        unsafe { bindings::finish_wait(self.wait_queue_head.get(), wait.get()) };

        ret
    }

    /// Wakes up waiters.
    ///
    /// Wakes all non-exclusive waiters and one exclusive waiter, if any.  Matches C's `wake_up()`.
    #[inline]
    pub fn wake_up(&self) {
        // SAFETY: `wait_queue_head` points to valid memory.
        unsafe { bindings::__wake_up(self.wait_queue_head.get(), TASK_NORMAL, 1, ptr::null_mut()) };
    }

    /// Wakes up all waiters.
    ///
    /// Wakes all non-exclusive and all exclusive waiters, if any.
    /// Matches C's `wake_up_all()`.
    #[inline]
    pub fn wake_up_all(&self) {
        // SAFETY: `wait_queue_head` points to valid memory.
        unsafe { bindings::__wake_up(self.wait_queue_head.get(), TASK_NORMAL, 0, ptr::null_mut()) };
    }

    /// Like [`wake_up()`](Self::wake_up), but hints to the scheduler that the current task is
    /// about to sleep, so the woken task should be scheduled on the same CPU to avoid unnecessary
    /// migration. Matches C's `wake_up_sync()`.
    #[inline]
    pub fn wake_up_sync(&self) {
        // SAFETY: `wait_queue_head` points to valid memory.
        unsafe { bindings::__wake_up_sync(self.wait_queue_head.get(), TASK_NORMAL) };
    }

    /// Wakes up all waiters and clears poll registrations.
    ///
    /// Used when a wait queue is about to be freed, to ensure epoll items are properly removed.
    /// Matches C's `wake_up_pollfree()`.
    #[inline]
    pub(super) fn wake_up_pollfree(&self) {
        // SAFETY: `wait_queue_head` points to valid memory.
        unsafe { bindings::__wake_up_pollfree(self.wait_queue_head.get()) };
    }
}

/// Error returned by [`WaitQueue`] wait functions.
#[derive(Debug, PartialEq)]
pub enum WaitError {
    /// Interrupted by a signal.
    Signal,
    /// The timeout elapsed without the condition being met.
    Timeout,
}

impl From<WaitError> for Error {
    #[inline]
    fn from(e: WaitError) -> Error {
        match e {
            WaitError::Signal => ERESTARTSYS,
            WaitError::Timeout => ETIMEDOUT,
        }
    }
}

#[macros::kunit_tests(rust_waitqueue)]
mod tests {
    use super::*;
    use crate::{
        sync::{
            atomic::{
                Atomic,
                Relaxed, //
            },
            Arc,
        },
        time::{
            delay::fsleep,
            Delta, //
        },
        workqueue,
    };

    #[pin_data]
    struct State {
        value: Atomic<i32>,
        #[pin]
        wq: WaitQueue,
    }

    impl State {
        fn new() -> Result<Arc<Self>> {
            Arc::pin_init(
                pin_init!(Self {
                    value: Atomic::new(0),
                    wq <- new_waitqueue!(),
                }),
                GFP_KERNEL,
            )
        }
    }

    #[test]
    fn wait_event_from_work() {
        let s = State::new().unwrap();

        let s2 = s.clone();
        workqueue::system_dfl()
            .try_spawn(GFP_KERNEL, move || {
                s2.value.store(1, Relaxed);
                s2.wq.wake_up();
            })
            .unwrap();

        s.wq.wait_event(|| s.value.load(Relaxed) == 1);
        assert_eq!(s.value.load(Relaxed), 1);
    }

    #[test]
    fn wait_event_condition_already_true() {
        let s = State::new().unwrap();
        s.value.store(1, Relaxed);

        s.wq.wait_event(|| s.value.load(Relaxed) == 1);
        assert_eq!(s.value.load(Relaxed), 1);
    }

    #[test]
    fn wait_event_condition_not_yet_met() {
        let s = State::new().unwrap();

        let s2 = s.clone();
        workqueue::system_dfl()
            .try_spawn(GFP_KERNEL, move || {
                s2.value.store(1, Relaxed);
                s2.wq.wake_up_all();

                fsleep(Delta::from_millis(50));

                s2.value.store(2, Relaxed);
                s2.wq.wake_up_all();
            })
            .unwrap();

        s.wq.wait_event(|| s.value.load(Relaxed) == 2);
        assert_eq!(s.value.load(Relaxed), 2);
    }

    #[test]
    fn wait_event_timeout_expires() {
        let s = State::new().unwrap();

        let ret = s.wq.wait_event_timeout(|| s.value.load(Relaxed) == 1, 1);
        assert_eq!(ret, Err(WaitError::Timeout));
    }
}
