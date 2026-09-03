// SPDX-License-Identifier: GPL-2.0

//! Utilities for const evaluation.

use core::{
    ops::Deref, //
};

#[doc(inline)]
pub use build_error::assert_in_const_eval;

#[doc(inline)]
pub use macros::{
    const_call,
    const_eval_only, //
};

/// Constant wrapper type.
///
/// Rust does not yet (as of 1.98) have stable const trait impl support; on 1.85 it does not have
/// unstable support either. Only inherent functions can be marked as const. There are a few const
/// methods that we want to add on core types; and this mean that we cannot use extension trait on
/// them.
///
/// This type serves as a middle layer. This type is local to the `kernel` crate, and thus we can
/// define inherent methods on it. For core types, we will define it for `Const<Type>`. Other kernel
/// crate types or even downstream types can also utilize it by defining inherent methods that
/// *receive* `Const<Self>`.
///
/// Caller should use the `const_call!()` macro so it also checks that the type signature matches
/// the trait.
///
/// # Examples
///
/// Say we want to define a extension method on u32. We can do
/// ```no_run
/// trait MyTrait {
///     fn trait_method(self);
/// }
///
/// impl MyTrait for u32 {
///     fn trait_method(self) {
///         /* impl */
///     }
/// }
/// ```
/// but we cannot mark it const.
///
/// Instead, we can do this
/// ```ignore (doctest is outside kernel crate)
/// trait MyTrait {
///     fn trait_method(self);
/// }
///
/// impl MyTrait for u32 {
///     #[inline]
///     fn trait_method(self) {
///         // Forwarding impl
///         Const(self).trait_method()
///     }
/// }
///
/// impl Const<u32> {
///     pub const fn trait_method(self) {
///         let Const(this) = self;
///         /* impl */
///     }
/// }
/// ```
///
/// For local or downstream types, implement it directly on the type with a different receiver:
/// ```no_run
/// # use kernel::const_eval::Const;
/// trait MyTrait {
///     fn trait_method(self);
/// }
///
/// struct Foo;
///
/// impl MyTrait for Foo {
///     #[inline]
///     fn trait_method(self) {
///         // Forwarding impl
///         Const(self).trait_method()
///     }
/// }
///
/// impl Foo {
///     pub const fn trait_method(self: Const<Self>) {
///         let Const(this) = self;
///         /* impl */
///     }
/// }
/// ```
///
/// For caller of the method, one would simply replace `expr.method()` with `Const(expr).method()`;
/// although the auto-ref coercion will be lost, so it a method expects `&self`, the caller would
/// need to explicitly use the `Const(&expr).method()` syntax to call it.
pub struct Const<T>(pub T);

impl<T> Deref for Const<T> {
    type Target = T;

    #[inline]
    fn deref(&self) -> &T {
        &self.0
    }
}

// Provide inference help only. Should never be code-generated.
#[doc(hidden)]
#[const_eval_only]
pub const fn would_call<T, U, F: FnOnce(T) -> U>(_: T, _: F) -> U {
    todo!()
}
