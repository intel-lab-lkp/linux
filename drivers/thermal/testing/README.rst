.. SPDX-License-Identifier: GPL-2.0

Thermal aggregation KUnit tests
==============================

Run the core and OF suites on UML from the kernel source directory::

  ./tools/testing/kunit/kunit.py run \
      --kunitconfig=drivers/thermal/testing/.kunitconfig

The configuration enables lockdep. Neither suite requires thermal hardware
or the debugfs-based CONFIG_THERMAL_CORE_TESTING facility.
Both suites require CONFIG_THERMAL_AGGREGATION.

The core suite covers default coefficients, validation, fixed-point arithmetic,
registration and source lock contention. Arithmetic cases include signed and
zero-sum coefficients, negative temperatures, rounding, read failures, overflow
and output preservation on failure. Zero coefficients are tested in the read
path only: before initialization they denote unspecified coefficients.

The OF suite applies a test-managed overlay in /thermal-kunit, outside the
platform's /thermal-zones. It exercises deferred creation, provider lookup,
zero-cell providers, coefficient conversion, registration failure cleanup,
source removal and recreation, and a temperature read racing with source
removal. Source and consumer zones are destroyed before removing the overlay.
The suite temporarily enables retries because UML's empty root has no
/thermal-zones node for the production late-init scan.

Core tests are included by thermal_aggregation.c and OF tests by thermal_of.c
to access static helpers without exporting them. Suites and overlay data are
enabled only by their KUnit
configuration options. These tests do not exercise platform-specific hardware,
the initial late-init discovery scan, or real module loading. Reload is modeled
by unregistering and recreating a source zone with the same provider and ID.
