====================
System State Changes
====================

Livepatches provide a way to update running systems without requiring a reboot.
However, managing compatibility between multiple livepatches can be challenging,
especially when they introduce changes that affect system behavior or memory
management.

Cumulative livepatches simplify this process by completely replacing older
versions with each update. This allows for the addition, modification, and
removal of fixes while maintaining compatibility through atomic replacement.
However, challenges can arise with callbacks and shadow variables.

Callbacks are functions that can alter system behavior when a livepatch is
applied. Shadow variables associate additional memory with existing data
structures. These modifications need to be reverted when a livepatch is
disabled or replaced with a livepatch not supporting the same state to ensure
system stability.

Unused shadow variables can lead to memory leaks and synchronization issues.
If a livepatch is replaced with one that doesn't maintain these variables,
their content may become outdated, potentially causing problems if a future
livepatch attempts to use them again.

To address these challenges, the livepatch system employs state tracking.
This mechanism offers several benefits:

  - Callbacks associated with a specific state are called only when that state
    is introduced or removed.

  - Shadow variables associated with a state are automatically freed when that
    state is no longer supported.

  - When a livepatch is atomically replaced with another supporting the same
    state, associated callbacks are not called, and shadow variables are not
    freed, ensuring continuity.

  - State tracking can prevent disabling a livepatch or proceeding with
    an atomic replacement if the current livepatch cannot revert the state.
    This safeguard is crucial when reverting modifications would be too complex
    or risky.

This approach ensures that changes introduced by livepatches are managed
effectively, minimizing the risk of conflicts and maintaining system stability.


1. Livepatch system state API
=============================

Any livepatch might support an arbitrary number of states. A particular state
represents either a change made by the associated callbacks and/or shadow
variables using the same *@id*.

The states are described by an array of `struct klp_state`, which is usually
statically defined. The `struct klp_state` is defined in
`include/linux/livepatch.h` and provides the following fields:

*id*

  - A unique, non-zero number that identifies the state.

*is_shadow*

  - A boolean value indicating whether the state is associated with a shadow
    variable using the same *@id*. These are automatically freed when
    the state is no longer supported after the livepatch transition.
    See also Documentation/livepatch/shadow-vars.rst.

*block_disable*

  - A boolean value that, when set, prevents transitions that would disable
    the state. In other words, it indicates that reverting the state is
    not supported.

*callbacks*

  - A `struct klp_state_callbacks` containing (optional) pointers to
    callbacks. These are invoked when a livepatch transition introduces
    or removes the state. See Documentation/livepatch/callbacks.rst
    for more information.


2. Livepatch compatibility
==========================

The *@block_disable* state flag is used when a livepatch modifies the system
state in a way that cannot be easily or safely reverted. This might be due
to the complexity of the changes or the risk of instability during
the reversion process.

Preventing the disable operation can also be a strategic decision to save
development costs, as implementing and testing the *pre_unpatch()* and
*post_unpatch()* callbacks can significantly increase resource requirements.

This flag prevents the livepatch from being disabled and also prevents atomic
replacement with a livepatch that does not support this state. These
livepatches are considered incompatible.

The kernel provides no mechanism for detecting incompatibility when atomic
replacement is not used. Livepatch authors must manage incompatibility in
other ways, such as through dependencies between the packages that install
the livepatch modules.
