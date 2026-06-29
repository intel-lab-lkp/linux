// SPDX-License-Identifier: GPL-2.0

//! Compile-time configuration selection.
//!
//! This module provides [`cfg_select!`](macro@crate::cfg_select), a backport of the
//! standard library's [`core::cfg_select`] macro (stable since Rust 1.95.0). It selects
//! between several blocks of code based on `cfg` predicates, which is most useful for
//! choosing an implementation depending on a kernel configuration option, i.e. a `CONFIG_*`
//! symbol.
//!
//! Once the kernel's minimum supported Rust version reaches 1.95.0, this module should be
//! deleted and `cfg_select!` taken from the prelude / [`core`] instead. In item and statement
//! position the syntax is identical, so those uses migrate unchanged; see
//! [`cfg_select!`](macro@crate::cfg_select) for the (small) differences that apply in expression
//! position.
//!
//! [`core::cfg_select`]: https://doc.rust-lang.org/core/macro.cfg_select.html

#[doc(inline)]
pub use crate::cfg_select;

/// Selects code at compile time based on `cfg` predicates.
///
/// This is a backport of the standard library's [`core::cfg_select`] macro (stable since Rust
/// 1.95.0). Each arm has the form `PREDICATE => { CODE }`, where `PREDICATE` is any [`cfg`]
/// predicate (for example a `CONFIG_*` symbol). The arms are evaluated top to bottom and the `CODE`
/// of the first arm whose predicate holds is emitted; all other arms are discarded. An optional
/// final `_ => { CODE }` arm is used as a fallback when no predicate matches.
///
/// # Differences from `core::cfg_select`
///
/// The standard library macro is a compiler built-in and can therefore be used in expression
/// position with a single pair of braces. This backport is a `macro_rules!` macro, so two
/// restrictions apply:
///
/// - Arms must be brace-delimited (`PREDICATE => { ... }`); the bare `PREDICATE => EXPR,` form
///   accepted by [`core::cfg_select`] is not supported (a `macro_rules!` macro cannot place
///   `#[cfg]` on a bare expression without the unstable [`stmt_expr_attributes`] feature).
/// - In expression position the whole invocation must be wrapped in an extra pair of braces, i.e.
///   `cfg_select! {{ ... }}`.
///
/// In item and statement position the syntax is identical to [`core::cfg_select`], so those uses
/// will migrate unchanged once the minimum Rust version reaches 1.95.0.
///
/// # Examples
///
/// Item position -- select an implementation based on a `CONFIG` option:
///
/// ```
/// use kernel::cfg_select;
///
/// cfg_select! {
///     CONFIG_64BIT => {
///         fn ptr_bytes() -> usize { 8 }
///     }
///     _ => {
///         fn ptr_bytes() -> usize { 4 }
///     }
/// }
/// # assert!(ptr_bytes() == 8 || ptr_bytes() == 4);
/// ```
///
/// Expression position (note the extra braces):
///
/// ```
/// use kernel::cfg_select;
///
/// let bits = cfg_select! {{
///     CONFIG_64BIT => { 64 }
///     _            => { 32 }
/// }};
/// # assert!(bits == 64 || bits == 32);
/// ```
///
/// [`core::cfg_select`]: https://doc.rust-lang.org/core/macro.cfg_select.html
/// [`cfg`]: https://doc.rust-lang.org/reference/conditional-compilation.html
/// [`stmt_expr_attributes`]: https://doc.rust-lang.org/beta/unstable-book/language-features/stmt-expr-attributes.html
#[macro_export]
#[doc(hidden)]
macro_rules! cfg_select {
    ({ $($tt:tt)* }) => {{
        $crate::cfg_select! { $($tt)* }
    }};
    (_ => { $($output:tt)* }) => {
        $($output)*
    };
    (
        $cfg:meta => $output:tt
        $($( $rest:tt )+)?
    ) => {
        #[cfg($cfg)]
        $crate::cfg_select! { _ => $output }
        $(
            #[cfg(not($cfg))]
            $crate::cfg_select! { $($rest)+ }
        )?
    };
}

#[macros::kunit_tests(rust_kernel_cfg)]
mod tests {
    use super::*;

    #[test]
    fn expr_true_arm_selected() {
        // Expression position (double braces): the first arm is true, so its value wins.
        let v = cfg_select! {{
            all() => { 64 }
            _ => { 32 }
        }};
        assert_eq!(v, 64);
    }

    #[test]
    fn expr_fallback_when_false() {
        // The only predicate is false, so the `_` fallback value is selected.
        let v = cfg_select! {{
            any() => { 64 }
            _ => { 32 }
        }};
        assert_eq!(v, 32);
    }

    #[test]
    fn expr_first_match_wins() {
        // Two true arms: the first one wins, the second one is discarded (top-to-bottom, first
        // match).
        let v = cfg_select! {{
            all() => { 1 }
            all() => { 2 }
            _ => { 3 }
        }};
        assert_eq!(v, 1);
    }

    #[test]
    fn expr_fallthrough_false_to_true() {
        // The false first arm is skipped and selection falls through to the later true arm, not to
        // the `_` fallback.
        let v = cfg_select! {{
            any() => { 1 }
            all() => { 2 }
            _ => { 3 }
        }};
        assert_eq!(v, 2);
    }

    #[test]
    fn expr_no_fallback_true_arm() {
        // No `_` arm: the chain may terminate on a true arm and still emit it.
        let v = cfg_select! {{
            any() => { 1 }
            all() => { 2 }
        }};
        assert_eq!(v, 2);
    }

    #[test]
    fn expr_not_predicate() {
        // `not()` in both polarities: `not(all())` is false (skipped), `not(any())` is true
        // (selected).
        let v = cfg_select! {{
            not(all()) => { 1 }
            not(any()) => { 2 }
            _ => { 3 }
        }};
        assert_eq!(v, 2);
    }

    #[test]
    fn expr_all_predicate() {
        // `all(a, b)`: one false operand makes it false (skipped); all true makes it true
        // (selected).
        let v = cfg_select! {{
            all(all(), any()) => { 1 }
            all(all(), all()) => { 2 }
            _ => { 3 }
        }};
        assert_eq!(v, 2);
    }

    #[test]
    fn expr_any_predicate() {
        // `any(a, b)`: all-false operands make it false (skipped); one true operand makes it true
        // (selected).
        let v = cfg_select! {{
            any(any(), any()) => { 1 }
            any(any(), all()) => { 2 }
            _ => { 3 }
        }};
        assert_eq!(v, 2);
    }

    #[test]
    fn expr_nested_select() {
        // A `cfg_select!` nested inside an output block: the outer picks its true arm, the inner
        // falls back (`any()` is false) to 2.
        let v = cfg_select! {{
            all() => {
                cfg_select! {{
                    any() => { 1 }
                    _ => { 2 }
                }}
            }
            _ => { 3 }
        }};
        assert_eq!(v, 2);
    }

    #[test]
    fn item_true_arm_wins_over_fallback() {
        // Item position: the true arm's `fn` is emitted, the `_` fallback stripped.
        cfg_select! {
            all() => {
                fn pick() -> u32 {
                    1
                }
            }
            _ => {
                fn pick() -> u32 {
                    2
                }
            }
        }
        assert_eq!(pick(), 1);
    }

    #[test]
    fn item_fallback_when_false() {
        // Item position: the only predicate is false, so the `_` fallback supplies the item.
        cfg_select! {
            any() => {
                fn pick() -> u32 {
                    1
                }
            }
            _ => {
                fn pick() -> u32 {
                    2
                }
            }
        }
        assert_eq!(pick(), 2);
    }

    #[test]
    fn item_no_fallback_all_false_emits_nothing() {
        // No `_` fallback and the sole predicate is false: the invocation must expand to nothing.
        // If the false arm were wrongly emitted, the duplicate definition would fail to compile.
        // Compiling at all shows the arm was dropped, and the assert confirms the surviving `pick`
        // is this outer one.
        fn pick() -> u32 {
            5
        }
        cfg_select! {
            any() => {
                fn pick() -> u32 {
                    1
                }
            }
        }
        assert_eq!(pick(), 5);
    }

    #[test]
    fn stmt_true_arm_selected() {
        // Statement position: the `v = 10` assignment is a `#[cfg(all())]`-gated statement, proving
        // the true arm's statements run. `let v;` defers initialization so the single surviving
        // assignment leaves no dead store.
        let v;
        cfg_select! {
            all() => {
                v = 10;
            }
            _ => {
                v = 20;
            }
        }
        assert_eq!(v, 10);
    }

    #[test]
    fn stmt_fallback_when_false() {
        // Statement position with the predicate false: the `_` fallback statement is the one
        // emitted.
        let v;
        cfg_select! {
            any() => {
                v = 1;
            }
            _ => {
                v = 2;
            }
        }
        assert_eq!(v, 2);
    }
}
