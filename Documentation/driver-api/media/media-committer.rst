.. _Media Committers:

Media Committers
================

Who is a Media Committer?
-------------------------

A Media Committer is a Media Maintainer with patchwork access who has been
granted commit access to push patches from other developers and their own
patches to the
`media-committers <https://gitlab.freedesktop.org/linux-media/media-committers>`_
tree.

These commit rights are granted with expectation of responsibility:
committers are people who care about the Linux Kernel as a whole and
about the Linux media subsystem and want to advance its development. It
is also based on a trust relationship among other committers, maintainers
and the Linux Media community.

.. Note::

   1. Patches you authored must have a Signed-off-by, Reviewed-by or Acked-by
      of another Media Maintainer;
   2. If a patch introduces a regression, then it is the Media Committer's
      responsibility to correct that as soon as possible. Typically the
      patch is either reverted, or an additional patch is committed to
      fix the regression;
   3. If patches are fixing bugs against already released Kernels, including
      the reverts above mentioned, the Media Committer shall add the needed
      tags. Please see :ref:`Media development workflow` for more details.

Becoming a Media Committer
--------------------------

Existing Media Committers can nominate a Media Maintainer to be granted
commit rights. The Media Maintainer must already have patchwork access and
have been in that role for some time, and has demonstrated a good
understanding of the maintainer's duties and processes.

The ultimate responsibility for accepting a nominated committer is up to
the Media Subsystem Maintainers. The nominated committer must have earned a
trust relationship with all Media Subsystem Maintainers, as, by granting you
commit rights, part of their responsibilities are handed over to you.

Due to that, to become a Media Committer, a consensus between all Media
Subsystem Maintainers is required.

.. Note::

   In order to preserve/protect the developers that could have their commit
   rights granted, denied or removed as well as the subsystem maintainers who
   have the task to accept or deny commit rights, all communication related to
   changing commit rights should happen in private as much as possible.

.. _media-committer-agreement:

Media Committer's agreement
---------------------------

Once a nominated committer is accepted by all Media Subsystem Maintainers,
they will ask if the developer is interested in the nomination and discuss
what area(s) of the media subsystem the committer will be responsible for.
Those areas will typically be the same as the areas that are already
maintained by the nominated committer.

When the developer accepts being a committer, the new committer shall
explicitly accept the Kernel development policies described under its
Documentation/, and in particular to the rules in this document, by writing
an e-mail to media-committers@linuxtv.org, with a declaration of intent
following the model below::

   I, John Doe, would like to change my status to: Committer

   As Media Maintainer I accept commit rights for the following areas of
   the media subsystem:

   ...

   For the purpose of committing patches to the media-committer's tree,
   I'll be using my user https://gitlab.freedesktop.org/users/<username>.

Followed by a formal declaration of agreement with the Kernel development
rules::

   I agree to follow the Kernel development rules described at:

   https://www.kernel.org/doc/html/latest/driver-api/media/media-committer.rst

   and to the Linux Kernel development process rules.

   I agree to abide by the Code of Conduct as documented in:
   https://www.kernel.org/doc/html/latest/process/code-of-conduct.rst

   I am aware that I can, at any point of time, retire. In that case, I will
   send an e-mail to notify the Media Subsystem Maintainers for them to revoke
   my commit rights.

   I am aware that the Kernel development rules change over time.
   By doing a new push to media-committer tree, I understand that I agree
   to follow the rules in effect at the time of the commit.

That e-mail shall be signed with a PGP key cross signed by other Kernel and
media developers. As described at :ref:`media-developers-gpg`, the PGP
signature, together with the gitlab user security are fundamental components
that ensure the authenticity of the merge requests that will happen at the
media-committer.git tree.

In case the kernel development process changes, by merging new commits
to the
`media-committer tree <https://gitlab.freedesktop.org/linux-media/media-committers>`_,
the Media Committer implicitly declares their agreement with the latest
version of the documented process including the contents of this file.

If a Media Committer decides to retire, it is the committer's duty to
notify the Media Subsystem Maintainers about that decision.

.. note::

   1. Changes to the kernel media development process shall be announced in
      the media-committers mailinglist with a reasonable review period. All
      committers are automatically subscribed to that mailinglist;
   2. Due to the distributed nature of the Kernel development, it is
      possible that kernel development process changes may end being
      reviewed/merged at the linux-docs mailing list, specially for the
      contents under Documentation/process and for trivial typo fixes.

Media Core Committers
---------------------

As described in Documentation/driver-api/media/maintainer-entry-profile.rst
a Media Core Maintainer maintains media core frameworks as well, besides
just drivers, and so is able to change core files and the media subsystem's
Kernel API. A Media Core Committer is a Media Core Maintainer with commit
rights. The extent of the core committer's grants will be detailed by the
Media Subsystem Maintainers when they nominate a Media Core Committer.

Existing Media Committers may become Media Core Committers and vice versa.
Such decisions will be taken in consensus between the Media Subsystem
Maintainers.

Media committers rules
----------------------

Media committers shall do their best efforts to avoid merging patches that
would break any existing drivers. If it breaks, fixup or revert patches
shall be merged as soon as possible, aiming to be merged at the same Kernel
cycle the bug is reported.

Media committers shall behave accordingly to the rights granted by
the Media Subsystem Maintainers, specially with regards of the scope of changes
they may apply directly at the media-committers tree. That scope can
change over time on a mutual agreement between media committers and
maintainers.

The Media Committer workflow is described at :ref:`Media development workflow`.

.. _Maintain Media Status:

Maintaining media maintainer or committer status
------------------------------------------------

A community of maintainers working together to move the Linux Kernel
forward is essential to creating successful projects that are rewarding
to work on. If there are problems or disagreements within the community,
they can usually be solved through healthy discussion and debate.

In the unhappy event that a media maintainer or committer continues to
disregard good citizenship (or actively disrupts the project), we may need
to revoke that person's status. In such cases, if someone suggests the
revocation with a good reason, then after discussing this among the media
maintainers, the final decision is taken by the Media Subsystem Maintainers.
As the decision to become a media maintainer or committer comes from a
consensus between Media Subsystem Maintainers, a single subsystem maintainer
not trusting the media maintainer or committer anymore is enough to revoke
the maintenance/patchwork or commit rights.

A previous committer that had their commit rights revoked can keep
contributing to the subsystem via the pull request workflow as documented
at the :ref:`Media development workflow`, unless they were also removed as
Media Maintainer.

If a maintainer is inactive for more than a couple of Kernel cycles,
maintainers will try to reach you via e-mail. If not possible, they may
revoke your maintainer/patchwork and committer rights and update MAINTAINERS file
entries accordingly. If you wish to resume contributing later on, then contact
the Media Subsystem Maintainers to ask if your maintenance/patchwork and
commit rights can be restored.

References
----------

Much of this was inspired by/copied from the committer policies of:

- `Chromium <https://chromium.googlesource.com/chromium/src/+/main/docs/contributing.md>`_;
- `WebKit <https://webkit.org/commit-and-review-policy/>`_;
- `Mozilla <https://www.mozilla.org/hacking/committer/>`_.
