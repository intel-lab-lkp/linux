// SPDX-License-Identifier: GPL-2.0

//! A high-level [`WwMutex`] execution helper.
//!
//! Provides a retrying lock mechanism on top of [`WwMutex`] and [`WwAcquireCtx`].
//! It detects [`EDEADLK`] and handles it by rolling back and retrying the
//! user-supplied locking algorithm until success.

use crate::prelude::*;
use crate::sync::lock::ww_mutex::{WwAcquireCtx, WwClass, WwMutex, WwMutexGuard};
use core::ptr;

/// High-level execution type for ww_mutex.
///
/// Tracks a series of locks acquired under a common [`WwAcquireCtx`].
/// It ensures proper cleanup and retry mechanism on deadlocks and provides
/// type-safe access to locked data via [`with_locked`].
///
/// Typical usage is through [`lock_all`], which retries a user-supplied
/// locking algorithm until it succeeds without deadlock.
pub struct ExecContext<'a> {
    class: &'a WwClass,
    acquire: Pin<KBox<WwAcquireCtx<'a>>>,
    taken: KVec<WwMutexGuard<'a, ()>>,
}

impl<'a> Drop for ExecContext<'a> {
    fn drop(&mut self) {
        self.release_all_locks();
    }
}

impl<'a> ExecContext<'a> {
    /// Creates a new [`ExecContext`] for the given lock class.
    ///
    /// All locks taken through this context must belong to the same class.
    ///
    /// TODO: Add some safety mechanism to ensure classes are not different.
    pub fn new(class: &'a WwClass) -> Result<Self> {
        Ok(Self {
            class,
            acquire: KBox::pin_init(WwAcquireCtx::new(class), GFP_KERNEL)?,
            taken: KVec::new(),
        })
    }

    /// Attempts to lock a [`WwMutex`] and records the guard.
    ///
    /// Returns [`EDEADLK`] if lock ordering would cause a deadlock.
    pub fn lock<T>(&mut self, mutex: &'a WwMutex<'a, T>) -> Result<()> {
        let guard = self.acquire.lock(mutex)?;
        // SAFETY: Type is erased for storage. Actual access uses `with_locked`
        // which safely casts back.
        let erased: WwMutexGuard<'a, ()> = unsafe { core::mem::transmute(guard) };
        self.taken.push(erased, GFP_KERNEL)?;

        Ok(())
    }

    /// Runs `locking_algorithm` until success with retrying on deadlock.
    ///
    /// `locking_algorithm` should attempt to acquire all needed locks.
    /// If [`EDEADLK`] is detected, this function will roll back, reset
    /// the context and retry automatically.
    ///
    /// Once all locks are acquired successfully, `on_all_locks_taken` is
    /// invoked for exclusive access to the locked values. Afterwards, all
    /// locks are released.
    ///
    /// # Example
    ///
    /// ```
    /// use kernel::alloc::KBox;
    /// use kernel::c_str;
    /// use kernel::prelude::*;
    /// use kernel::sync::Arc;
    /// use kernel::sync::lock::ww_mutex;
    /// use pin_init::stack_pin_init;
    ///
    /// stack_pin_init!(let class = ww_mutex::WwClass::new_wound_wait(c_str!("lock_all_example")));
    ///
    /// let mutex1 = Arc::pin_init(ww_mutex::WwMutex::new(0, &class), GFP_KERNEL)?;
    /// let mutex2 = Arc::pin_init(ww_mutex::WwMutex::new(0, &class), GFP_KERNEL)?;
    /// let mut ctx = KBox::pin_init(ww_mutex::exec::ExecContext::new(&class)?, GFP_KERNEL)?;
    ///
    /// ctx.lock_all(
    ///     |ctx| {
    ///         // Try to lock both mutexes.
    ///         ctx.lock(&mutex1)?;
    ///         ctx.lock(&mutex2)?;
    ///
    ///         Ok(())
    ///     },
    ///     |ctx| {
    ///         // Safely mutate both values while holding the locks.
    ///         ctx.with_locked(&mutex1, |v| *v += 1)?;
    ///         ctx.with_locked(&mutex2, |v| *v += 1)?;
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
        T: FnMut(&mut ExecContext<'a>) -> Result<()>,
        Y: FnMut(&mut ExecContext<'a>) -> Result<Z>,
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
                    // Deadlock detected, retry from scratch.
                    self.cleanup_on_deadlock()?;
                    continue;
                }
                Err(e) => {
                    return Err(e);
                }
            }
        }
    }

    /// Executes `f` with a mutable reference to the data behind `mutex`.
    ///
    /// Fails with [`EINVAL`] if the mutex was not locked in this context.
    pub fn with_locked<T, Y>(
        &mut self,
        mutex: &'a WwMutex<'a, T>,
        f: impl FnOnce(&mut T) -> Y,
    ) -> Result<Y> {
        // Find the matching guard.
        for guard in &mut self.taken {
            if mutex.as_ptr() == guard.mutex.as_ptr() {
                // SAFETY: We know this guard belongs to `mutex` and holds the lock.
                let typed = unsafe { &mut *ptr::from_mut(guard).cast::<WwMutexGuard<'a, T>>() };
                return Ok(f(&mut **typed));
            }
        }

        // `mutex` isn't locked in this `ExecContext`.
        Err(EINVAL)
    }

    /// Releases all currently held locks in this context.
    ///
    /// It is intended to be used for internal implementation only.
    fn release_all_locks(&mut self) {
        self.taken.clear();
    }

    /// Resets this context after a deadlock detection.
    ///
    /// Drops all held locks and reinitializes the [`WwAcquireCtx`].
    ///
    /// It is intended to be used for internal implementation only.
    fn cleanup_on_deadlock(&mut self) -> Result {
        self.release_all_locks();
        // Re-init fresh `WwAcquireCtx`.
        self.acquire = KBox::pin_init(WwAcquireCtx::new(self.class), GFP_KERNEL)?;

        Ok(())
    }
}

#[kunit_tests(rust_kernel_ww_exec)]
mod tests {
    use crate::c_str;
    use crate::prelude::*;
    use crate::sync::Arc;
    use pin_init::stack_pin_init;

    use super::*;

    #[test]
    fn test_exec_context_basic_lock_unlock() -> Result {
        stack_pin_init!(let class = WwClass::new_wound_wait(c_str!("exec_ctx_basic")));

        let mutex = Arc::pin_init(WwMutex::new(10, &class), GFP_KERNEL)?;
        let mut ctx = KBox::pin_init(ExecContext::new(&class)?, GFP_KERNEL)?;

        ctx.lock(&mutex)?;
        ctx.with_locked(&mutex, |v| {
            assert_eq!(*v, 10);
        })?;

        ctx.release_all_locks();
        assert!(!mutex.is_locked());

        Ok(())
    }

    #[test]
    fn test_exec_context_with_locked_mutates_data() -> Result {
        stack_pin_init!(let class = WwClass::new_wound_wait(c_str!("exec_ctx_with_locked")));

        let mutex = Arc::pin_init(WwMutex::new(5, &class), GFP_KERNEL)?;
        let mut ctx = KBox::pin_init(ExecContext::new(&class)?, GFP_KERNEL)?;

        ctx.lock(&mutex)?;

        ctx.with_locked(&mutex, |v| {
            assert_eq!(*v, 5);
            // Increment the value.
            *v += 7;
        })?;

        ctx.with_locked(&mutex, |v| {
            // Check that mutation took effect.
            assert_eq!(*v, 12);
        })?;

        Ok(())
    }

    #[test]
    fn test_lock_all_success() -> Result {
        stack_pin_init!(let class = WwClass::new_wound_wait(c_str!("lock_all_ok")));

        let mutex1 = Arc::pin_init(WwMutex::new(1, &class), GFP_KERNEL)?;
        let mutex2 = Arc::pin_init(WwMutex::new(2, &class), GFP_KERNEL)?;
        let mut ctx = KBox::pin_init(ExecContext::new(&class)?, GFP_KERNEL)?;

        let res = ctx.lock_all(
            |ctx| {
                let _ = ctx.lock(&mutex1)?;
                let _ = ctx.lock(&mutex2)?;
                Ok(())
            },
            |ctx| {
                ctx.with_locked(&mutex1, |v| *v += 10)?;
                ctx.with_locked(&mutex2, |v| *v += 20)?;
                Ok((
                    ctx.with_locked(&mutex1, |v| *v)?,
                    ctx.with_locked(&mutex2, |v| *v)?,
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
        stack_pin_init!(let class = WwClass::new_wound_wait(c_str!("lock_all_ok")));

        let mutex1 = Arc::pin_init(WwMutex::new(1, &class), GFP_KERNEL)?;
        let mutex2 = Arc::pin_init(WwMutex::new("hello", &class), GFP_KERNEL)?;
        let mut ctx = KBox::pin_init(ExecContext::new(&class)?, GFP_KERNEL)?;

        ctx.lock_all(
            |ctx| {
                ctx.lock(&mutex1)?;
                ctx.lock(&mutex2)?;
                Ok(())
            },
            |ctx| {
                ctx.with_locked(&mutex1, |v| assert_eq!(*v, 1))?;
                ctx.with_locked(&mutex2, |v| assert_eq!(*v, "hello"))?;
                Ok(())
            },
        )?;

        Ok(())
    }

    #[test]
    fn test_lock_all_retries_on_deadlock() -> Result {
        stack_pin_init!(let class = WwClass::new_wound_wait(c_str!("lock_all_retry")));

        let mutex = Arc::pin_init(WwMutex::new(99, &class), GFP_KERNEL)?;
        let mut ctx = KBox::pin_init(ExecContext::new(&class)?, GFP_KERNEL)?;
        let mut first_try = true;

        let res = ctx.lock_all(
            |ctx| {
                if first_try {
                    first_try = false;
                    // Simulate deadlock on first attempt.
                    return Err(EDEADLK);
                }
                ctx.lock(&mutex)
            },
            |ctx| {
                ctx.with_locked(&mutex, |v| {
                    *v += 1;
                    *v
                })
            },
        )?;

        assert_eq!(res, 100);
        Ok(())
    }

    #[test]
    fn test_with_locked_on_unlocked_mutex() -> Result {
        stack_pin_init!(let class = WwClass::new_wound_wait(c_str!("with_unlocked_mutex")));

        let mutex = Arc::pin_init(WwMutex::new(5, &class), GFP_KERNEL)?;
        let mut ctx = KBox::pin_init(ExecContext::new(&class)?, GFP_KERNEL)?;

        let ecode = ctx.with_locked(&mutex, |_v| {}).unwrap_err();
        assert_eq!(EINVAL, ecode);

        Ok(())
    }
}
