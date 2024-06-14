.. SPDX-License-Identifier: GPL-2.0

Check translation update
==========================

This script helps track the translation status of the documentation in
different locales, i.e., whether the documentation is update-to-date with
the English conterpart.

How it works
------------

It uses ``git log`` command to track the latest English commit from the
translation commit (order by author date) and the latest English commits
from HEAD. If any differences occur, the file is considered as out-of-date,
then commits that need to be updated will be collected and reported.

Features implemented
--------------------

-  check all files in a certain locale
-  check a single file or a set of files
-  provide options to change output format

Usage
-----

::

   checktransupdate.py [-h] [-l LOCALE] [--print-commits | --no-print-commits] [--print-updated-files | --no-print-updated-files] [--debug | --no-debug] [files ...]

Options
~~~~~~~

-  ``-l``, ``--locale``: locale to check when file is not specified
-  ``--[no-]print-commits``: whether to print commits between origin and
   translation
-  ``--[no-]print-updated-files``: whether to print files that do no
   need to be updated
-  ``files``: files to check, if this option is specified, the locale
   option will be ignored.

Samples
~~~~~~~

-  ``./scripts/checktransupdate.py -l zh_CN``
   This will print all the files that need to be updated in the zh_CN locale.
-  ``./scripts/checktransupdate.py Documentation/translations/zh_CN/process/coding-style.rst``
   This will only print the status of the specified file.

Then the output is something like:

::

    Documentation/translations/zh_CN/process/coding-style.rst       (2 commits)
    commit 6813216bbdba ("Documentation: coding-style: ask function-like macros to evaluate parameters")
    commit 185ea7676ef3 ("Documentation: coding-style: Update syntax highlighting for code-blocks")

Features to be implemented
----------------------------

- track the translation status of files that have no translation
- files can be a folder instead of only a file