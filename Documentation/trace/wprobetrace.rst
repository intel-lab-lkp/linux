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
                  Default is rw if omitted.
   ADDRESS      : Address to watch (hexadecimal). MUST be in kernel space.
   SYMBOL[+|-OFFS] : Symbol name to watch. (Optional positive/negative offset)
   LENGTH       : Length of the data to watch in bytes. (1, 2, 4, or 8)
                  Default is 4.

  FETCHARGS      : Arguments. Each probe can have up to 128 args.
   $addr         : Fetch the accessing address.
   $value        : Fetch the memory value at the accessing address (same as +0($addr)).
   @ADDR         : Fetch memory at ADDR (ADDR should be in kernel)
   @SYM[+|-offs] : Fetch memory at SYM +|- offs (SYM should be a data symbol)
   +|-[u]OFFS(FETCHARG) : Fetch memory at FETCHARG +|- OFFS address.(\*1)(\*2)
   \IMM          : Store an immediate value to the argument.
   NAME=FETCHARG : Set NAME as the argument name of FETCHARG.
   FETCHARG:TYPE : Set TYPE as the type of FETCHARG. Currently, basic types
                  (u8/u16/u32/u64/s8/s16/s32/s64), hexadecimal types
                  (x8/x16/x32/x64), "char", "string", "ustring", "symbol", "symstr"
                  and bitfield are supported.

   (\*1) This is useful for fetching a field of data structures.
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

You can see the code which writes to `jiffies` is `tick_do_update_jiffies64()`.

Notes
-----
Wprobe event does not disable itself even if the module is unloaded.
For example, if you add a wprobe event on a module variable, and then
unload the module, the wprobe event will still be enabled. This is for
watching the address is used unexpectedly after the module is unloaded.

Combination with trigger action
-------------------------------
The event trigger action can extend the utilization of this wprobe.

- set_wprobe:WPEVENT:FIELD[+|-ADJUST][:COUNT]
- clear_wprobe:WPEVENT[:FIELD[+|-ADJUST][:COUNT]]

Set these triggers to the target event, then the WPROBE event will be
setup to trace the memory access at FIELD[+|-ADJUST] address.
When clear_wprobe is hit, if FIELD is NOT specified, the WPEVENT is
forcibly cleared. If FIELD[+|-ADJUST] is set, it clears WPEVENT only
if its watching address is the same as the FIELD[+|-ADJUST] value.
If COUNT is specified, it will set/clear WPEVENT only if it hits COUNT
times.

Notes:
- set_wprobe only works on the wprobe which is NOT set a valid address yet,
  and it must be enabled after the trigger is set.
- clear_wprobe only works on the wprobe which is set a valid address, and it
  will be soft-disabled after the trigger is cleared.
- Therefore, if a trigger sets/clears a wprobe, other/same trigger events
  will not work (on the same event) while the wprobe is set.

The set_wprobe trigger does not change the type and length, these
must be set when creating a new wprobe.

The WPROBE event must be disabled when setting the new trigger
and it will be busy afterwards. Recommended usage is to add a new
wprobe at invalid dummy address (-1) and keep disabled.

Wprobe triggers only support target addresses in kernel memory. If a
set_wprobe trigger evaluates to a user-space memory address or NULL
pointer, the trigger action ignores the update and skips setting the
watchpoint.

Wprobe triggers are not supported on kprobe_events, because kprobes
themselves can use software breakpoints which conflicts with wprobe
operation.


For example, trace the first 8 bytes of the dentry data structure passed
to do_truncate() until it is deleted by dentry_kill().
(Note: all tracefs setup uses '>>' so that it does not kick do_truncate())
::

  # echo 'w:watch rw@-1:8 address=$addr value=+0($addr)' >> dynamic_events
  # echo 'f:truncate do_truncate dentry=$arg2' >> dynamic_events
  # echo 'set_wprobe:watch:dentry' >> events/fprobes/truncate/trigger
  # echo 'f:dentry_kill dentry_kill dentry=$arg1' >> dynamic_events
  # echo 'clear_wprobe:watch:dentry' >> events/fprobes/dentry_kill/trigger
  # echo 1 >> events/fprobes/truncate/enable
  # echo 1 >> events/fprobes/dentry_kill/enable

  # echo aaa > /tmp/hoge
  # echo bbb > /tmp/hoge
  # echo ccc > /tmp/hoge
  # rm /tmp/hoge

Then, the trace data will show::

 # tracer: nop
 #
 # entries-in-buffer/entries-written: 32/32   #P:8
 #
 #                                _-----=> irqs-off/BH-disabled
 #                               / _----=> need-resched
 #                              | / _---=> hardirq/softirq
 #                              || / _--=> preempt-depth
 #                              ||| / _-=> migrate-disable
 #                              |||| /     delay
 #           TASK-PID     CPU#  |||||  TIMESTAMP  FUNCTION
 #              | |         |   |||||     |         |
               sh-107     [004] ...1.     9.990418: dentry_kill: (dentry_kill+0x0/0x2c0) dentry=0xffff888004ad6618
               sh-107     [004] ...1.     9.990914: dentry_kill: (dentry_kill+0x0/0x2c0) dentry=0xffff888004b3de78
               sh-107     [004] ...1.     9.993175: dentry_kill: (dentry_kill+0x0/0x2c0) dentry=0xffff8880049ddd40
               sh-107     [004] .....     9.995198: truncate: (do_truncate+0x4/0x120) dentry=0xffff8880048083a8
               sh-107     [004] ...1.     9.995389: dentry_kill: (dentry_kill+0x0/0x2c0) dentry=0xffff8880049db998
               sh-107     [004] ..Zff     9.997503: watch: (lookup_fast+0xaa/0x150) address=0xffff8880048083a8 value=0x8200080
               sh-107     [004] ..Zff     9.997509: watch: (path_openat+0x211/0xda0) address=0xffff8880048083a8 value=0x8200080
               sh-107     [004] ..Zff     9.997514: watch: (path_openat+0xa56/0xda0) address=0xffff8880048083a8 value=0x8200080
               sh-107     [004] ..Zff     9.997518: watch: (path_openat+0xae2/0xda0) address=0xffff8880048083a8 value=0x8200080
               sh-107     [004] .....     9.997521: truncate: (do_truncate+0x4/0x120) dentry=0xffff8880048083a8
               sh-107     [004] ...1.     9.997582: dentry_kill: (dentry_kill+0x0/0x2c0) dentry=0xffff888004808270
               sh-107     [004] ...1.     9.999365: dentry_kill: (dentry_kill+0x0/0x2c0) dentry=0xffff8880049db728
               sh-107     [004] ...1.     9.999388: dentry_kill: (dentry_kill+0x0/0x2c0) dentry=0xffff888004b1c000
               rm-113     [005] ..Zff    10.000965: watch: (lookup_fast+0xaa/0x150) address=0xffff8880048083a8 value=0x8200080
               rm-113     [005] ..Zff    10.000971: watch: (path_lookupat+0x97/0x1e0) address=0xffff8880048083a8 value=0x8200080
               rm-113     [005] ..Zff    10.000984: watch: (lookup_fast+0xaa/0x150) address=0xffff8880048083a8 value=0x8200080
               rm-113     [005] ..Zff    10.000988: watch: (path_lookupat+0x97/0x1e0) address=0xffff8880048083a8 value=0x8200080
               rm-113     [005] ..Zff    10.001010: watch: (lookup_one_qstr_excl+0x28/0x140) address=0xffff8880048083a8 value=0x8200080
               rm-113     [005] ..Zff    10.001014: watch: (lookup_one_qstr_excl+0xd1/0x140) address=0xffff8880048083a8 value=0x8200080
               rm-113     [005] ..Zff    10.001018: watch: (may_delete_dentry+0x1c/0x200) address=0xffff8880048083a8 value=0x8200080
               rm-113     [005] ..Zff    10.001021: watch: (may_delete_dentry+0x195/0x200) address=0xffff8880048083a8 value=0x8200080
               rm-113     [005] ..Zff    10.001031: watch: (vfs_unlink+0x5e/0x260) address=0xffff8880048083a8 value=0x8200080
               rm-113     [005] d.Z..    10.001067: watch: (d_make_discardable+0x1b/0x40) address=0xffff8880048083a8 value=0x8200080
               rm-113     [005] d.Z..    10.001071: watch: (d_make_discardable+0x29/0x40) address=0xffff8880048083a8 value=0x200080
               rm-113     [005] ...1.    10.001072: dentry_kill: (dentry_kill+0x0/0x2c0) dentry=0xffff8880048083a8
               rm-113     [005] ...1.    10.001218: dentry_kill: (dentry_kill+0x0/0x2c0) dentry=0xffff8880048083a8
               sh-107     [004] ...1.    10.001416: dentry_kill: (dentry_kill+0x0/0x2c0) dentry=0xffff8880049db110
               sh-107     [004] ...1.    10.001444: dentry_kill: (dentry_kill+0x0/0x2c0) dentry=0xffff8880049db248
               sh-107     [004] ...1.    10.001500: dentry_kill: (dentry_kill+0x0/0x2c0) dentry=0xffff888004ad6618
               sh-107     [004] ...1.    10.002067: dentry_kill: (dentry_kill+0x0/0x2c0) dentry=0xffff888004b41e78
               sh-107     [004] ...1.    10.904920: dentry_kill: (dentry_kill+0x0/0x2c0) dentry=0xffff888004b41e78
               sh-107     [004] ...1.    10.905129: dentry_kill: (dentry_kill+0x0/0x2c0) dentry=0xffff888004ad6618

You can see the watch event is correctly configured on the dentry.
