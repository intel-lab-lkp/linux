.. SPDX-License-Identifier: GPL-2.0

===============
Tracing Remotes
===============

:Author: Vincent Donnefort <vdonnefort@google.com>

Overview
========
A trace remote relies on ring-buffer remotes to read and control compatible
tracing buffers, written by entity such as firmware or hypervisor.

Once registered, a tracefs instance will appear for this remote in the Tracefs
directory **remotes/**. This remote can be read and controlled using the same
files as regular Tracefs instances such as **trace_pipe**, **tracing_on** or
**trace**.

Register a remote
=================
A remote must provide a set of callbacks `struct trace_remote_callbacks` whom
description can be found below. Those callbacks allows Tracefs to enable and
disable tracing and events, to load and unload a tracing buffer (a set of
ring-buffers) and to swap a reader page with the head page, which enables
consuming reading.

.. kernel-doc:: include/linux/trace_remote.h

Declare a remote event
======================
Macros are provided to ease the declaration of remote events, in a similar
fashion to in-kernel events. A declaration must provide an ID, a description of
the event arguments and how to print the event:

.. code-block:: c

	REMOTE_EVENT(foo, EVENT_FOO_ID,
		RE_STRUCT(
			re_field(u64, bar)
		),
		RE_PRINTK("bar=%lld", __entry->bar)
	);

Then those events must be declared in a C file with the following:

.. code-block:: c

	#define REMOTE_EVENT_INCLUDE_FILE foo_events.h
	#include <trace/define_remote_events.h>

This will provide a `struct remote_event remote_event_foo` that can be given to
`trace_remote_register`.

Simple ring-buffer
==================
A simple implementation for a ring-buffer writer can be found in
kernel/trace/simple_ring_buffer.c.

.. kernel-doc:: include/linux/simple_ring_buffer.h
