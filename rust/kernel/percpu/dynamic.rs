// SPDX-License-Identifier: GPL-2.0
//! Dynamically allocated per-CPU variables.

use super::*;

use crate::{
    alloc::Flags,
    bindings::{
        alloc_percpu,
        free_percpu, //
    },
    cpumask::Cpumask,
    prelude::*,
    sync::Arc, //
};

use core::mem::{
    align_of,
    size_of,
    MaybeUninit, //
};

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
            // SAFETY: No preconditions to call `alloc_percpu`. The pointer is sized/aligned for a
            // `T`, and `MaybeUninit<T>` is `#[repr(transparent)]` (and thus same size/align), so
            // we can cast this `*mut c_void` to it.
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
            // SAFETY: No preconditions to call `alloc_percpu`. The pointer is sized/aligned for a
            // `T`, and `MaybeUninit<T>` is `#[repr(transparent)]` (and thus same size/align), so
            // we can cast this `*mut c_void` to it.
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
///
/// # Examples
/// ```
/// # use kernel::prelude::*;
/// # use kernel::percpu::*;
/// # use kernel::percpu::cpu_guard::CpuGuard;
///
/// let mut pcpu: DynamicPerCpu<u32> =
///   DynamicPerCpu::new_zero(GFP_KERNEL).expect("failed to allocate per-CPU variable");
/// {
///   let _guard = CpuGuard::new();
///   // SAFETY: No other `pcpu` reference
///   unsafe { pcpu.get_mut(CpuGuard::new()) }.with(|val| *val = 42);
///   // SAFETY: No other `pcpu` reference
///   unsafe { pcpu.get_mut(CpuGuard::new()) }.with(|val| assert!(*val == 42));
/// }
/// ```
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
        Self::new_from(|_| val.clone(), flags)
    }
}

impl<T> DynamicPerCpu<T> {
    /// Allocates a new per-CPU variable
    ///
    /// # Arguments
    /// * `initer` - A function that takes a `CpuId` and returns the initial value of the per-CPU
    ///   variable on the corresponding CPU. Called once for each possible CPU (i.e.,
    ///   `Cpumask::possible_cpus().weight()` times).
    /// * `flags` - Flags used to allocate an [`Arc`] that keeps track of the underlying
    ///   [`PerCpuAllocation`].
    pub fn new_from(mut initer: impl FnMut(CpuId) -> T, flags: Flags) -> Option<Self> {
        let alloc: PerCpuAllocation<T> = PerCpuAllocation::new_uninit()?;
        let ptr = alloc.0;

        let arc = Arc::new(alloc, flags).ok()?;

        for cpu in Cpumask::possible_cpus().iter() {
            let remote_ptr = ptr.get_remote_ptr(cpu);
            // SAFETY: `remote_ptr` is valid because `ptr` points to a live allocation and `cpu`
            // appears in `Cpumask::possible_cpus()`.
            //
            // Each CPU's slot corresponding to `ptr` is currently uninitialized, and no one else
            // has a reference to it. Therefore, we can freely write to it without worrying about
            // the need to drop what was there or whether we're racing with someone else.
            unsafe {
                (*remote_ptr).write(initer(cpu));
            }
        }

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
        // 4. `&mut self` holds a reference to the `PerCpuAllocation`, so the allocation it manages
        //    is live.
        // 5. The invariants of `DynamicPerCpu` ensure that the allocation is sized and aligned for
        //    a `T`.
        unsafe { PerCpuToken::new(guard, &self.ptr) }
    }
}

impl<T: InteriorMutable> CheckedPerCpu<T> for DynamicPerCpu<T> {
    fn get(&self, guard: CpuGuard) -> CheckedPerCpuToken<'_, T> {
        // SAFETY:
        // 1. Invariants of this type assure that `alloc` is `Some`.
        // 2. The invariants of `DynamicPerCpu` ensure that the contents of the allocation are
        //    initialized on each CPU.
        // 3. `&mut self` holds a reference to the `PerCpuAllocation`, so the allocation it manages
        //    is live.
        // 4. The invariants of `DynamicPerCpu` ensure that the allocation is sized and aligned for
        //    a `T`.
        unsafe { CheckedPerCpuToken::new(guard, &self.ptr) }
    }
}

impl<T> Drop for DynamicPerCpu<T> {
    fn drop(&mut self) {
        // SAFETY: This type's invariant ensures that `self.alloc` is `Some`.
        let alloc = unsafe { self.alloc.take().unwrap_unchecked() };
        if let Some(unique_alloc) = Arc::into_unique_or_drop(alloc) {
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
