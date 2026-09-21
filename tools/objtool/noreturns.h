/* SPDX-License-Identifier: GPL-2.0 */

/*
 * This is a list of noreturn functions which are exported *by modules*.
 * No other noreturns need to be listed here.
 */
NORETURN(__kunit_abort)
NORETURN(kunit_try_catch_throw)
NORETURN(mpt_halt_firmware)
