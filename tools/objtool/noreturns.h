/* SPDX-License-Identifier: GPL-2.0 */

/*
 * This is a (sorted!) list of all known __noreturn functions in the kernel.
 * It's needed for objtool to properly reverse-engineer the control flow graph.
 *
 * Yes, this is unfortunate.  A better solution is in the works.
 */
NORETURN(__fortify_panic)
NORETURN(__kunit_abort)
NORETURN(__module_put_and_kthread_exit)
NORETURN(__stack_chk_fail)
NORETURN(__ubsan_handle_builtin_unreachable)
NORETURN(abort)
NORETURN(acpi_processor_ffh_play_dead)
NORETURN(do_exit)
NORETURN(kthread_complete_and_exit)
NORETURN(kunit_try_catch_throw)
NORETURN(mpt_halt_firmware)
NORETURN(panic)
NORETURN(vpanic)
NORETURN(rust_helper_BUG)
NORETURN(sev_es_terminate)
NORETURN(xen_cpu_bringup_again)
NORETURN(xen_start_kernel)
