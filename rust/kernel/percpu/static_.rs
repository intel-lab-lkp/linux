// SPDX-License-Identifier: GPL-2.0
//! Statically allocated per-CPU variables.

use super::*;

/// A wrapper used for declaring static per-CPU variables.
///
/// These symbols are "virtual" in that the linker uses them to generate offsets into each CPU's
/// per-CPU area, but shouldn't be read from/written to directly. The fact that the statics are
/// immutable prevents them being written to (generally), this struct having _val be non-public
/// prevents reading from them.
///
/// The end-user of the per-CPU API should make use of the [`crate::define_per_cpu!`] macro instead
/// of declaring variables of this type directly. All instances of this type must be `static` and
/// `#[link_section = ".data..percpu"]` (which the macro handles).
#[repr(transparent)]
pub struct StaticPerCpuSymbol<T> {
    _val: T, // generate a correctly sized type
}

/// A wrapper for per-CPU variables declared in C.
///
/// As with [`StaticPerCpuSymbol`], this type should not be used directly. Instead, use the
/// [`crate::declare_extern_per_cpu!`] macro.
#[repr(transparent)]
pub struct ExternStaticPerCpuSymbol<T>(StaticPerCpuSymbol<T>);

/// Holds a statically-allocated per-CPU variable.
#[derive(Clone)]
pub struct StaticPerCpu<T>(pub(super) PerCpuPtr<T>);

impl<T: PerCpuSyncMarkerType> StaticPerCpuSymbol<T> {
    /// Removes the per-CPU marker type from a static per-CPU symbol.
    ///
    /// To declare a static per-CPU variable, Rust requires that the variable be `Sync`. However,
    /// in the case of per-CPU variables, this is silly. See [`PerCpuSyncMarkerType`]. Thus, our
    /// statics are actually of type `StaticPerCpuSymbol<PerCpuSyncMarkerType<T>>`, and this
    /// produces a `StaticPerCpuSymbol<T>`.
    pub fn forward(ptr: *const Self) -> *const StaticPerCpuSymbol<T::Inner> {
        ptr.cast()
    }
}

impl<T> ExternStaticPerCpuSymbol<T> {
    /// Gets a [`PerCpuPtr`] to the per-CPU variable.
    ///
    /// Usage of this [`PerCpuPtr`] must be very careful to keep in mind what's happening in C. In
    /// particular, the C code cannot modify the variable while any reference derived from the
    /// returned pointer is live.
    pub fn ptr(ptr: *const Self) -> PerCpuPtr<T> {
        // These casts are OK because, ExternStaticPerCpuSymbol, StaticPerCpuSymbol, and
        // MaybeUninit are transparent, everything is just a `T` from a memory layout perspective.
        // Casting to `mut` is OK because any usage of the returned pointer must satisfy the
        // soundness requirements for using it as such.
        PerCpuPtr::new(ptr.cast::<StaticPerCpuSymbol<T>>().cast_mut().cast())
    }
}

impl<T> StaticPerCpu<T> {
    /// Creates a [`StaticPerCpu<T>`] from a [`StaticPerCpuSymbol<T>`].
    ///
    /// Users should probably declare static per-CPU variables with [`crate::define_per_cpu!`] and
    /// then get instances of [`StaticPerCpu`] using [`crate::get_static_per_cpu!`].
    ///
    /// # Safety
    /// You should probably be using [`crate::get_static_per_cpu!`] instead.
    ///
    /// `ptr` must be a valid offset into the per-CPU area, sized and aligned for access to a `T`;
    /// typically this is the address of a static in the `.data..percpu` section, which is managed
    /// by the per-CPU subsystem.
    pub unsafe fn new(ptr: *const StaticPerCpuSymbol<T>) -> StaticPerCpu<T> {
        let pcpu_ptr = PerCpuPtr::new(ptr.cast_mut().cast());
        Self(pcpu_ptr)
    }
}

impl<T> PerCpu<T> for StaticPerCpu<T> {
    unsafe fn get_mut(&mut self, guard: CpuGuard) -> PerCpuToken<'_, T> {
        // SAFETY:
        // 1. By the requirements of `PerCpu::get_mut`, no other `[Checked]PerCpuToken` exists on
        //    the current CPU.
        // 2. The per-CPU subsystem guarantees that each CPU's instance of a statically allocated
        //    variable begins with a copy of the contents of the corresponding symbol in
        //    `.data..percpu` and is therefore initialized.
        // 3. The per-CPU subsystem guarantees that `self.0` is correctly aligned for a `T`.
        // 4. The per-CPU subsystem guarantees that `self.0` is lives forever as a per-CPU
        //    allocation, and that this allocation is the proper size for a `T`.
        unsafe { PerCpuToken::new(guard, &self.0) }
    }
}

impl<T: InteriorMutable> CheckedPerCpu<T> for StaticPerCpu<T> {
    fn get(&mut self, guard: CpuGuard) -> CheckedPerCpuToken<'_, T> {
        // SAFETY:
        // 1. The per-CPU subsystem guarantees that each CPU's instance of a statically allocated
        //    variable begins with a copy of the contents of the corresponding symbol in
        //    `.data..percpu` and is therefore initialized.
        // 2. The per-CPU subsystem guarantees that `self.0` is correctly aligned for a `T`.
        // 3. The per-CPU subsystem guarantees that `self.0` is lives forever as a per-CPU
        //    allocation, and that this allocation is the proper size for a `T`.
        unsafe { CheckedPerCpuToken::new(guard, &self.0) }
    }
}

/// Gets a [`StaticPerCpu<T>`] from a symbol declared with [`crate::define_per_cpu!`].
///
/// # Arguments
///
/// * `ident` - The identifier declared
#[macro_export]
macro_rules! get_static_per_cpu {
    ($id:ident) => {
        unsafe {
            // SAFETY: The signature of `StaticPerCpuSymbol::forward` guarantees that `&raw const
            // $id` is a `*const StaticPerCpuSymbol<PerCpuSyncMarkerType<T>>` if the macro
            // invocation compiles.
            //
            // Values of type `StaticPerCpuSymbol<T>` must be created via `define_per_cpu`, and so
            // the per-CPU subsystem guarantees that the requirements for `StaticPerCpu::new` are
            // satisfied.
            $crate::percpu::StaticPerCpu::new($crate::percpu::StaticPerCpuSymbol::forward(
                &raw const $id,
            ))
        }
    };
}

/// Declares an [`ExternStaticPerCpuSymbol`] corresponding to a per-CPU variable defined in C.
#[macro_export]
macro_rules! declare_extern_per_cpu {
    ($id:ident: $ty:ty) => {
        extern "C" {
            static $id: ExternStaticPerCpuSymbol<$ty>;
        }
    };
}

/// A trait implemented by static per-CPU wrapper types.
///
/// Internally, static per-CPU variables are declared as `static` variables. However, Rust doesn't
/// allow you to declare statics of a `!Sync` type. This trait is implemented by the marker type
/// that is declared as `Sync` and used to declare the static. See [`crate::define_per_cpu!`] for
/// the gory details.
///
/// # Safety
///
/// Implementations must be `#[repr(transparent)]` wrappers around an `Inner`. This trait should
/// only be used from the [`crate::define_per_cpu!`] macro.
pub unsafe trait PerCpuSyncMarkerType {
    /// The "true" type of the per-CPU variable. It must always be valid to cast a `*const Self` to
    /// a `*const Self::Inner`, which is guarnateed by this trait's safety requirement.
    type Inner;
}

/// Declares and initializes a static per-CPU variable.
///
/// Analogous to the C `DEFINE_PER_CPU` macro.
///
/// See also [`crate::get_static_per_cpu!`] for how to get a [`StaticPerCpu`] from this
/// declaration.
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

            // SAFETY: [<__PRIVATE_TYPE_ $id>] is a `#[repr(transparent)]` wrapper around a `$ty`.
            unsafe impl PerCpuSyncMarkerType for [<__PRIVATE_TYPE_ $id>] {
                type Inner = $ty;
            }

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
