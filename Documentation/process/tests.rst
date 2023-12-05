.. SPDX-License-Identifier: GPL-2.0

.. _tests:

Tests you can run
=================

There are many automated tests available for the Linux kernel, and some
userspace tests which happen to also test the kernel. Here are some of them,
along with the instructions on where to get them and how to run them for
various purposes.

This document has to follow a certain structure to allow tool access.
Second-level headers (underscored with dashes '-') must contain test suite
names, and the corresponding section must contain the test description.

The test suites can be referenced by name, preceded with a '*', in the "V:"
lines in the MAINTAINERS file, as well as in the "Tested-with:" tag in commit
messages. E.g::

  V: *xfstests

and::

  Tested-with: *xfstests

Additionally, test suite names cannot contain '@' or '#' characters, the same
as "V:" entries.

The test suite description should contain the test documentation in general:
where to get the test, how to run it, and how to interpret its results, but
could also start with a "field list", containing single-line entries, with the
following ones recognized by the tools (regardless of the case):

:Summary: a single-line summary of the test suite (singular, non-capitalized)
:Superset: the name of the test suite this one is a subset of
:Command: a shell command executing the test suite, not requiring setup
          beyond the kernel tree and regular developer environment
          (even if only to report what else needs setting up)

Any other entries are accepted, but not processed.
