// SPDX-License-Identifier: GPL-2.0
//! This module contains abstractions for creating and using per-CPU variables from Rust. In
//! particular, see the define_per_cpu! and unsafe_get_per_cpu_ref! macros.
pub mod cpu_guard;

use crate::percpu::cpu_guard::CpuGuard;
use crate::unsafe_get_per_cpu_ref;

use core::arch::asm;
use core::marker::PhantomData;
use core::ops::{Deref, DerefMut};

/// A PerCpuRef is obtained by the unsafe_get_per_cpu_ref! macro used on a PerCpuVariable defined
/// via the define_per_cpu! macro.
///
/// This type will transparently deref(mut) into a &(mut) T referencing this CPU's instance of the
/// underlying variable.
pub struct PerCpuRef<T> {
    offset: usize,
    deref_type: PhantomData<T>,
    _guard: CpuGuard,
}

/// A wrapper used for declaring static per-cpu variables. These symbols are "virtual" in that the
/// linker uses them to generate offsets into each cpu's per-cpu area, but shouldn't be read
/// from/written to directly. The fact that the statics are immutable prevents them being written
/// to (generally), this struct having _val be non-public prevents reading from them.
///
/// The end-user of the per-CPU API should make use of the define_per_cpu! macro instead of
/// declaring variables of this type directly.
#[repr(transparent)]
pub struct PerCpuVariable<T> {
    _val: T, // generate a correctly sized type
}

impl<T> PerCpuRef<T> {
    /// You should be using the unsafe_get_per_cpu! macro instead
    ///
    /// # Safety
    /// offset must be a valid offset into the per cpu area
    pub unsafe fn new(offset: usize, guard: CpuGuard) -> Self {
        PerCpuRef {
            offset,
            deref_type: PhantomData,
            _guard: guard,
        }
    }

    /// Computes this_cpu_ptr as a usize, ignoring issues of ownership and borrowing
    fn this_cpu_ptr_usize(&self) -> usize {
        // SAFETY: this_cpu_off is read only as soon as the per-CPU subsystem is initialized
        let off: PerCpuRef<u64> = unsafe { unsafe_get_per_cpu_ref!(this_cpu_off, CpuGuard::new()) };
        let mut this_cpu_area: usize;
        // SAFETY: gs + off_val is guaranteed to be a valid pointer by the per-CPU subsystem and
        // the invariants guaranteed by PerCpuRef (i.e., off.offset is valid)
        unsafe {
            asm!(
                // For some reason, the asm! parser doesn't like
                //     mov {out}, [gs:{off_val}]
                // so we use the less intuitive prefix version instead
                "gs mov {out}, [{off_val}]",
                off_val = in(reg) off.offset,
                out = out(reg) this_cpu_area,
            )
        };
        this_cpu_area + self.offset
    }

    /// Returns a pointer to self's associated per-CPU variable. Logically equivalent to C's
    /// this_cpu_ptr
    pub fn this_cpu_ptr(&self) -> *const T {
        self.this_cpu_ptr_usize() as *const T
    }

    /// Returns a mut pointer to self's associated per-CPU variable. Logically equivalent to C's
    /// this_cpu_ptr
    pub fn this_cpu_ptr_mut(&mut self) -> *mut T {
        self.this_cpu_ptr_usize() as *mut T
    }
}

impl<T> Deref for PerCpuRef<T> {
    type Target = T;
    fn deref(&self) -> &Self::Target {
        // SAFETY: By the contract of unsafe_get_per_cpu_ref!, we know that self is the only
        // PerCpuRef associated with the underlying per-CPU variable and that the underlying
        // variable is not mutated outside of rust.
        unsafe { &*(self.this_cpu_ptr()) }
    }
}

impl<T> DerefMut for PerCpuRef<T> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        // SAFETY: By the contract of unsafe_get_per_cpu_ref!, we know that self is the only
        // PerCpuRef associated with the underlying per-CPU variable and that the underlying
        // variable is not mutated outside of rust.
        unsafe { &mut *(self.this_cpu_ptr_mut()) }
    }
}

/// define_per_cpu! is analogous to the C DEFINE_PER_CPU macro in that it lets you create a
/// statically allocated per-CPU variable.
///
/// # Example
/// ```
/// use kernel::define_per_cpu;
/// use kernel::percpu::PerCpuVariable;
///
/// define_per_cpu!(pub MY_PERCPU: u64 = 0);
/// ```
#[macro_export]
macro_rules! define_per_cpu {
    ($vis:vis $id:ident: $ty:ty = $expr:expr) => {
        $crate::macros::paste! {
            // Expand $expr outside of the unsafe block to avoid silently allowing unsafe code to be
            // used without a user-facing unsafe block
            static [<__init_ $id>]: $ty = $expr;

            // SAFETY: PerCpuVariable<T> is #[repr(transparent)], so we can freely convert from T
            #[link_section = ".data..percpu"]
            $vis static $id: PerCpuVariable<$ty> = unsafe {
                core::mem::transmute::<$ty, PerCpuVariable<$ty>>([<__init_ $id>])
            };
        }
    };
}

/// Goes from a PerCpuVariable to a usable PerCpuRef. $id is the identifier of the PerCpuVariable
/// and $guard is an expression that evaluates to a CpuGuard.
///
/// # Safety
/// Don't create two PerCpuRef that point at the same per-cpu variable, as this would allow you to
/// accidentally break aliasing rules. Unless T is Sync, the returned PerCpuRef should not be used
/// from interrupt contexts.
///
/// If $id is `extern "C"` (i.e., declared via declare_extern_per_cpu!) then the underlying per-CPU
/// variable must not be written from C code while a PerCpuRef exists in Rust. That is, the
/// underlying per-CPU variable must not be written in any IRQ context (unless the user ensures
/// IRQs are disabled) and no FFI calls can be made to C functions that may write the per-CPU
/// variable. The underlying PerCpuVariable created via declare_extern_per_cpu must also have the
/// correct type.
#[macro_export]
macro_rules! unsafe_get_per_cpu_ref {
    ($id:ident, $guard:expr) => {{
        let off = core::ptr::addr_of!($id);
        PerCpuRef::new(off as usize, $guard)
    }};
}

/// Declares a PerCpuVariable corresponding to a per-CPU variable defined in C. Be sure to read the
/// safety requirements of unsafe_get_per_cpu_ref!.
#[macro_export]
macro_rules! declare_extern_per_cpu {
    ($id:ident: $ty:ty) => {
        extern "C" {
            static $id: PerCpuVariable<$ty>;
        }
    };
}

declare_extern_per_cpu!(this_cpu_off: u64);
