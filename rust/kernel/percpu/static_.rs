// SPDX-License-Identifier: GPL-2.0
//! Statically allocated per-CPU variables.

use super::*;

/// A wrapper used for declaring static per-CPU variables. These symbols are "virtual" in that the
/// linker uses them to generate offsets into each CPU's per-CPU area, but shouldn't be read
/// from/written to directly. The fact that the statics are immutable prevents them being written
/// to (generally), this struct having _val be non-public prevents reading from them.
///
/// The end-user of the per-CPU API should make use of the define_per_cpu! macro instead of
/// declaring variables of this type directly. All instances of this type must be `static` and
/// `#[link_section = ".data..percpu"]` (which the macro handles).
#[repr(transparent)]
pub struct StaticPerCpuSymbol<T> {
    _val: T, // generate a correctly sized type
}

/// Holds a statically-allocated per-CPU variable.
#[derive(Clone)]
pub struct StaticPerCpu<T>(pub(super) PerCpuPtr<T>);

impl<T> StaticPerCpu<T> {
    /// Creates a `StaticPerCpu<T>` from a `StaticPerCpuSymbol<T>`. You should probably be using
    /// `get_static_per_cpu!` instead.
    pub fn new(ptr: *const StaticPerCpuSymbol<T>) -> StaticPerCpu<T> {
        // SAFETY: `StaticPerCpuSymbol<T>` is `#[repr(transparent)]`, so we can safely cast a
        // pointer to it into a pointer to `MaybeUninit<T>`. The validity of it as a per-CPU
        // pointer is guaranteed by the per-CPU subsystem and invariants of the StaticPerCpuSymbol
        // type.
        let pcpu_ptr = unsafe { PerCpuPtr::new(ptr.cast_mut().cast()) };
        Self(pcpu_ptr)
    }
}

impl<T> PerCpu<T> for StaticPerCpu<T> {
    unsafe fn get_mut(&mut self, guard: CpuGuard) -> PerCpuToken<'_, T> {
        // SAFETY: The requirements of `PerCpu::get_mut` and the fact that statically-allocated
        // per-CPU variables are initialized by the per-CPU subsystem ensure that the requirements
        // of `PerCpuToken::new` are met.
        unsafe { PerCpuToken::new(guard, &self.0) }
    }
}

impl<T: InteriorMutable> CheckedPerCpu<T> for StaticPerCpu<T> {
    fn get(&mut self, guard: CpuGuard) -> CheckedPerCpuToken<'_, T> {
        // SAFETY: The per-CPU subsystem guarantees that each CPU's instance of a
        // statically allocated variable begins with a copy of the contents of the
        // corresponding symbol in `.data..percpu`. Thus, the requirements of
        // `CheckedPerCpuToken::new` are met.
        unsafe { CheckedPerCpuToken::new(guard, &self.0) }
    }
}

/// Gets a `StaticPerCpu<T>` from a symbol declared with `define_per_cpu!` or
/// `declare_extern_per_cpu!`.
///
/// # Arguments
/// * `ident` - The identifier declared
#[macro_export]
macro_rules! get_static_per_cpu {
    ($id:ident) => {
        $crate::percpu::StaticPerCpu::new((&raw const $id).cast())
    };
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
            // We might want to have a per-CPU variable that doesn't implement `Sync` (not paying
            // sync overhead costs is part of the point), but Rust won't let us declare a static of
            // a `!Sync` type. Of course, we don't actually have any synchronization issues, since
            // each CPU will see its own copy of the variable, so we cheat a little bit and tell
            // Rust it's fine.
            #[doc(hidden)]
            #[allow(non_camel_case_types)]
            #[repr(transparent)] // It needs to be the same size as $ty
            struct [<__PRIVATE_TYPE_ $id>]($ty);

            impl [<__PRIVATE_TYPE_ $id>] {
                #[doc(hidden)]
                const fn new(val: $ty) -> Self {
                    Self(val)
                }
            }

            // Expand $expr outside of the unsafe block to avoid silently allowing unsafe code to be
            // used without a user-facing unsafe block
            #[doc(hidden)]
            static [<__INIT_ $id>]: [<__PRIVATE_TYPE_ $id>] = [<__PRIVATE_TYPE_ $id>]::new($expr);

            // SAFETY: This type will ONLY ever be used to declare a `StaticPerCpuSymbol`
            // (which we then only ever use as input to `&raw`). Reading from the symbol is
            // already UB, so we won't ever actually have any variables of this type where
            // synchronization is a concern.
            #[doc(hidden)]
            unsafe impl Sync for [<__PRIVATE_TYPE_ $id>] {}

            // SAFETY: StaticPerCpuSymbol<T> is #[repr(transparent)], so we can freely convert from
            // [<__PRIVATE_TYPE_ $id>], which is also `#[repr(transparent)]` (i.e., everything is
            // just a `$ty` from a memory layout perspective).
            #[link_section = ".data..percpu"]
            $vis static $id: StaticPerCpuSymbol<[<__PRIVATE_TYPE_ $id>]> = unsafe {
                core::mem::transmute_copy::<
                    [<__PRIVATE_TYPE_ $id>], StaticPerCpuSymbol<[<__PRIVATE_TYPE_ $id>]>
                >(&[<__INIT_ $id>])
            };
        }
    };
}
