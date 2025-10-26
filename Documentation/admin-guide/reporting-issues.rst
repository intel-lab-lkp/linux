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

.. _reginquiry_repisbs:

* Do you face a regression? One still occurring in a kernel version less than
  two (ideally: one) weeks old? A kernel that is vanilla or close to it? If
  you answered all three questions with 'yes', feel free to send a brief email
  to the public 'Linux regressions mailing list <regressions@lists.linux.dev>'
  asking if the problem is known. If someone confirms this to be the case,
  there most likely is no need to follow this guide further; but do so in case
  there is no reply with a pointer to a matching report within two or three
  weekdays. You are also free to immediately continue if you feel like it.

 [:ref:`details <reginquiry_repiref>`]

.. _maintainers_repisbs:

* *You must* consult ':ref:`MAINTAINERS <maintainers>`' to determine where
  developers of the affected driver or subsystem want bugs to be submitted to;
  use your best guess if in doubt which is appropriate.

 [:ref:`details <maintainers_repiref>`]

.. _otherplaces_repisbs:

* If the developers of the affected driver or subsystem use a dedicated mailing
  list not `archived on lore <https://lore.kernel.org/>`_, search its archives
  for earlier reports and potential fixes; if the subsystem is among the few
  using one of various bug trackers, search it as well.

  Checking `bugzilla.kernel.org <https://bugzilla.kernel.org/>`_ might be worth
  a shot, too. But keep in mind that for most of the kernel it is the wrong
  place to submit bug reports: Many Linux developers do not care about the bug
  tracker and are not even notified about bugs in their code submitted there.

  If you find matching reports or fixes in either place, follow the instructions
  provided earlier. In the case of Bugzilla, check if the appropriate developers
  noticed the ticket -- and if not, consider sending an email to the responsible
  people and lists pointing them to it.

 [:ref:`details <otherplaces_repiref>`]

.. _backup_repisbs:

* You might want to create a fresh backup and put both system repair and
  restore tools at hand.

 [:ref:`details <backup_repiref>`]

.. _coarsely_repisbs:

* Write down coarsely how to reproduce the issue on a freshly booted system in
  a straightforward way you can easily reproduce.

 [:ref:`details <coarsely_repiref>`]

.. _verify_repisbs:

* *You must* report the problem using a kernel suitable for reporting -- so you
  have to verify it happens with such a kernel, unless you already run one. In
  case of a regression within a stable or longterm kernel, *you must*
  furthermore check if the latest mainline kernel is affected as well. For
  regressions in general, it is also recommended to locate the culprit using a
  Git bisection.

  There are two approaches to realize those three requirements:

  * Follow 'Documentation/admin-guide/verify-bugs-and-bisect-regressions.rst',
    which is the recommended way.

  * Handle all tasks that document covers on your own:

    * For regressions within a stable or longterm series, check if the series is
      still supported by ensuring `kernel.org <https://kernel.org/>`_ lists it
      without an 'EOL' tag. Then verify the problem still happens with the
      latest release from that series; afterwards, check if the latest mainline
      kernel is affected as well. When testing, ideally recheck with a vanilla
      version of the working kernel and rule out a config change as the root of
      your problem by building all newer Linux versions with a .config from the
      latest working kernel processed by ``make olddefconfig``.

    * In all other cases, check if the bug happens with a release, pre-release,
      or snapshot of Linux mainline ideally less than one week old and two at
      maximum. The latest release from the newest stable series might work out
      as well, while longterm kernels rarely will.

    All kernels used for verifying additionally must meet the following
    criteria:

    * The kernels should be 'vanilla', e.g., built from pristine Linux sources
      -- albeit reports from kernels built from lightly patched sources such as
      those used by default in Arch Linux, Debian GNU/Linux, Fedora Linux, and
      openSUSE Tumbleweed often work, too, as long as they are fresh enough (see
      above). The kernels of most other distributions are unsuitable; this
      includes those Ubuntu and its derivatives use by default.

    * The kernels must remain 'vanilla' and thus never load any externally
      developed modules, no matter if they are proprietary or Open Source. This,
      among others, means that you will have to steer clear of Nvidia's graphics
      drivers and OpenZFS as well as drivers VirtualBox or VMware Workstation
      install.

    * The kernels should not be 'tainted' before the issue occurs. If yours is,
      rule out that it has anything to do with the problem -- and if that really
      is the case, mention the reason for the tainting in your report later.

  Once you used either of the approaches to verify the problem with a suitable
  kernel, you are free to move on with this guide and report the problem. Note,
  though, in case it is regression not yet known, you most likely will be asked
  to perform a bisection. So if you feel like it, start one right after sending
  the report -- or perform one before sending it, which will tell you to whom
  the report needs to be sent.

  In case you failed to reproduce a problem with mainline: Is it a problem that
  is not a regression? One you want to see resolved in a stable or longterm
  series? If all that is the case, head over to ':ref:`Handling non-regressions
  only occurring in stable or longterm kernels' <readysolved_repisubs>`.

  Note: Don't take the requirements in this step lightheartedly, as otherwise
  there is quite a risk your report will be fruitless or even ignored.

 [:ref:`details <verify_repiref>`]

.. _checkloretwo_repisbs:

* If you performed a bisection or learned anything new about the bug while
  following this guide so far, search once more for earlier reports
  and fixes. In the bisection case, you want to search
  `lore <https://lore.kernel.org/all/>`_   for the culprit's mainline commit-id
  abbreviated to seven characters immediately followed by an asterisk (e.g.,
  '`1f2e3d4 <https://lore.kernel.org/all/?q=1f2e3d4*>`_'); if that does not
  produce any valuable insights, search for the commit's title, too.

 [:ref:`details <checkloretwo_repiref>`]

.. _attachments_repisbs:

* Collect relevant files to supply with the report.

  It almost always is wise to store the log messages (``journalctl -k``) from
  the kernel used for the verification to a file. In case of a regression,
  create a file with log messages from a working kernel, too.

  What else is appropriate to supply depends on your problem. In case of build
  errors, the build configuration (the '.config' file) used for the verification
  is important to provide. Other times it is wise to include separate files
  with the output from commands such as ``lsblk``, ``lspci -nn``, ``lsusb.py``,
  ``alsa-info.sh``, or ``grep -s '' /sys/class/dmi/id/*``; occasionally the
  output of ``lscpu``, ``lsirq``, ``lsmod``, ``sudo lspci -vvv``, or ``lsscsi``
  makes sense, too.

  Only compress files larger than a megabyte. Do not use an archiver to package
  multiple files together.

  If you later have to file the report in a bug tracker, attach the files. If
  you have to email it, attach them when they in total are smaller than 250
  KByte; if they are bigger, attach only the most relevant and send the rest in
  a reply-to-all to your own report. Alternatively, create a ticket in
  `bugzilla.kernel.org <https://bugzilla.kernel.org/>`_ with a brief note that
  the ticket is only meant to store files used in a mailed report; attach the
  files there and later link to them in your report.

 [:ref:`details <attachments_repiref>`]

.. _compile_repisbs:

* Prepare and optimize the report.

  Start by writing a text describing the problem. Ensure it contains all the
  important bits directly so that readers do not have to open attachments or
  follow links to understand roughly what the report is about -- you thus might
  want to copy error messages and similarly important parts from supplied files
  into the text.

  Early on in the text, mention the distribution and kernel version used for the
  bug verification.

  In case of a regression, start the subject with '[REGRESSION]'. Furthermore,
  specify early in the text the latest working versions and all known to be
  broken; if you performed a bisection, mention the culprit's commit-id, title,
  and authors instead.

  Mention the Linux distribution used and other aspects of your environment
  that might be relevant, like the machine's model name, the hardware
  components involved, or the version of related userspace drivers.

  Make sure to not overload the report with a long problem description, too
  many details, or many attachments: Developers will ask for additional
  information when needed.

  Now write a subject, which is the only thing most people will read -- hence
  try hard to make it as descriptive as possible without making it overly long,
  as that is your best chance to grab people's attention. Your second best
  chance is the first paragraph. If your problem description is longer than two
  or three paragraphs, you thus want to create a small intro paragraph
  describing the gist of the problem; if it is shorter, optimize the early
  sentences.

  At the end, review and optimize the report once more to make it as
  straightforward as possible while ensuring the problem is easy to grasp for
  people new to it.

 [:ref:`details <compile_repiref>`]

.. _submit_repisbs:

* Submit your report in the appropriate way, which depends on the outcome of
  the verification and the MAINTAINERS entry.

  * Are you facing a regression within a stable or longterm kernel series you
    were unable to reproduce with a fresh mainline kernel? Then report it by
    email to the stable team while CCing the regressions list (To: Greg
    Kroah-Hartman <gregkh@linuxfoundation.org>, Sasha Levin <sashal@kernel.org>;
    CC: stable@vger.kernel.org, regressions@lists.linux.dev); if you performed a
    bisection, CC everyone in the culprit's 'Signed-off-by' chain, too.

  * In all other cases, submit the report as specified in MAINTAINERS while
    keeping Documentation/process/security-bugs.rst in mind in case you deal
    with a security issue:

    * If that means reporting by email, CC linux-kernel@vger.kernel.org. In case
      of a regression, CC regressions@lists.linux.dev, too -- and when the
      culprit is known, also everyone in its 'Signed-off-by' chain, while
      addressing the email to the culprit's author.

    * If that means submitting a regression to a bug tracker, perform
      one more thing afterwards: Write a short heads-up email with a link to the
      report to regressions@lists.linux.dev -- and if the culprit is known, CC
      everyone that signed it off, while addressing the email to the culprit's
      author.

  Whichever way it is, in case you sent the brief inquiry mentioned initially
  to the regressions list, try to keep that discussion involved: Either send
  your report as a reply to the earlier inquiry while adding relevant parties
  or send a quick reply-to-self with a link to the proper report.

 [:ref:`details <submit_repiref>`]

 * Wait for reactions and keep the thing rolling until you can accept the
   outcome in one way or the other. Thus react publicly and in a timely manner
   to any inquiries. Test proposed fixes. Do proactive testing: retest with at
   least every first release candidate (RC) of a new mainline version and
   report your results. Send friendly reminders if things stall. And try to
   help yourself, if you don't get any help or if it's unsatisfying.


Handling non-regressions only occurring in stable or longterm kernels
---------------------------------------------------------------------

Follow the next few steps only if the step-by-step guide sent you here. That is
the case when you are (a) facing an issue in the latest release of a still
supported stable or longterm series that (b) you were unable to reproduce in
the current mainline and (c) is not a regression. If all of that holds true,
follow these steps:

* Be aware: It is possible the issue will not be resolved, as the fix might be
  too big or risky to backport.

* Search Linux's mainline Git repository or `lore
  <https://lore.kernel.org/all/>`_ for the change resolving the issue. In case
  you have trouble locating it, consider using a bisection; alternatively, ask
  on the list of the affected subsystem for advice while CCing the relevant
  maintainers and developers.

* Check if the change is already scheduled to be backported by searching the
  patch description for a 'stable tag' (e.g., a line like 'Cc:
  <stable@vger.kernel.org>') and the patch's title in lore.kernel.org:

  * If the change is already scheduled for backporting, it will usually be
    picked up within one or two weeks after being mainlined. Note, though, plans
    sometimes change; a comment next to the stable tag might also limit how far
    the fix is backported and thus exclude the series you care about. If there
    are good reasons for this, you are out of luck. If you can't spot any:

    Send a reply asking the involved developers if backporting to the series is
    an option. Note, though, the developers might greenlight backporting, but
    unwilling to handle the work themselves -- in which case you or somebody
    else must test and submit the fix and everything it depends on, as explained
    in Documentation/process/stable-kernel-rules.rst.

  * If the change is not scheduled for backporting, search `lore
    <https://lore.kernel.org/all/>`_ for the review of the fix and check if
    backporting is planned or was rejected. If it is neither, send a reply
    asking the involved developers if backporting to the series is an option.
    Just as mentioned in the previous paragraph, you might need to handle
    backporting on your own.


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


.. _reginquiry_repiref:

Fast track for regressions
--------------------------

  *Do you face a regression? One still occurring in a kernel version less than
  two (ideally: one) weeks old? A kernel that is vanilla or close to it? If you
  answered* [:ref:`... <reginquiry_repisbs>`]

This is an optional fast track that might relieve you from further work on
reporting in case the issue is already known. Note: It are volunteers that
answer these emails on a best-effort basis.

[:ref:`back to step-by-step guide <reginquiry_repisbs>`]


.. _maintainers_repiref:

Check how to report your issue
------------------------------

  *You must consult MAINTAINERS to determine where developers of the affected
  driver or subsystem want bugs to be submitted to; use your best guess, if* [:ref:`...  <maintainers_repisbs>`]

It is crucial to submit your report to the right place, as the Linux kernel is a
big project and most of its developers are only familiar with a small subset of
it. Quite a few programmers, for example, only care for just one driver, like
one for a particular WiFi chip; its developer likely will only have small or no
knowledge about the internals of near, remote, or unrelated subsystems, like
the TCP stack, the PCIe/PCI subsystem, memory management, or file systems.

Problem is: The Linux kernel lacks a central bug tracker where you can simply
file your issue and make it reach the developers that need to know about it.
That is why you have to find the right place and way to report issues yourself.
You can do that with the help of a script (see below), but it mainly targets
kernel developers and experts. For everybody else, using the MAINTAINERS file is
the better approach.

How to read the MAINTAINERS file
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

To illustrate how to use the :ref:`MAINTAINERS <maintainers>` file, let's assume
the WiFi in your Laptop misbehaves. In that
case it is likely an issue in the WiFi driver. Obviously it could also be some
underlying code from other subsystems, but unless something hints at that,
stick to the driver; if it is really something else, the driver's developers
will involve the
right people.

Sadly, there is no way to check which code is driving a particular hardware
component that is both universal and easy.

In case of a problem with the WiFi driver, you, for example, might want to look
at the output of ``lspci -k``, as it lists devices on the PCI/PCIe bus and the
kernel module driving it::

   [user@something ~]$ lspci -k
   [...]
   3a:00.0 Network controller: Qualcomm Atheros QCA6174 802.11ac Wireless Network Adapter (rev 32)
   Subsystem: Bigfoot Networks, Inc. Device 1535
   Kernel driver in use: ath10k_pci
   Kernel modules: ath10k_pci
   [...]

But this approach won't work if your WiFi chip is connected over USB or some
other internal bus. In those cases you might want to check your network manager
or the output of ``ip link``. Look for the name of the problematic network
interface, which might be something like 'wlp58s0'. This name can be used like
this to find the module driving it::

   [user@something ~]$ realpath --relative-to=/sys/module/ /sys/class/net/wlp58s0/device/driver/module
   ath10k_pci

In case tricks like these don't bring you any further, try to search the
internet on how to narrow down the driver or subsystem in question. And if you
are unsure which it is: Just try your best guess, somebody will usually help out
if you guessed poorly.

Once you know the driver or subsystem, you want to search for it in the
MAINTAINERS file. In the case of 'ath10k_pci' you won't find anything, as the
name is too specific. Sometimes you will need to search on the net for help;
but before doing so, try a somewhat shortened or modified name when searching
the MAINTAINERS file, as then you might find something like this::

   QUALCOMM ATHEROS ATH10K WIRELESS DRIVER
   Mail:          A. Some Human <shuman@example.com>
   Mailing list:  ath10k@lists.infradead.org
   Status:        Supported
   Web-page:      https://wireless.wiki.kernel.org/en/users/Drivers/ath10k
   SCM:           git git://git.kernel.org/pub/scm/linux/kernel/git/kvalo/ath.git
   Files:         drivers/net/wireless/ath/ath10k/

Note: Line descriptions like 'Status' will be abbreviations like 'S:' if you
read the plain MAINTAINERS file found in the root of the Linux source tree.

First look at the line 'Status' ('S:'). Ideally it should be 'Supported' or
'Maintained'. If it states 'Obsolete' then you are using some outdated approach
that was replaced by a newer solution you need to switch to. Sometimes the code
only has someone who provides 'Odd Fixes' when feeling motivated. And with
'Orphan' you are totally out of luck, as nobody takes care of the code anymore.
That only leaves these options: Arrange yourself to live with the issue, fix it
yourself, or find a programmer somewhere willing to fix it.

After checking the status, look for a line starting with 'bugs:' ('B:'): It
will tell you where to find a subsystem-specific bug tracker to file your
issue. The
example above does not have such a line. That is the case for most sections, as
Linux kernel development is completely driven by email: Very few subsystems use
a bug tracker, and only some of those rely on bugzilla.kernel.org.

In this and many other cases, you thus have to look for lines starting with
'Mail:' ('M:') instead. Those mention the name and the email addresses for the
maintainers of the particular code. Also look for a line starting with 'Mailing
List:' ('L:'), which tells you the public mailing list where the code is
developed. Your report later needs to go by email to those addresses.
Additionally, for all issue reports sent by email, make sure to add the Linux
Kernel Mailing List (LKML) <linux-kernel@vger.kernel.org> to CC. Don't omit
either of the mailing lists when sending your issue report by email later!
Maintainers are busy people and might leave some work for other developers on
the subsystem-specific list -- and LKML is important to have one place where all
issue reports can be found.


Finding the maintainers with the help of a script
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

For people that have the Linux sources at hand, there is a second option to find
the proper place to report: The script 'scripts/get_maintainer.pl' which tries
to find all people to contact. It queries the MAINTAINERS file and needs to be
called with a path to the source code in question. For drivers compiled as
module, it often can be found with a command like this::

   $ modinfo ath10k_pci | grep filename | sed 's!/lib/modules/.*/kernel/!!; s!filename:!!; s!\.ko\(\|\.xz\)!!'
   drivers/net/wireless/ath/ath10k/ath10k_pci.ko

Pass parts of this to the script::

   $ ./scripts/get_maintainer.pl --no-git -f drivers/net/wireless/ath/ath10k*
   Some Human <shuman@example.com> (supporter:QUALCOMM ATHEROS ATH10K WIRELESS DRIVER)
   Another S. Human <asomehuman@example.com> (maintainer:NETWORKING DRIVERS)
   ath10k@lists.infradead.org (open list:QUALCOMM ATHEROS ATH10K WIRELESS DRIVER)
   linux-wireless@vger.kernel.org (open list:NETWORKING DRIVERS (WIRELESS))
   netdev@vger.kernel.org (open list:NETWORKING DRIVERS)
   linux-kernel@vger.kernel.org (open list)

Usually you want to send your report to all of them.

Note: In case you cloned the Linux sources with Git, you might want to call
``get_maintainer.pl`` a second time with ``--git``. The script then will look
at the commit history to find which people recently worked on the code in
question, as they might be able to help. But use these results with care, as
they can easily send you in the wrong direction. That, for example, happens
quickly in areas rarely changed (like old or unmaintained drivers): Sometimes
such code is modified during tree-wide cleanups by developers that do not care
about the particular driver at all.

[:ref:`back to step-by-step guide <maintainers_repisbs>`]


.. _otherplaces_repiref:

Search for existing reports in other places
-------------------------------------------

  *If the developers of the affected driver or subsystem use a dedicated
  mailing list not archived on lore, search its archives for earlier reports
  and potential fixes; if the subsystem is* [:ref:`... <otherplaces_repisbs>`]

Now that you know where they need to be reported to, search the target for
existing reports again. If it is a mailing list, you will often find its
archives on `lore <https://lore.kernel.org/all/>`_.

But some lists are hosted in different places. That, for example, is the case
for the ath10k WiFi driver used as an example in the previous step. But you'll
often find the archives for these lists easily on the net. Searching for
'archive ath10k@lists.infradead.org', for example, will lead you to the `Info
page for the ath10k mailing list <https://lists.infradead.org/mailman/listinfo/ath10k>`_,
which at the top links to its `list archives <https://lists.infradead.org/pipermail/ath10k/>`_.
Sadly, this and quite a few other lists miss a way to search the archives. In
those cases, use a regular internet search engine and add something like
'site:lists.infradead.org/pipermail/ath10k/' to your search terms, which limits
the results to the archives at that URL.

It is also wise to check the internet, LKML and maybe bugzilla.kernel.org again
at this point. If your report needs to be filed in a bug tracker, you may want
to check the mailing list archives for the subsystem as well, as someone might
have reported it only there.

For details on how to search and what to do if you find matching reports, see
':ref:`Search for existing reports and fixes <checkloreone_repiref>`' above.

Do not hurry with this step of the reporting process: spending 30 to 60 minutes
or even more time can save everyone, including you, quite some trouble.

[:ref:`back to step-by-step guide <otherplaces_repisbs>`]


.. _backup_repiref:

Prepare for emergencies
-----------------------

  *You might want to create a fresh backup and put both* [:ref:`... <backup_repisbs>`]

Remember, you are dealing with computers, which sometimes do unexpected things,
especially if you fiddle with crucial parts like their operating system kernel.
That is what you are about to do in this process. You thus better want to create
a fresh backup. In case you need to install a kernel during the bug reporting
process, also ensure you have all tools at hand to repair or reinstall the
operating system as well as everything you need to restore the backup.

[:ref:`back to step-by-step guide <backup_repisbs>`]


.. _coarsely_repiref:

Start documenting how to reproduce issue
----------------------------------------

  *Write down coarsely how to reproduce the issue on a freshly booted system in* [:ref:`... <coarsely_repisbs>`]

During the reporting process, you most likely will have to test if the issue
happens with other kernel versions. Therefore, it will make your work easier if
you know exactly how to reproduce an issue quickly on a freshly booted system.
And for the report you need a description anyway.

This obviously is impossible in case you want to report an issue that happened
just once.  Be aware that it might be fruitless to report such problems, as
they might be caused by a bit flip due to cosmic radiation; but if you are
experienced enough to tell such a one-time hardware error apart from a kernel
issue that is worth reporting even without reproducing it, skip this step and
the verification.

[:ref:`back to step-by-step guide <coarsely_repisbs>`]


.. _verify_repiref:

Verify the problem with a suitable kernel
-----------------------------------------

  *You must report the problem using a kernel suitable for reporting -- so you
  [...] In case of a regression within a stable or longterm kernel, you also
  must check*  [:ref:`... <verify_repisbs>`]

Following the instructions in this step dramatically increases the chance some
developer will look into the report, as it ensures the bug is actually present
in a codebase they care about; for regressions in stable or longterm series,
they furthermore determine the right point of contact for the bug, which
depends on whether the problem happens in mainline as well.

The step-by-step guide outlines the gist of the required tasks; if you need
more detailed instructions, follow Documentation/admin-guide/verify-bugs-and-bisect-regressions.rst.

[:ref:`back to step-by-step guide <verify_repisbs>`]


.. _checkloretwo_repiref:

Search again
------------

  *If you performed a bisection or learned anything new about the bug
  while following this guide so far, search once more* [:ref:`... <checkloretwo_repisbs>`]

During the previous step you likely have learned a thing or two about the
issue you face. Use this knowledge and search again for existing reports
and potential fixes.

[:ref:`back to step-by-step guide <checkloretwo_repisbs>`]


.. _attachments_repiref:

Collect files to provide
------------------------

  *Collect relevant files to supply with the report.* [:ref:`... <attachments_repisbs>`]

The developers will usually ask for files with details about your system, as
what is needed highly depends on the nature of the problem. But it is often
wise to provide at least the kernel log and maybe a few things along with the
report, as outlined in the step-by-step guide.

When collecting the kernel's log messages with ``dmesg``, make sure they start
with a line like 'Linux version 5.8-1 (foobar@example.com) (gcc (GCC) 10.2.1,
GNU ld version 2.34) #1 SMP Mon Aug 3 14:54:37 UTC 2020'. The kernel discarded
messages from the first boot phase already if it is missing. In that case,
instead consider using ``journalctl -k``; alternatively, reboot and reproduce
the issue, before calling ``dmesg`` right afterwards.

In case the kernel's log messages contain a 'panic', 'Oops', 'warning', or
'BUG', you might want to decode them as described below if that is easy for you
-- but that is optional, as many bugs can be solved without this.

On many Linux distributions the tools mentioned by the guide are installed by
default, except maybe ``alsa-info.sh``, which `the sound subsystem developers
provide <https://www.alsa-project.org/wiki/AlsaInfo>`_.


Decode failure messages
~~~~~~~~~~~~~~~~~~~~~~~

A 'panic', 'Oops', 'warning', or 'BUG' includes a stack trace, which contains
addresses that allow pinpointing the exact path to the line in your kernel's
source code that triggered the issue. Many bugs can be resolved without
decoding these addresses, but for some it is helpful or required.

That is why it is fine to report problems without bothering about this, but
when asked for this, try to decode the stack trace. Note: This requires a
kernel build with CONFIG_DEBUG_INFO and CONFIG_KALLSYMS enabled.

Usually you want to decode using a script shipped in the Linux sources. If you
are running a kernel you compiled yourself, call it like this::

   [user@something ~]$ sudo dmesg | ./linux-5.10.5/scripts/decode_stacktrace.sh ./linux-5.10.5/vmlinux

If you are running a packaged kernel, you will likely have to install packages
with the corresponding debug symbols. Then call the script (which you might need
to fetch from the Linux sources if your distro does not package it) like this::

   [user@something ~]$ sudo dmesg | ./linux-5.10.5/scripts/decode_stacktrace.sh \
   /usr/lib/debug/lib/modules/5.10.10-4.1.x86_64/vmlinux /usr/src/debug/kernel-5.10.10-4.1.x86_64/

The script will work on log lines like the following, which show the address of
the code the kernel was executing when the error occurred::

   [   68.387301] RIP: 0010:test_module_init+0x5/0xffa [test_module]

Once decoded, these lines will look like this::

   [   68.387301] RIP: 0010:test_module_init (/home/user/linux-5.10.5/test-module/test-module.c:16) test_module

In this case the executed code was built from the file
'~/linux-5.10.5/test-module/test-module.c' and the error occurred during the
instructions found in line '16'.

The script will similarly decode the addresses mentioned in the section
starting with 'Call trace', which shows the path to the function where the
problem occurred. The script, furthermore, will show the assembler output for
the code section the kernel was executing at that time.

[:ref:`back to step-by-step guide <attachments_repisbs>`]


.. _compile_repiref:

Prepare and optimize the report
-------------------------------

  *Prepare and optimize the report.* [:ref:`... <compile_repisbs>`]

Most developers just take a few seconds to skim a report before deciding
between taking a closer look or moving on, as they receive a ton of messages.
That is why the title/subject, the first sentence, and the three or four
following it are crucial.

People will also stop reading if the report's text is long or hard to follow;
the same is true if crucial information is not at hand. So be sure to describe
things as short, straightforward, and simple as possible while providing
everything important.

How to do that is partly explained by the three documents linked to in the
reference section's intro. The next few subsections thus will only mention a
few essentials as well as things specific to the Linux kernel.


Things each report should mention
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Describe the problem while mentioning all the important details about the
environment others might need to fully understand the issue.:

* The output from ``uname -r`` from the Linux kernel used for the verification.

* The Linux distribution used (``hostnamectl | grep 'Operating System'``)

* The nature of the issue and when it occurs.

* If you are dealing with a regression and performed a bisection, mention
  The author, subject, and commit-id of the culprit change.

* If you are dealing with a 'warning', an 'OOPS' or a 'panic' from the kernel,
  include it. If you can't copy and paste it, take a picture of the screen.


Things that might be good to provide
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

In some cases it is wise to provide additional details:

* The processor architecture used (``uname -mi``).

* The relevant software in use. If you have problems with loading
  modules, you want to mention the versions of kmod, systemd, and udev in use.
  If one of the DRM drivers misbehaves, you want to state the versions of
  libdrm and Mesa; also specify your Wayland compositor or the X-Server and
  its driver.

* If the issue might be related to your hardware, mention what kind
  of system you use. If you, for example, have problems with your graphics card,
  mention its manufacturer, the card's model, and what chip it uses. If it is a
  laptop, specify its name, but try to make sure it is meaningful. 'Dell XPS
  13', for example, is not, because that might be the one from 2012 or 2020;
  the latter might not look that different, but apart from that it shares
  nothing with the former. In such cases add the exact model number, like '9380'
  or '7390' for XPS 13 models introduced during 2019. Names like 'Lenovo
  Thinkpad T590' are also somewhat ambiguous: There are variants of this laptop
  with and without a dedicated graphics chip, so try to find the exact model
  name or specify the main components.

Those examples should give you some ideas of what data might be wise to
specify, but you have to think through yourself what will be helpful for others
to know.

Don't worry too much about forgetting something, as developers will ask for
additional details they need. But making everything important available from
the start increases the chance someone will take a closer look.

Special handling for high-priority issues
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Reports for high-priority issues need special handling:

* Regressions: Make the report's subject start with '[REGRESSION]'.

  In case you performed a successful bisection, use the title of the change that
  introduced the regression as the second part of your subject. Make the report
  also mention the commit-id of the culprit. In case of an unsuccessful
  bisection, make your report mention the latest tested version that is working
  fine (say 5.7) and the oldest where the issue occurs (say 5.8-rc1).

  When sending the report by email, CC the Linux regressions mailing list
  (regressions@lists.linux.dev). In case the report needs to be filed to some
  web tracker, proceed to do so. Once filed, forward the report by email to the
  regressions list; CC the maintainer and the mailing list for the subsystem in
  question. Make sure to inline the forwarded report and do not attach it.
  Also add a short note at the top where you mention the URL to the ticket.

  When mailing or forwarding the report, in case of a successful bisection, add
  the author of the culprit to the recipients; also CC everyone in the
  signed-off-by   chain, which you find at the end of its commit message.

* Security issues: For these issues you will have to evaluate if a
  short-term risk to other users would arise if details were publicly disclosed.
  If that is not the case, simply proceed with reporting the issue as described.
  For issues that bear such a risk, you will need to adjust the reporting
  process slightly:

  * If the MAINTAINERS file instructed you to report the issue by email, do not
    CC any public mailing lists.

  * If you were supposed to file the issue in a bug tracker, make sure to mark
    the ticket as 'private' or 'security issue'. If the bug tracker does not
    offer a way to keep reports private, forget about it and send your report as
    a private email to the maintainers instead.

 In both cases, make sure to also email your report to the addresses the
 MAINTAINERS file lists in the section 'security contact'. Ideally, directly CC
 them when sending the report by email. If you filed it in a bug tracker, forward
 the report's text to these addresses; but on top of it, put a small note where
 you mention that you filed it with a link to the ticket.

 See Documentation/process/security-bugs.rst for more information.

Optimize the report and especially its head section
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Once you have everything covered in your report, it is wise to optimize the
most important section: The first few sentences.

If the report is long, it is usually a good idea to go to the top, add
something like 'The detailed description:' before the part you just wrote, and
insert two newlines at the top. Now write one normal length paragraph that
describes the issue roughly. Leave out all boring details and focus on the
crucial parts readers need to know to understand what this is all about.

Whenever you have or do not have such a paragraph with a gist, ideally start the
report with one sentence that explains quickly what the report is about.

Now try to write an even shorter subject/title for the report.

Spending time on these things is time well spent, as a lot of people will only
read the subject and maybe the first sentence or two before they decide if
reading the rest is worth it for them.

Now send or file the report like the :ref:`MAINTAINERS <maintainers>` file told
you, unless it is one of those 'issues of high-priority' outlined earlier: In
that case, please read the next subsection first before sending the report on
its way.

[:ref:`back to step-by-step guide <compile_repisbs>`]


.. _submit_repiref:

Submit the report
-----------------

  *Submit your report in the appropriate way, which depends on the outcome* [:ref:`... <submit_repisbs>`]

The step-by-step guide covers all the important details already.

[:ref:`back to step-by-step guide <submit_repisbs>`]


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
