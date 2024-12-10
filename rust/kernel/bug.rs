// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2024 FUJITA Tomonori

//! Support for BUG_* and WARN_* functionality.
//!
//! C header: [`include/asm-generic/bug.h`](srctree/include/asm-generic/bug.h)

#[macro_export]
#[doc(hidden)]
#[cfg(all(CONFIG_BUG, not(CONFIG_UML)))]
#[cfg(any(target_arch = "x86_64", target_arch = "riscv64"))]
macro_rules! warn_flags {
    ($flags:expr) => {
        const FLAGS: u32 = $crate::bindings::BUGFLAG_WARNING | $flags;
        // SAFETY: Just an FFI call.
        #[cfg(CONFIG_DEBUG_BUGVERBOSE)]
        unsafe {
            $crate::asm!(concat!(
                ".pushsection .rodata.str1.1, \"aMS\",@progbits, 1\n",
                "111:\t .string ", "\"", file!(), "\"\n",
                ".popsection\n",
                include!(concat!(env!("OBJTREE"), "/rust/kernel/generated_arch_warn_asm.rs")),
                include!(concat!(env!("OBJTREE"), "/rust/kernel/generated_arch_reachable_asm.rs")));
            line = const line!(),
            flags = const FLAGS,
            size = const ::core::mem::size_of::<$crate::bindings::bug_entry>(),
            );
        }
        // SAFETY: Just an FFI call.
        #[cfg(not(CONFIG_DEBUG_BUGVERBOSE))]
        unsafe {
            $crate::asm!(
            concat!(
                include!(concat!(env!("OBJTREE"), "/rust/kernel/generated_arch_warn_asm.rs")),
                include!(concat!(env!("OBJTREE"), "/rust/kernel/generated_arch_reachable_asm.rs")));
            flags = const FLAGS,
            size = const ::core::mem::size_of::<$crate::bindings::bug_entry>(),
            );
        }
    }
}

#[macro_export]
#[doc(hidden)]
#[cfg(all(CONFIG_BUG, not(CONFIG_UML)))]
#[cfg(any(target_arch = "aarch64", target_arch = "loongarch64"))]
macro_rules! warn_flags {
    ($flags:expr) => {
        const FLAGS: u32 = $crate::bindings::BUGFLAG_WARNING | $flags;
        // SAFETY: Just an FFI call.
        #[cfg(CONFIG_DEBUG_BUGVERBOSE)]
        unsafe {
            $crate::asm!(concat!(
                ".pushsection .rodata.str1.1, \"aMS\",@progbits, 1\n",
                "111:\t .string ", "\"", file!(), "\"\n",
                ".popsection\n",
                include!(concat!(env!("OBJTREE"), "/rust/kernel/generated_arch_warn_asm.rs")),
                include!(concat!(env!("OBJTREE"), "/rust/kernel/generated_arch_reachable_asm.rs")));
            line = const line!(),
            flags = const FLAGS,
            );
        }
        // SAFETY: Just an FFI call.
        #[cfg(not(CONFIG_DEBUG_BUGVERBOSE))]
        unsafe {
            $crate::asm!(concat!(
                include!(concat!(env!("OBJTREE"), "/rust/kernel/generated_arch_warn_asm.rs")),
                include!(concat!(env!("OBJTREE"), "/rust/kernel/generated_arch_reachable_asm.rs")));
            flags = const FLAGS,
            );
        }
    }
}

#[macro_export]
#[doc(hidden)]
#[cfg(all(CONFIG_BUG, CONFIG_UML))]
macro_rules! warn_flags {
    ($flags:expr) => {
        // SAFETY: Just an FFI call.
        unsafe {
            $crate::bindings::warn_slowpath_fmt(
                $crate::c_str!(::core::file!()).as_ptr() as *const ::core::ffi::c_char,
                line!() as i32,
                $flags as u32,
                ::core::ptr::null() as *const ::core::ffi::c_char,
            );
        }
    };
}

#[macro_export]
#[doc(hidden)]
#[cfg(not(CONFIG_BUG))]
macro_rules! warn_flags {
    ($flags:expr) => {};
}

#[doc(hidden)]
#[macro_export]
macro_rules! bugflag_taint {
    ($taint:expr) => {
        $taint << 8
    };
}

/// Report a warning only once.
#[macro_export]
macro_rules! warn_on_once {
    ($cond:expr) => {
        if $cond {
            $crate::warn_flags!(
                $crate::bindings::BUGFLAG_ONCE
                    | $crate::bugflag_taint!($crate::bindings::TAINT_WARN)
            );
        }
        $cond
    };
}

/// Report a warning.
#[macro_export]
macro_rules! warn_on {
    ($cond:expr) => {
        if $cond {
            $crate::warn_flags!($crate::bugflag_taint!($crate::bindings::TAINT_WARN));
        }
        $cond
    };
}
