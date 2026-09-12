// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2024, 2025 FUJITA Tomonori <fujita.tomonori@gmail.com>

//! Support for BUG and WARN functionality.
//!
//! C header: [`include/asm-generic/bug.h`](srctree/include/asm-generic/bug.h)

#[macro_export]
#[doc(hidden)]
#[cfg(not(testlib))]
#[cfg(all(CONFIG_BUG, not(CONFIG_UML), not(CONFIG_LOONGARCH), not(CONFIG_ARM)))]
#[cfg(CONFIG_DEBUG_BUGVERBOSE)]
macro_rules! warn_flags {
    ($file:expr, $flags:expr) => {
        const FLAGS: u32 = $crate::bindings::BUGFLAG_WARNING | $flags;
        const _FILE: &[u8] = $file.as_bytes();
        // Plus one for null-terminator.
        static FILE: [u8; _FILE.len() + 1] = {
            let mut bytes = [0; _FILE.len() + 1];
            let mut i = 0;
            while i < _FILE.len() {
                bytes[i] = _FILE[i];
                i += 1;
            }
            bytes
        };

        // SAFETY:
        // - `file`, `line`, `flags`, and `size` are all compile-time constants or
        // symbols, preventing any invalid memory access.
        // - The asm block has no side effects and does not modify any registers
        // or memory. It is purely for embedding metadata into the ELF section.
        unsafe {
            $crate::asm!(
                concat!(
                    "/* {size} */",
                    include!(concat!(env!("OBJTREE"), "/rust/kernel/generated_arch_warn_asm.rs")),
                    include!(concat!(env!("OBJTREE"), "/rust/kernel/generated_arch_reachable_asm.rs")));
                file = sym FILE,
                line = const line!(),
                flags = const FLAGS,
                size = const ::core::mem::size_of::<$crate::bindings::bug_entry>(),
            );
        }
    }
}

#[macro_export]
#[doc(hidden)]
#[cfg(not(testlib))]
#[cfg(all(CONFIG_BUG, not(CONFIG_UML), not(CONFIG_LOONGARCH), not(CONFIG_ARM)))]
#[cfg(not(CONFIG_DEBUG_BUGVERBOSE))]
macro_rules! warn_flags {
    ($file:expr, $flags:expr) => {
        const FLAGS: u32 = $crate::bindings::BUGFLAG_WARNING | $flags;

        if false {
            _ = $file;
        }

        // SAFETY:
        // - `flags` and `size` are all compile-time constants, preventing
        // any invalid memory access.
        // - The asm block has no side effects and does not modify any registers
        // or memory. It is purely for embedding metadata into the ELF section.
        unsafe {
            $crate::asm!(
                concat!(
                    "/* {size} */",
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
#[cfg(not(testlib))]
#[cfg(all(CONFIG_BUG, CONFIG_UML))]
macro_rules! warn_flags {
    ($file:expr, $flags:expr) => {
        if false {
            _ = $file;
        }

        // SAFETY: It is always safe to call `warn_slowpath_fmt()`
        // with a valid null-terminated string.
        unsafe {
            $crate::bindings::warn_slowpath_fmt(
                $crate::str::CStrExt::as_char_ptr($crate::c_str!(::core::file!())),
                line!() as $crate::ffi::c_int,
                $flags as $crate::ffi::c_uint,
                ::core::ptr::null(),
            );
        }
    };
}

#[macro_export]
#[doc(hidden)]
#[cfg(not(testlib))]
#[cfg(all(CONFIG_BUG, any(CONFIG_LOONGARCH, CONFIG_ARM)))]
macro_rules! warn_flags {
    ($file:expr, $flags:expr) => {
        if false {
            _ = $file;
            _ = $flags;
        }

        // SAFETY: It is always safe to call `WARN_ON()`.
        unsafe { $crate::bindings::WARN_ON(true) }
    };
}

#[macro_export]
#[doc(hidden)]
#[cfg(any(testlib, not(CONFIG_BUG)))]
macro_rules! warn_flags {
    ($file:expr, $flags:expr) => {
        if false {
            _ = $file;
            _ = $flags;
        }
    };
}

#[doc(hidden)]
pub const fn bugflag_taint(value: u32) -> u32 {
    value << 8
}

/// Report a warning if `cond` is true and return the condition's evaluation result.
#[macro_export]
macro_rules! warn_on {
    ($cond:expr) => {{
        let cond = $cond;

        #[cfg(CONFIG_DEBUG_BUGVERBOSE_DETAILED)]
        const COND_STR: &str = concat!("[", stringify!($cond), "] ", file!());
        #[cfg(not(CONFIG_DEBUG_BUGVERBOSE_DETAILED))]
        const COND_STR: &str = file!();

        if cond {
            const WARN_ON_FLAGS: u32 = $crate::bug::bugflag_taint($crate::bindings::TAINT_WARN);

            $crate::warn_flags!(COND_STR, WARN_ON_FLAGS);
        }
        cond
    }};
}

// Test-only constants and file static referenced by the global_asm block below.
//
// global_asm! is file-scope and always emitted — LLVM cannot eliminate it,
// unlike asm! inside a function which is subject to dead-code removal.
//
// BUG_KUNIT_TRAP_ADDR is declared as a .global symbol entirely inside the
// global_asm! block so the .dc.a 1b relocation lands directly on it.
// A Rust static initialized to zero would end up in BSS; the linker does
// not apply relocations to BSS, so the address would stay zero at runtime.
#[cfg(CONFIG_RUST_BUG_POWERPC_KUNIT_TEST)]
mod test_statics {
    use crate::bindings::{bug_entry, BUGFLAG_WARNING, TAINT_WARN};

    pub(super) const FLAGS: u32 = BUGFLAG_WARNING | (TAINT_WARN << 8);
    pub(super) const SIZE: usize = core::mem::size_of::<bug_entry>();
    pub(super) const LINE: u32 = line!();

    // Null-terminated source file name — the assembler references this symbol
    // for the verbose file pointer in __bug_table, same as warn_flags!.
    const _FILE: &[u8] = file!().as_bytes();
    #[no_mangle]
    pub(super) static BUG_KUNIT_FILE: [u8; _FILE.len() + 1] = {
        let mut bytes = [0u8; _FILE.len() + 1];
        let mut i = 0;
        while i < _FILE.len() {
            bytes[i] = _FILE[i];
            i += 1;
        }
        bytes
    };
}

// Emit ARCH_WARN_ASM at file scope and capture the trap address.
//
// BUG_KUNIT_TRAP_ADDR is defined as a .global symbol right on top of the
// .dc.a 1b directive so the linker resolves the relocation directly into
// that symbol's storage — no BSS, no zero-init problem.
// .dc.a emits a pointer-width word (4 bytes on ppc32, 8 bytes on ppc64),
// matching the usize declaration on the Rust side.
#[cfg(all(CONFIG_RUST_BUG_POWERPC_KUNIT_TEST, CONFIG_PPC64))]
::core::arch::global_asm!(
    concat!(
        include!(concat!(env!("OBJTREE"), "/rust/kernel/generated_arch_warn_asm.rs")),
        ".pushsection .data\n\t",
        ".balign 8\n\t",
        ".global BUG_KUNIT_TRAP_ADDR\n\t",
        "BUG_KUNIT_TRAP_ADDR:\n\t",
        ".dc.a 1b\n\t",
        ".popsection\n",
    ),
    file  = sym test_statics::BUG_KUNIT_FILE,
    line  = const test_statics::LINE,
    flags = const test_statics::FLAGS,
    size  = const test_statics::SIZE,
);

#[cfg(all(CONFIG_RUST_BUG_POWERPC_KUNIT_TEST, not(CONFIG_PPC64)))]
::core::arch::global_asm!(
    concat!(
        include!(concat!(env!("OBJTREE"), "/rust/kernel/generated_arch_warn_asm.rs")),
        ".pushsection .data\n\t",
        ".balign 4\n\t",
        ".global BUG_KUNIT_TRAP_ADDR\n\t",
        "BUG_KUNIT_TRAP_ADDR:\n\t",
        ".dc.a 1b\n\t",
        ".popsection\n",
    ),
    file  = sym test_statics::BUG_KUNIT_FILE,
    line  = const test_statics::LINE,
    flags = const test_statics::FLAGS,
    size  = const test_statics::SIZE,
);

#[cfg(CONFIG_RUST_BUG_POWERPC_KUNIT_TEST)]
#[::kernel::macros::kunit_tests(rust_kernel_bug_powerpc)]
mod tests {
    use crate::bindings;

    fn trap_addr() -> usize {
        // BUG_KUNIT_TRAP_ADDR is a .global symbol defined in the global_asm!
        // block above, placed in .data at the exact .dc.a 1b relocation word.
        // The linker resolves it to the virtual address of the twi instruction
        // before any Rust code runs, so reading it here is always safe.
        extern "C" {
            // .dc.a emits a pointer-width word: 4 bytes on ppc32, 8 on ppc64.
            // usize matches the native pointer width on both.
            static BUG_KUNIT_TRAP_ADDR: usize;
        }
        // SAFETY: read-only after link time, no concurrent mutation possible.
        unsafe { BUG_KUNIT_TRAP_ADDR }
    }

    /// The `__bug_table` entry emitted by `ARCH_WARN_ASM` must be locatable
    /// via `find_bug()` using the trap instruction's address.  A NULL result
    /// means the `1b` label reference in `_EMIT_BUG_ENTRY` resolved to the
    /// wrong address and the real trap handler would not recognise the site.
    #[test]
    fn bug_entry_found() {
        // Non-zero proves the .dc.a relocation was resolved by the linker.
        assert!(trap_addr() != 0);

        // SAFETY: find_bug() is always safe to call with any address; it
        // simply walks __bug_table and returns NULL if nothing matches.
        let entry = unsafe { bindings::find_bug(trap_addr()) };
        // Non-NULL proves the bug_addr displacement in _EMIT_BUG_ENTRY is correct.
        assert!(!entry.is_null());
    }

    /// The emitted entry must be flagged as a warning (not a hard BUG).
    #[test]
    fn bug_entry_is_warning() {
        assert!(trap_addr() != 0);
        let entry = unsafe { bindings::find_bug(trap_addr()) };
        assert!(!entry.is_null());
        // SAFETY: entry is non-null and points to a valid bug_entry.
        let flags = unsafe { (*entry).flags } as u32;
        assert!(flags & bindings::BUGFLAG_WARNING != 0);
    }

    /// With `CONFIG_DEBUG_BUGVERBOSE` the entry must record a non-null file
    /// pointer pointing back into this source file.
    #[test]
    #[cfg(CONFIG_DEBUG_BUGVERBOSE)]
    fn bug_entry_file() {
        use core::ffi::CStr;

        assert!(trap_addr() != 0);
        let entry = unsafe { bindings::find_bug(trap_addr()) };
        assert!(!entry.is_null());

        let mut file_ptr: *const core::ffi::c_char = core::ptr::null();
        let mut line: u32 = 0;
        // SAFETY: entry is non-null and valid; file_ptr and line are local
        // variables passed as out-parameters.
        unsafe { bindings::bug_get_file_line(entry, &mut file_ptr, &mut line) };

        assert!(!file_ptr.is_null());
        // SAFETY: file_ptr is a null-terminated C string from BUG_KUNIT_FILE.
        let file_str = unsafe { CStr::from_ptr(file_ptr) }.to_str().unwrap_or("");
        assert!(file_str.contains("bug"));
    }

    /// With `CONFIG_DEBUG_BUGVERBOSE` the recorded line number must be
    /// non-zero (a zero line would mean the asm operand was not substituted).
    #[test]
    #[cfg(CONFIG_DEBUG_BUGVERBOSE)]
    fn bug_entry_line() {
        assert!(trap_addr() != 0);
        let entry = unsafe { bindings::find_bug(trap_addr()) };
        assert!(!entry.is_null());

        let mut file_ptr: *const core::ffi::c_char = core::ptr::null();
        let mut line: u32 = 0;
        // SAFETY: entry is non-null and valid.
        unsafe { bindings::bug_get_file_line(entry, &mut file_ptr, &mut line) };

        assert!(line != 0);
    }

    /// The trap address stored in `__bug_table` must lie within the kernel
    /// text segment.  If the label reference in `_EMIT_BUG_ENTRY` resolved
    /// to data or zero, `kernel_text_address()` would return false.
    #[test]
    fn bug_entry_addr_is_in_text() {
        assert!(trap_addr() != 0);
        // SAFETY: kernel_text_address() is always safe to call with any addr.
        let in_text = unsafe { bindings::kernel_text_address(trap_addr()) };
        assert!(in_text != 0);
    }
}
