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

xfstests
--------

:Summary: File system regression test suite
:Source: git://git.kernel.org/pub/scm/fs/xfs/xfstests-dev.git
:Docs: https://github.com/tytso/xfstests-bld/blob/master/Documentation/what-is-xfstests.md

As the name might imply, xfstests is a file system regression test suite which
was originally developed by Silicon Graphics (SGI) for the XFS file system.
Originally, xfstests, like XFS was only supported on the SGI's Irix operating
system. When XFS was ported to Linux, so was xfstests, and now xfstests is
only supported on Linux.

Today, xfstests is used as a file system regression test suite for all of
Linux's major file systems: xfs, ext2, ext4, cifs, btrfs, f2fs, reiserfs, gfs,
jfs, udf, nfs, and tmpfs. Many file system maintainers will run a full set of
xfstests before sending patches to Linus, and will require that any major
changes be tested using xfstests before they are submitted for integration.

The easiest way to start running xfstests is under KVM with xfstests-bld:
https://github.com/tytso/xfstests-bld/blob/master/Documentation/kvm-quickstart.md

kvm-xfstests smoke
------------------

:Summary: File system smoke tests
:Superset: xfstests
:Docs: https://github.com/tytso/xfstests-bld/blob/master/Documentation/kvm-quickstart.md

The "kvm-xfstests smoke" is a minimal subset of xfstests for testing all major
file systems, running under KVM.

kunit
-----

:Summary: The complete set of KUnit unit tests
:Command: tools/testing/kunit/kunit.py run --alltests

kunit core
----------

:Summary: KUnit tests for the framework itself
:Superset: kunit
:Command: tools/testing/kunit/kunit.py run --kunitconfig lib/kunit
