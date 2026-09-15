#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) Masaharu Noguchi, 2026

"""
Detect Sphinx and Docutils pairs that break PDF builds
======================================================

Sphinx declares the range of Docutils versions it supports, but that range
does not always describe what its LaTeX builder can take.  A distribution
may relax the upper bound in order to ship a newer Docutils, and Sphinx
9.0.x declares support for Docutils 0.22 by itself.  Either way the pair
installs happily and ``make htmldocs`` builds without a complaint, so
nothing looks wrong until ``make pdfdocs`` is run, where several books fail
with::

    ! Dimension too large.
    \\fb@put@frame ...p \\ifdim \\dimen@ >\\ht \\@tempboxa

It takes Docutils, Sphinx and this documentation's largest literal blocks
together.  Docutils 0.22 renders a literal include into ``sphinxVerbatim``
rather than ``sphinxalltt``; ``sphinxVerbatim`` is framed, so it goes
through framed.sty and meets the size limit of sphinx-doc/sphinx#3099 [1]_,
which was fixed only in Sphinx 9.1.0.  memory-barriers.txt alone is over
3000 lines, well past what that path accepts.

admin-guide runs out of TeX main memory before it reaches those boxes and
reports that instead; given more memory it fails on them too.

Distributions reach the pair from either side.  Fedora 44 ships Sphinx
8.2.3 -- which declares ``docutils>=0.20,<0.22`` -- patched to accept
``<0.23`` alongside Docutils 0.22.4, and Ubuntu 26.04 LTS ships the same
two versions.  Upstream releases this sort of work in a new minor rather
than backporting it [2]_, so neither 8.2.x nor 9.0.x will grow the fix in a
point release.

.. [1] https://github.com/sphinx-doc/sphinx/issues/3099
.. [2] https://github.com/orgs/sphinx-doc/discussions/14055
"""

import re
import subprocess
import sys
import textwrap

from kdoc.python_version import PythonVersion

# Sphinx releases before this one carry the sphinx-doc/sphinx#3099 size
# limit; Docutils from this one on routes the blocks into the framed
# environment that hits it.
MIN_SPHINX = PythonVersion("9.1.0").version
DOCUTILS_BREAKS_AT = PythonVersion("0.22").version


class DocutilsVersionChecker:
    """
    Detect a Sphinx and Docutils pair whose LaTeX output cannot be built.
    """

    def __init__(self, sphinx_build=None):
        self.sphinx_build = sphinx_build

    def get_versions(self):
        """
        Get the Sphinx and Docutils versions a sphinx-build command uses.

        Both are Python modules rather than programs, so they have to be
        asked of the very interpreter that runs sphinx-build: a venv and the
        system install can hold different versions. Take it from the
        script's shebang, falling back to the interpreter running this.
        """
        python = sys.executable

        if self.sphinx_build:
            try:
                with open(self.sphinx_build, "r", encoding="utf-8") as f:
                    match = re.match(r"^#!\s*(\S+)", f.readline())
                    if match:
                        python = match.group(1)
            except (OSError, UnicodeDecodeError):
                pass

        script = "import sphinx, docutils; " \
                 "print(sphinx.__version__); print(docutils.__version__)"

        try:
            result = subprocess.run([python, "-c", script],
                                    stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE,
                                    text=True, check=True)
        except (OSError, subprocess.CalledProcessError):
            return None, None

        versions = []
        for line in result.stdout.splitlines()[:2]:
            match = re.match(r"^\s*([0-9]+(?:\.[0-9]+)*)", line)
            if not match:
                return None, None
            versions.append(PythonVersion.parse_version(match.group(1)))

        if len(versions) != 2:
            return None, None

        return versions[0], versions[1]

    def check(self):
        """
        Check for a Sphinx and Docutils pair that breaks PDF builds.
        """

        sphinx_ver, docutils_ver = self.get_versions()

        if not sphinx_ver or sphinx_ver >= MIN_SPHINX:
            return None

        if not docutils_ver or docutils_ver < DOCUTILS_BREAKS_AT:
            return None

        sphinx_str = PythonVersion.ver_str(sphinx_ver)
        docutils_str = PythonVersion.ver_str(docutils_ver)
        min_str = PythonVersion.ver_str(MIN_SPHINX)
        breaks_str = PythonVersion.ver_str(DOCUTILS_BREAKS_AT)

        head = (f"Sphinx {sphinx_str} with Docutils {docutils_str} cannot "
                f"build the largest literal blocks in this documentation.")

        body = (f"Docutils {breaks_str} and later render them into a framed "
                f"LaTeX environment, and Sphinx below {min_str} limits how "
                f"large that environment may get.")

        ways_out = (f"Either upgrade Sphinx to {min_str} or later, or use a "
                    f"Docutils below {breaks_str}.  A virtual environment "
                    f"built from Documentation/sphinx/requirements.txt gives "
                    f"a working pair.")

        msg = "=" * 77 + "\n"
        msg += textwrap.fill(head, width=77) + "\n\n"
        msg += textwrap.fill(body, width=77) + "\n\n"
        msg += textwrap.fill(ways_out, width=77) + "\n\n"
        msg += "HTML builds are unaffected.\n"
        msg += "=" * 77

        return msg
