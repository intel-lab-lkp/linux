.. SPDX-License-Identifier: (GPL-2.0+ OR CC-BY-4.0)
.. See the bottom of this file for additional redistribution information.

Reporting issues
++++++++++++++++

An email with a problem description sent to the appropriate developers and
mailing lists -- that is all it takes to report a Linux kernel bug.

This might sound easy, but be aware: Your bug reporting experience is likely to
become tedious or fruitless unless you get a few implicit aspects right.

The Linux kernel used, for example, must ideally be a recent mainline version;
longterm kernels will rarely do the trick, unless for reporting series-specific
regressions. That alone makes the vast majority of kernels shipped by hardware
vendors and Linux distributors unsuitable for upstream reporting. But those
almost always are inadequate anyway, as most are built from modified sources or
use externally developed kernel modules. Both aspects can lead to issues that
never occurred with the upstream Linux kernel, which is why most of its
developers do not really care about bugs reported with such kernels.

Identifying how to submit a report is also easier said than done. The file
MAINTAINERS answers this and usually points to email addresses. But a small
number of subsystems prefer reports through one of various bug trackers. Bugs
with most graphics drivers have to go to https://gitlab.freedesktop.org/drm/;
https://bugzilla.kernel.org seems like a universal place, but it is rarely the
right destination, as submissions there often do not even reach the developers.

The stable team, furthermore, is only the right point of contact for regressions
within a particular stable or longterm kernel series that at the same time do
not happen with latest mainline -- which you thus have to rule out first.

To avoid an ineffective, frustrating, or fruitless bug reporting experience, it
thus is in your best interest to follow the step-by-step guide below.

..
   Note: If you see this note, you are reading the text's source file. You
   might want to switch to a rendered version: It makes it a lot easier to
   read and navigate this document -- especially when you want to look something
   up in the reference section, then jump back to where you left off.
..
   Find the latest rendered version of this text here:
   https://docs.kernel.org/admin-guide/reporting-issues.html


Step-by-step guide on reporting Linux kernel issues
===================================================

Note: Only the steps starting with '*you must*' are strictly required -- but
following the others is usually in your own interest.

.. _intro_repisbs:

* Be aware:

  * You should report issues using a Linux kernel that is both really fresh and
    vanilla. That often means that you will have to remove software that
    requires externally developed kernel modules and install the newest upstream
    Linux development kernel yourself.

  * There is a decent chance you will have to report the problem by email, in
    which case your email address will become part of public archives.

  * You might need to patch and build your own kernel to help developers debug
    and fix the bug.

 If these three aspects sound too demanding, consider reporting the issue to
 your Linux distributor or hardware manufacturer instead.

 [:ref:`details <intro_repiref>`]

.. _multiple_repisbs:

* *You must* process and report each issue separately if you deal with multiple.
  If there is a slim chance they are related, remember to briefly mention the
  other problems in each of the reports, ideally cross-linking them in the end.

 [:ref:`details <multiple_repiref>`]

.. _checklog_repisbs:

* Skim ``journalctl -k`` (alternatively: ``dmesg``) for failures and warnings,
  as maybe there is just something wrong with your setup.

 [:ref:`details <checklog_repiref>`]

.. _taintone_repisbs:

* Check if the kernel was already 'tainted' when the issue first occurred: The
  event that resulted in this flag being set might have led to issues that
  otherwise would never happen.

 [:ref:`details <taintone_repiref>`]

.. _checkenv_repisbs:

* Evaluate briefly if some glitch in your kernel's environment might make it
  misbehave -- like a hardware defect, an updated system firmware, a
  misconfigured BIOS, an overclocked component, a kernel parameter enabling
  something unsupported, a broken initramfs, an inconsistent file system,
  changes to the linux-firmware files, or some malfunction/misconfiguration in
  your Linux distribution.

 [:ref:`details <checkenv_repiref>`]

.. _checkloreone_repisbs:

* Search `lore <https://lore.kernel.org/all/>`_ for similar reports and
  potential fixes; afterwards the wider internet, too.

  If you find a matching report, check it carefully:

  * If it is less than a month old and without a single doubt about the same
    issue, consider replying to tell involved people that you are affected as
    well.

  * If it just looks quite a lot like the same issue, send a reply briefly
    describing your problem and ask if it might be the same issue; if you do
    receive a negative reply or none at all, report the problem anew separately.

  * In all other cases, prepare a separate report by following this guide
    further and linking to any possibly related reports in yours.

 When you find fixes, consider trying them. If they work and are not yet
 committed, write a short reply to let the developers know. If they don't work
 while fixing the issue for other people, you most likely face a different
 problem you have to report independently while linking to the earlier report.

 [:ref:`details <checkloreone_repiref>`]

.. _specialtreat_repisbs:

* Evaluate if the issue you are dealing with qualifies as a regression or
  security issue, as those receive special treatment in some of the following
  steps.

 [:ref:`details <specialtreat_repiref>`]

 * Create a fresh backup and put system repair and restore tools at hand.

 * Ensure your system does not enhance its kernels by building additional
   kernel modules on-the-fly, which solutions like DKMS might be doing locally
   without your knowledge.

 * Write down coarsely how to reproduce the issue.

 * If you are facing a regression within a stable or longterm version line
   (say something broke when updating from 5.10.4 to 5.10.5), scroll down to
   'Dealing with regressions within a stable and longterm kernel line'.

 * Locate the driver or kernel subsystem that seems to be causing the issue.
   Find out how and where its developers expect reports. Note: most of the
   time this won't be bugzilla.kernel.org, as issues typically need to be sent
   by mail to a maintainer and a public mailing list.

 * Search the archives of the bug tracker or mailing list in question
   thoroughly for reports that might match your issue. If you find anything,
   join the discussion instead of sending a new report.

After these preparations you'll now enter the main part:

 * Unless you are already running the latest 'mainline' Linux kernel, better
   go and install it for the reporting process. Testing and reporting with
   the latest 'stable' Linux can be an acceptable alternative in some
   situations; during the merge window that actually might be even the best
   approach, but in that development phase it can be an even better idea to
   suspend your efforts for a few days anyway. Whatever version you choose,
   ideally use a 'vanilla' build. Ignoring these advices will dramatically
   increase the risk your report will be rejected or ignored.

 * Ensure the kernel you just installed does not 'taint' itself when
   running.

 * Reproduce the issue with the kernel you just installed. If it doesn't show
   up there, scroll down to the instructions for issues only happening with
   stable and longterm kernels.

 * Optimize your notes: try to find and write the most straightforward way to
   reproduce your issue. Make sure the end result has all the important
   details, and at the same time is easy to read and understand for others
   that hear about it for the first time. And if you learned something in this
   process, consider searching again for existing reports about the issue.

 * If your failure involves a 'panic', 'Oops', 'warning', or 'BUG', consider
   decoding the kernel log to find the line of code that triggered the error.

 * If your problem is a regression, try to narrow down when the issue was
   introduced as much as possible.

 * Start to compile the report by writing a detailed description about the
   issue. Always mention a few things: the latest kernel version you installed
   for reproducing, the Linux Distribution used, and your notes on how to
   reproduce the issue. Ideally, make the kernel's build configuration
   (.config) and the output from ``dmesg`` available somewhere on the net and
   link to it. Include or upload all other information that might be relevant,
   like the output/screenshot of an Oops or the output from ``lspci``. Once
   you wrote this main part, insert a normal length paragraph on top of it
   outlining the issue and the impact quickly. On top of this add one sentence
   that briefly describes the problem and gets people to read on. Now give the
   thing a descriptive title or subject that yet again is shorter. Then you're
   ready to send or file the report like the MAINTAINERS file told you, unless
   you are dealing with one of those 'issues of high priority': they need
   special care which is explained in 'Special handling for high priority
   issues' below.

 * Wait for reactions and keep the thing rolling until you can accept the
   outcome in one way or the other. Thus react publicly and in a timely manner
   to any inquiries. Test proposed fixes. Do proactive testing: retest with at
   least every first release candidate (RC) of a new mainline version and
   report your results. Send friendly reminders if things stall. And try to
   help yourself, if you don't get any help or if it's unsatisfying.


Reporting regressions within a stable and longterm kernel line
--------------------------------------------------------------

This subsection is for you, if you followed above process and got sent here at
the point about regression within a stable or longterm kernel version line. You
face one of those if something breaks when updating from 5.10.4 to 5.10.5 (a
switch from 5.9.15 to 5.10.5 does not qualify). The developers want to fix such
regressions as quickly as possible, hence there is a streamlined process to
report them:

 * Check if the kernel developers still maintain the Linux kernel version
   line you care about: go to the  `front page of kernel.org
   <https://kernel.org/>`_ and make sure it mentions
   the latest release of the particular version line without an '[EOL]' tag.

 * Check the archives of the `Linux stable mailing list
   <https://lore.kernel.org/stable/>`_ for existing reports.

 * Install the latest release from the particular version line as a vanilla
   kernel. Ensure this kernel is not tainted and still shows the problem, as
   the issue might have already been fixed there. If you first noticed the
   problem with a vendor kernel, check a vanilla build of the last version
   known to work performs fine as well.

 * Send a short problem report to the Linux stable mailing list
   (stable@vger.kernel.org) and CC the Linux regressions mailing list
   (regressions@lists.linux.dev); if you suspect the cause in a particular
   subsystem, CC its maintainer and its mailing list. Roughly describe the
   issue and ideally explain how to reproduce it. Mention the first version
   that shows the problem and the last version that's working fine. Then
   wait for further instructions.

The reference section below explains each of these steps in more detail.


Reporting issues only occurring in older kernel version lines
-------------------------------------------------------------

This subsection is for you, if you tried the latest mainline kernel as outlined
above, but failed to reproduce your issue there; at the same time you want to
see the issue fixed in a still supported stable or longterm series or vendor
kernels regularly rebased on those. If that is the case, follow these steps:

 * Prepare yourself for the possibility that going through the next few steps
   might not get the issue solved in older releases: the fix might be too big
   or risky to get backported there.

 * Perform the first three steps in the section "Dealing with regressions
   within a stable and longterm kernel line" above.

 * Search the Linux kernel version control system for the change that fixed
   the issue in mainline, as its commit message might tell you if the fix is
   scheduled for backporting already. If you don't find anything that way,
   search the appropriate mailing lists for posts that discuss such an issue
   or peer-review possible fixes; then check the discussions if the fix was
   deemed unsuitable for backporting. If backporting was not considered at
   all, join the newest discussion, asking if it's in the cards.

 * One of the former steps should lead to a solution. If that doesn't work
   out, ask the maintainers for the subsystem that seems to be causing the
   issue for advice; CC the mailing list for the particular subsystem as well
   as the stable mailing list.

The reference section below explains each of these steps in more detail.


Conclusion of the step-by-step guide
------------------------------------

Did you run into trouble following the step-by-step guide not cleared up by the
reference section below? Did you spot errors? Or do you have ideas on how to
improve the guide?

If any of that applies, please take a moment and let the primary author of this
text, Thorsten Leemhuis <linux@leemhuis.info>, know by email while ideally CCing
the public Linux docs mailing list <linux-doc@vger.kernel.org>. Such feedback is
vital to improve this text further, which is in everybody's interest, as it will
enable more people to master the task described here.


Reference section: Reporting issues to the kernel maintainers
=============================================================

The step-by-step guide above outlines all the major steps in brief fashion,
which usually covers everything required. But even experienced users will
sometimes wonder how to actually realize some of those steps or why they are
needed; there are also corner cases the guide ignores for readability. That is
what the entries in this reference section are for, which provide additional
information for each of the steps in the detailed guide.

A few words of general advice:

* The Linux kernel developers are well aware that reporting bugs to them is
  more complicated and demanding than in other FLOSS projects. Quite a few
  would love to make it simpler. But that would require convincing a lot of
  developers to change their habits; it, furthermore, would require improvements
  on several technical fronts and people that constantly take care of various
  things. Nobody has stepped up to do or fund that work.

* A warranty or support contract with some vendor doesn't entitle you to
  request fixes from the upstream Linux developers: Such contracts are
  completely outside the scope of the upstream Linux kernel, its development
  community, and this document -- even if those handling the issue work for the
  vendor who issued the contract. If you want to claim your rights, use the
  vendor's support channel.

* If you never reported an issue to a FLOSS project before, consider skimming
  guides like `How to ask good questions
  <https://jvns.ca/blog/good-questions/>`_, `How To Ask Questions The Smart Way
  <http://www.catb.org/esr/faqs/smart-questions.html>`_, and `How to Report
  Bugs Effectively <https://www.chiark.greenend.org.uk/~sgtatham/bugs.html>`_,.

With that off the table, find below details for the steps from the detailed
guide on reporting issues to the Linux kernel developers.


.. _intro_repiref:

You likely need to compile at least one really fresh kernel
-----------------------------------------------------------

  *Be aware:You should report issues using a Linux kernel that is both really
  fresh and vanilla. [...] Your email address will become part of public
  archives [...] You might need to patch and build your own kernel* [:ref:`... <intro_repisbs>`]

You most likely will have to fiddle with your setup and install at least one
fresh Linux kernel while reporting the issue or trying to resolve it. The
step-by-step guide mentions a few, but the most important is:

The kernels most Linux users utilize are unsuitable for reporting bugs
upstream, as the problem might have been fixed already or never happened with
the upstream code in the first place. Such situations occur frequently when it
comes to Linux:

1. Many developers consider all 'longterm' (aka 'LTS') kernels as too old and
   thus unsuitable for reporting, except for series-specific regressions (say
   from 6.1.13 to 6.1.15) in still-supported series (see `frontpage of
   kernel.org <https://kernel.org/>`_). Reporting using the newest version from
   the latest 'stable' series can work -- but some developers only take a
   closer look at bugs reported using a fresh mainline kernel, as the bug might
   have been fixed already.

2. Almost all Linux-based kernels pre-installed on devices (computers, laptops,
   smartphones, routers, …) are therefore too old as well, but even when not,
   often unsuitable for a second reason:

  Most vendors modify Linux's source code (some heavily!) before building their
  kernels; frequently their kernels also load externally developed modules
  ('out-of-tree drivers'), too. Such modifications or enhancements might be
  causing your issue, even when they seem tiny and unrelated. Upstream
  developers can do nothing about such problems.

  You therefore have to report issues with such kernels to the vendor. Its
  developers should look into the report and, in case it turns out to be an
  upstream issue, report or directly fix it there. In practice that often does
  not work out or might not be what you want; installing your own fresh vanilla
  kernel while steering clear of externally developed modules is your way out
  here.

Note: Some developers, despite the aforementioned aspects, are willing to handle
reports about issues in kernels that are somewhat older or slightly diverged.
If they will highly depends on the circumstances:

* Your chances are quite good if the vendor performed only small changes to a
  recent mainline codebase; that, for example, often holds true for the kernels
  shipped by Debian GNU/Linux Sid (aka 'unstable') or Fedora Rawhide.

* Chances are slightly worse but still relatively good for reports about issues
  with the newest version from the latest Linux stable series. That is the case
  even when the distributor slightly modified or enhanced the kernel's code --
  this, for example, is often the case for the default kernels of Arch Linux and
  openSUSE Tumbleweed, as well as regular Fedora releases when using the latest
  stable series.

You are free to ignore this advice and report problems to the upstream Linux
developers occurring with kernels that are outdated, modified, or utilizing
out-of-tree drivers. But be aware that developers might react coldly or never
at all. Nevertheless, it is still better than not reporting the issue:
sometimes such a report directly or indirectly helps to resolve issues over
time.

[:ref:`back to step-by-step guide <intro_repisbs>`]


.. _multiple_repiref:

Issues must be reported one by one
----------------------------------

  *You must process and report each issue separately if you deal with
  multiple. If there is a slim chance* [:ref:`... <multiple_repisbs>`]

You will have to report issues one by one if you deal with multiple, as they
likely will be handled by different developers; describing various issues in
one report also makes it difficult for others to understand the situation.
Hence, only combine issues in one report if they are very strongly
entangled or clearly have the same cause.

[:ref:`back to step-by-step guide <multiple_repisbs>`]


.. _checklog_repiref:

Evaluate the logs
-----------------

  *Skim 'journalctl -k' (alternatively: 'dmesg') for failures and warnings, as
  maybe there is just something wrong with your setup.* [:ref:`... <checklog_repisbs>`]

Sometimes a bug you face is just a symptom of something going sideways that the
kernel detected and logged -- like a missing firmware file, for example. To rule
such things out, check the kernel's log messages.

Preferably use 'journalctl' if your distribution supports it, as in contrast to
'dmesg' it always contains all messages since the kernel started.

Especially look out for messages in bold, yellow, or red, as both tools use such
to set warnings and errors apart.

[:ref:`back to step-by-step guide <checklog_repisbs>`]


.. _taintone_repiref:

Check 'taint' flag
------------------

  *Check if the kernel was already 'tainted' when the issue first occurred* [:ref:`... <taintone_repisbs>`]

The kernel marks itself with a 'taint' flag when something happens that might
lead to follow-up errors looking totally unrelated. Your issue might
be such an error, in which case there is nothing to report. That is why it is
in your interest to check the taint status early in the reporting process. This
is the main reason why this step is here in the guide, as you most likely will
have to install a different kernel for reporting later -- and then need to
recheck the flag, as that is when it matters.

To check the tainted flag, execute ``cat /proc/sys/kernel/tainted``: If it
returns '0' everything is fine; if it contains a higher number, it is tainted.

In some situations it is impossible to check that file. That is
why the kernel also mentions the taint status when it reports small (a
'warning' or a 'bug') or big (an 'Oops' or a 'panic') problems. In such cases,
search for a line starting with 'CPU:' near the top of the error messages
printed on the screen or in the log. If the kernel at that point considered
itself to be fine, it will end with 'Not tainted'; if not, you will see
'Tainted:' followed by a few spaces and some letters.

If your kernel is tainted, check Documentation/admin-guide/tainted-kernels.rst
to find out why. Note: It is quite possible that the problem you ran into
caused the kernel to taint itself, in which case you are free to ignore the
flag. But if the kernel was tainted beforehand, you might have to eliminate the
cause or rule out that it is an influence.

These are the most frequent reasons why the kernel set the flag:

1. Some kind of error (like a 'kernel Oops') occurred. The kernel then taints
   itself, as it might misbehave in unexpected ways afterwards.
   In that case check your kernel or system log and look for a section
   starting with::

       Oops: 0000 [#1] SMP

   That is the first Oops since boot-up, as the '#1' between the brackets shows.
   Every later Oops and any other problem that happens afterwards might be
   a follow-up issue
   that would never have happened otherwise, even if both look totally unrelated.
   Rule this out by eliminating the cause for the first Oops and reproducing
   the issue afterwards. Sometimes simply restarting will be enough; other times
   a change to the configuration followed by a reboot can eliminate the Oops.

   Note: Do not invest too much time into this while you are still on an
   outdated or vendor kernel: The cause for the Oops might already be fixed in
   a newer Linux kernel
   version you most likely will have to install for reporting while following
   this guide.

2. Your system uses software that installs externally developed kernel modules,
   for example, kernel modules from Nvidia, OpenZFS, VirtualBox, or VMware. The
   kernel taints itself when it loads such 'out-of-tree' modules -- no matter
   what license they use, as such modules can cause errors in unrelated kernel
   areas and thus might be leading to the issue you face. You therefore have to
   prevent those modules from loading when you want to report an issue to the
   Linux kernel developers. Most of the time the easiest way to do that:
   temporarily uninstall such software including any modules they might have
   installed; afterwards reboot.

3. The kernel taints itself when it loads a module that resides in
   the staging section of the Linux kernel source. That is a special area for
   code (mostly drivers) that does not yet fulfill the normal Linux kernel
   quality standards. When you report an issue with such a module, it is
   totally okay if the kernel is tainted; just make sure the module in
   question is the only reason for the taint. If the issue happens in an
   unrelated area, it is wise to rule out that it is an influence. To do so,
   reboot and temporarily block the module from being loaded by specifying
   ``foo.blacklist=1`` as kernel boot parameter (replace 'foo' with the name of
   the module in question).

[:ref:`back to step-by-step guide <taintone_repisbs>`]


.. _checkenv_repiref:

Ensure there is nothing wrong with the kernel's surroundings
------------------------------------------------------------

  *Evaluate briefly if some glitch in your kernel's environment might make it
  misbehave -- like a* [:ref:`... <checkenv_repisbs>`]

Problems that look like a kernel issue are sometimes caused by its
surroundings. It is impossible to detect sometimes -- but it is wise to rule
out a few common causes before wasting time on a meaningless bug report:

* When dealing with a regression (e.g., something stopped working or works worse
  after updating the kernel), make sure it is not something else that changed
  in parallel. That could be something else you updated at the same time, like
  the BIOS, the boot loader, Mesa, the linux-firmware package, or something
  else close to the kernel; but it could also be some change you performed in
  the BIOS setup or your Linux distribution's configuration.

* Try to make sure the hardware is healthy, as problems with it can result in a
  multitude of issues that look like kernel bugs.

  Ideally try to rule out faulty RAM or a dying device causes the problem.

  Also ensure your computer components run within their design specifications;
  that is especially important for the main processor, the RAM, and the
  motherboard. Therefore, stop undervolting or overclocking when facing a
  potential kernel issue.

* Temporarily remove any optional kernel parameters you use, as they might
  enable unsupported or experimental features.

* In case of any problems related to booting, check if the initramfs was
  generated correctly.

* When dealing with a file system issue, check the file
  system in question with ``fsck``, as it might be damaged in a way that leads
  to unexpected kernel behavior.

* Use proven tools when building your kernel, as bugs in the compiler or the
  linker can cause the resulting kernel to misbehave.

[:ref:`back to step-by-step guide <checkenv_repisbs>`]


.. _checkloreone_repiref:

Search for existing reports and fixes
-------------------------------------

  *Search lore.kernel.org for similar reports and potential fixes; afterwards
  the wider internet, too.*  [:ref:`... <checkloreone_repisbs>`]

You don't want to waste your time reporting an issue anew someone already
brought forward or resolved already. So it is in your own interest to check for
existing reports and fixes.

Searching for fixes and existing reports
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

If your search on `lore <https://lore.kernel.org/all/>`_ and the web results in
a flood of results, consider limiting the search timeframe. In lore you can do
so by adding something like 'rt:3.months.ago..' or 'rt:1.years.ago..' to your
query.

Wherever you search, make sure to use good terms; vary them a few times, too.

Start with something specific and become broader if there are no or too few
results. Also try to look at the issue from the perspective of someone else:
that might help you to come up with other terms to use in your search.

Remember to search with and without information like the name of the kernel
driver or the name of the affected hardware component. But its exact brand name
(say, 'ASUS Red Devil Radeon RX 5700 XT Gaming OC') often is way too specific;
instead, try search terms like the model line ('Radeon 5700' or 'Radeon 5000')
and the code name of the main chip ('Navi' or 'Navi10') with and without the
manufacturer of the main chip's name ('AMD') or the product's series
('Radeon').

Try fixes and evaluate matching reports closely
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

If you find a potential fix, give it a try; if it is still under discussion and
helps, let the developers know through a short reply.

You found matching reports or a fix that does not help? Then evaluate them
closely, as they might be about a different issue with similar symptoms. Your
next steps depend on the outcome:

* Is the report or fix still discussed and without any doubt about an issue like
  yours? Then join the exchange, as you might be able to provide valuable
  additional information or test results.

* If the report or fix seems to be about a different issue, ignore it and
  proceed with this guide, but briefly mention and link the earlier report or
  fix in your report later. After reporting, it might also be wise to send a
  short reply to the earlier report with a text along the lines of 'To whom it
  may concern, I ran into a similar, but from my understanding slightly
  different problem', coupled with a link to the report.

* When unsure if it is the same or a different problem, send a short reply to
  the earlier report or fix; in it, very briefly outline the problem while
  asking if that seems to be the same problem or a different one better
  reported separately.

While doing so, keep in mind:

* Chaos and confusion easily ensue when an issue is reported in a bug tracker
  ticket or mailing list thread that looks related, but, in fact, is about a
  different issue. Try hard to avoid such an outcome, as then it can quickly
  happen that none of the problems will be addressed in the end. The best
  strategy to avoid that: Whenever there is a slight chance that your issue
  might be different, report it in a new ticket or thread, but mention the
  earlier reports you found; afterwards send a short reply to the earlier
  ticket/thread with a text along the lines of 'I have a similar problem which
  might or might not be related' coupled with a link to your report.

* Never report an issue in a bug tracker ticket or a mailing list thread that
  looks related, but is considered resolved. Always report in a new thread
  instead; afterwards send a short reply to the earlier ticket/thread with a
  text along the lines of 'I have a similar problem which might or might not be
  related' coupled with a link to your report.

* When spotting matching reports on `bugzilla.kernel.org
  <https://bugzilla.kernel.org/>`_, keep in mind that the appropriate
  developers to handle the issue might not even be aware of the report. That is
  because Bugzilla might not have forwarded the report to them: It lacks the
  necessary information to do so for many of the kernel's subsystems, as their
  developers expect reports in different places -- ':ref:`Check how to report
  your issue <maintainers_repiref>`' describes this in more detail. If in
  doubt, add a comment to the Bugzilla report; if no reassuring answer is
  forthcoming, report the issue briefly through the proper channel while
  mentioning the Bugzilla report; afterwards add a comment to the latter
  pointing to your report.

[:ref:`back to step-by-step guide <checkloreone_repisbs>`]


.. _specialtreat_repiref:

Issues receiving special treatment
----------------------------------

  *Evaluate if the issue you are dealing with qualifies as a regression or
  security issue, as those* [:ref:`... <specialtreat_repisbs>`]

Check if you face an issue that receives special treatment in the Linux
development process:

* You deal with a regression, if some application or practical use case running
  fine with one Linux kernel version works worse or not at all with a newer
  version compiled using a similar configuration; the 'no regression' rule
  forbids that. The document
  Documentation/admin-guide/reporting-regressions.rst explains these and
  additional aspects in more detail, but everything important is covered in
  this document.

* What qualifies as a security issue is left to your judgment. Consider reading
  Documentation/process/security-bugs.rst before proceeding, which
  provides instructions on handling security issues.

[:ref:`back to step-by-step guide <specialtreat_repisbs>`]


Prepare for emergencies
-----------------------

    *Create a fresh backup and put system repair and restore tools at hand.*

Reminder, you are dealing with computers, which sometimes do unexpected things,
especially if you fiddle with crucial parts like the kernel of its operating
system. That's what you are about to do in this process. Thus, make sure to
create a fresh backup; also ensure you have all tools at hand to repair or
reinstall the operating system as well as everything you need to restore the
backup.


Make sure your kernel doesn't get enhanced
------------------------------------------

    *Ensure your system does not enhance its kernels by building additional
    kernel modules on-the-fly, which solutions like DKMS might be doing locally
    without your knowledge.*

The risk your issue report gets ignored or rejected dramatically increases if
your kernel gets enhanced in any way. That's why you should remove or disable
mechanisms like akmods and DKMS: those build add-on kernel modules
automatically, for example when you install a new Linux kernel or boot it for
the first time. Also remove any modules they might have installed. Then reboot
before proceeding.

Note, you might not be aware that your system is using one of these solutions:
they often get set up silently when you install Nvidia's proprietary graphics
driver, VirtualBox, or other software that requires a some support from a
module not part of the Linux kernel. That why your might need to uninstall the
packages with such software to get rid of any 3rd party kernel module.


Document how to reproduce issue
-------------------------------

    *Write down coarsely how to reproduce the issue.*

During the reporting process you will have to test if the issue
happens with other kernel versions. Therefore, it will make your work easier if
you know exactly how to reproduce an issue quickly on a freshly booted system.

Note: it's often fruitless to report issues that only happened once, as they
might be caused by a bit flip due to cosmic radiation. That's why you should
try to rule that out by reproducing the issue before going further. Feel free
to ignore this advice if you are experienced enough to tell a one-time error
due to faulty hardware apart from a kernel issue that rarely happens and thus
is hard to reproduce.


Regression in stable or longterm kernel?
----------------------------------------

    *If you are facing a regression within a stable or longterm version line
    (say something broke when updating from 5.10.4 to 5.10.5), scroll down to
    'Dealing with regressions within a stable and longterm kernel line'.*

Regression within a stable and longterm kernel version line are something the
Linux developers want to fix badly, as such issues are even more unwanted than
regression in the main development branch, as they can quickly affect a lot of
people. The developers thus want to learn about such issues as quickly as
possible, hence there is a streamlined process to report them. Note,
regressions with newer kernel version line (say something broke when switching
from 5.9.15 to 5.10.5) do not qualify.


Check where you need to report your issue
-----------------------------------------

    *Locate the driver or kernel subsystem that seems to be causing the issue.
    Find out how and where its developers expect reports. Note: most of the
    time this won't be bugzilla.kernel.org, as issues typically need to be sent
    by mail to a maintainer and a public mailing list.*

It's crucial to send your report to the right people, as the Linux kernel is a
big project and most of its developers are only familiar with a small subset of
it. Quite a few programmers for example only care for just one driver, for
example one for a WiFi chip; its developer likely will only have small or no
knowledge about the internals of remote or unrelated "subsystems", like the TCP
stack, the PCIe/PCI subsystem, memory management or file systems.

Problem is: the Linux kernel lacks a central bug tracker where you can simply
file your issue and make it reach the developers that need to know about it.
That's why you have to find the right place and way to report issues yourself.
You can do that with the help of a script (see below), but it mainly targets
kernel developers and experts. For everybody else the MAINTAINERS file is the
better place.

How to read the MAINTAINERS file
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
To illustrate how to use the :ref:`MAINTAINERS <maintainers>` file, let's assume
the WiFi in your Laptop suddenly misbehaves after updating the kernel. In that
case it's likely an issue in the WiFi driver. Obviously it could also be some
code it builds upon, but unless you suspect something like that stick to the
driver. If it's really something else, the driver's developers will get the
right people involved.

Sadly, there is no way to check which code is driving a particular hardware
component that is both universal and easy.

In case of a problem with the WiFi driver you for example might want to look at
the output of ``lspci -k``, as it lists devices on the PCI/PCIe bus and the
kernel module driving it::

       [user@something ~]$ lspci -k
       [...]
       3a:00.0 Network controller: Qualcomm Atheros QCA6174 802.11ac Wireless Network Adapter (rev 32)
         Subsystem: Bigfoot Networks, Inc. Device 1535
         Kernel driver in use: ath10k_pci
         Kernel modules: ath10k_pci
       [...]

But this approach won't work if your WiFi chip is connected over USB or some
other internal bus. In those cases you might want to check your WiFi manager or
the output of ``ip link``. Look for the name of the problematic network
interface, which might be something like 'wlp58s0'. This name can be used like
this to find the module driving it::

       [user@something ~]$ realpath --relative-to=/sys/module/ /sys/class/net/wlp58s0/device/driver/module
       ath10k_pci

In case tricks like these don't bring you any further, try to search the
internet on how to narrow down the driver or subsystem in question. And if you
are unsure which it is: just try your best guess, somebody will help you if you
guessed poorly.

Once you know the driver or subsystem, you want to search for it in the
MAINTAINERS file. In the case of 'ath10k_pci' you won't find anything, as the
name is too specific. Sometimes you will need to search on the net for help;
but before doing so, try a somewhat shorted or modified name when searching the
MAINTAINERS file, as then you might find something like this::

       QUALCOMM ATHEROS ATH10K WIRELESS DRIVER
       Mail:          A. Some Human <shuman@example.com>
       Mailing list:  ath10k@lists.infradead.org
       Status:        Supported
       Web-page:      https://wireless.wiki.kernel.org/en/users/Drivers/ath10k
       SCM:           git git://git.kernel.org/pub/scm/linux/kernel/git/kvalo/ath.git
       Files:         drivers/net/wireless/ath/ath10k/

Note: the line description will be abbreviations, if you read the plain
MAINTAINERS file found in the root of the Linux source tree. 'Mail:' for
example will be 'M:', 'Mailing list:' will be 'L', and 'Status:' will be 'S:'.
A section near the top of the file explains these and other abbreviations.

First look at the line 'Status'. Ideally it should be 'Supported' or
'Maintained'. If it states 'Obsolete' then you are using some outdated approach
that was replaced by a newer solution you need to switch to. Sometimes the code
only has someone who provides 'Odd Fixes' when feeling motivated. And with
'Orphan' you are totally out of luck, as nobody takes care of the code anymore.
That only leaves these options: arrange yourself to live with the issue, fix it
yourself, or find a programmer somewhere willing to fix it.

After checking the status, look for a line starting with 'bugs:': it will tell
you where to find a subsystem specific bug tracker to file your issue. The
example above does not have such a line. That is the case for most sections, as
Linux kernel development is completely driven by mail. Very few subsystems use
a bug tracker, and only some of those rely on bugzilla.kernel.org.

In this and many other cases you thus have to look for lines starting with
'Mail:' instead. Those mention the name and the email addresses for the
maintainers of the particular code. Also look for a line starting with 'Mailing
list:', which tells you the public mailing list where the code is developed.
Your report later needs to go by mail to those addresses. Additionally, for all
issue reports sent by email, make sure to add the Linux Kernel Mailing List
(LKML) <linux-kernel@vger.kernel.org> to CC. Don't omit either of the mailing
lists when sending your issue report by mail later! Maintainers are busy people
and might leave some work for other developers on the subsystem specific list;
and LKML is important to have one place where all issue reports can be found.


Finding the maintainers with the help of a script
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

For people that have the Linux sources at hand there is a second option to find
the proper place to report: the script 'scripts/get_maintainer.pl' which tries
to find all people to contact. It queries the MAINTAINERS file and needs to be
called with a path to the source code in question. For drivers compiled as
module if often can be found with a command like this::

       $ modinfo ath10k_pci | grep filename | sed 's!/lib/modules/.*/kernel/!!; s!filename:!!; s!\.ko\(\|\.xz\)!!'
       drivers/net/wireless/ath/ath10k/ath10k_pci.ko

Pass parts of this to the script::

       $ ./scripts/get_maintainer.pl -f drivers/net/wireless/ath/ath10k*
       Some Human <shuman@example.com> (supporter:QUALCOMM ATHEROS ATH10K WIRELESS DRIVER)
       Another S. Human <asomehuman@example.com> (maintainer:NETWORKING DRIVERS)
       ath10k@lists.infradead.org (open list:QUALCOMM ATHEROS ATH10K WIRELESS DRIVER)
       linux-wireless@vger.kernel.org (open list:NETWORKING DRIVERS (WIRELESS))
       netdev@vger.kernel.org (open list:NETWORKING DRIVERS)
       linux-kernel@vger.kernel.org (open list)

Don't sent your report to all of them. Send it to the maintainers, which the
script calls "supporter:"; additionally CC the most specific mailing list for
the code as well as the Linux Kernel Mailing List (LKML). In this case you thus
would need to send the report to 'Some Human <shuman@example.com>' with
'ath10k@lists.infradead.org' and 'linux-kernel@vger.kernel.org' in CC.

Note: in case you cloned the Linux sources with git you might want to call
``get_maintainer.pl`` a second time with ``--git``. The script then will look
at the commit history to find which people recently worked on the code in
question, as they might be able to help. But use these results with care, as it
can easily send you in a wrong direction. That for example happens quickly in
areas rarely changed (like old or unmaintained drivers): sometimes such code is
modified during tree-wide cleanups by developers that do not care about the
particular driver at all.


Search for existing reports, second run
---------------------------------------

    *Search the archives of the bug tracker or mailing list in question
    thoroughly for reports that might match your issue. If you find anything,
    join the discussion instead of sending a new report.*

As mentioned earlier already: reporting an issue that someone else already
brought forward is often a waste of time for everyone involved, especially you
as the reporter. That's why you should search for existing report again, now
that you know where they need to be reported to. If it's mailing list, you will
often find its archives on `lore.kernel.org <https://lore.kernel.org/>`_.

But some list are hosted in different places. That for example is the case for
the ath10k WiFi driver used as example in the previous step. But you'll often
find the archives for these lists easily on the net. Searching for 'archive
ath10k@lists.infradead.org' for example will lead you to the `Info page for the
ath10k mailing list <https://lists.infradead.org/mailman/listinfo/ath10k>`_,
which at the top links to its
`list archives <https://lists.infradead.org/pipermail/ath10k/>`_. Sadly this and
quite a few other lists miss a way to search the archives. In those cases use a
regular internet search engine and add something like
'site:lists.infradead.org/pipermail/ath10k/' to your search terms, which limits
the results to the archives at that URL.

It's also wise to check the internet, LKML and maybe bugzilla.kernel.org again
at this point. If your report needs to be filed in a bug tracker, you may want
to check the mailing list archives for the subsystem as well, as someone might
have reported it only there.

For details how to search and what to do if you find matching reports see
"Search for existing reports, first run" above.

Do not hurry with this step of the reporting process: spending 30 to 60 minutes
or even more time can save you and others quite a lot of time and trouble.


Install a fresh kernel for testing
----------------------------------

    *Unless you are already running the latest 'mainline' Linux kernel, better
    go and install it for the reporting process. Testing and reporting with
    the latest 'stable' Linux can be an acceptable alternative in some
    situations; during the merge window that actually might be even the best
    approach, but in that development phase it can be an even better idea to
    suspend your efforts for a few days anyway. Whatever version you choose,
    ideally use a 'vanilla' built. Ignoring these advices will dramatically
    increase the risk your report will be rejected or ignored.*

As mentioned in the detailed explanation for the first step already: Like most
programmers, Linux kernel developers don't like to spend time dealing with
reports for issues that don't even happen with the current code. It's just a
waste everybody's time, especially yours. That's why it's in everybody's
interest that you confirm the issue still exists with the latest upstream code
before reporting it. You are free to ignore this advice, but as outlined
earlier: doing so dramatically increases the risk that your issue report might
get rejected or simply ignored.

In the scope of the kernel "latest upstream" normally means:

 * Install a mainline kernel; the latest stable kernel can be an option, but
   most of the time is better avoided. Longterm kernels (sometimes called 'LTS
   kernels') are unsuitable at this point of the process. The next subsection
   explains all of this in more detail.

 * The over next subsection describes way to obtain and install such a kernel.
   It also outlines that using a pre-compiled kernel are fine, but better are
   vanilla, which means: it was built using Linux sources taken straight `from
   kernel.org <https://kernel.org/>`_ and not modified or enhanced in any way.

Choosing the right version for testing
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Head over to `kernel.org <https://kernel.org/>`_ to find out which version you
want to use for testing. Ignore the big yellow button that says 'Latest release'
and look a little lower at the table. At its top you'll see a line starting with
mainline, which most of the time will point to a pre-release with a version
number like '5.8-rc2'. If that's the case, you'll want to use this mainline
kernel for testing, as that where all fixes have to be applied first. Do not let
that 'rc' scare you, these 'development kernels' are pretty reliable — and you
made a backup, as you were instructed above, didn't you?

In about two out of every nine to ten weeks, mainline might point you to a
proper release with a version number like '5.7'. If that happens, consider
suspending the reporting process until the first pre-release of the next
version (5.8-rc1) shows up on kernel.org. That's because the Linux development
cycle then is in its two-week long 'merge window'. The bulk of the changes and
all intrusive ones get merged for the next release during this time. It's a bit
more risky to use mainline during this period. Kernel developers are also often
quite busy then and might have no spare time to deal with issue reports. It's
also quite possible that one of the many changes applied during the merge
window fixes the issue you face; that's why you soon would have to retest with
a newer kernel version anyway, as outlined below in the section 'Duties after
the report went out'.

That's why it might make sense to wait till the merge window is over. But don't
to that if you're dealing with something that shouldn't wait. In that case
consider obtaining the latest mainline kernel via git (see below) or use the
latest stable version offered on kernel.org. Using that is also acceptable in
case mainline for some reason does currently not work for you. An in general:
using it for reproducing the issue is also better than not reporting it issue
at all.

Better avoid using the latest stable kernel outside merge windows, as all fixes
must be applied to mainline first. That's why checking the latest mainline
kernel is so important: any issue you want to see fixed in older version lines
needs to be fixed in mainline first before it can get backported, which can
take a few days or weeks. Another reason: the fix you hope for might be too
hard or risky for backporting; reporting the issue again hence is unlikely to
change anything.

These aspects are also why longterm kernels (sometimes called "LTS kernels")
are unsuitable for this part of the reporting process: they are to distant from
the current code. Hence go and test mainline first and follow the process
further: if the issue doesn't occur with mainline it will guide you how to get
it fixed in older version lines, if that's in the cards for the fix in question.

How to obtain a fresh Linux kernel
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Using a pre-compiled kernel**: This is often the quickest, easiest, and safest
way for testing — especially is you are unfamiliar with the Linux kernel. The
problem: most of those shipped by distributors or add-on repositories are build
from modified Linux sources. They are thus not vanilla and therefore often
unsuitable for testing and issue reporting: the changes might cause the issue
you face or influence it somehow.

But you are in luck if you are using a popular Linux distribution: for quite a
few of them you'll find repositories on the net that contain packages with the
latest mainline or stable Linux built as vanilla kernel. It's totally okay to
use these, just make sure from the repository's description they are vanilla or
at least close to it. Additionally ensure the packages contain the latest
versions as offered on kernel.org. The packages are likely unsuitable if they
are older than a week, as new mainline and stable kernels typically get released
at least once a week.

Please note that you might need to build your own kernel manually later: that's
sometimes needed for debugging or testing fixes, as described later in this
document. Also be aware that pre-compiled kernels might lack debug symbols that
are needed to decode messages the kernel prints when a panic, Oops, warning, or
BUG occurs; if you plan to decode those, you might be better off compiling a
kernel yourself (see the end of this subsection and the section titled 'Decode
failure messages' for details).

**Using git**: Developers and experienced Linux users familiar with git are
often best served by obtaining the latest Linux kernel sources straight from the
`official development repository on kernel.org
<https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/>`_.
Those are likely a bit ahead of the latest mainline pre-release. Don't worry
about it: they are as reliable as a proper pre-release, unless the kernel's
development cycle is currently in the middle of a merge window. But even then
they are quite reliable.

**Conventional**: People unfamiliar with git are often best served by
downloading the sources as tarball from `kernel.org <https://kernel.org/>`_.

How to actually build a kernel is not described here, as many websites explain
the necessary steps already. If you are new to it, consider following one of
those how-to's that suggest to use ``make localmodconfig``, as that tries to
pick up the configuration of your current kernel and then tries to adjust it
somewhat for your system. That does not make the resulting kernel any better,
but quicker to compile.

Note: If you are dealing with a panic, Oops, warning, or BUG from the kernel,
please try to enable CONFIG_KALLSYMS when configuring your kernel.
Additionally, enable CONFIG_DEBUG_KERNEL and CONFIG_DEBUG_INFO, too; the
latter is the relevant one of those two, but can only be reached if you enable
the former. Be aware CONFIG_DEBUG_INFO increases the storage space required to
build a kernel by quite a bit. But that's worth it, as these options will allow
you later to pinpoint the exact line of code that triggers your issue. The
section 'Decode failure messages' below explains this in more detail.

But keep in mind: Always keep a record of the issue encountered in case it is
hard to reproduce. Sending an undecoded report is better than not reporting
the issue at all.


Check 'taint' flag
------------------

    *Ensure the kernel you just installed does not 'taint' itself when
    running.*

As outlined above in more detail already: the kernel sets a 'taint' flag when
something happens that can lead to follow-up errors that look totally
unrelated. That's why you need to check if the kernel you just installed does
not set this flag. And if it does, you in almost all the cases needs to
eliminate the reason for it before you reporting issues that occur with it. See
the section above for details how to do that.


Reproduce issue with the fresh kernel
-------------------------------------

    *Reproduce the issue with the kernel you just installed. If it doesn't show
    up there, scroll down to the instructions for issues only happening with
    stable and longterm kernels.*

Check if the issue occurs with the fresh Linux kernel version you just
installed. If it was fixed there already, consider sticking with this version
line and abandoning your plan to report the issue. But keep in mind that other
users might still be plagued by it, as long as it's not fixed in either stable
and longterm version from kernel.org (and thus vendor kernels derived from
those). If you prefer to use one of those or just want to help their users,
head over to the section "Details about reporting issues only occurring in
older kernel version lines" below.


Optimize description to reproduce issue
---------------------------------------

    *Optimize your notes: try to find and write the most straightforward way to
    reproduce your issue. Make sure the end result has all the important
    details, and at the same time is easy to read and understand for others
    that hear about it for the first time. And if you learned something in this
    process, consider searching again for existing reports about the issue.*

An unnecessarily complex report will make it hard for others to understand your
report. Thus try to find a reproducer that's straight forward to describe and
thus easy to understand in written form. Include all important details, but at
the same time try to keep it as short as possible.

In this in the previous steps you likely have learned a thing or two about the
issue you face. Use this knowledge and search again for existing reports
instead you can join.


Decode failure messages
-----------------------

    *If your failure involves a 'panic', 'Oops', 'warning', or 'BUG', consider
    decoding the kernel log to find the line of code that triggered the error.*

When the kernel detects an internal problem, it will log some information about
the executed code. This makes it possible to pinpoint the exact line in the
source code that triggered the issue and shows how it was called. But that only
works if you enabled CONFIG_DEBUG_INFO and CONFIG_KALLSYMS when configuring
your kernel. If you did so, consider to decode the information from the
kernel's log. That will make it a lot easier to understand what lead to the
'panic', 'Oops', 'warning', or 'BUG', which increases the chances that someone
can provide a fix.

Decoding can be done with a script you find in the Linux source tree. If you
are running a kernel you compiled yourself earlier, call it like this::

       [user@something ~]$ sudo dmesg | ./linux-5.10.5/scripts/decode_stacktrace.sh ./linux-5.10.5/vmlinux

If you are running a packaged vanilla kernel, you will likely have to install
the corresponding packages with debug symbols. Then call the script (which you
might need to get from the Linux sources if your distro does not package it)
like this::

       [user@something ~]$ sudo dmesg | ./linux-5.10.5/scripts/decode_stacktrace.sh \
        /usr/lib/debug/lib/modules/5.10.10-4.1.x86_64/vmlinux /usr/src/kernels/5.10.10-4.1.x86_64/

The script will work on log lines like the following, which show the address of
the code the kernel was executing when the error occurred::

       [   68.387301] RIP: 0010:test_module_init+0x5/0xffa [test_module]

Once decoded, these lines will look like this::

       [   68.387301] RIP: 0010:test_module_init (/home/username/linux-5.10.5/test-module/test-module.c:16) test_module

In this case the executed code was built from the file
'~/linux-5.10.5/test-module/test-module.c' and the error occurred by the
instructions found in line '16'.

The script will similarly decode the addresses mentioned in the section
starting with 'Call trace', which show the path to the function where the
problem occurred. Additionally, the script will show the assembler output for
the code section the kernel was executing.

Note, if you can't get this to work, simply skip this step and mention the
reason for it in the report. If you're lucky, it might not be needed. And if it
is, someone might help you to get things going. Also be aware this is just one
of several ways to decode kernel stack traces. Sometimes different steps will
be required to retrieve the relevant details. Don't worry about that, if that's
needed in your case, developers will tell you what to do.


Special care for regressions
----------------------------

    *If your problem is a regression, try to narrow down when the issue was
    introduced as much as possible.*

Linux lead developer Linus Torvalds insists that the Linux kernel never
worsens, that's why he deems regressions as unacceptable and wants to see them
fixed quickly. That's why changes that introduced a regression are often
promptly reverted if the issue they cause can't get solved quickly any other
way. Reporting a regression is thus a bit like playing a kind of trump card to
get something quickly fixed. But for that to happen the change that's causing
the regression needs to be known. Normally it's up to the reporter to track
down the culprit, as maintainers often won't have the time or setup at hand to
reproduce it themselves.

To find the change there is a process called 'bisection' which the document
Documentation/admin-guide/bug-bisect.rst describes in detail. That process
will often require you to build about ten to twenty kernel images, trying to
reproduce the issue with each of them before building the next. Yes, that takes
some time, but don't worry, it works a lot quicker than most people assume.
Thanks to a 'binary search' this will lead you to the one commit in the source
code management system that's causing the regression. Once you find it, search
the net for the subject of the change, its commit id and the shortened commit id
(the first 12 characters of the commit id). This will lead you to existing
reports about it, if there are any.

Note, a bisection needs a bit of know-how, which not everyone has, and quite a
bit of effort, which not everyone is willing to invest. Nevertheless, it's
highly recommended performing a bisection yourself. If you really can't or
don't want to go down that route at least find out which mainline kernel
introduced the regression. If something for example breaks when switching from
5.5.15 to 5.8.4, then try at least all the mainline releases in that area (5.6,
5.7 and 5.8) to check when it first showed up. Unless you're trying to find a
regression in a stable or longterm kernel, avoid testing versions which number
has three sections (5.6.12, 5.7.8), as that makes the outcome hard to
interpret, which might render your testing useless. Once you found the major
version which introduced the regression, feel free to move on in the reporting
process. But keep in mind: it depends on the issue at hand if the developers
will be able to help without knowing the culprit. Sometimes they might
recognize from the report want went wrong and can fix it; other times they will
be unable to help unless you perform a bisection.

When dealing with regressions make sure the issue you face is really caused by
the kernel and not by something else, as outlined above already.

In the whole process keep in mind: an issue only qualifies as regression if the
older and the newer kernel got built with a similar configuration. This can be
achieved by using ``make olddefconfig``, as explained in more detail by
Documentation/admin-guide/reporting-regressions.rst; that document also
provides a good deal of other information about regressions you might want to be
aware of.


Write and send the report
-------------------------

    *Start to compile the report by writing a detailed description about the
    issue. Always mention a few things: the latest kernel version you installed
    for reproducing, the Linux Distribution used, and your notes on how to
    reproduce the issue. Ideally, make the kernel's build configuration
    (.config) and the output from ``dmesg`` available somewhere on the net and
    link to it. Include or upload all other information that might be relevant,
    like the output/screenshot of an Oops or the output from ``lspci``. Once
    you wrote this main part, insert a normal length paragraph on top of it
    outlining the issue and the impact quickly. On top of this add one sentence
    that briefly describes the problem and gets people to read on. Now give the
    thing a descriptive title or subject that yet again is shorter. Then you're
    ready to send or file the report like the MAINTAINERS file told you, unless
    you are dealing with one of those 'issues of high priority': they need
    special care which is explained in 'Special handling for high priority
    issues' below.*

Now that you have prepared everything it's time to write your report. How to do
that is partly explained by the three documents linked to in the preface above.
That's why this text will only mention a few of the essentials as well as
things specific to the Linux kernel.

There is one thing that fits both categories: the most crucial parts of your
report are the title/subject, the first sentence, and the first paragraph.
Developers often get quite a lot of mail. They thus often just take a few
seconds to skim a mail before deciding to move on or look closer. Thus: the
better the top section of your report, the higher are the chances that someone
will look into it and help you. And that is why you should ignore them for now
and write the detailed report first. ;-)

Things each report should mention
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Describe in detail how your issue happens with the fresh vanilla kernel you
installed. Try to include the step-by-step instructions you wrote and optimized
earlier that outline how you and ideally others can reproduce the issue; in
those rare cases where that's impossible try to describe what you did to
trigger it.

Also include all the relevant information others might need to understand the
issue and its environment. What's actually needed depends a lot on the issue,
but there are some things you should include always:

 * the output from ``cat /proc/version``, which contains the Linux kernel
   version number and the compiler it was built with.

 * the Linux distribution the machine is running (``hostnamectl | grep
   "Operating System"``)

 * the architecture of the CPU and the operating system (``uname -mi``)

 * if you are dealing with a regression and performed a bisection, mention the
   subject and the commit-id of the change that is causing it.

In a lot of cases it's also wise to make two more things available to those
that read your report:

 * the configuration used for building your Linux kernel (the '.config' file)

 * the kernel's messages that you get from ``dmesg`` written to a file. Make
   sure that it starts with a line like 'Linux version 5.8-1
   (foobar@example.com) (gcc (GCC) 10.2.1, GNU ld version 2.34) #1 SMP Mon Aug
   3 14:54:37 UTC 2020' If it's missing, then important messages from the first
   boot phase already got discarded. In this case instead consider using
   ``journalctl -b 0 -k``; alternatively you can also reboot, reproduce the
   issue and call ``dmesg`` right afterwards.

These two files are big, that's why it's a bad idea to put them directly into
your report. If you are filing the issue in a bug tracker then attach them to
the ticket. If you report the issue by mail do not attach them, as that makes
the mail too large; instead do one of these things:

 * Upload the files somewhere public (your website, a public file paste
   service, a ticket created just for this purpose on `bugzilla.kernel.org
   <https://bugzilla.kernel.org/>`_, ...) and include a link to them in your
   report. Ideally use something where the files stay available for years, as
   they could be useful to someone many years from now; this for example can
   happen if five or ten years from now a developer works on some code that was
   changed just to fix your issue.

 * Put the files aside and mention you will send them later in individual
   replies to your own mail. Just remember to actually do that once the report
   went out. ;-)

Things that might be wise to provide
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Depending on the issue you might need to add more background data. Here are a
few suggestions what often is good to provide:

 * If you are dealing with a 'warning', an 'OOPS' or a 'panic' from the kernel,
   include it. If you can't copy'n'paste it, try to capture a netconsole trace
   or at least take a picture of the screen.

 * If the issue might be related to your computer hardware, mention what kind
   of system you use. If you for example have problems with your graphics card,
   mention its manufacturer, the card's model, and what chip is uses. If it's a
   laptop mention its name, but try to make sure it's meaningful. 'Dell XPS 13'
   for example is not, because it might be the one from 2012; that one looks
   not that different from the one sold today, but apart from that the two have
   nothing in common. Hence, in such cases add the exact model number, which
   for example are '9380' or '7390' for XPS 13 models introduced during 2019.
   Names like 'Lenovo Thinkpad T590' are also somewhat ambiguous: there are
   variants of this laptop with and without a dedicated graphics chip, so try
   to find the exact model name or specify the main components.

 * Mention the relevant software in use. If you have problems with loading
   modules, you want to mention the versions of kmod, systemd, and udev in use.
   If one of the DRM drivers misbehaves, you want to state the versions of
   libdrm and Mesa; also specify your Wayland compositor or the X-Server and
   its driver. If you have a filesystem issue, mention the version of
   corresponding filesystem utilities (e2fsprogs, btrfs-progs, xfsprogs, ...).

 * Gather additional information from the kernel that might be of interest. The
   output from ``lspci -nn`` will for example help others to identify what
   hardware you use. If you have a problem with hardware you even might want to
   make the output from ``sudo lspci -vvv`` available, as that provides
   insights how the components were configured. For some issues it might be
   good to include the contents of files like ``/proc/cpuinfo``,
   ``/proc/ioports``, ``/proc/iomem``, ``/proc/modules``, or
   ``/proc/scsi/scsi``. Some subsystem also offer tools to collect relevant
   information. One such tool is ``alsa-info.sh`` `which the audio/sound
   subsystem developers provide <https://www.alsa-project.org/wiki/AlsaInfo>`_.

Those examples should give your some ideas of what data might be wise to
attach, but you have to think yourself what will be helpful for others to know.
Don't worry too much about forgetting something, as developers will ask for
additional details they need. But making everything important available from
the start increases the chance someone will take a closer look.


The important part: the head of your report
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Now that you have the detailed part of the report prepared let's get to the
most important section: the first few sentences. Thus go to the top, add
something like 'The detailed description:' before the part you just wrote and
insert two newlines at the top. Now write one normal length paragraph that
describes the issue roughly. Leave out all boring details and focus on the
crucial parts readers need to know to understand what this is all about; if you
think this bug affects a lot of users, mention this to get people interested.

Once you did that insert two more lines at the top and write a one sentence
summary that explains quickly what the report is about. After that you have to
get even more abstract and write an even shorter subject/title for the report.

Now that you have written this part take some time to optimize it, as it is the
most important parts of your report: a lot of people will only read this before
they decide if reading the rest is time well spent.

Now send or file the report like the :ref:`MAINTAINERS <maintainers>` file told
you, unless it's one of those 'issues of high priority' outlined earlier: in
that case please read the next subsection first before sending the report on
its way.

Special handling for high priority issues
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Reports for high priority issues need special handling.

**Severe issues**: make sure the subject or ticket title as well as the first
paragraph makes the severeness obvious.

**Regressions**: make the report's subject start with '[REGRESSION]'.

In case you performed a successful bisection, use the title of the change that
introduced the regression as the second part of your subject. Make the report
also mention the commit id of the culprit. In case of an unsuccessful bisection,
make your report mention the latest tested version that's working fine (say 5.7)
and the oldest where the issue occurs (say 5.8-rc1).

When sending the report by mail, CC the Linux regressions mailing list
(regressions@lists.linux.dev). In case the report needs to be filed to some web
tracker, proceed to do so. Once filed, forward the report by mail to the
regressions list; CC the maintainer and the mailing list for the subsystem in
question. Make sure to inline the forwarded report, hence do not attach it.
Also add a short note at the top where you mention the URL to the ticket.

When mailing or forwarding the report, in case of a successful bisection add the
author of the culprit to the recipients; also CC everyone in the signed-off-by
chain, which you find at the end of its commit message.

**Security issues**: for these issues your will have to evaluate if a
short-term risk to other users would arise if details were publicly disclosed.
If that's not the case simply proceed with reporting the issue as described.
For issues that bear such a risk you will need to adjust the reporting process
slightly:

 * If the MAINTAINERS file instructed you to report the issue by mail, do not
   CC any public mailing lists.

 * If you were supposed to file the issue in a bug tracker make sure to mark
   the ticket as 'private' or 'security issue'. If the bug tracker does not
   offer a way to keep reports private, forget about it and send your report as
   a private mail to the maintainers instead.

In both cases make sure to also mail your report to the addresses the
MAINTAINERS file lists in the section 'security contact'. Ideally directly CC
them when sending the report by mail. If you filed it in a bug tracker, forward
the report's text to these addresses; but on top of it put a small note where
you mention that you filed it with a link to the ticket.

See Documentation/process/security-bugs.rst for more information.


Duties after the report went out
--------------------------------

    *Wait for reactions and keep the thing rolling until you can accept the
    outcome in one way or the other. Thus react publicly and in a timely manner
    to any inquiries. Test proposed fixes. Do proactive testing: retest with at
    least every first release candidate (RC) of a new mainline version and
    report your results. Send friendly reminders if things stall. And try to
    help yourself, if you don't get any help or if it's unsatisfying.*

If your report was good and you are really lucky then one of the developers
might immediately spot what's causing the issue; they then might write a patch
to fix it, test it, and send it straight for integration in mainline while
tagging it for later backport to stable and longterm kernels that need it. Then
all you need to do is reply with a 'Thank you very much' and switch to a version
with the fix once it gets released.

But this ideal scenario rarely happens. That's why the job is only starting
once you got the report out. What you'll have to do depends on the situations,
but often it will be the things listed below. But before digging into the
details, here are a few important things you need to keep in mind for this part
of the process.


General advice for further interactions
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Always reply in public**: When you filed the issue in a bug tracker, always
reply there and do not contact any of the developers privately about it. For
mailed reports always use the 'Reply-all' function when replying to any mails
you receive. That includes mails with any additional data you might want to add
to your report: go to your mail applications 'Sent' folder and use 'reply-all'
on your mail with the report. This approach will make sure the public mailing
list(s) and everyone else that gets involved over time stays in the loop; it
also keeps the mail thread intact, which among others is really important for
mailing lists to group all related mails together.

There are just two situations where a comment in a bug tracker or a 'Reply-all'
is unsuitable:

 * Someone tells you to send something privately.

 * You were told to send something, but noticed it contains sensitive
   information that needs to be kept private. In that case it's okay to send it
   in private to the developer that asked for it. But note in the ticket or a
   mail that you did that, so everyone else knows you honored the request.

**Do research before asking for clarifications or help**: In this part of the
process someone might tell you to do something that requires a skill you might
not have mastered yet. For example, you might be asked to use some test tools
you never have heard of yet; or you might be asked to apply a patch to the
Linux kernel sources to test if it helps. In some cases it will be fine sending
a reply asking for instructions how to do that. But before going that route try
to find the answer own your own by searching the internet; alternatively
consider asking in other places for advice. For example ask a friend or post
about it to a chatroom or forum you normally hang out.

**Be patient**: If you are really lucky you might get a reply to your report
within a few hours. But most of the time it will take longer, as maintainers
are scattered around the globe and thus might be in a different time zone – one
where they already enjoy their night away from keyboard.

In general, kernel developers will take one to five business days to respond to
reports. Sometimes it will take longer, as they might be busy with the merge
windows, other work, visiting developer conferences, or simply enjoying a long
summer holiday.

The 'issues of high priority' (see above for an explanation) are an exception
here: maintainers should address them as soon as possible; that's why you
should wait a week at maximum (or just two days if it's something urgent)
before sending a friendly reminder.

Sometimes the maintainer might not be responding in a timely manner; other
times there might be disagreements, for example if an issue qualifies as
regression or not. In such cases raise your concerns on the mailing list and
ask others for public or private replies how to move on. If that fails, it
might be appropriate to get a higher authority involved. In case of a WiFi
driver that would be the wireless maintainers; if there are no higher level
maintainers or all else fails, it might be one of those rare situations where
it's okay to get Linus Torvalds involved.

**Proactive testing**: Every time the first pre-release (the 'rc1') of a new
mainline kernel version gets released, go and check if the issue is fixed there
or if anything of importance changed. Mention the outcome in the ticket or in a
mail you sent as reply to your report (make sure it has all those in the CC
that up to that point participated in the discussion). This will show your
commitment and that you are willing to help. It also tells developers if the
issue persists and makes sure they do not forget about it. A few other
occasional retests (for example with rc3, rc5 and the final) are also a good
idea, but only report your results if something relevant changed or if you are
writing something anyway.

With all these general things off the table let's get into the details of how
to help to get issues resolved once they were reported.

Inquires and testing request
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Here are your duties in case you got replies to your report:

**Check who you deal with**: Most of the time it will be the maintainer or a
developer of the particular code area that will respond to your report. But as
issues are normally reported in public it could be anyone that's replying —
including people that want to help, but in the end might guide you totally off
track with their questions or requests. That rarely happens, but it's one of
many reasons why it's wise to quickly run an internet search to see who you're
interacting with. By doing this you also get aware if your report was heard by
the right people, as a reminder to the maintainer (see below) might be in order
later if discussion fades out without leading to a satisfying solution for the
issue.

**Inquiries for data**: Often you will be asked to test something or provide
additional details. Try to provide the requested information soon, as you have
the attention of someone that might help and risk losing it the longer you
wait; that outcome is even likely if you do not provide the information within
a few business days.

**Requests for testing**: When you are asked to test a diagnostic patch or a
possible fix, try to test it in timely manner, too. But do it properly and make
sure to not rush it: mixing things up can happen easily and can lead to a lot
of confusion for everyone involved. A common mistake for example is thinking a
proposed patch with a fix was applied, but in fact wasn't. Things like that
happen even to experienced testers occasionally, but they most of the time will
notice when the kernel with the fix behaves just as one without it.

What to do when nothing of substance happens
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Some reports will not get any reaction from the responsible Linux kernel
developers; or a discussion around the issue evolved, but faded out with
nothing of substance coming out of it.

In these cases wait two (better: three) weeks before sending a friendly
reminder: maybe the maintainer was just away from keyboard for a while when
your report arrived or had something more important to take care of. When
writing the reminder, kindly ask if anything else from your side is needed to
get the ball running somehow. If the report got out by mail, do that in the
first lines of a mail that is a reply to your initial mail (see above) which
includes a full quote of the original report below: that's on of those few
situations where such a 'TOFU' (Text Over, Fullquote Under) is the right
approach, as then all the recipients will have the details at hand immediately
in the proper order.

After the reminder wait three more weeks for replies. If you still don't get a
proper reaction, you first should reconsider your approach. Did you maybe try
to reach out to the wrong people? Was the report maybe offensive or so
confusing that people decided to completely stay away from it? The best way to
rule out such factors: show the report to one or two people familiar with FLOSS
issue reporting and ask for their opinion. Also ask them for their advice how
to move forward. That might mean: prepare a better report and make those people
review it before you send it out. Such an approach is totally fine; just
mention that this is the second and improved report on the issue and include a
link to the first report.

If the report was proper you can send a second reminder; in it ask for advice
why the report did not get any replies. A good moment for this second reminder
mail is shortly after the first pre-release (the 'rc1') of a new Linux kernel
version got published, as you should retest and provide a status update at that
point anyway (see above).

If the second reminder again results in no reaction within a week, try to
contact a higher-level maintainer asking for advice: even busy maintainers by
then should at least have sent some kind of acknowledgment.

Remember to prepare yourself for a disappointment: maintainers ideally should
react somehow to every issue report, but they are only obliged to fix those
'issues of high priority' outlined earlier. So don't be too devastating if you
get a reply along the lines of 'thanks for the report, I have more important
issues to deal with currently and won't have time to look into this for the
foreseeable future'.

It's also possible that after some discussion in the bug tracker or on a list
nothing happens anymore and reminders don't help to motivate anyone to work out
a fix. Such situations can be devastating, but is within the cards when it
comes to Linux kernel development. This and several other reasons for not
getting help are explained in 'Why some issues won't get any reaction or remain
unfixed after being reported' near the end of this document.

Don't get devastated if you don't find any help or if the issue in the end does
not get solved: the Linux kernel is FLOSS and thus you can still help yourself.
You for example could try to find others that are affected and team up with
them to get the issue resolved. Such a team could prepare a fresh report
together that mentions how many you are and why this is something that in your
option should get fixed. Maybe together you can also narrow down the root cause
or the change that introduced a regression, which often makes developing a fix
easier. And with a bit of luck there might be someone in the team that knows a
bit about programming and might be able to write a fix.


Reference for "Reporting regressions within a stable and longterm kernel line"
------------------------------------------------------------------------------

This subsection provides details for the steps you need to perform if you face
a regression within a stable and longterm kernel line.

Make sure the particular version line still gets support
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

    *Check if the kernel developers still maintain the Linux kernel version
    line you care about: go to the front page of kernel.org and make sure it
    mentions the latest release of the particular version line without an
    '[EOL]' tag.*

Most kernel version lines only get supported for about three months, as
maintaining them longer is quite a lot of work. Hence, only one per year is
chosen and gets supported for at least two years (often six). That's why you
need to check if the kernel developers still support the version line you care
for.

Note, if kernel.org lists two stable version lines on the front page, you
should consider switching to the newer one and forget about the older one:
support for it is likely to be abandoned soon. Then it will get a "end-of-life"
(EOL) stamp. Version lines that reached that point still get mentioned on the
kernel.org front page for a week or two, but are unsuitable for testing and
reporting.

Search stable mailing list
~~~~~~~~~~~~~~~~~~~~~~~~~~

    *Check the archives of the Linux stable mailing list for existing reports.*

Maybe the issue you face is already known and was fixed or is about to. Hence,
`search the archives of the Linux stable mailing list
<https://lore.kernel.org/stable/>`_ for reports about an issue like yours. If
you find any matches, consider joining the discussion, unless the fix is
already finished and scheduled to get applied soon.

Reproduce issue with the newest release
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

    *Install the latest release from the particular version line as a vanilla
    kernel. Ensure this kernel is not tainted and still shows the problem, as
    the issue might have already been fixed there. If you first noticed the
    problem with a vendor kernel, check a vanilla build of the last version
    known to work performs fine as well.*

Before investing any more time in this process you want to check if the issue
was already fixed in the latest release of version line you're interested in.
This kernel needs to be vanilla and shouldn't be tainted before the issue
happens, as detailed outlined already above in the section "Install a fresh
kernel for testing".

Did you first notice the regression with a vendor kernel? Then changes the
vendor applied might be interfering. You need to rule that out by performing
a recheck. Say something broke when you updated from 5.10.4-vendor.42 to
5.10.5-vendor.43. Then after testing the latest 5.10 release as outlined in
the previous paragraph check if a vanilla build of Linux 5.10.4 works fine as
well. If things are broken there, the issue does not qualify as upstream
regression and you need switch back to the main step-by-step guide to report
the issue.

Report the regression
~~~~~~~~~~~~~~~~~~~~~

    *Send a short problem report to the Linux stable mailing list
    (stable@vger.kernel.org) and CC the Linux regressions mailing list
    (regressions@lists.linux.dev); if you suspect the cause in a particular
    subsystem, CC its maintainer and its mailing list. Roughly describe the
    issue and ideally explain how to reproduce it. Mention the first version
    that shows the problem and the last version that's working fine. Then
    wait for further instructions.*

When reporting a regression that happens within a stable or longterm kernel
line (say when updating from 5.10.4 to 5.10.5) a brief report is enough for
the start to get the issue reported quickly. Hence a rough description to the
stable and regressions mailing list is all it takes; but in case you suspect
the cause in a particular subsystem, CC its maintainers and its mailing list
as well, because that will speed things up.

And note, it helps developers a great deal if you can specify the exact version
that introduced the problem. Hence if possible within a reasonable time frame,
try to find that version using vanilla kernels. Let's assume something broke when
your distributor released a update from Linux kernel 5.10.5 to 5.10.8. Then as
instructed above go and check the latest kernel from that version line, say
5.10.9. If it shows the problem, try a vanilla 5.10.5 to ensure that no patches
the distributor applied interfere. If the issue doesn't manifest itself there,
try 5.10.7 and then (depending on the outcome) 5.10.8 or 5.10.6 to find the
first version where things broke. Mention it in the report and state that 5.10.9
is still broken.

What the previous paragraph outlines is basically a rough manual 'bisection'.
Once your report is out your might get asked to do a proper one, as it allows to
pinpoint the exact change that causes the issue (which then can easily get
reverted to fix the issue quickly). Hence consider to do a proper bisection
right away if time permits. See the section 'Special care for regressions' and
the document Documentation/admin-guide/bug-bisect.rst for details how to
perform one. In case of a successful bisection add the author of the culprit to
the recipients; also CC everyone in the signed-off-by chain, which you find at
the end of its commit message.


Reference for "Reporting issues only occurring in older kernel version lines"
-----------------------------------------------------------------------------

This section provides details for the steps you need to take if you could not
reproduce your issue with a mainline kernel, but want to see it fixed in older
version lines (aka stable and longterm kernels).

Some fixes are too complex
~~~~~~~~~~~~~~~~~~~~~~~~~~

    *Prepare yourself for the possibility that going through the next few steps
    might not get the issue solved in older releases: the fix might be too big
    or risky to get backported there.*

Even small and seemingly obvious code-changes sometimes introduce new and
totally unexpected problems. The maintainers of the stable and longterm kernels
are very aware of that and thus only apply changes to these kernels that are
within rules outlined in Documentation/process/stable-kernel-rules.rst.

Complex or risky changes for example do not qualify and thus only get applied
to mainline. Other fixes are easy to get backported to the newest stable and
longterm kernels, but too risky to integrate into older ones. So be aware the
fix you are hoping for might be one of those that won't be backported to the
version line your care about. In that case you'll have no other choice then to
live with the issue or switch to a newer Linux version, unless you want to
patch the fix into your kernels yourself.

Common preparations
~~~~~~~~~~~~~~~~~~~

    *Perform the first three steps in the section "Reporting issues only
    occurring in older kernel version lines" above.*

You need to carry out a few steps already described in another section of this
guide. Those steps will let you:

 * Check if the kernel developers still maintain the Linux kernel version line
   you care about.

 * Search the Linux stable mailing list for exiting reports.

 * Check with the latest release.


Check code history and search for existing discussions
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

    *Search the Linux kernel version control system for the change that fixed
    the issue in mainline, as its commit message might tell you if the fix is
    scheduled for backporting already. If you don't find anything that way,
    search the appropriate mailing lists for posts that discuss such an issue
    or peer-review possible fixes; then check the discussions if the fix was
    deemed unsuitable for backporting. If backporting was not considered at
    all, join the newest discussion, asking if it's in the cards.*

In a lot of cases the issue you deal with will have happened with mainline, but
got fixed there. The commit that fixed it would need to get backported as well
to get the issue solved. That's why you want to search for it or any
discussions abound it.

 * First try to find the fix in the Git repository that holds the Linux kernel
   sources. You can do this with the web interfaces `on kernel.org
   <https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/>`_
   or its mirror `on GitHub <https://github.com/torvalds/linux>`_; if you have
   a local clone you alternatively can search on the command line with ``git
   log --grep=<pattern>``.

   If you find the fix, look if the commit message near the end contains a
   'stable tag' that looks like this:

          Cc: <stable@vger.kernel.org> # 5.4+

   If that's case the developer marked the fix safe for backporting to version
   line 5.4 and later. Most of the time it's getting applied there within two
   weeks, but sometimes it takes a bit longer.

 * If the commit doesn't tell you anything or if you can't find the fix, look
   again for discussions about the issue. Search the net with your favorite
   internet search engine as well as the archives for the `Linux kernel
   developers mailing list <https://lore.kernel.org/lkml/>`_. Also read the
   section `Locate kernel area that causes the issue` above and follow the
   instructions to find the subsystem in question: its bug tracker or mailing
   list archive might have the answer you are looking for.

 * If you see a proposed fix, search for it in the version control system as
   outlined above, as the commit might tell you if a backport can be expected.

   * Check the discussions for any indicators the fix might be too risky to get
     backported to the version line you care about. If that's the case you have
     to live with the issue or switch to the kernel version line where the fix
     got applied.

   * If the fix doesn't contain a stable tag and backporting was not discussed,
     join the discussion: mention the version where you face the issue and that
     you would like to see it fixed, if suitable.


Ask for advice
~~~~~~~~~~~~~~

    *One of the former steps should lead to a solution. If that doesn't work
    out, ask the maintainers for the subsystem that seems to be causing the
    issue for advice; CC the mailing list for the particular subsystem as well
    as the stable mailing list.*

If the previous three steps didn't get you closer to a solution there is only
one option left: ask for advice. Do that in a mail you sent to the maintainers
for the subsystem where the issue seems to have its roots; CC the mailing list
for the subsystem as well as the stable mailing list (stable@vger.kernel.org).


Appendix: additional background information
===========================================

.. _unfixedbugs_repiapdx:

Why some bugs remain unfixed and some report are ignored
--------------------------------------------------------

When reporting a problem to the Linux developers, be aware that they are only
obliged to fix regressions, security issues, and severe problems. Developers,
maintainers, or, if all else fails, Linus Torvalds himself will make sure of
that. They will fix a lot of other issues as well, but sometimes they can't or
won't help -- and sometimes there isn't even anyone to send a report to.

This situation is best explained using kernel developers that contribute to the
Linux kernel in their spare time. Quite a few of the drivers in the kernel were
written by such programmers; often they simply wanted to make the
hardware they owned usable on their favorite operating system.

These programmers most of the time will happily fix problems other people
report. But nobody can force them to do so, as they are contributing
voluntarily.

There are also situations where such developers would like to fix issues,
but can't: They might lack programming documentation to do so or hardware to
test. The former can happen when the publicly available docs are superficial or
when a driver was written with the help of reverse engineering.

Sooner or later, spare-time developers usually stop caring for the driver.
Maybe their test hardware broke, was replaced by something more fancy, or
became so old that it is something you don't find much outside of computer
museums anymore. Other times developers also stop caring when
something different in life becomes more important to them. Then sometimes
nobody is willing to take over the job as maintainer -- and nobody else can be
forced to, as contributing is voluntary. The code nevertheless often stays
around, as it is useful for people; removing it would also cause a regression,
which is not allowed in Linux.

The situation is not that different with developers that are paid for their
work on the upstream Linux kernel. Those contribute the most changes these days.
But their employers set the priorities. And those sooner or later stop caring
for some code or make their
employees focus on other things. Hardware vendors, for example, earn their money
mainly by selling new hardware -- they thus often are not much interested in
investing much time and energy in maintaining a Linux kernel driver for a chip
they stopped selling years ago. Enterprise Linux distributors often care for a
longer time period, but in new versions might set support for old and rare
hardware aside to limit the scope, too. Often spare-time contributors take over
once employed developers orphan some code, but as mentioned earlier: Sooner or
later they will usually leave the code behind, too.

Priorities are another reason why some issues are not fixed, as developers
quite often are forced to set those: The spare-time of volunteers or the time
employers allot for upstream Linux kernel work is often limited. Sometimes
developers are also flooded with good and bad reports, even if a driver is
working well. To
not get completely stuck, the programmers might have no other choice than
to prioritize bug reports and ignore some.

But do not worry too much about all of this, a lot of drivers have active
maintainers who are quite interested in fixing as many issues as possible.


Why reporting Linux kernel bugs is somewhat complicated
-------------------------------------------------------

The Linux kernel's development model differs from typical Open Source projects
in a few important aspects. Four of those complicate bug reporting:

1. Developers are free to solely focus on the latest mainline codebase.

2. The 'stable team' maintains the stable and longterm kernel series, but is not
   allowed to fix many bugs just there if they happen in mainline, too.

3. There is no central bug tracker.

4. Most kernels used in Linux distributions are totally unsuitable for reporting
   bugs upstream.

Due to the first aspect, some of the developers ignore or react coldly to
reports about bugs in, say, Linux 6.1 when 6.2-rc1 is already out.

The combination of the first and the second aspect is why some of the
developers are unwilling to look into reports with stable or longterm kernels:
the problem might never have happened in the code they work on, for example
because the stable team did something wrong between 6.1.1 and 6.1.2.

The stable team due to those two aspects is often in a similar but opposite
situation when it comes to bugs reported using a kernel like 6.1.2: If that
issue already happened in 6.1 and still happens in the latest mainline kernel,
then it must be fixed there first. That is among the reasons why reporters in
the end always have to check if mainline is affected, as the stable team often
is the wrong point of contact, unless it is a series specific regression.

There are various reasons why no central bug tracker exists. They, among others,
were not a thing yet when Linux started, which is why reporting my email was
the norm initially -- and still is, as quite a few developers prefer to handle
all aspects of kernel development via email. Some, on the other hand, saw
benefits in trackers and set up bugzilla.kernel.org, which due to the
aforementioned aspect never became mandatory and has some problematic aspects;
this is why it frequently does not even forward newly filed reports to the
appropriate developers. Yet other developers prefer the comfort of Git forges
for development and issue tracking; but subsystems use various forges, so those
trackers are spread over the web.

The fourth aspect is explained in the second point of the reference section
already: Old codebases, modified sources, and add-ons lead to bugs that were
fixed a long time ago already or never happened upstream in the first place.
These are problems for many other software packaged by Linux distributions as
well. But there it is usually a smaller problem, as the modifications and
extensions distributors apply to the kernel are often much bigger and more
dangerous; the kernel also changes way more quickly, is a lot more
complex, and naturally more fragile. Due to these aspects, many developers
expect reports to be based on fresh and vanilla kernels. Furthermore, most of
them receive way more bug reports than they are able to handle, which is
why they prioritize the ones that look more promising.

Reporting a bug due to these and other unmentioned aspects is harder than in
other Free/Libre & Open Source Software projects -- the complexity of this
document proves that. But that is the state of affairs for now. The primary
author of this text hopes documenting it will lay some groundwork to improve
the situation over time.


..
   end-of-content
..
   This document is maintained by Thorsten Leemhuis <linux@leemhuis.info>. If
   you spot a typo or small mistake, feel free to let him know directly and
   he'll fix it. You are free to do the same in a mostly informal way if you
   want to contribute changes to the text, but for copyright reasons please CC
   linux-doc@vger.kernel.org and 'sign-off' your contribution as
   Documentation/process/submitting-patches.rst outlines in the section 'Sign
   your work - the Developer's Certificate of Origin'.
..
   This text is available under GPL-2.0+ or CC-BY-4.0, as stated at the top
   of the file. If you want to distribute this text under CC-BY-4.0 only,
   please use 'The Linux kernel developers' for author attribution and link
   this as source:
   https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/Documentation/admin-guide/reporting-issues.rst
..
   Note: Only the content of this RST file as found in the Linux kernel sources
   is available under CC-BY-4.0, as versions of this text that were processed
   (for example by the kernel's build system) might contain content taken from
   files which use a more restrictive license.
