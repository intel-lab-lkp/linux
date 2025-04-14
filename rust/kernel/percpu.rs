// SPDX-License-Identifier: GPL-2.0
//! This module contains abstractions for creating and using per-CPU variables from Rust.
//! See the define_per_cpu! macro and the PerCpu<T> type.
pub mod cpu_guard;

use bindings::{alloc_percpu, free_percpu};

use crate::percpu::cpu_guard::CpuGuard;
use crate::prelude::*;
use crate::sync::Arc;

use core::arch::asm;
use core::marker::PhantomData;

use ffi::c_void;

/// Represents an allocation of a per-CPU variable via alloc_percpu or a static per-CPU
/// allocation. Calls free_percpu when dropped if not a static allocation.
pub struct PerCpuAllocation<T> {
    /// Offset into the per-CPU area: either the address of a statically-allocated per-CPU variable
    /// or a "pointer" returned by alloc_percpu or similar.
    offset: usize,
    /// Whether `self` is statically allocated
    is_static: bool,
    /// PhantomData so the compiler doesn't complain about `T` being unused.
    deref_type: PhantomData<T>,
}

/// Holds a per-CPU variable.
pub struct PerCpu<T> {
    alloc: Arc<PerCpuAllocation<T>>,
}

/// Represents exclusive access to the memory location pointed at by a particular PerCpu<T>.
pub struct PerCpuToken<'a, T> {
    _guard: CpuGuard,
    pcpu: &'a mut PerCpu<T>,
}

/// A wrapper used for declaring static per-CPU variables. These symbols are "virtual" in that the
/// linker uses them to generate offsets into each cpu's per-cpu area, but shouldn't be read
/// from/written to directly. The fact that the statics are immutable prevents them being written
/// to (generally), this struct having _val be non-public prevents reading from them.
///
/// The end-user of the per-CPU API should make use of the define_per_cpu! macro instead of
/// declaring variables of this type directly.
#[repr(transparent)]
pub struct StaticPerCpuSymbol<T> {
    _val: T, // generate a correctly sized type
}

impl<T> PerCpuAllocation<T> {
    /// Dynamically allocates a space in the per-CPU area suitably sized and aligned to hold a `T`.
    ///
    /// Returns `None` under the same circumstances the C function `alloc_percpu` returns `NULL`.
    pub fn new() -> Option<PerCpuAllocation<T>> {
        // SAFETY: No preconditions to call alloc_percpu
        let ptr: *mut c_void = unsafe { alloc_percpu(size_of::<T>(), align_of::<T>()) };
        if ptr.is_null() {
            return None;
        }

        Some(Self {
            offset: ptr as usize,
            is_static: false,
            deref_type: PhantomData,
        })
    }

    /// Convert a statically allocated per-CPU variable (via its offset, i.e., the address of the
    /// declared symbol) into a `PerCpuAllocation`.
    ///
    /// # Safety
    /// `offset` must be a valid offset into the per-CPU area that's suitably sized and aligned to
    /// hold a `T`.
    pub unsafe fn new_static(offset: usize) -> PerCpuAllocation<T> {
        Self {
            offset,
            is_static: true,
            deref_type: PhantomData,
        }
    }
}

impl<T> Drop for PerCpuAllocation<T> {
    fn drop(&mut self) {
        if !self.is_static {
            // SAFETY: self.offset was returned by alloc_percpu, and so was a valid pointer into
            // the percpu area, and has remained valid by the invariants of PerCpuAllocation<T>.
            unsafe { free_percpu(self.offset as *mut c_void) }
        }
    }
}

impl<T> PerCpu<T> {
    /// Allocates a new per-CPU variable
    pub fn new() -> Option<Self> {
        let alloc: PerCpuAllocation<T> = PerCpuAllocation::new()?;

        let arc = Arc::new(alloc, GFP_KERNEL).ok()?;

        Some(Self { alloc: arc })
    }

    /// Wraps a `PerCpuAllocation<T>` in a `PerCpu<T>`
    pub fn new_from_allocation(alloc: PerCpuAllocation<T>) -> Option<Self> {
        let arc = Arc::new(alloc, GFP_KERNEL).ok()?;
        Some(Self { alloc: arc })
    }

    /// Creates a new PerCpu<T> pointing to the same underlying variable
    pub fn clone(&self) -> Self {
        Self {
            alloc: self.alloc.clone(),
        }
    }

    /// Get a `&mut T` to the per-CPU variable represented by `&mut self`
    ///
    /// # Safety
    /// The returned `&mut T` must follow Rust's aliasing rules. That is, no other `&(mut) T` may
    /// exist that points to the same location in memory. In practice, this means that any PerCpu
    /// holding a reference to the same PerCpuAllocation as `self` mut not call `get_ref` for as
    /// long as the returned reference lives.
    unsafe fn get_ref(&mut self) -> &mut T {
        // SAFETY: addr_of!(this_cpu_off) is guaranteed to be a valid per-CPU offset by the per-CPU
        // subsystem
        let this_cpu_off_pcpu: PerCpuAllocation<u64> =
            unsafe { PerCpuAllocation::new_static(core::ptr::addr_of!(this_cpu_off) as usize) };
        let mut this_cpu_area: *mut c_void;
        // SAFETY: gs + this_cpu_off_pcpu.offset is guaranteed to be a valid pointer because `gs`
        // points to the per-CPU area and this_cpu_off_pcpu is a valid per-CPU allocation.
        unsafe {
            asm!(
                // For some reason, the asm! parser doesn't like
                //     mov {out}, [gs:{off_val}]
                // so we use the less intuitive prefix version instead
                "gs mov {out}, [{off_val}]",
                off_val = in(reg) this_cpu_off_pcpu.offset,
                out = out(reg) this_cpu_area,
            )
        };
        // SAFETY: this_cpu_area + self.alloc.offset is guaranteed to be a valid pointer by the
        // per-CPU subsystem and the invariant that self.offset is a valid offset into the per-CPU
        // area.
        //
        // We have exclusive access to self via &mut self, so we know no-one else has a reference
        // to the underlying pcpu variable because of the safety requirements of this function.
        unsafe { &mut *((this_cpu_area.add(self.alloc.offset)) as *mut T) }
    }

    /// Produces a token, asserting that the holder has exclusive access to the underlying memory
    /// pointed to by `self`
    ///
    /// # Safety
    /// `func` (or its callees that execute on the same CPU) may not call `get_ref` on another
    /// `PerCpu<T>` that represents the same per-CPU variable as `&mut self` (that is, they must
    /// not be `clone()`s of each other or, in the case of statically allocated variables,
    /// additionally can't both have come from the same `define_per_cpu!`) for the lifetime of the
    /// returned token.
    ///
    /// In particular, this requires that the underlying per-CPU variable cannot ever be mutated
    /// from an interrupt context, unless irqs are disabled for the lifetime of the returned
    /// `PerCpuToken`.
    pub unsafe fn get(&mut self, guard: CpuGuard) -> PerCpuToken<'_, T> {
        PerCpuToken {
            _guard: guard,
            pcpu: self,
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
        func(unsafe { self.pcpu.get_ref() });
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

/// Gets a `PerCpu<T>` from a symbol declared with `define_per_cpu!` or `declare_extern_per_cpu!`.
///
/// # Safety
/// `$id` must be declared with either `define_per_cpu!` or `declare_extern_per_cpu!`, and the
/// returned value must be stored in a `PerCpu<T>` where `T` matches the declared type of `$id`.
#[macro_export]
macro_rules! unsafe_get_per_cpu {
    ($id:ident, $guard:expr) => {{
        let off = core::ptr::addr_of!($id);
        $crate::percpu::PerCpu::new_from_allocation($crate::percpu::PerCpuAllocation::new_static(
            off as usize,
        ))
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
