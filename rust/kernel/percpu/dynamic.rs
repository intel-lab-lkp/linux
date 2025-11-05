// SPDX-License-Identifier: GPL-2.0
//! Dynamically allocated per-CPU variables.

use super::*;

use crate::alloc::Flags;
use crate::bindings::{alloc_percpu, free_percpu};
use crate::cpumask::Cpumask;
use crate::prelude::*;
use crate::sync::Arc;
use core::mem::{align_of, size_of, MaybeUninit};

/// Represents a dynamic allocation of a per-CPU variable via `alloc_percpu`. Calls `free_percpu`
/// when dropped.
///
/// # Contents
/// Note that the allocated memory need not be initialized, and this type does not track when/if
/// the memory location on any particular CPU has been initialized. This means that it cannot tell
/// whether it should drop the *contents* of the allocation when it is dropped. It is up to the
/// user to do this via something like [`core::ptr::drop_in_place`].
pub struct PerCpuAllocation<T>(pub(super) PerCpuPtr<T>);

impl<T: Zeroable> PerCpuAllocation<T> {
    /// Dynamically allocates a space in the per-CPU area suitably sized and aligned to hold a `T`,
    /// initially filled with the zero value for `T`.
    ///
    /// Returns [`None`] under the same circumstances the C function `alloc_percpu` returns `NULL`.
    pub fn new_zero() -> Option<PerCpuAllocation<T>> {
        let ptr: *mut MaybeUninit<T> =
            // SAFETY: No preconditions to call `alloc_percpu`; `MaybeUninit<T>` is
            // `#[repr(transparent)]`, so we can cast a `*mut T` to it.
            unsafe { alloc_percpu(size_of::<T>(), align_of::<T>()) }.cast();
        if ptr.is_null() {
            return None;
        }

        // alloc_percpu returns zero'ed memory
        Some(Self(PerCpuPtr::new(ptr)))
    }
}

impl<T> PerCpuAllocation<T> {
    /// Makes a per-CPU allocation sized and aligned to hold a `T`.
    ///
    /// Returns [`None`] under the same circumstances the C function `alloc_percpu` returns `NULL`.
    pub fn new_uninit() -> Option<PerCpuAllocation<T>> {
        let ptr: *mut MaybeUninit<T> =
            // SAFETY: No preconditions to call `alloc_percpu`; `MaybeUninit<T>` is
            // `#[repr(transparent)]`, so we can cast a `*mut T` to it.
            unsafe { alloc_percpu(size_of::<T>(), align_of::<T>()) }.cast();
        if ptr.is_null() {
            return None;
        }

        Some(Self(PerCpuPtr::new(ptr)))
    }
}

impl<T> Drop for PerCpuAllocation<T> {
    fn drop(&mut self) {
        // SAFETY: self.0.0 was returned by alloc_percpu, and so was a valid pointer into
        // the percpu area, and has remained valid by the invariants of PerCpuAllocation<T>.
        unsafe { free_percpu(self.0 .0.cast()) }
    }
}

/// Holds a dynamically-allocated per-CPU variable.
#[derive(Clone)]
pub struct DynamicPerCpu<T> {
    // INVARIANT: `alloc` is `Some` unless this object is in the process of being dropped.
    // INVARIANT: The allocation held by `alloc` is sized and aligned for a `T`.
    // INVARIANT: The memory location in each CPU's per-CPU area pointed at by the alloc is
    // initialized.
    alloc: Option<Arc<PerCpuAllocation<T>>>,
    // INVARIANT: `ptr` is the per-CPU pointer managed by `alloc`, which does not change for the
    // lifetime of `self`.
    pub(super) ptr: PerCpuPtr<T>,
}

impl<T: Zeroable> DynamicPerCpu<T> {
    /// Allocates a new per-CPU variable
    ///
    /// # Arguments
    /// * `flags` - [`Flags`] used to allocate an [`Arc`] that keeps track of the underlying
    ///   [`PerCpuAllocation`].
    pub fn new_zero(flags: Flags) -> Option<Self> {
        let alloc: PerCpuAllocation<T> = PerCpuAllocation::new_zero()?;

        let ptr = alloc.0;
        let arc = Arc::new(alloc, flags).ok()?;

        Some(Self {
            alloc: Some(arc),
            ptr,
        })
    }
}

impl<T: Clone> DynamicPerCpu<T> {
    /// Allocates a new per-CPU variable
    ///
    /// # Arguments
    /// * `val` - The initial value of the per-CPU variable on all CPUs.
    /// * `flags` - Flags used to allocate an [`Arc`] that keeps track of the underlying
    ///   [`PerCpuAllocation`].
    pub fn new_with(val: &T, flags: Flags) -> Option<Self> {
        let alloc: PerCpuAllocation<T> = PerCpuAllocation::new_uninit()?;
        let ptr = alloc.0;

        for cpu in Cpumask::possible_cpus().iter() {
            let remote_ptr = ptr.get_remote_ptr(cpu);
            // SAFETY: `remote_ptr` is valid because `ptr` points to a live allocation and `cpu`
            // appears in `Cpumask::possible_cpus()`.
            //
            // Each CPU's slot corresponding to `ptr` is currently uninitialized, and no one else
            // has a reference to it. Therefore, we can freely write to it without worrying about
            // the need to drop what was there or whether we're racing with someone else.
            unsafe {
                (*remote_ptr).write(val.clone());
            }
        }

        let arc = Arc::new(alloc, flags).ok()?;

        Some(Self {
            alloc: Some(arc),
            ptr,
        })
    }
}

impl<T> PerCpu<T> for DynamicPerCpu<T> {
    unsafe fn get_mut(&mut self, guard: CpuGuard) -> PerCpuToken<'_, T> {
        // SAFETY:
        // 1. Invariants of this type assure that `alloc` is `Some`.
        // 2. The requirements of `PerCpu::get_mut` ensure that no other `[Checked]PerCpuToken`
        //    exists on the current CPU.
        // 3. The invariants of `DynamicPerCpu` ensure that the contents of the allocation are
        //    initialized on each CPU.
        // 4. The existence of a reference to the `PerCpuAllocation` ensures that the allocation is
        //    live.
        // 5. The invariants of `DynamicPerCpu` ensure that the allocation is sized and aligned for
        //    a `T`.
        unsafe { PerCpuToken::new(guard, &self.alloc.as_ref().unwrap_unchecked().0) }
    }
}

impl<T: InteriorMutable> CheckedPerCpu<T> for DynamicPerCpu<T> {
    fn get(&mut self, guard: CpuGuard) -> CheckedPerCpuToken<'_, T> {
        // SAFETY:
        // 1. Invariants of this type assure that `alloc` is `Some`.
        // 2. The invariants of `DynamicPerCpu` ensure that the contents of the allocation are
        //    initialized on each CPU.
        // 3. The existence of a reference to the `PerCpuAllocation` ensures that the allocation is
        //    live.
        // 4. The invariants of `DynamicPerCpu` ensure that the allocation is sized and aligned for
        //    a `T`.
        unsafe { CheckedPerCpuToken::new(guard, &self.alloc.as_ref().unwrap_unchecked().0) }
    }
}

impl<T> Drop for DynamicPerCpu<T> {
    fn drop(&mut self) {
        // SAFETY: This type's invariant ensures that `self.alloc` is `Some`.
        let alloc = unsafe { self.alloc.take().unwrap_unchecked() };
        if let Some(unique_alloc) = alloc.into_unique_or_drop() {
            let ptr = unique_alloc.0;
            for cpu in Cpumask::possible_cpus().iter() {
                let remote_ptr = ptr.get_remote_ptr(cpu);
                // SAFETY: `remote_ptr` is valid because the allocation it points to is still live,
                // `cpu` appears in `Cpumask::possible_cpus()`, and the original allocation was
                // sized and aligned for a `T`.
                //
                // This type's invariant ensures that the memory location in each CPU's per-CPU
                // area pointed at by `alloc.0` has been initialized. We have a `UniqueArc`, so we
                // know we're the only ones with a reference to the memory. These two facts
                // together satisfy the requirements for `assume_init_drop`.
                unsafe {
                    (*remote_ptr).assume_init_drop();
                }
            }
        }
    }
}
