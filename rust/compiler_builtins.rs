// SPDX-License-Identifier: GPL-2.0

//! Our own `compiler_builtins`.
//!
//! Rust provides [`compiler_builtins`] as a port of LLVM's [`compiler-rt`].
//! Since we do not need the vast majority of them, we avoid the dependency
//! by providing this file.
//!
//! At the moment, some builtins are required that should not be. For instance,
//! [`core`] has 128-bit integers functionality which we should not be compiling
//! in. We will work with upstream [`core`] to provide feature flags to disable
//! the parts we do not need. For the moment, we define them to print an error
//! and call `BUG()` at runtime to catch mistakes, instead of performing surgery
//! on `core.o`.
//!
//! In any case, all these symbols are weakened to ensure we do not override
//! those that may be provided by the rest of the kernel.
//!
//! [`compiler_builtins`]: https://github.com/rust-lang/compiler-builtins
//! [`compiler-rt`]: https://compiler-rt.llvm.org/

#![allow(internal_features)]
#![feature(compiler_builtins)]
#![compiler_builtins]
#![no_builtins]
#![no_std]

unsafe extern "C" {
    #[cfg(CONFIG_PRINTK)]
    fn _printk(fmt: *const u8, ...) -> core::ffi::c_int;
    fn rust_helper_BUG() -> !;
}

// In a `#![compiler_builtins]` crate, code cannot link to functions in other
// crates. `rustc` rejects such calls, except that it silently turns the ones
// that do not return (e.g. `panic!`) into a trap.
#[cold]
#[inline(never)]
#[cfg_attr(not(CONFIG_PRINTK), allow(unused_variables))]
fn intrinsic_called(name: &str, reason: &str) -> ! {
    #[cfg(CONFIG_PRINTK)]
    // SAFETY: The format string is NUL-terminated. Each `%.*s` takes a
    // `c_int` length and a pointer. `name.as_ptr()` is valid for reads of
    // `name.len()` bytes, and so is `reason.as_ptr()` for `reason.len()`
    // bytes.
    unsafe {
        _printk(
            // "\x010" is `KERN_EMERG`.
            c"\x010%.*s called: %.*s\n".to_bytes_with_nul().as_ptr(),
            name.len() as core::ffi::c_int,
            name.as_ptr(),
            reason.len() as core::ffi::c_int,
            reason.as_ptr(),
        )
    };

    // SAFETY: FFI call.
    unsafe { rust_helper_BUG() }
}

macro_rules! define_panicking_intrinsics(
    ($reason: tt, { $($ident: ident, )* }) => {
        $(
            #[doc(hidden)]
            #[export_name = concat!("__rust", stringify!($ident))]
            pub extern "C" fn $ident() {
                intrinsic_called(stringify!($ident), $reason)
            }
        )*
    }
);

define_panicking_intrinsics!("`f32` should not be used", {
    __addsf3,
    __eqsf2,
    __extendsfdf2,
    __gesf2,
    __lesf2,
    __ltsf2,
    __mulsf3,
    __nesf2,
    __truncdfsf2,
    __unordsf2,
});

define_panicking_intrinsics!("`f64` should not be used", {
    __adddf3,
    __eqdf2,
    __ledf2,
    __ltdf2,
    __muldf3,
    __unorddf2,
});

define_panicking_intrinsics!("`i128` should not be used", {
    __ashrti3,
    __muloti4,
    __multi3,
});

define_panicking_intrinsics!("`u128` should not be used", {
    __ashlti3,
    __lshrti3,
    __udivmodti4,
    __udivti3,
    __umodti3,
});

#[cfg(target_arch = "arm")]
define_panicking_intrinsics!("`f32` should not be used", {
    __aeabi_fadd,
    __aeabi_fmul,
    __aeabi_fcmpeq,
    __aeabi_fcmple,
    __aeabi_fcmplt,
    __aeabi_fcmpun,
});

#[cfg(target_arch = "arm")]
define_panicking_intrinsics!("`f64` should not be used", {
    __aeabi_dadd,
    __aeabi_dmul,
    __aeabi_dcmple,
    __aeabi_dcmplt,
    __aeabi_dcmpun,
});

#[cfg(target_arch = "arm")]
define_panicking_intrinsics!("`u64` division/modulo should not be used", {
    __aeabi_uldivmod,
});

#[cfg(target_arch = "powerpc")]
define_panicking_intrinsics!("`u64` division/modulo should not be used", {
    __udivdi3,
    __umoddi3,
});

// NOTE: if you are adding a new intrinsic here, you should also add it to
// `redirect-intrinsics` in `rust/Makefile`.
