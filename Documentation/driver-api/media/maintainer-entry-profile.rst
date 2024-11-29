Media Subsystem Profile
=======================

Overview
--------

The media subsystem covers support for a variety of devices: stream
capture, analog and digital TV streams, cameras, remote controllers, HDMI CEC
and media pipeline control.

It covers, mainly, the contents of those directories:

  - drivers/media
  - drivers/staging/media
  - Documentation/admin-guide/media
  - Documentation/driver-api/media
  - Documentation/userspace-api/media
  - Documentation/devicetree/bindings/media/\ [1]_
  - include/media

.. [1] Device tree bindings are maintained by the
       OPEN FIRMWARE AND FLATTENED DEVICE TREE BINDINGS maintainers
       (see the MAINTAINERS file). So, changes there must be reviewed
       by them before being merged via the media subsystem's development
       tree.

Both media userspace and Kernel APIs are documented and the documentation
must be kept in sync with the API changes. It means that all patches that
add new features to the subsystem must also bring changes to the
corresponding API documentation files.

Due to the size and wide scope of the media subsystem, the media's
maintainership model is to have committers that have a broad knowledge of
a specific aspect of the subsystem. It is the committers' task to
review the patches, providing feedback to users if the patches are
following the subsystem rules and are properly using the media kernel and
userspace APIs.

Media committers
----------------

In the media subsystem, there are experienced developers who can push
patches directly to the development tree. These developers are called
Media committers and are divided into the following categories:

- Committers: responsible for one or more drivers within the media subsystem.
  They can push changes to the tree that do not affect the core or ABI.

- Core committers: responsible for part of the media core. They are typically
  responsible for one or more drivers within the media subsystem, but, besides
  that, they can also merge patches that change the code common to multiple
  drivers, including the kernel internal API.

- Subsystem maintainers: responsible for the subsystem as a whole, with
  access to the entire subsystem.

  Only subsystem maintainers can push changes that affect the userspace
  API/ABI.

Media committers shall explicitly agree with the Kernel development process
as described at Documentation/process/index.rst and to the Kernel
development rules inside the Kernel documentation, including its code of
conduct.

Media development tree
----------------------

The main development tree used by the media subsystem is hosted at LinuxTV.org,
where we also maintain news about the subsystem, wiki pages and a patchwork
instance where we track patches though their lifetime.

The main tree used by media developers is at:

https://git.linuxtv.org/media.git/

.. _Media development workflow:

Media development workflow
++++++++++++++++++++++++++

All changes for the media subsystem must be sent first as e-mails to the
media mailing list, following the process documented at
Documentation/process/index.rst.

It means that patches shall be submitted as plain text only via e-mail to:

  `https://subspace.kernel.org/vger.kernel.org.html <linux-media@vger.kernel.org>`_

Emails with HTML will be automatically rejected by the mail server.

It could be wise to also copy the media committer(s). You should use
``scripts/get_maintainers.pl`` to identify whom else needs to be copied.
Please always copy driver's authors and maintainers.

Such patches need to be based against a public branch or tag as follows:

1. Patches for new features need to be based at the ``next`` branch of
   media.git tree;

2. Fixes against an already released kernel should preferably be against
   the latest released Kernel. If they require a previously-applied
   change at media.git tree, they need to be against its ``fixes`` branch.

3. Fixes for issues not present at the latest released kernel should
   preferably be against the latest -rc1 Kernel. If they require a
   previously-applied change at media.git tree, they need to be against
   its ``fixes`` branch.

Patches with fixes shall have:
- a ``Fixes:`` tag pointing to the first commit that introduced the bug;
- when applicable, a ``Cc: stable@vger.kernel.org``.

Patches that were fixing bugs publicly reported by someone at the
linux-media@vger.kernel.org mailing list shall have:
- a ``Reported-by:`` tag immediately followed by a ``Closes:`` tag.

Patches that change API shall update documentation accordingly at the
same patch series.

See Documentation/process/index.rst for more details about e-mail submission.

Once a patch is submitted, it may follow either one of the following
workflows:

a. Pull request workflow: patches are handled by subsystem maintainers::

     +------+   +---------+   +-------+   +-----------------------+   +---------+
     |e-mail|-->|patchwork|-->|pull   |-->|maintainers merge      |-->|media.git|
     +------+   |picks it |   |request|   |in media-committers.git|   +---------+
                +---------+   +-------+   +-----------------------+

   For this workflow, pull requests can be generated by a committer,
   a previous committer, subsystem maintainers or by a couple of trusted
   long-time contributors. If you are not in such group, please don't submit
   pull requests, as they will likely be ignored.

b. Committers' workflow: patches are handled by media committers::

     +------+   +---------+   +--------------------+   +-----------+   +---------+
     |e-mail|-->|patchwork|-->|committers merge at |-->|maintainers|-->|media.git|
     +------+   |picks it |   |media-committers.git|   |approval   |   +---------+
                +---------+   +--------------------+   +-----------+

On both workflows, all patches shall be properly reviewed at
linux-media@vger.kernel.org before being merged at media-committers.git.

When patches are picked by patchwork and when merged at media-committers,
CI bots will check for errors and may provide e-mail feedback about
patch problems. When this happens, the patch submitter must fix them
and send another version of the patch.

Patches will only be moved to the next stage in those two workflows if they
don't fail on CI or if there are false-positives in the CI reports.

Failures during e-mail submission
+++++++++++++++++++++++++++++++++

Media's workflow is heavily based on Patchwork, meaning that, once a patch
is submitted, the e-mail will first be accepted by the mailing list
server, and, after a while, it should appear at:

   - https://patchwork.linuxtv.org/project/linux-media/list/

If it doesn't automatically appear there after some time [2]_, then
probably something went wrong on your submission. Please check if the
email is in plain text\ [3]_ only and if your emailer is not mangling
whitespaces before complaining or submitting them again.

To troubleshoot problems, you should first check if the mailing list
server has accepted your patch, by looking at:

   - https://lore.kernel.org/linux-media/

If the patch is there and not at patchwork, it is likely that your e-mailer
mangled the patch. Patchwork internally has a logic that checks if the
received e-mail contain a valid patch. Any whitespace and new line
breakages mangling the patch won't be recognized by patchwork, thus such
patch will be rejected.

.. [2] It usually takes a few minutes for the patch to arrive, but
       the e-mail server may be busy, so it may take up to a few hours
       for a patch to be picked by patchwork.

.. [3] If your email contains HTML, the mailing list server will simply
       drop it, without any further notice.

.. _media-developers-gpg:

Authentication for pull and merge requests
++++++++++++++++++++++++++++++++++++++++++

The authenticity of developers submitting pull requests and merge requests
shall be validated by using PGP sign, via the
:ref:`kernel_org_trust_repository`.

With the pull request workflow, pull requests shall use a GPG-signed tag.

For more details about PGP sign, please read
Documentation/process/maintainer-pgp-guide.rst.

Subsystem maintainers
---------------------

The subsystem maintainers are:
  - Mauro Carvalho Chehab <mchehab@kernel.org> and
  - Hans Verkuil <hverkuil@xs4all.nl>

Submit Checklist Addendum
-------------------------

Patches that change the Open Firmware/Device Tree bindings must be
reviewed by the Device Tree maintainers. So, DT maintainers should be
Cc:ed when those are submitted via devicetree@vger.kernel.org mailing
list.

There is a set of compliance tools at https://git.linuxtv.org/v4l-utils.git/
that should be used in order to check if the drivers are properly
implementing the media APIs:

====================	=======================================================
Type			Tool
====================	=======================================================
V4L2 drivers\ [4]_	``v4l2-compliance``
V4L2 virtual drivers	``contrib/test/test-media``
CEC drivers		``cec-compliance``
====================	=======================================================

.. [4] The ``v4l2-compliance`` also covers the media controller usage inside
       V4L2 drivers.

Those tests need to pass before the patches go upstream.

Also, please notice that we build the Kernel with::

	make CF=-D__CHECK_ENDIAN__ CONFIG_DEBUG_SECTION_MISMATCH=y C=1 W=1 CHECK=check_script

Where the check script is::

	#!/bin/bash
	/devel/smatch/smatch -p=kernel $@ >&2
	/devel/sparse/sparse $@ >&2

Be sure to not introduce new warnings on your patches without a
very good reason.

Please see `Media development workflow`_ for e-mail submission rules.

Style Cleanup Patches
+++++++++++++++++++++

Style cleanups are welcome when they come together with other changes
at the files where the style changes will affect.

We may accept pure standalone style cleanups, but they should ideally
be one patch for the whole subsystem (if the cleanup is low volume),
or at least be grouped per directory. So, for example, if you're doing a
big cleanup change set at drivers under drivers/media, please send a single
patch for all drivers under drivers/media/pci, another one for
drivers/media/usb and so on.

Coding Style Addendum
+++++++++++++++++++++

Media development uses ``checkpatch.pl`` on strict mode to verify the code
style, e.g.::

	$ ./scripts/checkpatch.pl --strict --max-line-length=80

In principle, patches should follow the coding style rules, but exceptions
are allowed if there are good reasons. On such case, maintainers and reviewers
may question about the rationale for not addressing the ``checkpatch.pl``.

Please notice that the goal here is to improve code readability. On
a few cases, ``checkpatch.pl`` may actually point to something that would
look worse. So, you should use good sense.

Note that addressing one ``checkpatch.pl`` issue (of any kind) alone may lead
to having longer lines than 80 characters per line. While this is not
strictly prohibited, efforts should be made towards staying within 80
characters per line. This could include using re-factoring code that leads
to less indentation, shorter variable or function names and last but not
least, simply wrapping the lines.

In particular, we accept lines with more than 80 columns:

    - on strings, as they shouldn't be broken due to line length limits;
    - when a function or variable name need to have a big identifier name,
      which keeps hard to honor the 80 columns limit;
    - on arithmetic expressions, when breaking lines makes them harder to
      read;
    - when they avoid a line to end with an open parenthesis or an open
      bracket.

Key Cycle Dates
---------------

New submissions can be sent at any time, but if they intend to hit the
next merge window they should be sent before -rc5, and ideally stabilized
in the linux-media branch by -rc6.

Review Cadence
--------------

Provided that your patch is at https://patchwork.linuxtv.org, it should
be sooner or later handled, so you don't need to re-submit a patch.

Except for bug fixes, we don't usually add new patches to the development
tree between -rc6 and the next -rc1.

Please notice that the media subsystem is a high traffic one, so it
could take a while for us to be able to review your patches. Feel free
to ping if you don't get a feedback in a couple of weeks or to ask
other developers to publicly add ``Reviewed-by:`` and, more importantly,
``Tested-by:`` tags.

Please note that we expect a detailed description for ``Tested-by:``,
identifying what boards were used at the test and what it was tested.
