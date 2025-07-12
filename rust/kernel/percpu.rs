// SPDX-License-Identifier: GPL-2.0
//! This module contains abstractions for creating and using per-CPU variables from Rust.
//! See the define_per_cpu! macro and the DynamicPerCpu<T> type, as well as the PerCpu<T> trait.
pub mod cpu_guard;
pub mod numeric;

use bindings::{alloc_percpu, free_percpu};

use crate::alloc::Flags;
use crate::percpu::cpu_guard::CpuGuard;
use crate::prelude::*;
use crate::sync::Arc;

use core::arch::asm;
use core::mem::{align_of, size_of};

use ffi::c_void;

/// A per-CPU pointer; that is, an offset into the per-CPU area. Note that this type is NOT a smart
/// pointer, it does not manage the allocation.
pub struct PerCpuPtr<T>(*mut T);

/// Represents a dynamic allocation of a per-CPU variable via alloc_percpu. Calls free_percpu when
/// dropped.
pub struct PerCpuAllocation<T>(PerCpuPtr<T>);

/// Holds a dynamically-allocated per-CPU variable.
pub struct DynamicPerCpu<T> {
    alloc: Arc<PerCpuAllocation<T>>,
}

/// Holds a statically-allocated per-CPU variable.
pub struct StaticPerCpu<T>(PerCpuPtr<T>);

/// Represents exclusive access to the memory location pointed at by a particular PerCpu<T>.
pub struct PerCpuToken<'a, T> {
    _guard: CpuGuard,
    ptr: &'a PerCpuPtr<T>,
}

/// A wrapper used for declaring static per-CPU variables. These symbols are "virtual" in that the
/// linker uses them to generate offsets into each CPU's per-CPU area, but shouldn't be read
/// from/written to directly. The fact that the statics are immutable prevents them being written
/// to (generally), this struct having _val be non-public prevents reading from them.
///
/// The end-user of the per-CPU API should make use of the define_per_cpu! macro instead of
/// declaring variables of this type directly.
#[repr(transparent)]
pub struct StaticPerCpuSymbol<T> {
    _val: T, // generate a correctly sized type
}

impl<T> PerCpuPtr<T> {
    /// Makes a new PerCpuPtr from a raw per-CPU pointer.
    ///
    /// # Safety
    /// `ptr` must be a valid per-CPU pointer.
    pub unsafe fn new(ptr: *mut T) -> Self {
        Self(ptr)
    }

    /// Get a `&mut T` to the per-CPU variable represented by `&self`
    ///
    /// # Safety
    /// The returned `&mut T` must follow Rust's aliasing rules. That is, no other `&(mut) T` may
    /// exist that points to the same location in memory. In practice, this means that `get_ref`
    /// must not be called on another `PerCpuPtr<T>` that is a copy/clone of `&self` for as long as
    /// the returned reference lives.
    ///
    /// CPU preemption must be disabled before calling this function and for the lifetime of the
    /// returned reference. Otherwise, the returned &mut T might end up being a reference to a
    /// different CPU's per-CPU area, causing the potential for a data race.
    #[allow(clippy::mut_from_ref)] // Safety requirements prevent aliasing issues
    pub unsafe fn get_ref(&self) -> &mut T {
        let this_cpu_off_pcpu = core::ptr::addr_of!(this_cpu_off);
        let mut this_cpu_area: *mut c_void;
        // SAFETY: gs + this_cpu_off_pcpu is guaranteed to be a valid pointer because `gs` points
        // to the per-CPU area and this_cpu_off_pcpu is a valid per-CPU allocation.
        unsafe {
            asm!(
                "mov {out}, gs:[{off_val}]",
                off_val = in(reg) this_cpu_off_pcpu,
                out = out(reg) this_cpu_area,
            )
        };
        // SAFETY: this_cpu_area + self.0 is guaranteed to be a valid pointer by the per-CPU
        // subsystem and the invariant that self.0 is a valid offset into the per-CPU area.
        //
        // We know no-one else has a reference to the underlying pcpu variable because of the
        // safety requirements of this function.
        unsafe { &mut *((this_cpu_area).wrapping_add(self.0 as usize) as *mut T) }
    }
}

impl<T> Clone for PerCpuPtr<T> {
    fn clone(&self) -> Self {
        *self
    }
}

/// PerCpuPtr is just a pointer, so it's safe to copy.
impl<T> Copy for PerCpuPtr<T> {}

impl<T: Zeroable> PerCpuAllocation<T> {
    /// Dynamically allocates a space in the per-CPU area suitably sized and aligned to hold a `T`.
    ///
    /// Returns `None` under the same circumstances the C function `alloc_percpu` returns `NULL`.
    pub fn new() -> Option<PerCpuAllocation<T>> {
        // SAFETY: No preconditions to call alloc_percpu
        let ptr: *mut T = unsafe { alloc_percpu(size_of::<T>(), align_of::<T>()) } as *mut T;
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
        unsafe { free_percpu(self.0 .0 as *mut c_void) }
    }
}

/// A trait representing a per-CPU variable. This is implemented for both `StaticPerCpu<T>` and
/// `DynamicPerCpu<T>`. The main usage of this trait is to call `get` to get a `PerCpuToken` that
/// can be used to access the underlying per-CPU variable. See `PerCpuToken::with`.
///
/// # Safety
/// The returned value from `ptr` must be valid for the lifetime of `&mut self`.
pub unsafe trait PerCpu<T> {
    /// Gets a `PerCpuPtr<T>` to the per-CPU variable represented by `&mut self`
    ///
    /// # Safety
    /// `self` may be doing all sorts of things to track when the underlying per-CPU variable can
    /// be deallocated. You almost certainly shouldn't be calling this function directly (it's
    /// essentially an implementation detail of the trait), and you certainly shouldn't be making
    /// copies of the returned `PerCpuPtr<T>` that may outlive `&mut self`.
    ///
    /// Implementers of this trait must ensure that the returned `PerCpuPtr<T>` is valid for the
    /// lifetime of `&mut self`.
    unsafe fn ptr(&mut self) -> &PerCpuPtr<T>;

    /// Produces a token, asserting that the holder has exclusive access to the underlying memory
    /// pointed to by `self`
    ///
    /// # Safety
    /// `func` (or its callees that execute on the same CPU) may not, for any `x: PerCpu<T>` that
    /// is a `clone` of `&mut self` (or, for a statically allocated variable, a `StaticPerCpu<T>`
    /// that came from the same `define_per_cpu!`):
    /// - call `x.get()`
    /// - make use of the value returned by `x.ptr()`
    ///
    /// `func` and its callees must not access or modify the memory associated with `&mut self`'s
    /// allocation in the per-CPU area, except via (reborrows of) the reference passed to `func`.
    ///
    /// The underlying per-CPU variable cannot ever be mutated from an interrupt context, unless
    /// irqs are disabled for the lifetime of the returned `PerCpuToken`.
    unsafe fn get(&mut self, guard: CpuGuard) -> PerCpuToken<'_, T> {
        PerCpuToken {
            _guard: guard,
            // SAFETY: The lifetime of the returned `PerCpuToken<'_, T>` is bounded by the lifetime
            // of `&mut self`.
            ptr: unsafe { self.ptr() },
        }
    }
}

impl<T> StaticPerCpu<T> {
    /// Creates a new PerCpu<T> pointing to the statically allocated variable at `ptr`. End-users
    /// should probably be using the `unsafe_get_per_cpu!` macro instead of calling this function.
    ///
    /// # Safety
    /// `ptr` must be a valid pointer to a per-CPU variable. This means that it must be a valid
    /// offset into the per-CPU area, and that the per-CPU area must be suitably sized and aligned
    /// to hold a `T`.
    pub unsafe fn new(ptr: *mut T) -> Self {
        Self(PerCpuPtr(ptr))
    }
}

// SAFETY: The `PerCpuPtr<T>` returned by `ptr` is valid for the lifetime of `self` (and in fact,
// forever).
unsafe impl<T> PerCpu<T> for StaticPerCpu<T> {
    unsafe fn ptr(&mut self) -> &PerCpuPtr<T> {
        &self.0
    }
}

impl<T> Clone for StaticPerCpu<T> {
    fn clone(&self) -> Self {
        Self(self.0)
    }
}

impl<T: Zeroable> DynamicPerCpu<T> {
    /// Allocates a new per-CPU variable
    ///
    /// # Arguments
    /// * `flags` - Flags used to allocate an `Arc` that keeps track of the underlying
    ///   `PerCpuAllocation`.
    pub fn new(flags: Flags) -> Option<Self> {
        let alloc: PerCpuAllocation<T> = PerCpuAllocation::new()?;

        let arc = Arc::new(alloc, flags).ok()?;

        Some(Self { alloc: arc })
    }
}

impl<T> DynamicPerCpu<T> {
    /// Wraps a `PerCpuAllocation<T>` in a `PerCpu<T>`
    ///
    /// # Arguments
    /// * `alloc` - The allocation to use
    /// * `flags` - The flags used to allocate an `Arc` that keeps track of the `PerCpuAllocation`.
    pub fn new_from_allocation(alloc: PerCpuAllocation<T>, flags: Flags) -> Option<Self> {
        let arc = Arc::new(alloc, flags).ok()?;
        Some(Self { alloc: arc })
    }
}

// SAFETY: The `PerCpuPtr<T>` returned by `ptr` is valid for the lifetime of `self` because we
// don't deallocate the underlying `PerCpuAllocation` until `self` is dropped.
unsafe impl<T> PerCpu<T> for DynamicPerCpu<T> {
    unsafe fn ptr(&mut self) -> &PerCpuPtr<T> {
        &self.alloc.0
    }
}

impl<T> Clone for DynamicPerCpu<T> {
    fn clone(&self) -> Self {
        Self {
            alloc: self.alloc.clone(),
        }
    }
}

impl<T> PerCpuToken<'_, T> {
    /// Immediately invokes `func` with a `&mut T` that points at the underlying per-CPU variable
    /// that `&mut self` represents.
    pub fn with<U>(&mut self, func: U)
    where
        U: FnOnce(&mut T),
    {
        // SAFETY: The existence of a PerCpuToken means that the requirements for get_ref are
        // satisfied.
        func(unsafe { self.ptr.get_ref() });
    }
}

/// define_per_cpu! is analogous to the C DEFINE_PER_CPU macro in that it lets you create a
/// statically allocated per-CPU variable.
///
/// # Example
/// ```
/// use kernel::define_per_cpu;
/// use kernel::percpu::StaticPerCpuSymbol;
///
/// define_per_cpu!(pub MY_PERCPU: u64 = 0);
/// ```
#[macro_export]
macro_rules! define_per_cpu {
    ($vis:vis $id:ident: $ty:ty = $expr:expr) => {
        $crate::macros::paste! {
            // Expand $expr outside of the unsafe block to avoid silently allowing unsafe code to be
            // used without a user-facing unsafe block
            static [<__INIT_ $id>]: $ty = $expr;

            // SAFETY: StaticPerCpuSymbol<T> is #[repr(transparent)], so we can freely convert from T
            #[link_section = ".data..percpu"]
            $vis static $id: StaticPerCpuSymbol<$ty> = unsafe {
                core::mem::transmute::<$ty, StaticPerCpuSymbol<$ty>>([<__INIT_ $id>])
            };
        }
    };
}

/// Gets a `StaticPerCpu<T>` from a symbol declared with `define_per_cpu!` or
/// `declare_extern_per_cpu!`.
///
/// # Arguments
/// * `ident` - The identifier declared
///
/// # Safety
/// `$id` must be declared with either `define_per_cpu!` or `declare_extern_per_cpu!`, and the
/// returned value must be stored in a `StaticPerCpu<T>` where `T` matches the declared type of
/// `$id`.
#[macro_export]
macro_rules! unsafe_get_per_cpu {
    ($id:ident) => {{
        $crate::percpu::StaticPerCpu::new((&$id) as *const _ as *mut _)
    }};
}

/// Declares a StaticPerCpuSymbol corresponding to a per-CPU variable defined in C. Be sure to read
/// the safety requirements of `PerCpu::get`.
#[macro_export]
macro_rules! declare_extern_per_cpu {
    ($id:ident: $ty:ty) => {
        extern "C" {
            static $id: StaticPerCpuSymbol<$ty>;
        }
    };
}

declare_extern_per_cpu!(this_cpu_off: u64);
