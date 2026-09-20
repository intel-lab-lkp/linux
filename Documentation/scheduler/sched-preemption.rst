=====================
Scheduler preemption
=====================

The kernel can be built to run kernel code either uninterruptibly, or with
varying degrees of preemptibility.  These are the *preemption models*.

When CONFIG_PREEMPT_DYNAMIC is enabled the preemption model can additionally
be selected at boot time with the ``preempt=`` command line parameter, without
rebuilding the kernel.  See
Documentation/admin-guide/kernel-parameters.txt for the parameter itself.

The models selectable at runtime are:

	=========	====================================================
	none		No preemption of kernel code other than at explicit
			``cond_resched()`` / blocking points.
	voluntary	As ``none``, plus ``might_sleep()`` sites.
	full		Any section that is not explicitly preempt disabled
			may be preempted at any time.  Tasks also yield
			contended spinlocks.
	lazy		As ``full``, except that a reschedule requested by
			the fair scheduler does not interrupt the target
			CPU.  It is committed at the next return to user
			space or at the next tick, whichever comes first.
	=========	====================================================

Not every model is available on every kernel.  On a kernel that selects
ARCH_HAS_PREEMPT_LAZY, only ``full`` and ``lazy`` are offered and ``none`` and
``voluntary`` are rejected.  A PREEMPT_RT kernel likewise does not offer
``none`` or ``voluntary``.

The model that is actually active is reported in the boot log::

	Dynamic Preempt: full

PREEMPT_LAZY
============

``lazy`` is the interesting one, because its behaviour cannot be understood
from the "can this be preempted" question alone.

When a wakeup makes a running CFS task preemptible, the fair scheduler calls
``resched_curr_lazy()``.  With the lazy model inactive that is just
``resched_curr()``; with ``lazy`` active it sets ``TIF_NEED_RESCHED_LAZY``
instead of ``TIF_NEED_RESCHED``.  The consequence is in ``__resched_curr()``:
the cross-CPU reschedule IPI is only sent for ``TIF_NEED_RESCHED``.

A lazy reschedule therefore does *not* interrupt the target CPU.  It is
committed later, in one of two ways:

1. On the next return to user mode.  ``__exit_to_user_mode_loop()`` treats
   ``_TIF_NEED_RESCHED | _TIF_NEED_RESCHED_LAZY`` as a reschedule request, and
   ``xfer_to_guest_mode_work()`` does the same for a vCPU returning to guest
   mode.

2. In ``scheduler_tick()``, which promotes a pending
   ``TIF_NEED_RESCHED_LAZY`` to a full ``TIF_NEED_RESCHED`` once per tick.

The tick is thus an upper bound on lazy preemption latency for CFS tasks: a
runnable CFS task that has been passed over is picked up within one tick, not
immediately.  The tick is guaranteed to run in the case that matters: while
more than one CFS task is runnable on the CPU, ``sched_can_stop_tick()``
refuses to stop the tick.  A lazy reschedule aimed at the idle task is promoted
to a full one instead.

That is the intended trade-off -- it avoids sending an IPI to every CPU that
has a runnable CFS task -- and it is why ``lazy`` gives up latency in exchange
for fewer inter-processor interrupts.

The idle task is never delayed: ``__resched_curr()`` promotes a lazy
reschedule to a full one when the target is the idle task.

Only the fair scheduler issues lazy reschedules.  RT and deadline tasks are
still preempted immediately.  ``lazy`` is therefore not a replacement for
PREEMPT_RT; it is a way to keep most of the responsiveness of ``full`` while
removing a large part of its IPI traffic.

Debugging
=========

The scheduler debugfs directory provides ``/sys/kernel/debug/sched/preempt``,
which lists the models that may be switched to; the active one is enclosed in
parentheses::

	# cat /sys/kernel/debug/sched/preempt
	full (lazy)

Writing a model name to the file switches to it.  A name that the kernel
cannot select (either because the model is not built in, or because it is not
available on this kernel) is rejected with EINVAL.  The ``preempt=``
boot parameter rejects such a value as well, but only prints an ``unsupported
mode`` warning and keeps the default model.

Measuring
=========

Two things routinely trip people up when measuring preemption latency.

First, the classic real-time latency tools do not exercise lazy preemption at
all.  Lazy only applies to the fair class, so waking a SCHED_FIFO task takes
``resched_curr()`` and preempts immediately, whichever model is active.  And
when the woken task runs on the CPU the wakeup happens on, even a fair lazy
reschedule is committed as soon as that CPU returns to user mode, which for an
already runnable task is immediate.  For both reasons such workloads measure
the same under ``lazy`` and ``full``.

The difference only shows up when the woken task is *not* running on the CPU
where the wakeup happens.  In that case the waker requests the reschedule, and
under ``lazy`` that request is not turned into an IPI; the target CPU only
notices at its next tick.  A measurement that cannot distinguish ``none`` from
``full`` is usually not measuring the preemption model at all: in the absence
of ``lazy``, the reschedule IPI is sent regardless of the model and provides a
preemption point even on PREEMPT_NONE.

Second, when driving cross-CPU wakeups from a periodic source, keep the wakeup
period away from integer multiples of the tick.  A period that is phase-locked
to the tick will hide (or invent) exactly the effect that is being measured.
Add jitter to the wakeup period, and confirm the active model from the
``Dynamic Preempt:`` line rather than assuming that ``preempt=`` took effect.
