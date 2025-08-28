// SPDX-License-Identifier: GPL-2.0
//! Dynamically allocated per-CPU variables.

use super::*;

use crate::cpumask::Cpumask;

/// Represents a dynamic allocation of a per-CPU variable via alloc_percpu. Calls free_percpu when
/// dropped.
pub struct PerCpuAllocation<T>(pub(super) PerCpuPtr<T>);

impl<T: Zeroable> PerCpuAllocation<T> {
    /// Dynamically allocates a space in the per-CPU area suitably sized and aligned to hold a `T`,
    /// initially filled with the zero value for `T`.
    ///
    /// Returns `None` under the same circumstances the C function `alloc_percpu` returns `NULL`.
    pub fn new_zero() -> Option<PerCpuAllocation<T>> {
        let ptr: *mut MaybeUninit<T> =
            // SAFETY: No preconditions to call alloc_percpu; MaybeUninit<T> is
            // `#[repr(transparent)]`, so we can cast a `*mut T` to it.
            unsafe { alloc_percpu(size_of::<T>(), align_of::<T>()) }.cast();
        if ptr.is_null() {
            return None;
        }

        // alloc_percpu returns zero'ed memory
        Some(Self(PerCpuPtr(ptr)))
    }
}

impl<T> PerCpuAllocation<T> {
    /// Makes a per-CPU allocation sized and aligned to hold a `T`.
    ///
    /// Returns `None` under the same circumstances the C function `alloc_percpu` returns `NULL`.
    pub fn new_uninit() -> Option<PerCpuAllocation<T>> {
        let ptr: *mut MaybeUninit<T> =
            // SAFETY: No preconditions to call alloc_percpu; MaybeUninit<T> is
            // `#[repr(transparent)]`, so we can cast a `*mut T` to it.
            unsafe { alloc_percpu(size_of::<T>(), align_of::<T>()) }.cast();
        if ptr.is_null() {
            return None;
        }

        Some(Self(PerCpuPtr(ptr)))
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
    // INVARIANT: The memory location in each CPU's per-CPU area pointed at by `alloc.0` has been
    // initialized.
    // INVARIANT: `ptr` is the per-CPU pointer managed by `alloc`, which does not change for the
    // lifetime of `self`.
    pub(super) alloc: Arc<PerCpuAllocation<T>>,
    pub(super) ptr: PerCpuPtr<T>,
}

impl<T: Zeroable> DynamicPerCpu<T> {
    /// Allocates a new per-CPU variable
    ///
    /// # Arguments
    /// * `flags` - Flags used to allocate an `Arc` that keeps track of the underlying
    ///   `PerCpuAllocation`.
    pub fn new_zero(flags: Flags) -> Option<Self> {
        let alloc: PerCpuAllocation<T> = PerCpuAllocation::new_zero()?;

        let ptr = alloc.0;
        let arc = Arc::new(alloc, flags).ok()?;

        Some(Self { alloc: arc, ptr })
    }
}

impl<T: Clone> DynamicPerCpu<T> {
    /// Allocates a new per-CPU variable
    ///
    /// # Arguments
    /// * `val` - The initial value of the per-CPU variable on all CPUs.
    /// * `flags` - Flags used to allocate an `Arc` that keeps track of the underlying
    ///   `PerCpuAllocation`.
    pub fn new_with(val: T, flags: Flags) -> Option<Self> {
        let alloc: PerCpuAllocation<T> = PerCpuAllocation::new_uninit()?;
        let ptr = alloc.0;

        for cpu in Cpumask::possible().iter() {
            // SAFETY: `ptr` is a valid allocation, and `cpu` appears in `Cpumask::possible()`
            let remote_ptr = unsafe { ptr.get_remote_ptr(cpu) };
            // SAFETY: Each CPU's slot corresponding to `ptr` is currently uninitialized, and no
            // one else has a reference to it. Therefore, we can freely write to it without
            // worrying about the need to drop what was there or whether we're racing with someone
            // else. `PerCpuPtr::get_remote_ptr` guarantees that the pointer is valid since we
            // derived it from a valid allocation and `cpu`.
            unsafe {
                (*remote_ptr).write(val.clone());
            }
        }

        let arc = Arc::new(alloc, flags).ok()?;

        Some(Self { alloc: arc, ptr })
    }
}

impl<T> PerCpu<T> for DynamicPerCpu<T> {
    unsafe fn get_mut(&mut self, guard: CpuGuard) -> PerCpuToken<'_, T> {
        // SAFETY: The requirements of `PerCpu::get_mut` and this type's invariant ensure that the
        // requirements of `PerCpuToken::new` are met.
        unsafe { PerCpuToken::new(guard, &self.alloc.0) }
    }
}

impl<T: InteriorMutable> CheckedPerCpu<T> for DynamicPerCpu<T> {
    fn get(&mut self, guard: CpuGuard) -> CheckedPerCpuToken<'_, T> {
        // SAFETY: By the invariant of `DynamicPerCpu`, the memory location in each CPU's
        // per-CPU area corresponding to this variable has been initialized.
        unsafe { CheckedPerCpuToken::new(guard, &self.alloc.0) }
    }
}
