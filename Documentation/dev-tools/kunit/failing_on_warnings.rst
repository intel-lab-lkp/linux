.. SPDX-License-Identifier: GPL-2.0

====================================================
Failing Tests on Lockdep Reports and Kernel Warnings
====================================================

By default, a KUnit test only fails if one of its own expectations or
assertions fails. If the code under test triggers a lockdep report or a
``WARN()``, the problem is printed to the kernel log, but the test still
passes and a bug is easily missed.

KUnit provides two optional config options that turn these problems into
test failures.

Config options
==============

``CONFIG_KUNIT_FAIL_TEST_ON_LOCKDEP``
	Fail a test case if lockdep reports a problem (and turns off
	``debug_locks``) while the test runs, which is useful for any test
	that takes locks. Locking bugs are reported by the test that
	triggered them. Requires lockdep, e.g. ``CONFIG_PROVE_LOCKING=y``.

``CONFIG_KUNIT_FAIL_TEST_ON_WARN``
	Fail a test case if the kernel becomes tainted with ``TAINT_WARN``
	while the test runs, i.e. if the test triggered a ``WARN()``. Since
	a warning usually indicates a bug, this is useful for any test that
	does not intentionally trigger a ``WARN()``. If you trigger a ``WARN()``
	on purpose use ``kunit_warning_suppress(test)`` otherwise your test
	will be marked as a failure.

Forcefully skipped tests
========================

Once lockdep has reported a problem, ``debug_locks`` stays off, and the
``TAINT_WARN`` taint cannot be cleared. The kernel is therefore considered
unreliable for the rest of the run, so after the failing test, all
remaining test cases are *forcefully skipped* instead of being run.

A forcefully skipped test case is reported as a skip with a
``force skip:`` reason, for example:

.. code-block:: none

	ok 2 foo_test # SKIP force skip: debug_locks are off

Unlike an ordinary skip, a forced skip does not mean the test does not
apply; it means the test could not be trusted to run. A suite in which
at least one test case was forcefully skipped is reported as follows:

- **Failed**, if a test case failed, and the remaining test cases were
  forcefully skipped as a result. This is the common case.
- **Passed**, if the forcefully skipped test cases only follow passing
  ones. This is rare, and implies that something other than the test
  cases themselves triggered the lockdep report or ``TAINT_WARN``.
- **Forced skipped**, if all test cases in the suite were forcefully
  skipped, typically because an earlier suite already left the kernel
  unreliable. In the KTAP output, such a suite is reported as
  ``not ok``.

The suite statistics count forced skips separately as ``forced_skip``,
and ``kunit.py`` shows such test cases as ``[FORCE SKIPPED]``.

Running tests with the options enabled
======================================

The options can be added to a ``kunit.py`` run with ``--kconfig_add``,
options CONFIG_KUNIT_FAIL_TEST_ON_WARN and CONFIG_KUNIT_FAIL_TEST_ON_LOCKDEP
are independent of each other. Here is an example of running kunit tests with both configs:

.. code-block:: bash

	./tools/testing/kunit/kunit.py run \
		--kconfig_add CONFIG_DEBUG_KERNEL=y \
		--kconfig_add CONFIG_PROVE_LOCKING=y \
		--kconfig_add CONFIG_KUNIT_FAIL_TEST_ON_WARN=y \
		--kconfig_add CONFIG_KUNIT_FAIL_TEST_ON_LOCKDEP=y

