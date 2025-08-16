// SPDX-License-Identifier: GPL-2.0-only
/// @description: Finds direct and inter-procedural calls to sleeping functions
///   (including spin_lock in PREEMPT_RT) within atomic contexts, such as
///   preemption or interrupt disabled regions. Detects two types of bugs:
///   1. Direct: A sleeping function is called inside an atomic context.
///   2. Indirect: A function called from an atomic context eventually calls
///      a sleeping function through a chain of calls.
///
// Confidence: High
// Copyright: (C) 2025 Yunseong Kim <ysk@kzalloc.com>
// Options: --no-includes --include-headers

virtual report

// =========================================================================
// 1. Main Rules to Find Violations
// =========================================================================

// --- PART 1: Direct (Intra-procedural) Violation Detection ---

@find_direct_sleep_in_atomic@
position p_atomic, p_call;
identifier bad_func =~ "^(mutex_lock|mutex_lock_interruptible|mutex_lock_killable|down|down_interruptible|down_killable|down_trylock|rwsem_down_read|rwsem_down_write|ww_mutex_lock|msleep|ssleep|usleep_range|wait_for_completion|schedule|cond_resched|copy_from_user|copy_to_user|get_user|put_user|vmalloc|spin_lock|read_lock|write_lock)$";
expression lock, flags;
@@
(
  raw_spin_lock@p_atomic(...)
| raw_spin_lock_irq@p_atomic(...)
| raw_spin_lock_irqsave@p_atomic(...)
| raw_spin_lock_bh@p_atomic(...)
| raw_read_lock@p_atomic(...)
| raw_read_lock_irq@p_atomic(...)
| raw_read_lock_irqsave@p_atomic(...)
| raw_read_lock_bh@p_atomic(...)
| raw_write_lock@p_atomic(...)
| raw_write_lock_irq@p_atomic(...)
| raw_write_lock_irqsave@p_atomic(...)
| raw_write_lock_bh@p_atomic(...)
| preempt_disable@p_atomic()
| local_irq_disable@p_atomic()
| local_irq_save@p_atomic(...)
| local_bh_disable@p_atomic()
| bit_spin_lock@p_atomic(...)
)
<...
  bad_func@p_call(...)
...>
(
  raw_spin_unlock(lock)
| raw_spin_unlock_irq(lock)
| raw_spin_unlock_irqrestore(lock, flags)
| raw_spin_unlock_bh(lock)
| raw_read_unlock(lock)
| raw_read_unlock_irq(lock)
| raw_read_unlock_irqrestore(lock, flags)
| raw_read_unlock_bh(lock)
| raw_write_unlock(lock)
| raw_write_unlock_irq(lock)
| raw_write_unlock_irqrestore(lock, flags)
| raw_write_unlock_bh(lock)
| preempt_enable()
| local_irq_enable()
| local_irq_restore(flags)
| local_bh_enable()
| bit_spin_unlock(...)
)

@find_direct_sleep_alloc_in_atomic@
position p_atomic, p_call;
identifier alloc_func =~ "^(kmalloc|kzalloc|kcalloc|kvmalloc|kvzalloc|kvcalloc)$";
expression gfp, lock, flags;
@@
(
  raw_spin_lock@p_atomic(...)
| raw_spin_lock_irq@p_atomic(...)
| raw_spin_lock_irqsave@p_atomic(...)
| raw_spin_lock_bh@p_atomic(...)
| raw_read_lock@p_atomic(...)
| raw_read_lock_irq@p_atomic(...)
| raw_read_lock_irqsave@p_atomic(...)
| raw_read_lock_bh@p_atomic(...)
| raw_write_lock@p_atomic(...)
| raw_write_lock_irq@p_atomic(...)
| raw_write_lock_irqsave@p_atomic(...)
| raw_write_lock_bh@p_atomic(...)
| preempt_disable@p_atomic()
| local_irq_disable@p_atomic()
| local_irq_save@p_atomic(...)
| local_bh_disable@p_atomic()
| bit_spin_lock@p_atomic(...)
)
<...
  alloc_func@p_call(..., gfp)
...>
(
  raw_spin_unlock(lock)
| raw_spin_unlock_irq(lock)
| raw_spin_unlock_irqrestore(lock, flags)
| raw_spin_unlock_bh(lock)
| raw_read_unlock(lock)
| raw_read_unlock_irq(lock)
| raw_read_unlock_irqrestore(lock, flags)
| raw_read_unlock_bh(lock)
| raw_write_unlock(lock)
| raw_write_unlock_irq(lock)
| raw_write_unlock_irqrestore(lock, flags)
| raw_write_unlock_bh(lock)
| preempt_enable()
| local_irq_enable()
| local_irq_restore(flags)
| local_bh_enable()
| bit_spin_unlock(...)
)

// --- PART 2: Indirect (Inter-procedural) Violation Data Collection ---

@collect_atomic_callees@
position p_atomic, p_callee_call;
identifier callee_func !~ "^\\b(raw_spin|raw_read|raw_write|preempt|local_irq|local_bh|printk|pr_|dev_)\\b";
expression lock, flags;
@@
(
  raw_spin_lock@p_atomic(...)
| raw_spin_lock_irq@p_atomic(...)
| raw_spin_lock_irqsave@p_atomic(...)
| raw_spin_lock_bh@p_atomic(...)
| raw_read_lock@p_atomic(...)
| raw_read_lock_irq@p_atomic(...)
| raw_read_lock_irqsave@p_atomic(...)
| raw_read_lock_bh@p_atomic(...)
| raw_write_lock@p_atomic(...)
| raw_write_lock_irq@p_atomic(...)
| raw_write_lock_irqsave@p_atomic(...)
| raw_write_lock_bh@p_atomic(...)
| preempt_disable@p_atomic()
| local_irq_disable@p_atomic()
| local_irq_save@p_atomic(...)
| local_bh_disable@p_atomic()
| bit_spin_lock@p_atomic(...)
)
<...
  callee_func@p_callee_call(...)
...>
(
  raw_spin_unlock(lock)
| raw_spin_unlock_irq(lock)
| raw_spin_unlock_irqrestore(lock, flags)
| raw_spin_unlock_bh(lock)
| raw_read_unlock(lock)
| raw_read_unlock_irq(lock)
| raw_read_unlock_irqrestore(lock, flags)
| raw_read_unlock_bh(lock)
| raw_write_unlock(lock)
| raw_write_unlock_irq(lock)
| raw_write_unlock_irqrestore(lock, flags)
| raw_write_unlock_bh(lock)
| preempt_enable()
| local_irq_enable()
| local_irq_restore(flags)
| local_bh_enable()
| bit_spin_unlock(...)
)

@collect_potential_sleepers@
position p_def, p_bad_call;
identifier func_def;
identifier bad_func =~ "^(mutex_lock|mutex_lock_interruptible|mutex_lock_killable|down|down_interruptible|down_killable|down_trylock|rwsem_down_read|rwsem_down_write|ww_mutex_lock|msleep|ssleep|usleep_range|wait_for_completion|schedule|cond_resched|copy_from_user|copy_to_user|get_user|put_user|vmalloc|spin_lock|read_lock|write_lock)$";
@@
(
func_def@p_def(...) {
  <...
    bad_func@p_bad_call(...)
  ...>
}
|
static inline func_def@p_def(...) {
  <...
    bad_func@p_bad_call(...)
  ...>
}
)

@collect_potential_alloc_sleepers@
position p_def, p_bad_call;
identifier func_def;
identifier alloc_func =~ "^(kmalloc|kzalloc|kcalloc|kvmalloc|kvzalloc|kvcalloc)$";
expression gfp;
@@
(
func_def@p_def(...) {
  <...
    alloc_func@p_bad_call(..., gfp)
  ...>
}
|
static inline func_def@p_def(...) {
  <...
    alloc_func@p_bad_call(..., gfp)
  ...>
}
)

@collect_call_graph@
position p_def, p_call;
identifier caller_func, callee_func !~ "^\\b(raw_spin|raw_read|raw_write|preempt|local_irq|local_bh|printk|pr_|dev_)\\b";
@@
(
caller_func@p_def(...) {
  <...
    callee_func@p_call(...)
  ...>
}
|
static inline caller_func@p_def(...) {
  <...
    callee_func@p_call(...)
  ...>
}
)

// =========================================================================
// 2. Python Scripts for Data Collection and Rich Reporting
// =========================================================================

@initialize:python@
@@
REASONS = {
    "mutex_lock": "is a sleeping lock",
    "down": "is a sleeping semaphore operation",
    "rwsem_down": "is a sleeping lock",
    "ww_mutex_lock": "is a sleeping lock",
    "msleep": "is an explicit sleep",
    "ssleep": "is an explicit sleep",
    "usleep_range": "is an explicit sleep",
    "wait_for_completion": "waits for an event and sleeps",
    "schedule": "explicitly invokes the scheduler",
    "cond_resched": "may invoke the scheduler",
    "copy_from_user": "may sleep on page fault",
    "copy_to_user": "may sleep on page fault",
    "get_user": "may sleep on page fault",
    "put_user": "may sleep on page fault",
    "vmalloc": "can sleep",
    "spin_lock": "is a sleeping lock on PREEMPT_RT",
    "read_lock": "is a sleeping lock on PREEMPT_RT",
    "write_lock": "is a sleeping lock on PREEMPT_RT",
    "kmalloc": "may sleep in PREEMPT_RT",
    "kzalloc": "may sleep in PREEMPT_RT",
    "kcalloc": "may sleep in PREEMPT_RT",
    "kvmalloc": "may sleep in PREEMPT_RT",
    "kvzalloc": "may sleep in PREEMPT_RT",
    "kvcalloc": "may sleep in PREEMPT_RT",
}

def get_reason(func_name):
    for key in REASONS:
        if func_name.startswith(key):
            return REASONS[key]
    return "is prohibited in atomic context"

// --- PART 1 Report: Direct Violations ---

@script:python depends on find_direct_sleep_in_atomic@
p_atomic << find_direct_sleep_in_atomic.p_atomic;
p_call << find_direct_sleep_in_atomic.p_call;
bad_func << find_direct_sleep_in_atomic.bad_func;
@@
bad_func_name = str(bad_func)
reason_str = get_reason(bad_func_name)

# Handle p_call and p_atomic as list or tuple or single Location
if isinstance(p_call, (list, tuple)):
    if p_call:
        p_call = p_call[0]
    else:
        p_call = None
if isinstance(p_atomic, (list, tuple)):
    if p_atomic:
        p_atomic = p_atomic[0]
    else:
        p_atomic = None

if p_call and hasattr(p_call, 'file') and hasattr(p_call, 'line') and hasattr(p_call, 'current_element') and p_atomic and hasattr(p_atomic, 'line'):
    coccilib.report.print_report(p_call,
        f"BUG (Direct): Prohibited call to {bad_func_name}() ({reason_str}) "
        f"inside atomic context started at line {p_atomic.line} "
        f"in function {p_call.current_element}.")
else:
    print(f"Warning: Invalid position info for direct sleep {bad_func_name} at p_call={repr(p_call)}, p_atomic={repr(p_atomic)}")

@script:python depends on find_direct_sleep_alloc_in_atomic@
p_atomic << find_direct_sleep_alloc_in_atomic.p_atomic;
p_call << find_direct_sleep_alloc_in_atomic.p_call;
alloc_func << find_direct_sleep_alloc_in_atomic.alloc_func;
gfp << find_direct_sleep_alloc_in_atomic.gfp;
@@
alloc_func_name = str(alloc_func)
reason_str = get_reason(alloc_func_name)

# Handle p_call and p_atomic as list or tuple or single Location
if isinstance(p_call, (list, tuple)):
    if p_call:
        p_call = p_call[0]
    else:
        p_call = None
if isinstance(p_atomic, (list, tuple)):
    if p_atomic:
        p_atomic = p_atomic[0]
    else:
        p_atomic = None

if p_call and hasattr(p_call, 'file') and hasattr(p_call, 'line') and hasattr(p_call, 'current_element') and p_atomic and hasattr(p_atomic, 'line'):
    coccilib.report.print_report(p_call,
        f"BUG (Direct): Prohibited call to {alloc_func_name}() with {gfp} ({reason_str}) "
        f"inside atomic context started at line {p_atomic.line} "
        f"in function {p_call.current_element}.")
else:
    print(f"Warning: Invalid position info for direct alloc {alloc_func_name} at p_call={repr(p_call)}, p_atomic={repr(p_atomic)}")

// --- PART 2 Collect: Data for Indirect Violations ---

@script:python depends on collect_atomic_callees@
p_atomic << collect_atomic_callees.p_atomic;
p_callee_call << collect_atomic_callees.p_callee_call;
callee_func << collect_atomic_callees.callee_func;
@@
if "ATOMIC_CALLEES" not in globals():
    ATOMIC_CALLEES = {}

# Handle p_callee_call and p_atomic as list or tuple or single Location
if isinstance(p_callee_call, (list, tuple)):
    if p_callee_call:
        p_callee_call = p_callee_call[0]
    else:
        p_callee_call = None
if isinstance(p_atomic, (list, tuple)):
    if p_atomic:
        p_atomic = p_atomic[0]
    else:
        p_atomic = None

if p_callee_call and hasattr(p_callee_call, 'file') and hasattr(p_callee_call, 'line') and hasattr(p_callee_call, 'current_element') and p_atomic and hasattr(p_atomic, 'file') and hasattr(p_atomic, 'line'):
    context_info = (f"{p_callee_call.current_element} at {p_callee_call.file}:{p_callee_call.line} "
                    f"from atomic context at {p_atomic.file}:{p_atomic.line}")
else:
    print(f"Warning: Invalid position info for {callee_func} at p_callee_call={repr(p_callee_call)}, p_atomic={repr(p_atomic)}")
    context_info = f"{callee_func} (unknown location)"

key = str(callee_func)
if key not in ATOMIC_CALLEES:
    ATOMIC_CALLEES[key] = set()
ATOMIC_CALLEES[key].add(context_info)

@script:python depends on collect_potential_sleepers@
p_def << collect_potential_sleepers.p_def;
p_bad_call << collect_potential_sleepers.p_bad_call;
func_def << collect_potential_sleepers.func_def;
bad_func << collect_potential_sleepers.bad_func;
@@
if "POTENTIAL_SLEEPERS" not in globals():
    POTENTIAL_SLEEPERS = {}

bad_func_name = str(bad_func)
reason_str = get_reason(bad_func_name)

# Handle p_bad_call as list or tuple or single Location
if isinstance(p_bad_call, (list, tuple)):
    if p_bad_call:
        p_bad_call = p_bad_call[0]
    else:
        p_bad_call = None

if p_bad_call and hasattr(p_bad_call, 'file') and hasattr(p_bad_call, 'line'):
    sleeper_info = (f"{bad_func_name}() at {p_bad_call.file}:{p_bad_call.line} ({reason_str})")
else:
    print(f"Warning: Invalid position info for sleeper {bad_func_name} at p_bad_call={repr(p_bad_call)}")
    sleeper_info = f"{bad_func_name}() (unknown location) ({reason_str})"

key = str(func_def)
if key not in POTENTIAL_SLEEPERS:
    POTENTIAL_SLEEPERS[key] = set()
POTENTIAL_SLEEPERS[key].add(sleeper_info)

@script:python depends on collect_potential_alloc_sleepers@
p_def << collect_potential_alloc_sleepers.p_def;
p_bad_call << collect_potential_alloc_sleepers.p_bad_call;
func_def << collect_potential_alloc_sleepers.func_def;
alloc_func << collect_potential_alloc_sleepers.alloc_func;
gfp << collect_potential_alloc_sleepers.gfp;
@@
if "POTENTIAL_SLEEPERS" not in globals():
    POTENTIAL_SLEEPERS = {}

alloc_func_name = str(alloc_func)
reason_str = get_reason(alloc_func_name)

# Handle p_bad_call as list or tuple or single Location
if isinstance(p_bad_call, (list, tuple)):
    if p_bad_call:
        p_bad_call = p_bad_call[0]
    else:
        p_bad_call = None

if p_bad_call and hasattr(p_bad_call, 'file') and hasattr(p_bad_call, 'line'):
    sleeper_info = (f"{alloc_func_name}() with {gfp} at {p_bad_call.file}:{p_bad_call.line} ({reason_str})")
else:
    print(f"Warning: Invalid position info for alloc {alloc_func_name} at p_bad_call={repr(p_bad_call)}")
    sleeper_info = f"{alloc_func_name}() with {gfp} (unknown location) ({reason_str})"

key = str(func_def)
if key not in POTENTIAL_SLEEPERS:
    POTENTIAL_SLEEPERS[key] = set()
POTENTIAL_SLEEPERS[key].add(sleeper_info)

@script:python depends on collect_call_graph@
p_def << collect_call_graph.p_def;
caller_func << collect_call_graph.caller_func;
callee_func << collect_call_graph.callee_func;
@@
if "CALL_GRAPH" not in globals():
    CALL_GRAPH = {}

key = str(caller_func)
callee_str = str(callee_func)
if key not in CALL_GRAPH:
    CALL_GRAPH[key] = set()
CALL_GRAPH[key].add(callee_str)

// --- PART 3 Report: Indirect Violations ---

@script:python final@
@@
import collections

def build_call_path(func, reverse_graph, visited=None):
    if visited is None:
        visited = set()
    if func not in reverse_graph or func in visited:
        return []
    visited.add(func)
    paths = []
    for parent in reverse_graph.get(func, set()):
        sub_paths = build_call_path(parent, reverse_graph, visited.copy())
        if sub_paths:
            for path in sub_paths:
                paths.append([func] + path)
        else:
            paths.append([func])
    return paths if paths else [[func]]

if "ATOMIC_CALLEES" in globals() and "POTENTIAL_SLEEPERS" in globals() and "CALL_GRAPH" in globals():
    print("\n--- Inter-procedural Sleep-in-Atomic Analysis ---")
    found_bugs = False

    # Build reverse graph
    REVERSE_GRAPH = {}
    for caller, callees in CALL_GRAPH.items():
        for callee in callees:
            if callee not in REVERSE_GRAPH:
                REVERSE_GRAPH[callee] = set()
            REVERSE_GRAPH[callee].add(caller)

    # Find transitive sleepers
    may_sleep = set()
    transitive_reasons = {}
    queue = collections.deque(POTENTIAL_SLEEPERS.keys())
    visited = set()

    while queue:
        func = queue.popleft()
        if func in visited:
            continue
        visited.add(func)
        may_sleep.add(func)
        if func in POTENTIAL_SLEEPERS:
            transitive_reasons[func] = POTENTIAL_SLEEPERS[func]
        else:
            transitive_reasons[func] = set()
        for parent in REVERSE_GRAPH.get(func, set()):
            if parent not in visited:
                queue.append(parent)
                transitive_reasons[parent] = transitive_reasons.get(parent, set()) | transitive_reasons[func]

    # Find risky functions
    risky_funcs = set(ATOMIC_CALLEES.keys()) & may_sleep
    for func in sorted(risky_funcs):
        found_bugs = True
        print(f"\n[!] BUG (Indirect): Function '{func}' can sleep and is called from an atomic context.")
        print(f"  - Sleep details:")
        for reason in sorted(transitive_reasons.get(func, set())):
            print(f"    - {reason}")
        print(f"  - Atomic call sites:")
        for context in sorted(ATOMIC_CALLEES.get(func, set())):
            print(f"    - {context}")
        # Print call paths
        print(f"  - Call paths to sleepers:")
        for path in build_call_path(func, REVERSE_GRAPH):
            print(f"    - {' -> '.join(path)}")

    if not found_bugs:
        print("No indirect sleep-in-atomic bugs found.")
else:
    print("Error: Required data (ATOMIC_CALLEES, POTENTIAL_SLEEPERS, or CALL_GRAPH) not collected.")
    # Debug globals state
    if "ATOMIC_CALLEES" in globals():
        print(f"Debug: ATOMIC_CALLEES = {ATOMIC_CALLEES}")
    if "POTENTIAL_SLEEPERS" in globals():
        print(f"Debug: POTENTIAL_SLEEPERS = {POTENTIAL_SLEEPERS}")
    if "CALL_GRAPH" in globals():
        print(f"Debug: CALL_GRAPH = {CALL_GRAPH}")