// SPDX-License-Identifier: GPL-2.0

//! Provides [`LockSet`] which automatically detects [`EDEADLK`],
//! releases all locks, waits for the contended mutex and retries the user
//! supplied locking algorithm with the same acquire context.

use super::{
    AcquireCtx,
    Class,
    Mutex, //
};
use crate::{
    bindings,
    prelude::*,
    types::NotThreadSafe, //
};
use core::ptr::NonNull;

/// A tracked set of [`Mutex`] locks acquired under the same [`Class`].
///
/// It ensures proper cleanup and retry mechanism on deadlocks and provides
/// safe access to locked data via [`LockSet::with_locked`].
///
/// Typical usage is through [`LockSet::lock_all`], which retries a
/// user supplied locking algorithm until it succeeds without deadlock.
pub struct LockSet<'a> {
    acquire_ctx: Pin<KBox<AcquireCtx<'a>>>,
    taken: KVec<RawGuard>,
    // Set by `lock()` on `EDEADLK`; the mutex remains valid for `'a`.
    contended: Option<NonNull<bindings::ww_mutex>>,
}

/// Used by [`LockSet`] to track acquired locks.
///
/// This type is strictly crate-private and must never be exposed
/// outside this crate.
struct RawGuard {
    mutex_ptr: NonNull<bindings::ww_mutex>,
    _not_send: NotThreadSafe,
}

impl Drop for RawGuard {
    fn drop(&mut self) {
        // SAFETY: `mutex_ptr` originates from a locked `Mutex` and remains
        // valid for the lifetime of this guard, so unlocking here is sound.
        unsafe { bindings::ww_mutex_unlock(self.mutex_ptr.as_ptr()) };
    }
}

impl<'a> Drop for LockSet<'a> {
    fn drop(&mut self) {
        self.release_all_locks();
    }
}

impl<'a> LockSet<'a> {
    /// Creates a new [`LockSet`] with the given [`Class`].
    ///
    /// All locks taken through this [`LockSet`] must belong to the
    /// same [`Class`].
    pub fn new(class: &'a Class) -> Result<Self> {
        Ok(Self {
            acquire_ctx: KBox::pin_init(AcquireCtx::new(class), GFP_KERNEL)?,
            taken: KVec::new(),
            contended: None,
        })
    }

    /// Creates a new [`LockSet`] using an existing [`AcquireCtx`].
    ///
    /// # Safety
    ///
    /// The caller must ensure that `acquire_ctx` is properly initialized
    /// and holds no [`Mutex`]es.
    pub unsafe fn new_with_acquire_ctx(acquire_ctx: Pin<KBox<AcquireCtx<'a>>>) -> Self {
        Self {
            acquire_ctx,
            taken: KVec::new(),
            contended: None,
        }
    }

    /// Attempts to lock the given [`Mutex`] and stores a guard for it.
    ///
    /// Returns success if the mutex is already held by this set.
    pub fn lock<T>(&mut self, mutex: &'a Mutex<'a, T>) -> Result {
        // Preserve the first contended mutex until deadlock recovery has completed.
        if self.contended.is_some() {
            return Err(EDEADLK);
        }

        // SAFETY: All tracked locks are released before the context is dropped.
        // Forgetting the set leaks the context.
        let guard = match unsafe { self.acquire_ctx.lock(mutex) } {
            Ok(guard) => guard,
            Err(e) if e == EDEADLK => {
                self.contended = NonNull::new(mutex.inner.get());
                return Err(e);
            }
            // The mutex acquired during backoff is encountered again on retry.
            Err(e) if e == EALREADY => return Ok(()),
            Err(e) => return Err(e),
        };

        let raw_guard = RawGuard {
            // SAFETY: We just locked it above so it's a valid pointer.
            mutex_ptr: unsafe { NonNull::new_unchecked(guard.mutex.inner.get()) },
            _not_send: NotThreadSafe,
        };

        // Transfer unlocking to `raw_guard` before the fallible push, so failure
        // drops only one guard and unlocks the mutex exactly once.
        core::mem::forget(guard);
        self.taken.push(raw_guard, GFP_KERNEL)?;

        Ok(())
    }

    /// Runs `locking_algorithm` until success with retrying on deadlock.
    ///
    /// `locking_algorithm` must acquire all needed locks and immediately propagate
    /// any error from [`Self::lock`] unchanged (e.g. using `?`).
    /// If [`Self::lock`] returns [`EDEADLK`], this function releases all held locks
    /// and waits to acquire the contended mutex. It then retries with that mutex
    /// held, preserving the acquire context's original ticket.
    ///
    /// Once all locks are acquired successfully, `on_all_locks_taken` is
    /// invoked for exclusive access to the locked values. It must not acquire
    /// additional locks through this set. Afterwards, all locks are released.
    ///
    /// # Example
    ///
    /// ```
    /// use kernel::{
    ///     alloc::KBox,
    ///     define_ww_class,
    ///     prelude::*,
    ///     sync::{
    ///         lock::ww_mutex::{
    ///             LockSet,
    ///             Mutex, //
    ///         },
    ///         Arc,
    ///     },
    /// };
    /// use pin_init::stack_pin_init;
    ///
    /// define_ww_class!(SOME_WOUND_WAIT_CLASS);
    ///
    /// let mutex1 = Arc::pin_init(Mutex::new(0, &SOME_WOUND_WAIT_CLASS), GFP_KERNEL)?;
    /// let mutex2 = Arc::pin_init(Mutex::new(0, &SOME_WOUND_WAIT_CLASS), GFP_KERNEL)?;
    /// let mut lock_set = KBox::pin_init(LockSet::new(&SOME_WOUND_WAIT_CLASS)?, GFP_KERNEL)?;
    ///
    /// lock_set.lock_all(
    ///     // `locking_algorithm` closure
    ///     |lock_set| {
    ///         lock_set.lock(&mutex1)?;
    ///         lock_set.lock(&mutex2)?;
    ///
    ///         Ok(())
    ///     },
    ///     // `on_all_locks_taken` closure
    ///     |lock_set| {
    ///         // Safely mutate both values while holding the locks.
    ///         lock_set.with_locked(&mutex1, |v| *v += 1)?;
    ///         lock_set.with_locked(&mutex2, |v| *v += 1)?;
    ///
    ///         Ok(())
    ///     },
    /// )?;
    ///
    /// # Ok::<(), Error>(())
    /// ```
    pub fn lock_all<T, Y, Z>(
        &mut self,
        mut locking_algorithm: T,
        mut on_all_locks_taken: Y,
    ) -> Result<Z>
    where
        T: FnMut(&mut LockSet<'a>) -> Result,
        Y: FnMut(&mut LockSet<'a>) -> Result<Z>,
    {
        loop {
            match locking_algorithm(self) {
                Ok(()) => {
                    // All locks in `locking_algorithm` succeeded.
                    // The user can now safely use them in `on_all_locks_taken`.
                    let res = on_all_locks_taken(self);
                    self.release_all_locks();

                    return res;
                }
                Err(e) if e == EDEADLK => {
                    self.cleanup_on_deadlock()?;
                    continue;
                }
                Err(e) => {
                    self.release_all_locks();
                    return Err(e);
                }
            }
        }
    }

    /// Executes `access` with a mutable reference to the data behind [`Mutex`].
    ///
    /// Fails with [`EINVAL`] if the [`Mutex`] was not locked in this [`LockSet`].
    pub fn with_locked<T: Unpin, Y>(
        &mut self,
        mutex: &'a Mutex<'a, T>,
        access: impl for<'b> FnOnce(&'b mut T) -> Y,
    ) -> Result<Y> {
        let mutex_ptr = mutex.inner.get();

        if self
            .taken
            .iter()
            .any(|guard| guard.mutex_ptr.as_ptr() == mutex_ptr)
        {
            // SAFETY: We hold the lock corresponding to `mutex`, so we have
            // exclusive access to its protected data.
            let value = unsafe { &mut *mutex.data.get() };
            Ok(access(value))
        } else {
            // `mutex` isn't locked in this `LockSet`.
            Err(EINVAL)
        }
    }

    /// Releases all currently held locks in this [`LockSet`].
    fn release_all_locks(&mut self) {
        // `Drop` implementation of the `RawGuard` takes care of the unlocking.
        self.taken.clear();
    }

    /// Releases all locks and acquires the contended mutex with the same context.
    fn cleanup_on_deadlock(&mut self) -> Result {
        self.release_all_locks();

        // A callback may return `EDEADLK` without a failed lock acquisition.
        let contended = self.contended.take().ok_or(EDEADLK)?;

        // SAFETY: `lock()` recorded this pointer from a mutex borrowed for `'a`,
        // so both the mutex and its class remain valid for that lifetime.
        let mutex = unsafe { Mutex::from_raw(contended.as_ptr()) };

        // With no locks held, regular locking waits for this mutex to become available.
        self.lock(mutex)
    }
}

#[kunit_tests(rust_kernel_lock_set)]
mod tests {
    use super::*;
    use crate::{
        define_wd_class,
        define_ww_class,
        sync::Arc, //
    };

    define_ww_class!(TEST_WOUND_WAIT_CLASS);
    define_wd_class!(TEST_WAIT_DIE_CLASS);

    #[test]
    fn test_lock_set_basic_lock_unlock() -> Result {
        let mutex = Arc::pin_init(Mutex::new(10, &TEST_WOUND_WAIT_CLASS), GFP_KERNEL)?;
        let mut lock_set = KBox::pin_init(LockSet::new(&TEST_WOUND_WAIT_CLASS)?, GFP_KERNEL)?;

        lock_set.lock(&mutex)?;
        lock_set.lock(&mutex)?;
        assert_eq!(lock_set.taken.len(), 1);

        lock_set.with_locked(&mutex, |v| {
            assert_eq!(*v, 10);
        })?;

        lock_set.release_all_locks();
        assert!(!mutex.is_locked());

        Ok(())
    }

    #[test]
    fn test_lock_set_with_locked_mutates_data() -> Result {
        let mutex = Arc::pin_init(Mutex::new(5, &TEST_WOUND_WAIT_CLASS), GFP_KERNEL)?;
        let mut lock_set = KBox::pin_init(LockSet::new(&TEST_WOUND_WAIT_CLASS)?, GFP_KERNEL)?;

        lock_set.lock(&mutex)?;

        lock_set.with_locked(&mutex, |v| {
            assert_eq!(*v, 5);
            // Increment the value.
            *v += 7;
        })?;

        lock_set.with_locked(&mutex, |v| {
            // Check that mutation took effect.
            assert_eq!(*v, 12);
        })?;

        Ok(())
    }

    #[test]
    fn test_lock_all_success() -> Result {
        let mutex1 = Arc::pin_init(Mutex::new(1, &TEST_WOUND_WAIT_CLASS), GFP_KERNEL)?;
        let mutex2 = Arc::pin_init(Mutex::new(2, &TEST_WOUND_WAIT_CLASS), GFP_KERNEL)?;
        let mut lock_set = KBox::pin_init(LockSet::new(&TEST_WOUND_WAIT_CLASS)?, GFP_KERNEL)?;

        let res = lock_set.lock_all(
            // `locking_algorithm` closure
            |lock_set| {
                lock_set.lock(&mutex1)?;
                lock_set.lock(&mutex2)?;
                Ok(())
            },
            // `on_all_locks_taken` closure
            |lock_set| {
                lock_set.with_locked(&mutex1, |v| *v += 10)?;
                lock_set.with_locked(&mutex2, |v| *v += 20)?;
                Ok((
                    lock_set.with_locked(&mutex1, |v| *v)?,
                    lock_set.with_locked(&mutex2, |v| *v)?,
                ))
            },
        )?;

        assert_eq!(res, (11, 22));
        assert!(!mutex1.is_locked());
        assert!(!mutex2.is_locked());

        Ok(())
    }

    #[test]
    fn test_with_different_input_type() -> Result {
        let mutex1 = Arc::pin_init(Mutex::new(1, &TEST_WOUND_WAIT_CLASS), GFP_KERNEL)?;
        let mutex2 = Arc::pin_init(Mutex::new("hello", &TEST_WOUND_WAIT_CLASS), GFP_KERNEL)?;
        let mut lock_set = KBox::pin_init(LockSet::new(&TEST_WOUND_WAIT_CLASS)?, GFP_KERNEL)?;

        lock_set.lock_all(
            // `locking_algorithm` closure
            |lock_set| {
                lock_set.lock(&mutex1)?;
                lock_set.lock(&mutex2)?;

                Ok(())
            },
            // `on_all_locks_taken` closure
            |lock_set| {
                lock_set.with_locked(&mutex1, |v| assert_eq!(*v, 1))?;
                lock_set.with_locked(&mutex2, |v| assert_eq!(*v, "hello"))?;
                Ok(())
            },
        )?;

        Ok(())
    }

    #[test]
    fn test_lock_all_retries_on_deadlock() -> Result {
        let first = Arc::pin_init(Mutex::new(1, &TEST_WOUND_WAIT_CLASS), GFP_KERNEL)?;
        let mutex = Arc::pin_init(Mutex::new(99, &TEST_WOUND_WAIT_CLASS), GFP_KERNEL)?;
        let mut lock_set = KBox::pin_init(LockSet::new(&TEST_WOUND_WAIT_CLASS)?, GFP_KERNEL)?;
        let mut first_try = true;
        let ctx = lock_set.acquire_ctx.inner.get();
        // SAFETY: The set owns this initialized context throughout the test.
        let stamp = unsafe { (*ctx).stamp };

        let res = lock_set.lock_all(
            // `locking_algorithm` closure
            |lock_set| {
                if first_try {
                    first_try = false;
                    lock_set.lock(&first)?;
                    // Simulate a failed acquisition with its contended mutex recorded.
                    lock_set.contended = NonNull::new(mutex.inner.get());
                    return Err(EDEADLK);
                }

                assert!(!first.is_locked());
                // Backoff must acquire the contended mutex before retrying this callback.
                lock_set.with_locked(&mutex, |v| assert_eq!(*v, 99))?;
                lock_set.lock(&mutex)?;
                assert_eq!(lock_set.taken.len(), 1);
                Ok(())
            },
            // `on_all_locks_taken` closure
            |lock_set| {
                lock_set.with_locked(&mutex, |v| {
                    *v += 1;
                    *v
                })
            },
        )?;

        assert_eq!(res, 100);
        assert!(!first.is_locked());
        assert!(!mutex.is_locked());
        // SAFETY: The context is still owned by the set and remains initialized.
        assert_eq!(unsafe { (*ctx).stamp }, stamp);
        Ok(())
    }

    #[test]
    fn test_lock_all_deadlock_without_contended_mutex() -> Result {
        let mutex = Arc::pin_init(Mutex::new(1, &TEST_WOUND_WAIT_CLASS), GFP_KERNEL)?;
        let mut lock_set = KBox::pin_init(LockSet::new(&TEST_WOUND_WAIT_CLASS)?, GFP_KERNEL)?;
        let mut attempts = 0;

        let res = lock_set.lock_all(
            |lock_set| {
                attempts += 1;
                lock_set.lock(&mutex)?;
                // Return a different error on an unexpected retry to avoid an endless loop.
                Err(if attempts == 1 { EDEADLK } else { EINVAL })
            },
            |_| Ok(()),
        );

        assert_eq!(res, Err(EDEADLK));
        assert_eq!(attempts, 1);
        assert!(!mutex.is_locked());
        Ok(())
    }

    #[test]
    fn test_with_locked_on_unlocked_mutex() -> Result {
        let mutex = Arc::pin_init(Mutex::new(5, &TEST_WOUND_WAIT_CLASS), GFP_KERNEL)?;
        let mut lock_set = KBox::pin_init(LockSet::new(&TEST_WOUND_WAIT_CLASS)?, GFP_KERNEL)?;

        let ecode = lock_set.with_locked(&mutex, |_v| {}).unwrap_err();
        assert_eq!(EINVAL, ecode);

        Ok(())
    }

    #[test]
    fn test_with_different_classes() -> Result {
        let mutex = Arc::pin_init(Mutex::new(5, &TEST_WOUND_WAIT_CLASS), GFP_KERNEL)?;
        let mut lock_set = KBox::pin_init(LockSet::new(&TEST_WAIT_DIE_CLASS)?, GFP_KERNEL)?;

        let ecode = lock_set.lock(&mutex).unwrap_err();
        assert_eq!(EINVAL, ecode);

        Ok(())
    }
}
