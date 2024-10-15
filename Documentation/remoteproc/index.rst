.. SPDX-License-Identifier: GPL-2.0

========================================================================
remoteproc - remote processor subsystem in Linux(TM) kernel
========================================================================

Authors:
	- anish kumar  <yesanishhere@gmail.com>

   remote processor subsystem is a way to manage the lifecycle of
   a subsytem that is external to the Linux. The remoteproc framework
   allows different platforms/architectures to control (power on,
   load firmware, power off) those remote processors while abstracting
   the hardware differences, so the entire driver doesn't need to be
   duplicated.

.. toctree::
   :maxdepth: 1

   core
   rproc-api
   rproc-kernel-api

Mailing List
------------
To post a message, send an email to
linux-remoteproc@vger.kernel.org
