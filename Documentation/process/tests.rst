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

The test suites can be referred to by name, from the "V:" lines in the
MAINTAINERS file, as well as from the "Tested-with:" tag in commit messages.

The test suite description should contain the test documentation in general:
where to get the test, how to run it, and how to interpret its results, but
could also start with a "field list", with the following entries recognized by
the tools (regardless of the case):

:Summary: A single-line summary of the test suite
:Superset: The name of the test suite this one is a subset of
:Command: A shell command executing the test suite, not requiring setup
          beyond the kernel tree and regular developer environment
          (even if only to report what else needs setting up)

Any other entries are accepted, but not processed. The following could be
particularly useful:

:Source: A URL pointing to the source code of the test suite
:Docs: A URL pointing to further test suite documentation
