.. SPDX-License-Identifier: GPL-2.0

==================
Userspace Test API
==================

This file documents all of the userspace testing API.
Userspace tests are built as :ref:`kbuild userprogs <kbuild_userprogs>`,
linked statically and without any external dependencies.

For the widest platform compatibility they should use nolibc, as provided by `init/Makefile.nolibc`.

.. kernel-doc:: include/kunit/uapi.h
   :internal:
