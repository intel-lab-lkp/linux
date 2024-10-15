==================================
The Linux Remoteproc userspace API
==================================

Introduction
============

A Remoteproc (rproc) is a subsystem for managing the lifecycle
of a processor that is connected to Linux.

At times, userspace may need to check the state of the remote processor to
prevent other processes from using it. For instance, if the remote processor
is a DSP used for playback, there may be situations where the DSP is
undergoing recovery and cannot be used. In such cases, attempts to access the
DSP for playback should be blocked. The rproc framework provides sysfs APIs
to inform userspace of the processor's current status which should be utilised
to achieve the same.

Additionally, there are scenarios where userspace applications need to explicitly
control the rproc. In these cases, rproc also offers the file descriptors.

The simplest API
================

Below set of api's can be used to start and stop the rproc
where 'X' refers to instance of associated remoteproc. There can be systems
where there are more than one rprocs such as multiple DSP's
connected to application processors running Linux.
::
   echo start > /sys/class/remoteproc/remoteprocX/state
   echo stop > /sys/class/remoteproc/remoteprocX/state

To know the state of rproc:

.. code-block::

   cat /sys/class/remoteproc/remoteprocX/state


To dynamically replace firmware, execute the following commands:

.. code-block::

   echo stop > /sys/class/remoteproc/remoteprocX/state
   echo -n <firmware_name> >
   /sys/class/remoteproc/remoteprocX/firmware
   echo start > /sys/class/remoteproc/remoteprocX/state

To simulate a remote crash, execute:

.. code-block::

   echo 1 > /sys/kernel/debug/remoteproc/remoteprocX/crash

To get the trace logs, execute

.. code-block::

   cat /sys/kernel/debug/remoteproc/remoteprocX/crashX

where X will be 0 or 1 if there are 2 resources. Also, this
file will only exist if resources are defined in ELF firmware
file.

The coredump feature can be disabled with the following command:

.. code-block::

   echo disabled > /sys/kernel/debug/remoteproc/remoteprocX/coredump

Userspace can also control start/stop of rproc by using a
remoteproc Character Device, it can open the open a file descriptor
and write `start` to initiate it, and `stop` to terminate it.

[FIXME -- better explanations]
