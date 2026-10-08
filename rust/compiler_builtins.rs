// SPDX-License-Identifier: GPL-2.0

//! Our own `compiler_builtins`.
//!
//! Rust provides [`compiler_builtins`] as a port of LLVM's [`compiler-rt`].
//! Since we do not need the vast majority of them, we avoid the dependency.
//! But `rustc` still needs a `#![compiler_builtins]` crate for every crate it
//! builds, so this crate is provided and left empty. The few builtins that
//! `core` needs are in the `kernel` crate (`rust/kernel/core_builtins.rs`).
//!
//! [`compiler_builtins`]: https://github.com/rust-lang/compiler-builtins
//! [`compiler-rt`]: https://compiler-rt.llvm.org/

#![allow(internal_features)]
#![feature(compiler_builtins)]
#![compiler_builtins]
#![no_builtins]
#![no_std]
