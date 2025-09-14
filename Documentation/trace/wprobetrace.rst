.. SPDX-License-Identifier: GPL-2.0

=======================================
Watchpoint probe (wprobe) Event Tracing
=======================================

.. Author: Masami Hiramatsu <mhiramat@kernel.org>

Overview
--------

Wprobe event is a dynamic event based on the hardware breakpoint, which is
similar to other probe events, but it is for watching data access. It allows
you to trace which code accesses a specified data.

As same as other dynamic events, wprobe events are defined via
`dynamic_events` interface file on tracefs.

Synopsis of wprobe-events
-------------------------
::

  w:[GRP/][EVENT] SPEC [FETCHARGS]                       : Probe on data access

 GRP            : Group name for wprobe. If omitted, use "wprobes" for it.
 EVENT          : Event name for wprobe. If omitted, an event name is
                  generated based on the address or symbol.
 SPEC           : Breakpoint specification.
                  [r|w|rw]@<ADDRESS|SYMBOL[+|-OFFS]>[:LENGTH]

   r|w|rw       : Access type, r for read, w for write, and rw for both.
                  Use rw if omitted.
   ADDRESS      : Address to trace (hexadecimal).
   SYMBOL       : Symbol name to trace.
   LENGTH       : Length of the data to trace in bytes. (1, 2, 4, or 8)

 FETCHARGS      : Arguments. Each probe can have up to 128 args.
  $addr         : Fetch the accessing address.
  @ADDR         : Fetch memory at ADDR (ADDR should be in kernel)
  @SYM[+|-offs] : Fetch memory at SYM +|- offs (SYM should be a data symbol)
  +|-[u]OFFS(FETCHARG) : Fetch memory at FETCHARG +|- OFFS address.(\*1)(\*2)
  \IMM          : Store an immediate value to the argument.
  NAME=FETCHARG : Set NAME as the argument name of FETCHARG.
  FETCHARG:TYPE : Set TYPE as the type of FETCHARG. Currently, basic types
                  (u8/u16/u32/u64/s8/s16/s32/s64), hexadecimal types
                  (x8/x16/x32/x64), "char", "string", "ustring", "symbol", "symstr"
                  and bitfield are supported.

  (\*1) this is useful for fetching a field of data structures.
  (\*2) "u" means user-space dereference.

For the details of TYPE, see :ref:`kprobetrace documentation <kprobetrace_types>`.

Usage examples
--------------
Here is an example to add a wprobe event on a variable `jiffies`.
::

  # echo 'w:my_jiffies w@jiffies' >> dynamic_events
  # cat dynamic_events
  w:wprobes/my_jiffies w@jiffies
  # echo 1 > events/wprobes/enable
  # cat trace | head
  #           TASK-PID     CPU#  |||||  TIMESTAMP  FUNCTION
  #              | |         |   |||||     |         |
           <idle>-0       [000] d.Z1.  717.026259: my_jiffies: (tick_do_update_jiffies64+0xbe/0x130)
           <idle>-0       [000] d.Z1.  717.026373: my_jiffies: (tick_do_update_jiffies64+0xbe/0x130)

You can see the code which writes to `jiffies` is `do_timer()`.

Combination with trigger action
-------------------------------
The event trigger action can extend the utilization of this wprobe.

- set_wprobe:WPEVENT:FIELD[+|-ADJUST]
- clear_wprobe:WPEVENT[:FIELD[+|-]ADJUST]

Set these triggers to the target event, then the WPROBE event will be
setup to trace the memory access at FIELD[+|-ADJUST] address.
When clear_wprobe is hit, if FIELD is NOT specified, the WPEVENT is
forcibly cleared. If FIELD[[+|-]ADJUST] is set, it clears WPEVENT only
if its watching address is the same as the FIELD[[+|-]ADJUST] value.

Notes:
The set_wprobe trigger do not change type and length. That should be
set when a new wprobe is created.

The WPROBE event must be disabled when setting the new trigger
and it will be busy afterwards. Recommended usage is to add a new
wprobe at NULL address and keep disabled.


For example, trace the first 8 byte of the dentry data structure passed
to do_truncate() until it is deleted by __dentry_kill().
(Note: all tracefs setup uses '>>' so that it does not kick do_truncate())

  # echo 'w:watch rw@0:8 address=$addr value=+0($addr)' > dynamic_events

  # echo 'f:truncate do_truncate dentry=$arg2' >> dynamic_events
  # echo 'set_wprobe:watch:dentry' >> events/fprobes/truncate/trigger

  # echo 'f:dentry_kill __dentry_kill dentry=$arg1' >> dynamic_events
  # echo 'clear_wprobe:watch:dentry' >> events/fprobes/dentry_kill/trigger

  # echo 1 >> events/fprobes/truncate/enable
  # echo 1 >> events/fprobes/dentry_kill/enable

  # echo aaa > /tmp/hoge
  # echo bbb > /tmp/hoge
  # echo ccc > /tmp/hoge
  # rm /tmp/hoge

Then, the trace data will show;

# tracer: nop
#
# entries-in-buffer/entries-written: 16/16   #P:8
#
#                                _-----=> irqs-off/BH-disabled
#                               / _----=> need-resched
#                              | / _---=> hardirq/softirq
#                              || / _--=> preempt-depth
#                              ||| / _-=> migrate-disable
#                              |||| /     delay
#           TASK-PID     CPU#  |||||  TIMESTAMP  FUNCTION
#              | |         |   |||||     |         |
              sh-113     [004] .....     6.467444: truncate: (do_truncate+0x4/0x120) dentry=0xffff8880044f0fd8
              sh-113     [004] ..Zff     6.468534: watch: (lookup_fast+0xaa/0x150) address=0xffff8880044f0fd8 value=0x200080
              sh-113     [004] ..Zff     6.468542: watch: (step_into+0x82/0x360) address=0xffff8880044f0fd8 value=0x200080
              sh-113     [004] ..Zff     6.468547: watch: (step_into+0x9f/0x360) address=0xffff8880044f0fd8 value=0x200080
              sh-113     [004] ..Zff     6.468553: watch: (path_openat+0xb3a/0xe70) address=0xffff8880044f0fd8 value=0x200080
              sh-113     [004] ..Zff     6.468557: watch: (path_openat+0xb9a/0xe70) address=0xffff8880044f0fd8 value=0x200080
              sh-113     [004] .....     6.468563: truncate: (do_truncate+0x4/0x120) dentry=0xffff8880044f0fd8
              sh-113     [004] ...1.     6.469826: dentry_kill: (__dentry_kill+0x0/0x220) dentry=0xffff8880044f0ea0
              sh-113     [004] ...1.     6.469859: dentry_kill: (__dentry_kill+0x0/0x220) dentry=0xffff8880044f0d68
              rm-118     [001] ..Zff     6.472360: watch: (lookup_fast+0xaa/0x150) address=0xffff8880044f0fd8 value=0x200080
              rm-118     [001] ..Zff     6.472366: watch: (step_into+0x82/0x360) address=0xffff8880044f0fd8 value=0x200080
              rm-118     [001] ..Zff     6.472370: watch: (step_into+0x9f/0x360) address=0xffff8880044f0fd8 value=0x200080
              rm-118     [001] ..Zff     6.472386: watch: (lookup_fast+0xaa/0x150) address=0xffff8880044f0fd8 value=0x200080
              rm-118     [001] ..Zff     6.472390: watch: (step_into+0x82/0x360) address=0xffff8880044f0fd8 value=0x200080
              rm-118     [001] ..Zff     6.472394: watch: (step_into+0x9f/0x360) address=0xffff8880044f0fd8 value=0x200080
              rm-118     [001] ..Zff     6.472415: watch: (lookup_one_qstr_excl+0x2c/0x150) address=0xffff8880044f0fd8 value=0x200080
              rm-118     [001] ..Zff     6.472419: watch: (lookup_one_qstr_excl+0xd5/0x150) address=0xffff8880044f0fd8 value=0x200080
              rm-118     [001] ..Zff     6.472424: watch: (may_delete+0x18/0x200) address=0xffff8880044f0fd8 value=0x200080
              rm-118     [001] ..Zff     6.472428: watch: (may_delete+0x194/0x200) address=0xffff8880044f0fd8 value=0x200080
              rm-118     [001] ..Zff     6.472446: watch: (vfs_unlink+0x63/0x1c0) address=0xffff8880044f0fd8 value=0x200080
              rm-118     [001] d.Z..     6.472528: watch: (dont_mount+0x19/0x30) address=0xffff8880044f0fd8 value=0x200180
              rm-118     [001] ..Zff     6.472533: watch: (vfs_unlink+0x11a/0x1c0) address=0xffff8880044f0fd8 value=0x200180
              rm-118     [001] ..Zff     6.472538: watch: (vfs_unlink+0x12e/0x1c0) address=0xffff8880044f0fd8 value=0x200180
              rm-118     [001] d.Z1.     6.472543: watch: (d_delete+0x61/0xa0) address=0xffff8880044f0fd8 value=0x200080
              rm-118     [001] d.Z1.     6.472547: watch: (dentry_unlink_inode+0x14/0x110) address=0xffff8880044f0fd8 value=0x200080
              rm-118     [001] d.Z1.     6.472551: watch: (dentry_unlink_inode+0x1e/0x110) address=0xffff8880044f0fd8 value=0x80
              rm-118     [001] d.Z..     6.472563: watch: (fast_dput+0x8d/0x120) address=0xffff8880044f0fd8 value=0x80
              rm-118     [001] ...1.     6.472567: dentry_kill: (__dentry_kill+0x0/0x220) dentry=0xffff8880044f0fd8
              sh-113     [004] ...2.     6.473049: dentry_kill: (__dentry_kill+0x0/0x220) dentry=0xffff888006e383a8

You can see the watch event is correctly configured on the dentry.
