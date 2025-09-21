NFSD Maintainer Entry Profile
=============================

A Maintainer Entry Profile supplements the top-level process
documents (submitting-patches, submitting drivers...) with customs
that are specific to a subsystem and its maintainers. A contributor
uses this document to set their expectations and avoid common
mistakes. A maintainer may use these profiles to look across
subsystems for opportunities to converge on best common practices.

Overview
--------
The Network File System (NFS) is a standardized family of network
protocols that enable access to files across a set of network-
connected peer hosts. Applications on NFS clients access files that
reside on file systems that are shared by NFS servers. A single
network peer can act as both an NFS client and an NFS server.

NFSD refers to the NFS server implementation included in the Linux
kernel. An in-kernel NFS server has faster access to files stored
in file systems local to that server. NFSD can share files stored
on most of the file system types native to Linux, including xfs,
ext4, btrfs, and tmpfs.

Mailing list
------------
The linux-nfs@vger.kernel.org mailing list is a public list. Its
purpose is to enable collaboration among developers working on the
Linux NFS stack, both client and server. It is not a place for
conversations that are not related directly to the Linux NFS stack.

The linux-nfs mailing list is archived on lore.kernel.org.

The Linux NFS community does not have a chat room.

Reporting bugs
--------------
If you experience an NFSD-related bug on a distribution-built
kernel, please start by working with your Linux distributor.

Bug reports against upstream Linux code bases are welcome on the
linux-nfs@vger.kernel.org mailing list, where some active triage
can be done. NFSD bugs may also be reported in the Linux kernel
community's bugzilla at:

  https://bugzilla.kernel.org

Please file NFSD-related bugs under the "Filesystems/NFSD"
component. In general, including as much detail as possible is a
good start.

For user space software related to NFSD, such as mountd or the
exportfs command, report problems on linux-nfs@vger.kernel.org.
You might be asked to move the report to a specific bug tracker.

Contributor's Guide
-------------------

Standards compliance
~~~~~~~~~~~~~~~~~~~~
The NFSD community strives to provide an NFS server implementation
that interoperates with standards-compliant NFS client
implementations. This is done by staying close to the normative
mandates in IETF's NFS standards documents.

It is always useful to provide an RFC and section citation in a
code comment where behavior deviates from the standard (and even
when the behavior is compliant but the implementation is curious).

On the rare occasion when deviation from standards are needed,
clear documentation of the use case or deficiencies in the
standard is a required part of code documentation.

Note that care must always be taken to avoid leaking local error
codes (ie, errnos) to clients of NFSD. A proper NFS status code
is always required.

Testing
~~~~~~~
The kdevops project

  https://github.com/linux-kdevops/kdevops

contains several NFS-specific workflows, as well as the community
standard fstests suite. These workflows are based on open source
testing tools such as ltp and fio. Contributors are encouraged to
use these tools without kdevops, or contributors should install and
use kdevops themselves to verify their patches before submission.

Patch preparation
~~~~~~~~~~~~~~~~~
Like all kernel submissions, please use tagging to identify all
patch authors. Reviewers and testers can be added by replying to
the email patch submission. Email is extensively used in order to
publicly archive review and testing attributions, and will be
automatically inserted into your patches when they are applied.

The patch description must contain information that does not appear
in the body of the diff. The code should be good enough to tell a
story -- self-documenting -- but the patch description needs to
provide rationale ("why does NFSD benefit from this change?") or
a clear problem statement ("what is this patch trying to fix?").

Brief benchmarking results and stack traces are also welcome
sources of context. And, functioal test results should be included
in patch descriptions.

Identify the point in history that the issue being addressed was
introduced by using a Fixes: tag.

Note in the patch description if that point in history cannot be
determined -- that is, no Fixes: tag can be provided. In this case,
please make it clear to maintainers that an LTS backport is needed
even though there is no Fixes: tag.

The NFSD maintainers prefer to add stable tagging themselves, after
public discussion in response to the patch submission. Contributors
can suggest stable tagging, but be aware that most tools will add
the stable Cc's when you post the submission, which is generally
a nuisance but can also result in outing an embargoed security
issue accidentally. So don't add "Cc: stable" unless you are
absolutely sure the patch needs to go to stable during the initial
submission process.

If your change is a bug fix, make sure your patch description
indicates the end-user visible symptom, the underlying reason as to
why it happens, and then, if necessary, explain why the fix proposed
is the best way to get things done.

Break your work into incremental changes that each look "obviously
correct". Scrub out any incidental clean-ups that are not necessary
to the purpose of your patches -- those should be included as
separate patches.

A patch series must be fully bisectable. The kernel needs to fully
compile after each patch. A series should not break some function
in one patch then fix it in a later patch.

Don't mangle white space, and as is common, don't mis-indent
function arguments that span multiple lines. Add local variables in
reverse Christmas tree order.

Put yourself in the shoes of the reviewers. Each patch is read
separately and therefore should constitute a comprehensible step
towards your stated goal.

Avoid frequently reposting large series with only small changes. As
a rule of thumb, posting substantial changes more than once a week
will result in reviewer overload.

Post changes to kernel source code and user space source code as
separate series. You can connect the two series with comments in
your cover letters.

Patch submission
~~~~~~~~~~~~~~~~
Patches to NFSD are accepted only via the kernel's email-based
review process.

Just before each submission, rebase your patch or series on the
nfsd-testing branch at

  https://git.kernel.org/pub/scm/linux/kernel/git/cel/linux.git

The NFSD subsystem is maintained separately from the Linux in-kernel
NFS client. The NFSD maintainers do not normally take submissions
for client changes, nor can they respond authoritatively to bug and
feature requests for NFS client code.

This means that contributors might be asked to resubmit patches if
they were emailed to the incorrect set of maintainers and reviewers.
This is not a rejection, but simply a correction of the submission
process.

The proper set of email addresses for NFSD patches are:

To: the NFSD maintainers and reviewers listed in MAINTAINERS
Cc: linux-nfs@vger.kernel.org and optionally linux-kernel@

If there are other subsystems involved in the patches (for example
MM or RDMA) their primary mailing list address can be included in
the Cc: field. Other contributors and interested parties may be
included there as well.

In general we prefer that contributors use common patch email tools
such as "git send-email" or "stg email format/send", which tend to
get the details right without a lot of fuss.

A series consisting of a single patch is not required to have a
cover letter. However, a cover letter can be included if there is
substantial context that is not appropriate to include in the
patch description.

Please note that cover letters are not part of the work that is
committed to the kernel source code base or its commit history.
Therefore always try to keep the most pertinent information in the
patch descriptions.

Design documentation is welcome, but as cover letters are not
preserved, a perhaps better option is to include a patch that adds
such documentation under Documentation/filesystems/nfs/.

Reviewers will ask about test coverage and what use cases the
patches are expected to address. Please be prepared to answer these
questions.

Review requests from maintainers might be politely stated, but in
general, these are not optional to address when they are actionable.
If necessary, the maintainers retain the right to not apply patches
when contributors refuse to address reasonable requests.

Generally the NFSD maintainers ask for a reposts even for simple
modifications in order to publicly archive the request and the
resulting repost before it is pulled into the NFSD trees. This
also enables us to rebuild a patch series quickly without missing
changes that might have been discussed via email.

Remember, there are only a handful of subsystem maintainers and
reviewers, but potentially many sources of contributions. The
maintainer, therefore, is always the less scalable resource. Be
kind to your friendly neighborhood maintainer.

Key Cycle Dates
~~~~~~~~~~~~~~~
Currently the NFSD patch queues are maintained in branches here:

  https://git.kernel.org/pub/scm/linux/kernel/git/cel/linux.git

The NFSD maintainers apply patches initially to nfsd-testing, and
often do so while review is ongoing. nfsd-testing is a topic
branch, so it can change frequently, it will be rebased, and your
patch might get dropped if there is a problem with it.

Generally a script-generated "thank you" email will indicate when
your patch has been added to the nfsd-testing branch in the repo
mentioned above. You can track the progress of your patch using the
linux-nfs patchworks instance:

  https://patchworks.kernel.org/linux-nfs/

While your patch is in nfsd-testing, it is exposed to a variety
of test environments, including community zero-day bots, static
analysis tools, and NFSD continuous integration testing. The soak
period is typically three to four weeks.

Once that period is up, surviving patches are moved to nfsd-next.
That branch is merged into linux-next and fs-next on a nightly
basis.

All patches that survive in nfsd-next are included in the next NFSD
merge window pull request. These windows occur once every eight
weeks.

In some circumstances, a fix might be needed in the stable or LTS
kernels on short notice. Please make it clear when submitting a
patch that immediate action is needed. Otherwise, we prefer that
/all/ patches go through the long cycle described above to avoid
introducing regressions.

Sensitive patch submissions and bug reports
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
CVEs are generated by specific members of the Linux kernel community
and several external entities. The Linux NFS community does not emit
or assign CVEs. CVEs are assigned after an issue and its fix are
known.

However, the NFSD maintainers sometimes receive sensitive security
reports, and at times these are significant enough to need to be
embargoed. In such rare cases, fixes can be developed and reviewed
out of the public eye.

LLM-generated submissions
~~~~~~~~~~~~~~~~~~~~~~~~~
The Linux kernel community as a whole is still exploring the new
world of LLM-generated code. The NFSD maintainers will entertain
submission of patches that are partially or wholy generated by
LLM-based development tools. Such submissions are held to the
same standards as submissions created entirely by human authors.

Therefore:

- The human contributor identifies themselves via a Signed-off-by:
  tag. This tag counts as a DoC.

- The human contributor is solely responsible for code provenance
  and any contamination by inadvertently-included code with a
  conflicting license, as usual.

- The human contributor must be able to answer and address review
  questions. A patch description such as "This fixed my problem
  but I don't know why" is not acceptable.

- The contribution is subjected to the same test regimen as all
  other submissions.

- An indication (via a Generated-by: tag or otherwise) that the
  contribution is LLM-generated is not required.

It is easy to address review comments and fix requests in LLM
generated code. So easy, in fact, that it becomes tempting to repost
refreshed code immediately. Please resist that temptation.

As always, please do not repost patches frequently.

Clean-up patches
~~~~~~~~~~~~~~~~
The NFSD maintainers discourage patches which perform simple clean-
ups, which are not in the context of other work. For example:

* Addressing ``checkpatch.pl`` warnings after merge
* Addressing :ref:`Local variable ordering<rcs>` issues
* Addressing long-standing whitespace damage

This is because it is felt that the churn that such changes produce
comes at a greater cost than the value of such clean-ups.

Conversely, spelling and grammar fixes are encouraged.

Stable and LTS support
----------------------
Upstream NFSD continuous integration testing runs against LTS trees
whenever they are updated.

Please indicate when a patch containing a fix needs to be considered
for LTS kernels, either via a Fixes: tag or explicit mention.

Feature requests
----------------
Feature requests can sometimes be nebulous. A requester might not
understand what a "use case" or "user story" is. These are
descriptive paradigms that developers and architects use to
understand what is required of a design.

In order to prevent contributors and maintainers from becoming
overwhelmed, we won't be afraid of saying "no" politely. However
we can take nebulous requests and add them to our NFSD project
Kanban board, to be fleshed in over time into an actionable
plan for building a new feature.

There is no one way to make an official feature request, but
discussion about the request should eventually make its way to
the linux-nfs@vger.kernel.org mailing list for public review by
the community.

Community roles and their authority
-----------------------------------
The purpose of Linux subsystem communities is to provide active
stewardship of a narrow set of source files in the Linux kernel.
This can include managing user space tooling as well.

To contextualize the structure of the Linux NFS community that
is responsible for stewardship of the NFS server code base, we
define the community roles here.

One person often takes on more than one of these roles. One role
can be filled by multiple people. The roles and the people filling
them are often fluid. Sometimes a person will say "Wearing my XYZ
hat" -- which means, roughly, "speaking as the person filling the
XYZ role."

- **Contributor** : Anyone who submits a code change, bug fix,
  recommendation, documentation fix, and so on. A contributor can
  submit regularly or infrequently.

- **Outside Contributor** : A contributor who is not a regular actor
  in the Linux NFS community. This can mean someone who contributes
  to other parts of the kernel, or someone who just noticed a
  mis-spelling in a comment and sent a patch.

- **Reviewer** : Someone who is named in the MAINTAINERS file as a
  reviewer is an area expert who can request changes to contributed
  code, and expects that contributors will address the request.

- **Upstream Release Manager** : This role is responsible for
  curating contributions into a branch, reviewing test results, and
  then sending a pull request during merge windows. There is a
  trust relationship between the release manager and Linus.

- **Bug Triager** : Someone who is a first responder to bug reports
  submitted to the linux-nfs mailing list or the bugzilla and helps
  troubleshoot and identify next steps.

- **Testing Lead** : The testing lead builds and runs the test
  infrastructure for the subsystem. The testing lead can ask for
  patches to be dropped because of ongoing high defect rates.

- **LTS Maintainer** : The LTS maintainer is responsible for managing
  the Fixes: and Cc: stable annotations on patches, and seeing that
  patches that cannot be automatically applied to LTS kernels get
  proper backports as necessary.

- **Community Manager** : This umpire role can be asked to call balls
  and strikes during conflicts, but is also responsible for ensuring
  the health of the relationships within the community and
  facilitating discussions on long-term topics such as how to manage
  growing technical debt.
