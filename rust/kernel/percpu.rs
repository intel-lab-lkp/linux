// SPDX-License-Identifier: GPL-2.0
//! Per-CPU variables.
//!
//! See the [`crate::define_per_cpu!`] macro, the [`DynamicPerCpu`] type, and the [`PerCpu<T>`]
//! trait.

pub mod cpu_guard;
mod dynamic;
mod static_;

#[doc(inline)]
pub use dynamic::*;
#[doc(inline)]
pub use static_::*;

use crate::cpu::CpuId;
use crate::declare_extern_per_cpu;
use crate::percpu::cpu_guard::CpuGuard;
use crate::types::Opaque;

use core::arch::asm;
use core::cell::{Cell, RefCell, UnsafeCell};
use core::mem::MaybeUninit;

use ffi::c_void;

/// A per-CPU pointer; that is, an offset into the per-CPU area.
///
/// Note that this type is NOT a smart pointer, it does not manage the allocation.
pub struct PerCpuPtr<T>(*mut MaybeUninit<T>);

/// Represents exclusive access to the memory location pointed at by a particular [`PerCpu<T>`].
pub struct PerCpuToken<'a, T> {
    // INVARIANT: the current CPU's memory location associated with the per-CPU variable pointed at
    // by `ptr` (i.e., the entry in the per-CPU area on the current CPU) has been initialized.
    _guard: CpuGuard,
    ptr: &'a PerCpuPtr<T>,
}

/// Represents access to the memory location pointed at by a particular [`PerCpu<T>`] where the
/// type `T` manages access to the underlying memory to avoid aliaising troubles.
///
/// For example, `T` might be a [`Cell`] or [`RefCell`].
pub struct CheckedPerCpuToken<'a, T> {
    // INVARIANT: the current CPU's memory location associated with the per-CPU variable pointed at
    // by `ptr` (i.e., the entry in the per-CPU area on the current CPU) has been initialized.
    _guard: CpuGuard,
    ptr: &'a PerCpuPtr<T>,
}

impl<T> PerCpuPtr<T> {
    /// Makes a new [`PerCpuPtr`] from a raw per-CPU pointer.
    ///
    /// Note that the returned [`PerCpuPtr`] is valid only as long as the given pointer is. This
    /// also requires that the allocation pointed to by `ptr` must be correctly sized and aligned
    /// to hold a `T`.
    pub fn new(ptr: *mut MaybeUninit<T>) -> Self {
        Self(ptr)
    }

    /// Get a [`&mut MaybeUninit<T>`](MaybeUninit) to the per-CPU variable on the current CPU
    /// represented by `&self`
    ///
    /// # Safety
    ///
    /// The returned `&mut T` must follow Rust's aliasing rules. That is, no other `&(mut) T` may
    /// exist that points to the same location in memory. In practice, this means that
    /// [`Self::get_ref`] and [`Self::get_mut_ref`] must not be called on another [`PerCpuPtr<T>`]
    /// that is a copy/clone of `&self` for as long as the returned reference lives. If the per-CPU
    /// variable represented by `&self` is available to C, the caller of this function must ensure
    /// that the C code does not modify the variable for as long as this reference lives.
    ///
    /// `self` must point to a live allocation correctly sized and aligned to hold a `T`.
    ///
    /// CPU preemption must be disabled before calling this function and for the lifetime of the
    /// returned reference. Otherwise, the returned reference might end up being a reference to a
    /// different CPU's per-CPU area, causing the potential for a data race.
    #[allow(clippy::mut_from_ref)] // Safety requirements prevent aliasing issues
    pub unsafe fn get_mut_ref(&self) -> &mut MaybeUninit<T> {
        // SAFETY: `self.get_ptr()` returns a valid pointer to a `MaybeUninit<T>` by its contract,
        // and the safety requirements of this function ensure that the returned reference is
        // exclusive.
        unsafe { &mut *(self.get_ptr()) }
    }

    /// Get a [`&mut MaybeUninit<T>`](MaybeUninit) to the per-CPU variable on the current CPU
    /// represented by `&self`
    ///
    /// # Safety
    ///
    /// The returned `&T` must follow Rust's aliasing rules. That is, no `&mut T` may exist that
    /// points to the same location in memory. In practice, this means that [`Self::get_mut_ref`]
    /// must not be called on another [`PerCpuPtr<T>`] that is a copy/clone of `&self` for as long
    /// as the returned reference lives. If the per-CPU variable represented by `&self` is
    /// available to C, the caller of this function must ensure that the C code does not modify the
    /// variable for as long as this reference lives.
    ///
    /// `self` must point to a live allocation correctly sized and aligned to hold a `T`.
    ///
    /// CPU preemption must be disabled before calling this function and for the lifetime of the
    /// returned reference. Otherwise, the returned reference might end up being a reference to a
    /// different CPU's per-CPU area, causing the potential for a data race.
    pub unsafe fn get_ref(&self) -> &MaybeUninit<T> {
        // SAFETY: `self.get_ptr()` returns a valid pointer to a `MaybeUninit<T>` by its contract.
        // The safety requirements of this function ensure that the returned reference isn't
        // aliased by a `&mut MaybeUninit<T>`.
        unsafe { &*self.get_ptr() }
    }

    /// Get a [`*mut MaybeUninit<T>`](MaybeUninit) to the per-CPU variable on the current CPU
    /// represented by `&self`. Note that if CPU preemption is not disabled before calling this
    /// function, use of the returned pointer may cause a data race without some other
    /// synchronization mechanism. Buyer beware!
    pub fn get_ptr(&self) -> *mut MaybeUninit<T> {
        let this_cpu_off_pcpu = ExternStaticPerCpuSymbol::ptr(&raw const this_cpu_off);
        let mut this_cpu_area: *mut c_void;
        // SAFETY: gs + this_cpu_off_pcpu is guaranteed to be a valid pointer because `gs` points
        // to the per-CPU area and this_cpu_off_pcpu is a valid per-CPU allocation.
        unsafe {
            asm!(
                "mov {out}, gs:[{off_val}]",
                off_val = in(reg) this_cpu_off_pcpu.0,
                out = out(reg) this_cpu_area,
            )
        };

        // This_cpu_area + self.0 is guaranteed to be a valid pointer by the per-CPU subsystem and
        // the invariant that self.0 is a valid offset into the per-CPU area.
        (this_cpu_area).wrapping_add(self.0 as usize).cast()
    }

    /// Get a [`*mut MaybeUninit<T>`](MaybeUninit) to the per-CPU variable on the CPU represented
    /// by `cpu`. Note that without some kind of synchronization, use of the returned pointer may
    /// cause a data race. It is the caller's responsibility to use the returned pointer in a
    /// reasonable way.
    ///
    /// # Returns
    /// - The returned pointer is valid only if `self` is (that is, it points to a live allocation
    ///   correctly sized and aligned to hold a `T`)
    /// - The returned pointer is valid only if the bit corresponding to `cpu` is set in
    ///   [`kernel::cpumask::Cpumask::possible_cpus()`].
    pub fn get_remote_ptr(&self, cpu: CpuId) -> *mut MaybeUninit<T> {
        // SAFETY: `bindings::per_cpu_ptr` is just doing pointer arithmetic. The returned pointer
        // may not be valid (under the conditions specified in this function's documentation), but
        // the act of producing the pointer is safe.
        unsafe { bindings::per_cpu_ptr(self.0.cast(), cpu.as_u32()) }.cast()
    }
}

// SAFETY: Sending a [`PerCpuPtr<T>`] to another thread is safe because as soon as it's sent, the
// pointer is logically referring to a different place in memory in the other CPU's per-CPU area.
// In particular, this means that there are no restrictions on the type `T`.
unsafe impl<T> Send for PerCpuPtr<T> {}

// SAFETY: Two threads concurrently making use of a [`PerCpuPtr<T>`] will each see the `T` in their
// own per-CPU area, so there's no potential for a data race (regardless of whether `T` is itself
// `Sync`).
unsafe impl<T> Sync for PerCpuPtr<T> {}

impl<T> Clone for PerCpuPtr<T> {
    fn clone(&self) -> Self {
        *self
    }
}

/// [`PerCpuPtr`] is just a wrapper around a pointer.
impl<T> Copy for PerCpuPtr<T> {}

/// A trait representing a per-CPU variable.
///
/// This is implemented for both [`StaticPerCpu<T>`] and [`DynamicPerCpu<T>`]. The main usage of
/// this trait is to call [`Self::get_mut`] to get a [`PerCpuToken`] that can be used to access the
/// underlying per-CPU variable.
///
/// See [`PerCpuToken::with`].
pub trait PerCpu<T> {
    /// Produces a token, asserting that the holder has exclusive access to the underlying memory
    /// pointed to by `self`
    ///
    /// # Safety
    ///
    /// No other [`PerCpuToken`] or [`CheckedPerCpuToken`] may exist on the current CPU (which is a
    /// sensible notion, since we keep a [`CpuGuard`] around) that is derived from the same
    /// [`PerCpu<T>`] or a clone thereof.
    unsafe fn get_mut(&mut self, guard: CpuGuard) -> PerCpuToken<'_, T>;
}

/// A marker trait for types that are interior mutable. Types that implement this trait can be used
/// to create "checked" per-CPU variables. See [`CheckedPerCpu<T>`].
pub trait InteriorMutable {}

impl<T> InteriorMutable for Cell<T> {}
impl<T> InteriorMutable for RefCell<T> {}
impl<T> InteriorMutable for UnsafeCell<T> {}
impl<T> InteriorMutable for Opaque<T> {}

/// A trait representing a per-CPU variable that is usable via a `&T`.
///
/// The unsafety of [`PerCpu<T>`] stems from the fact that the holder of a [`PerCpuToken`] can use
/// it to get a `&mut T` to the underlying per-CPU variable. This is problematic because the
/// existence of aliaising `&mut T` is undefined behavior in Rust. This type avoids that issue by
/// only allowing access via a `&T`, with the tradeoff that then `T` must be interior mutable or
/// the underlying per-CPU variable must be a constant for the lifetime of any corresponding
/// [`CheckedPerCpuToken<T>`].
///
/// Currently, only the case where `T` is interior mutable has first-class support, though a custom
/// implementation of [`PerCpu<T>`]/[`CheckedPerCpu<T>`] could be created for the const case.
pub trait CheckedPerCpu<T>: PerCpu<T> {
    /// Produces a token via which the holder can access the underlying per-CPU variable.
    fn get(&mut self, guard: CpuGuard) -> CheckedPerCpuToken<'_, T>;
}

impl<'a, T> PerCpuToken<'a, T> {
    /// Asserts that the holder has exclusive access to the initialized underlying memory pointed
    /// to by `ptr` on the current CPU, producing a token.
    ///
    /// # Safety
    ///
    /// No other [`PerCpuToken`] or [`CheckedPerCpuToken`] may exist on the current CPU (which is a
    /// sensible notion, since we keep a `CpuGuard` around) that uses the same [`PerCpuPtr<T>`].
    ///
    /// The current CPU's memory location associated with the per-CPU variable pointed at by `ptr`
    /// (i.e., the entry in the per-CPU area on this CPU) must be initialized; `ptr` must be valid
    /// (that is, pointing at a live per-CPU allocation correctly sized and aligned to hold a `T`)
    /// for `'a`.
    pub unsafe fn new(guard: CpuGuard, ptr: &'a PerCpuPtr<T>) -> PerCpuToken<'a, T> {
        Self { _guard: guard, ptr }
    }

    /// Immediately invokes `func` with a `&mut T` that points at the underlying per-CPU variable
    /// that `&mut self` represents.
    pub fn with<U>(&mut self, func: U)
    where
        U: FnOnce(&mut T),
    {
        // SAFETY: The existence of a PerCpuToken means that the requirements for get_mut_ref are
        // satisfied. Likewise, the requirements for assume_init_mut are satisfied because the
        // invariants of this type ensure that on the current CPU (which is a sensible notion
        // because we have a CpuGuard), the memory location pointed to by `ptr` is initialized.
        func(unsafe { self.ptr.get_mut_ref().assume_init_mut() });
    }
}

impl<'a, T> CheckedPerCpuToken<'a, T> {
    /// Asserts that the memory pointed to by `ptr` is initialized on the current CPU, producing a
    /// token.
    ///
    /// # Safety
    ///
    /// The current CPU's memory location associated with the per-CPU variable pointed at by `ptr`
    /// (i.e., the entry in the per-CPU area on this CPU) must be initialized; `ptr` must be valid
    /// (that is, pointing at a live per-CPU allocation correctly sized and aligned to hold a `T`)
    /// for `'a`.
    pub unsafe fn new(guard: CpuGuard, ptr: &'a PerCpuPtr<T>) -> CheckedPerCpuToken<'a, T> {
        Self { _guard: guard, ptr }
    }

    /// Immediately invokes `func` with a `&T` that points at the underlying per-CPU variable that
    /// `&mut self` represents.
    pub fn with<U>(&mut self, func: U)
    where
        U: FnOnce(&T),
    {
        // SAFETY: The existence of a CheckedPerCpuToken means that the requirements for get_ref
        // are satisfied. Likewise, the requirements for assume_init_ref are satisfied because the
        // invariants of this type ensure that on the current CPU (which is a sensible notion
        // because we have a CpuGuard), the memory location pointed to by `ptr` is initialized.
        func(unsafe { self.ptr.get_ref().assume_init_ref() });
    }
}

declare_extern_per_cpu!(this_cpu_off: *mut c_void);
