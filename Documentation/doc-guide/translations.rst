.. SPDX-License-Identifier: GPL-2.0

====================================
Translating the kernel documentation
====================================

The kernel documentation is written in English, and English is the only
official version of it.  Translations exist to make that documentation
accessible to people who are more comfortable reading in another
language; they live under ``Documentation/translations/``, with one
directory per language.

This document explains how to contribute to those translations: how to
pick something to translate, how a translated document is laid out, how
to check your work, how to send it, what to expect from the review, and
how to keep a translation current once it has been merged.  It also
covers what is needed to start a translation into a new language.

It does not replace the general documents on how to contribute to the
kernel; a translation patch is a kernel patch, and everything described
in Documentation/process/submitting-patches.rst applies to it.  What
follows focuses on the details that are specific to translations and on
the mistakes that most often cause translation patches to be sent back.

Some translations have additional, language-specific guidance written in
their own language; for example, the Simplified Chinese team maintains
Documentation/translations/zh_CN/how-to.rst.  Where such a guide exists,
read it as well.


Principles
==========

Before starting, read the disclaimer in
:ref:`Documentation/translations/index.rst <translations_disclaimer>`.
Its main points shape everything else in this document:

- **English is authoritative.**  When a translation and the English
  original disagree, the English version is correct.

- **A translation is not a fork.**  It must not add, remove or change
  information.  If you find an error, an omission or an outdated
  statement in the original, fix the English document first, with a
  separate patch sent to the documentation maintainers.  Once that change
  is merged, the translation can follow it.

- **Translation maintainers only accept translation work**: new
  translations, updates that follow changes in the original, and fixes
  to existing translations.

- **Adapting is allowed; changing the meaning is not.**  Every language
  has its own grammar and idioms, and a literal word-for-word rendering
  is often wrong.  Rephrase as much as your language needs, as long as
  the reader ends up with the same information as a reader of the
  original.


Before you start
================

Find the translation team
-------------------------

Each translation is listed in the MAINTAINERS file with a ``F:`` line
pointing to its directory.  You can find the people responsible for a
translation with::

  $ scripts/get_maintainer.pl -f Documentation/translations/<lang>/

Look at recent discussions on the linux-doc mailing list
(https://lore.kernel.org/linux-doc/) to get a feel for how the team works,
which conventions it follows and what reviewers usually ask for.  If you
are unsure about something, ask on the list before investing a lot of
work; translation maintainers are generally happy to help newcomers.

Choose what to translate
------------------------

Good candidates are documents that many people read: the development
process documents under ``Documentation/process/``, the admin guide,
and the guides for writing documentation.  Long, rapidly changing or very
specialized documents are harder to keep current and are better left for
later.

Check that the document is not already translated, and that nobody else
is working on it:

- look in ``Documentation/translations/<lang>/``;

- search the linux-doc archive on lore.kernel.org for the file name; a
  pending patch, or a recent review, means someone is already on it;

- use ``tools/docs/checktransupdate.py`` to list the documents that have
  no translation yet::

    $ tools/docs/checktransupdate.py -l <lang> --print-missing-translations

  See Documentation/doc-guide/checktransupdate.rst for the details.

Avoid having several people translate files in the same directory at the
same time.  The patches then tend to touch the same ``index.rst`` and end
up conflicting with each other.  If you plan to translate a whole
directory, say so on the list.

If you are new to kernel development, start with a short document.  The
first patch is where most of the process problems show up, and they are
much easier to fix on a 60-line file than on a 2000-line one.

Set up your environment
-----------------------

Translations are merged through the documentation tree, so base your
work on its ``docs-next`` branch (the ``T:`` line of the DOCUMENTATION
entry in MAINTAINERS gives its location)::

  $ git remote add docs git://git.lwn.net/linux.git
  $ git fetch docs
  $ git switch -c my-translation docs/docs-next

Some translations are collected in their own tree first; if the
MAINTAINERS entry for your language has its own ``T:`` line, ask the
translation maintainers which branch to use.

You will need a working Sphinx installation to build the documentation.
``tools/docs/sphinx-pre-install`` checks your system and tells you what is
missing; Documentation/doc-guide/sphinx.rst has the details.

You will also need to be able to send plain-text email with
``git send-email`` or a mail client configured as described in
Documentation/process/email-clients.rst.  The vger.kernel.org mailing
lists reject HTML email, so replies sent from a web mail interface in its
default configuration never reach the list.


Anatomy of a translated document
================================

Location and file name
----------------------

A translation mirrors the path of its original under the language
directory, and keeps the same file name::

  Documentation/process/howto.rst
  Documentation/translations/<lang>/process/howto.rst

Do not translate file or directory names.  The documentation build uses
this mapping to add, at the top of every page, links to the same page in
the other languages; it only works if the paths match.

The file header
---------------

Start the file with the same SPDX license identifier as the original.
Most translations then indicate, in some form, which document was
translated and include the language's disclaimer.  For example, the
Italian translation uses::

  .. include:: ../disclaimer-ita.rst

  :Original: Documentation/core-api/symbol-namespaces.rst

A plain path is enough in the ``:Original:`` field: the documentation
build turns ``Documentation/...`` paths into links automatically.

The exact header differs from one language to another: some include a
disclaimer in every file, some only on their index page, and some list
the translators while others leave contact information to MAINTAINERS.
Copy the header of a recently merged document in your language rather
than inventing your own, and adjust the relative path of any
``include::`` directive to the depth of your file.

If your language lists translators in each file, avoid duplicating
information that already lives in MAINTAINERS.  Email addresses in
documents go stale; MAINTAINERS is the place where they are kept current.

Labels
------

Sphinx labels (``.. _label:``) are global to the whole documentation
build.  A label copied unchanged from the original therefore defines the
same name twice, which breaks the build or, at best, makes references
resolve to the wrong document.

- Labels placed at the very top of a file are usually not needed at all
  in the translation; simply drop them.

- Labels that are needed, for example because other parts of the
  translated document refer to them, must be made unique by adding a
  language prefix.  Each translation already uses one; look at existing
  files to find it (``it_``, ``sp_``, ``cn_``, ``tw_``, ``pt_``, ...)::

    .. _it_process_howto:

- References to labels of the English documentation are fine and
  expected.  When the target document is not translated yet, linking to
  the English version is the right thing to do.

Citations and other named targets
---------------------------------

Named citations are global as well.  If the original contains::

  ... the C programming language [c-language]_ ...

  .. [c-language] http://www.open-std.org/jtc1/sc22/wg14/www/standards

then a translation that keeps the name ``c-language`` produces a
"duplicate citation" warning for every citation in the file.  Add the
language prefix to both the definitions and their uses::

  ... a linguagem de programação C [pt-c-language]_ ...

  .. [pt-c-language] http://www.open-std.org/jtc1/sc22/wg14/www/standards

The same applies to any other construct that defines a name visible
outside the file.

Cross-references
----------------

Keep every cross-reference of the original, and decide for each one where
it should point:

- if the target document is already translated into your language, point
  to the translation, so that readers stay in their language;

- otherwise, keep the reference to the English document.

Inside the translation directory, ``:doc:`` accepts paths relative to the
current file, so a document in ``Documentation/translations/<lang>/process/``
can refer to the translated ``howto.rst`` with ``:doc:`howto```.  Absolute
paths such as ``:doc:`/process/howto``` always point to the English
documentation.

Translate the visible text of a reference where appropriate, but never
the target::

  :ref:`Criando um tracepoint <process/debugging/driver_development_debugging_guide:ftrace>`

What must not be translated
---------------------------

Some content has to stay exactly as it is in the original:

- literal blocks (``::``), ``code-block`` directives, command lines and
  their output, log messages and code examples; they are copied verbatim,
  including indentation and tab characters;

- identifiers: function, structure, variable and macro names, Kconfig
  options, file names and paths, command names and options, sysfs and
  procfs entries;

- the targets of references, URLs, and the arguments of directives such
  as ``kernel-doc``, ``include`` or ``toctree``;

- the names of tags and other fixed strings that people have to type,
  such as ``Signed-off-by:`` or ``Reviewed-by:``.

The text around those elements is translated as usual.  When a sentence
quotes an identifier, keep the identifier and its markup and translate
the rest of the sentence.

Document structure and formatting
---------------------------------

Keep the structure of the original: the same sections, in the same order,
with the same heading levels, and ideally the same paragraphs.  This makes
the translation much easier to review, and much easier to update later,
because a change in the original maps to an obvious place in the
translation.

Follow the usual rules for kernel documentation:

- wrap lines at 80 columns; literal blocks, tables and long URLs are the
  usual exceptions;

- make heading underlines (and overlines) at least as long as the heading
  text, taking into account that East Asian characters occupy two
  columns;

- use UTF-8, without a byte order mark, and no trailing whitespace;

- keep reStructuredText markup valid.  Inline markup, such as emphasis or
  inline literals, must be separated from the surrounding text by
  whitespace or punctuation, and the word order of a translation can
  change what surrounds it.  When markup has to be followed directly by
  text, separate them with an escaped space (a backslash followed by a
  space), which does not appear in the output::

    ``vmstat``\ *, mas por processo*

If something really needs to be explained to readers of the translation,
such as a term that has no established equivalent, mark it clearly as a
translator's note (``.. note::``) instead of mixing it into the translated
text.  Use such notes sparingly; they are not a way to add content.


Translation quality
===================

Faithfulness
------------

Compare your translation with the original paragraph by paragraph before
sending it.  Look for:

- sentences or list items that were skipped or added;

- statements whose meaning has shifted, for example a relative pronoun
  that refers to a different noun after translation, or a "should" that
  became a "must";

- numbers, versions, option names and paths that changed while being
  retyped.

Terminology
-----------

Use the same terms as the rest of the translation into your language.
Before choosing a translation for a technical term, search the existing
documents::

  $ git grep -i "<term>" Documentation/translations/<lang>/

Some teams maintain a glossary (for example
Documentation/translations/zh_CN/glossary.rst); follow it if there is
one.  Many English terms (kernel, patch, commit, driver, ...) are commonly
used as they are by developers who speak other languages, and the
translation should follow the established usage of the community rather
than force a literal equivalent.

If you disagree with an established term, propose a change separately,
for the whole translation, instead of introducing a second term in a new
document.

Language
--------

Write natural, grammatically correct text in your language.  A reviewer
who has to correct agreement, punctuation or word order in every
paragraph cannot pay attention to the content.  Reading the rendered HTML
page, rather than the source file, is a good way to catch awkward
sentences.

Machine translation and AI tools
--------------------------------

You are responsible for every word you submit.  If you use machine
translation or an AI assistant, review and correct the result as
carefully as if you had written it yourself; translations submitted
without that work are unlikely to be accepted, and reviewers may ask how
a translation was produced.

The rules in Documentation/process/coding-assistants.rst apply to
translations as to any other contribution; in particular, the
``Signed-off-by:`` tag can only be added by the human who certifies the
contribution.


Adding the document to the build
================================

A new document has to be listed in a ``toctree``, or Sphinx warns that it
"isn't included in any toctree" and readers cannot find it.

Add it to the ``index.rst`` of the directory in your language, in the
same position it occupies in the English ``index.rst``.  If the
directory does not exist yet in your translation, create it with an
``index.rst`` of its own and link that index from the parent one.

Some translations keep a list of documents that are not translated yet
in their index files.  If yours does, remove the document from that list
in the same patch.


Building and checking your work
===============================

Build the documentation
-----------------------

Build the complete HTML documentation and make sure your change does not
introduce any new warning::

  $ make htmldocs

Building only a subdirectory with ``SPHINXDIRS`` is faster, and fine
while you are working::

  $ make SPHINXDIRS=translations/<lang> htmldocs

but it is not enough before sending.  Such a partial build does not
contain the English documents, so it cannot detect duplicate labels or
citations, and it reports every reference to an English document as
unknown, which hides real problems among the noise.

The full build of the tree already produces some warnings that have
nothing to do with your change.  The simplest way to check that you did
not add any is to compare the warnings before and after your patch::

  $ make cleandocs && make htmldocs 2>&1 | tee ../before.log
  $ git am my-translation.patch      # or switch to your branch
  $ make cleandocs && make htmldocs 2>&1 | tee ../after.log
  $ diff <(grep -E 'WARNING|ERROR' ../before.log | sort) \
         <(grep -E 'WARNING|ERROR' ../after.log | sort)

``make cleandocs`` matters: Sphinx only rebuilds the files that changed,
so an incremental build does not repeat warnings for files it skipped.

Look at the result
------------------

Open the generated page, under ``Documentation/output/``, in a browser.
Check that headings, lists, tables, literal blocks and links render as
intended, and that the language links at the top of the page lead to the
original and to the other translations of the same document.

For languages that need special fonts, it is also worth building the PDF
output (``make pdfdocs``) from time to time.

Run checkpatch
--------------

Run ``scripts/checkpatch.pl`` on your patch and fix what it reports::

  $ scripts/checkpatch.pl --strict -g HEAD

The warnings most often seen on translation patches are:

- a missing SPDX license identifier;

- lines longer than the limit;

- a missing commit description;

- "added, moved or deleted file(s), does MAINTAINERS need updating?";
  this one can be ignored when the translation directory is already
  covered by a MAINTAINERS entry, which is the case for all existing
  translations.


Preparing the patch
===================

Scope
-----

Send one document per patch.  Several related documents can be sent as a
patch series, with one patch per document.

Never mix changes to the English documentation and to a translation in
the same patch.  They are reviewed by different people, and the English
change may need several revisions before the translation can follow it.

Subject line
------------

Use the prefix that your translation already uses; ``git log --oneline
-- Documentation/translations/<lang>/`` shows it.  Examples of prefixes
currently in use are ``docs/zh_CN:``, ``docs/zh_TW:``, ``doc:it_IT:`` and
``docs: pt_BR:``.  Then describe the change briefly::

  docs: pt_BR: translate process/programming-language.rst

Commit message
--------------

Write the commit message in English, so that everybody involved in
merging the patch can read it.  Say which document is translated or
updated, and on which state of the original the translation is based.
The ``checktransupdate.py`` script recognizes this sentence, followed by
the abbreviated (12-character) hash and the subject of the last commit
that changed the original::

  Update the translation through commit 1234567890ab
  ("docs: some subject of the last change to the original")

With it, the script can tell exactly which changes to the original are
still missing from the translation, even if the translation was written
long after that commit.  The hash and subject are shown by::

  $ git log -1 --format='%h ("%s")' --abbrev=12 -- Documentation/<path>.rst

Tags
----

The tags of a patch go in a single block at the end of the commit
message, without blank lines between them.  Git only parses the last
paragraph of the message as trailers, so a blank line in the middle makes
it ignore the tags above it::

  Reviewed-by: Some Reviewer <reviewer@example.org>
  Signed-off-by: Your Name <you@example.org>

``Signed-off-by:`` must carry your real name; see "Sign your work - the
Developer's Certificate of Origin" in
Documentation/process/submitting-patches.rst.

When you send a new version of a patch, keep the ``Reviewed-by:`` and
``Acked-by:`` tags you received for it, unless the patch changed
significantly since they were given.


Sending the patch
=================

Recipients
----------

Run ``scripts/get_maintainer.pl`` on the patch to get the list of
recipients::

  $ scripts/get_maintainer.pl 0001-docs-pt_BR-translate-something.patch

For a translation this normally includes the maintainers of the
translation, the documentation maintainer and the linux-doc mailing list.
Send the patch to all of them; leaving out the documentation maintainer,
who applies most translation patches, is a common way of getting a patch
ignored.

Sending
-------

Send the patch with ``git send-email``, or with a mail client that does
not alter it.  It is a good idea to send it to yourself first and check
that it applies cleanly with ``git am`` on top of the branch you used as
a base.

Mention below the ``---`` line anything that reviewers should know but
that should not end up in the git history, such as questions about
terms you were unsure of.

New versions
------------

Reviewers will often ask for changes.  Reply to their comments on the
list, inline and in plain text, then send a new version of the patch:

- mark it as such in the subject, for example ``[PATCH v2]``;

- describe what changed since the previous version below the ``---``
  line, and add links to earlier versions on lore.kernel.org::

    ---
    Changes in v2:
    - Use "namespace de usuário", as in the other pt_BR documents.
    - Prefix the citation names to avoid duplicates.

    v1: https://lore.kernel.org/linux-doc/<message-id>/

- address every comment, either by changing the patch or by explaining
  why you did not;

- do not hurry; give reviewers a day or so to comment on a version before
  sending the next one.

Do not add ``RESEND`` to the subject of a modified patch; that word is
reserved for resubmitting a patch unchanged when it received no answer,
as described in Documentation/process/submitting-patches.rst.


The review process
==================

Translation patches are reviewed by the translation maintainers and by
other speakers of the language, and are usually applied by the
documentation maintainer once the translation maintainer is satisfied.
A ``Reviewed-by:`` from the translation maintainer is usually the signal
that the patch is ready.

Reviewers typically check:

- that the patch applies and builds without new warnings;

- the file header, labels, citations and index entries described above;

- faithfulness to the original;

- grammar, style and consistency of terminology;

- the commit message and the tags.

Everyone is welcome to review translation patches.  Reviews from other
speakers of the language are valuable, and reviewing other people's work
is also one of the best ways to learn the conventions of a translation.
If you have checked a patch carefully and agree with it, say so with a
``Reviewed-by:`` tag.

Be patient.  Translation maintainers are volunteers, and it can take a
week or two to get an answer, more during the merge window.


Keeping translations up to date
===============================

The English documentation changes all the time, and an outdated
translation can be worse than none.  ``tools/docs/checktransupdate.py``
lists, for each translated file, the commits to the original that are
not reflected in the translation yet::

  $ tools/docs/checktransupdate.py -l <lang>
  $ tools/docs/checktransupdate.py Documentation/translations/<lang>/process/howto.rst

To update a translation, apply the same changes that the listed commits
made to the original, and nothing else.  Mention those commits in the
commit message, ending with the "Update the translation through commit"
sentence described above, so that the next run of the script starts from
the right place.

When an original document is moved or renamed, move its translation in
the same way, so that the paths keep matching.  When an original is
removed, remove the translation as well.


Starting a new translation
==========================

A new language is a long-term commitment: someone has to review
contributions and keep the existing documents up to date for years.
Before starting one, discuss it on the linux-doc mailing list.

The first patch series of a new translation should contain a small number
of carefully translated documents that build without warnings, plus the
infrastructure that every translation needs:

- a directory ``Documentation/translations/<ll_CC>/``, named after the
  language and country codes (for example ``pt_BR``);

- an ``index.rst`` for the language, with a translation of the disclaimer
  in :ref:`Documentation/translations/index.rst <translations_disclaimer>`,
  usually in a separate file included by the index;

- an entry in the ``toctree`` of Documentation/translations/index.rst;

- an entry in the ``all_languages`` dictionary of
  ``Documentation/sphinx/translations.py``, which produces the language
  links at the top of each page.

The last two lists are sorted alphabetically by the English name of the
language; keep them that way.

Look at the index of an existing translation as a model.  Note that the
``raw:: latex`` directives found in those files select the fonts used for
the PDF output, and that languages written with East Asian characters
need different settings from the others.

A MAINTAINERS entry for the new translation is not part of the first
submission.  It is something that is earned by demonstrated, sustained
effort, and is best discussed with the documentation maintainer once the
translation has been around for a while.


Checklist
=========

Before sending a translation patch, check that:

- the original document is not already translated, and nobody else is
  working on it;

- the file mirrors the path and name of the original;

- the header follows the conventions of your language, starting with the
  SPDX identifier;

- top-of-file labels from the original have been removed, and every
  remaining label and citation name carries the language prefix;

- references point to translated documents where they exist, and to the
  English ones otherwise;

- literal blocks, code, identifiers and reference targets are unchanged;

- the structure of the original is preserved and lines fit in 80
  columns;

- the translation is faithful, grammatically correct and uses the
  established terminology;

- the document is listed in the right ``index.rst``;

- the full ``make htmldocs`` build adds no warning, and the HTML page
  renders correctly;

- ``scripts/checkpatch.pl --strict`` is clean;

- the commit message is in English, describes the change and says on
  which commit of the original the translation is based;

- all tags are in one block and ``Signed-off-by:`` uses your real name;

- the patch goes to everyone listed by ``scripts/get_maintainer.pl``, as
  plain text.
