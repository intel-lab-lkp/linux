Media committers
================

Who is a media committer?
-------------------------

A media committer is a developer who has been granted commit access to push
patches from other developers and their own patches to the
`media-committers <https://gitlab.freedesktop.org/linux-media/media-committers>`_
tree.

It is a media committer's duty to ensure that their entries in the MAINTAINERS
file are kept up-to-date, and that submitted patches for files for which
they are listed as maintainers are timely reviewed on the mailing list,
ideally not waiting in patchwork as ``New`` for more than one Kernel merge
cycle, and, if accepted, applying them at the media committer's tree.

These commit rights are granted with expectation of responsibility:
committers are people who care about the Linux Kernel as a whole and
about the Linux media subsystem and want to advance its development. It
is also based on a trust relationship among other committers, maintainers
and the Linux Media community[1].

As such, a media committer is not just someone who is capable of creating
code, but someone who has demonstrated their ability to collaborate
with the team, get the most knowledgeable people to review code,
contribute high-quality code, and follow through to fix issues (in code
or tests).

.. Note::

   1. If a patch introduces a regression, then it is the media committer's
      responsibility to correct that as soon as possible. Typically the
      patch is either reverted, or an additional patch is committed to
      fix the regression;
   2. if patches are fixing bugs against already released Kernels, including
      the reverts above mentioned, the media committer shall add the needed
      tags. Please see :ref:`Media development workflow` for more details.

[1] The Linux Media Community, also called LinuxTV Community, has its primary
    site at https://linuxtv.org.

    Media committers and developers are reachable via the #linux-media
    IRC channel at OFTC.

Becoming a media committer
--------------------------

The most important aspect of volunteering to be a committer is that you have
demonstrated the ability to give good code reviews. So we are looking for
whether or not we think you will be good at doing that.

As such, potential committers must earn enough credibility and trust from the
Linux Media Community. To do that, developers shall be familiar with the open
source model and have been active in the Linux Kernel community for some time,
and, in particular, in the media subsystem.

So, in addition to actually making the code changes, you are basically
demonstrating your:

- commitment to the project;
- ability to collaborate with the team and communicate well;
- understand of how upstream and the Linux Media Community work
  (policies, processes for testing, code review, ...)
- reasonable knowledge about:

  - the Kernel development process:
    Documentation/process/index.rst

  - the Media development profile:
    Documentation/driver-api/media/maintainer-entry-profile.rst

- understanding of the projects' code base and coding style;
- ability to provide feedback to the patch authors;
- ability to judge when a patch might be ready for review and to submit;
- ability to write good code (last but certainly not least).

Developers that desire to become committers are encouraged to participate
at the yearly Linux Media Summit, typically co-located with a Linux related
conference.

If you are doing such tasks and have become a valued developer, an
existing committer can nominate you to the media subsystem maintainers.

The ultimate responsibility for accepting a nominated committer is up to
the subsystem's maintainers. The committers must earn a trust relationship
with all subsystem maintainers, as, by granting you commit rights, they will
be a part of their maintenance tasks.

Due to that, to become a committer or a core committer, a consensus between
all subsystem maintainers is required, as they all need to trust a developer
well enough to be delegated the responsibility to maintain part of the code
and to properly review patches from third parties for the drivers that they
maintain in a timely manner and keeping the status of the patches at
https://patchwork.linuxtv.org updated.

.. Note::

   In order to preserve/protect the developers that could have their commit
   rights granted, denied or removed as well as the subsystem maintainers who
   have the task to accept or deny commit rights, all communication related to
   changing commit rights should happen in private as much as possible.

.. _media-committer-agreement:

Media committer's agreement
---------------------------

Once a nominated committer is accepted by all subsystem maintainers,
they will ask if the developer is interested in the nomination and discuss
what area(s) of the media subsystem the committer will be responsible for.

Once the developer accepts being a committer, the new committer shall
explicitly accept the Kernel development policies described under its
Documentation/, and, in particular to the rules on this document, by writing
an e-mail to media-committers@linuxtv.org, with a declaration of intent
following the model below::

   I, John Doe, would like to change my status to: Committer

   I intend to actively develop the XYZ driver, send fixes to drivers
   that I can test, optionally reviewing patches and merging trivial
   fixes in other areas of the subsystem, ...

   For the purpose of committing patches to the media-committer's tree,
   I'll be using my user https://gitlab.freedesktop.org/users/<username>.

Followed by a formal declaration of agreement with the Kernel development
rules::

   I hereby declare that I agree with the Kernel development rules described at:

   https://www.kernel.org/doc/html/latest/driver-api/media/media-committer.rst

   and to the Linux Kernel development process rules.

   I agree to the Code of Conduct as documented in:
   https://www.kernel.org/doc/html/latest/process/code-of-conduct.rst

   I am aware that I can, at any point of time, retire. In that case, I will
   send an e-mail to notify the subsystem maintainers for them to revoke my
   commit rights.

   I am aware that the Kernel development rules change over time.
   By doing a new push to media-committer tree, I understand that I agree
   with the rules in effect at the time of the commit.

That e-mail shall be signed with a PGP key cross signed by other Kernel and
media developers. As described at :ref:`media-developers-gpg`, the PGP
signature, together with the gitlab user security are fundamental components
that ensure the authenticity of the merge requests that will happen at the
media-committer.git tree.

In case the kernel development process changes, by merging new commits
to the
`media-committer tree <https://gitlab.freedesktop.org/linux-media/media-committers>`_,
the media committer implicitly declares their agreement with the latest
version of the documented process including the contents of this file.

If a media committer decides to retire, it is the committer's duty to
notify the subsystem maintainers about that decision.

.. note::

   1. Changes to the kernel media development process shall be announced in
      the media-committers mailinglist with a reasonable review period. All
      committers are automatically subscribed to that mailinglist;
   2. Due to the distributed nature of the Kernel development, it is
      possible that kernel development process changes may end being
      reviewed/merged at the linux-docs mailing list, specially for the
      contents under Documentation/process and for trivial typo fixes.

Core committers
---------------

As described in Documentation/driver-api/media/maintainer-entry-profile.rst
a committer may be granted with additional rights to also be able to
change a core file and/or media subsystem's Kernel API. The extent of
the core committer's grants will be detailed by the subsystem maintainers
when they nominate a core committer.

Existing committers may become core committers and vice versa. Such
decisions will be taken in consensus between the subsystem maintainers.

Media committers rules
----------------------

Media committers shall do their best efforts to avoid merged patches that
would break any existing drivers. If it breaks, fixup or revert patches
shall be merged as soon as possible, aiming to be merged at the same Kernel
cycle the bug is reported.

Media committers shall behave accordingly to the rights granted by
the subsystem maintainers, specially with regards of the scope of changes
they may apply directly at the media-committers tree. Such scope can
change over time on a mutual agreement between media committers and
maintainers.

As described at :ref:`Media development workflow`, there are workflows.
For the committers' workflow, the following rules apply:

- Each merged patch shall pass CI tests;

- Media committers shall request reviews from other committers and
  developers where applicable, i.e. because those developers have more
  knowledge about some areas that are changed by a patch;

- There shall be no open issues or unresolved or conflicting feedback
  from anyone. Clear them up first. Defer to subsystem maintainers as needed.

Patches that do not fall under the committer's workflow criteria will follow
the pull request workflow as described at :ref:`Media development workflow`.

Only a subsystem maintainer can override such rules.

All media committers shall ensure that patchwork will reflect the current
status, e.g. patches shall be delegated to the media committer who is
handling them and the patch status shall be updated according to these rules:

- ``Under review``: Used if the patch requires a second opinion
  or when it is part of a pull request;
- ``Accepted``: Once a patch is merged in the multi-committer tree.
- ``Superseded``: There is a newer version of the patch posted to the
  mailing list.
- ``Duplicated``: There was another patch doing the same thing from someone
  else that was accepted.
- ``Not Applicable``: Use for patch series that are not merged at media.git
  tree (e.g. drm, dmabuf, upstream merge, etc.) but were cross-posted to the
  linux-media mailing list.

If the committer decides not to merge it, then reply by email to patch
authors, explaining why it is not merged, and patchwork shall be updated
accordingly with either:

- ``Changes Requested``: if a new revision was requested;
- ``Rejected``: if the proposed change won't be merged upstream.

.. Note::

   Patchwork supports a couple of clients to help semi-automating
   status updates via its REST interface:

   https://patchwork.readthedocs.io/en/latest/usage/clients/

Maintaining media committer status
----------------------------------

A community of committers working together to move the Linux Kernel
forward is essential to creating successful projects that are rewarding
to work on. If there are problems or disagreements within the community,
they can usually be solved through healthy discussion and debate.

In the unhappy event that a media committer continues to disregard good
citizenship (or actively disrupts the project), we may need to revoke
that person's status. In such cases, if someone suggests the revocation
with a good reason, then after discussing this among the media committers,
the final decision is taken by the subsystem maintainers. As the decision
to become a media committer comes from a consensus between subsystem
maintainers, a single subsystem maintainer not trusting the media committer
anymore is enough to revoke the commit rights.

If a committer is inactive for more than a couple of Kernel cycles,
maintainers will try to reach you via e-mail. If not possible, they may
revoke your committer rights and update MAINTAINERS file entries
accordingly. If you wish to resume contributing later on, then contact
the subsystem maintainers to ask if your commit rights can be restored.

A previous committer that had their commit rights revoked can keep
contributing to the subsystem via the pull request workflow as documented
at the :ref:`Media development workflow`.

References
----------

Much of this was inspired by/copied from the committer policies of:

- `Chromium <https://chromium.googlesource.com/chromium/src/+/main/docs/contributing.md>`_;
- `WebKit <https://webkit.org/commit-and-review-policy/>`_;
- `Mozilla <https://www.mozilla.org/hacking/committer/>`_.

