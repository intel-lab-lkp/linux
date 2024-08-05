.. SPDX-License-Identifier: (GPL-2.0+ OR CC-BY-4.0)
.. [see the bottom of this file for redistribution information]

===============
Bisecting a bug
===============

This document describes how to find a change causing a kernel regression using
``git bisect``.

The text focuses on the gist of the process. If you are new to bisecting the
kernel, better follow Documentation/admin-guide/verify-bugs-and-bisect-regressions.rst
instead: it depicts everything from start to finish while covering multiple
aspects even kernel developers occasionally forget. This includes:

- Detecting situations where a bisections would be a waste of time, as nobody
  would care about the result -- for example, because the problem is triggered
  by a .config change, was already fixed, is caused by something your Linux
  distributor changed, occurs in an abandoned version, or happens after the
  kernel marked itself as 'tainted'.
- Preparing the .config file using an appropriate kernel while enabling or
  disabling debug symbols depending on the situation's needs -- while optionally
  trimming the .config to tremendously reduce the build time per bisection step.
- For regressions in stable or longterm kernels: checking mainline as well, as
  the result determines to whom the regression must be reported to.

Neither document describes how to report a regression, as that is covered by
Documentation/admin-guide/reporting-issues.rst.

Finding the change causing a kernel issue using a bisection
===========================================================

*Note: the following process assumes you prepared everything for a bisection;
this includes having a Git clone with the appropriate sources, installing the
software required to build and install kernels, as well as a .config file stored
in a safe place (the following example assumes '~/prepared_kernel_.config') to
use as pristine base at each bisection step.*

* Preparation: start the bisection and tell Git about the points in the history
  you consider to be working and broken, which Git calls 'good' and 'bad'::

    git bisect start
    git bisect good v6.0
    git bisect bad v6.1

  Instead of Git tags like 'v6.0' and 'v6.1' you can specify commit-ids, too.

1. Copy your prepared .config into the build directory and adjust it to the
   needs of the codebase Git checked out for testing::

     cp ~/prepared_kernel_.config .config
     make olddefconfig

2. Now build, install, and boot a kernel; if any of this fails for unrelated
   reasons, run ``git bisect skip`` and go back to step 1.

3. Check if the feature that regressed works in the kernel you just built.

   If it does, execute::

     git bisect good

   If it does not, run::

     git bisect bad

   Be sure what you tell Git is correct, as getting this wrong just once will
   send the rest of the bisection totally off course.

   Go back to back to step 1, if Git after issuing one of those commands checks
   out another bisection point while printing something like 'Bisecting:
   675 revisions left to test after this (roughly 10 steps)'.

   You finished the bisection and move to the next point below, if Git instead
   prints something like 'cafecaca0c0dacafecaca0c0dacafecaca0c0da is the first
   bad commit'; right afterwards it will show some details about the culprit
   including its patch description. The latter can easily fill your terminal,
   so you might need to scroll up to see the message mentioning the culprit's
   commit-id; alternatively, run ``git bisect log`` to show the result.

* Recommended complementary task: put the bisection log and the current
  .config file aside for the bug report; furthermore tell Git to reset the
  sources to the state before the bisection::

     git bisect log > ~/bisection-log
     cp .config ~/bisection-config-culprit
     git bisect reset

* Recommended optional task: try reverting the culprit on top of the latest
  codebase; if successful, this will validate your bisection and enable
  developers to resolve the regression through a revert.

  To try this, update your clone and check out latest mainline. Then tell Git to
  revert the change::

     git revert --no-edit cafec0cacaca0

  This might be impossible, for example when the bisection landed on a merge
  commit. In that case, abandon the attempt. Do the same, if Git fails to revert
  the culprit because later changes depend on it -- unless you bisected using a
  stable or longterm kernel series, in which case you want to retry using the
  latest code from that series.

  If a revert succeeds, build and test another kernel to validate the result of
  the bisection. Mention the outcome in your bug report.

Additional reading material
---------------------------

* The `man page for 'git bisect' <https://git-scm.com/docs/git-bisect>`_ and
  `fighting regressions with 'git bisect' <https://git-scm.com/docs/git-bisect-lk2009.html>`_
  in the Git documentation.
* `Working with git bisect <https://nathanchance.dev/posts/working-with-git-bisect/>`_
  from kernel developer Nathan Chancellor.
* `Using Git bisect to figure out when brokenness was introduced <http://webchick.net/node/99>`_.
* `Fully automated bisecting with 'git bisect run' <https://lwn.net/Articles/317154>`_.

..
   end-of-content
..
   This document is maintained by Thorsten Leemhuis <linux@leemhuis.info>. If
   you spot a typo or small mistake, feel free to let him know directly and
   he'll fix it. You are free to do the same in a mostly informal way if you
   want to contribute changes to the text -- but for copyright reasons please CC
   linux-doc@vger.kernel.org and 'sign-off' your contribution as
   Documentation/process/submitting-patches.rst explains in the section 'Sign
   your work - the Developer's Certificate of Origin'.
..
   This text is available under GPL-2.0+ or CC-BY-4.0, as stated at the top
   of the file. If you want to distribute this text under CC-BY-4.0 only,
   please use 'The Linux kernel development community' for author attribution
   and link this as source:
   https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/Documentation/admin-guide/bug-bisect.rst

..
   Note: Only the content of this RST file as found in the Linux kernel sources
   is available under CC-BY-4.0, as versions of this text that were processed
   (for example by the kernel's build system) might contain content taken from
   files which use a more restrictive license.
