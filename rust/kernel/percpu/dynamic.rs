// SPDX-License-Identifier: GPL-2.0
//! Dynamically allocated per-CPU variables.

use super::*;

/// Represents a dynamic allocation of a per-CPU variable via alloc_percpu. Calls free_percpu when
/// dropped.
pub struct PerCpuAllocation<T>(PerCpuPtr<T>);

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
    pub(super) alloc: Arc<PerCpuAllocation<T>>,
}

impl<T: Zeroable> DynamicPerCpu<T> {
    /// Allocates a new per-CPU variable
    ///
    /// # Arguments
    /// * `flags` - Flags used to allocate an `Arc` that keeps track of the underlying
    ///   `PerCpuAllocation`.
    pub fn new_zero(flags: Flags) -> Option<Self> {
        let alloc: PerCpuAllocation<T> = PerCpuAllocation::new_zero()?;

        let arc = Arc::new(alloc, flags).ok()?;

        Some(Self { alloc: arc })
    }
}

impl<T> PerCpu<T> for DynamicPerCpu<T> {
    unsafe fn get_mut(&mut self, guard: CpuGuard) -> PerCpuToken<'_, T> {
        // SAFETY: The requirements of `PerCpu::get_mut` and this type's invariant ensure that the
        // requirements of `PerCpuToken::new` are met.
        unsafe { PerCpuToken::new(guard, &self.alloc.0) }
    }
}
