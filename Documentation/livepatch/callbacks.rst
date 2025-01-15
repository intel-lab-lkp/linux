======================
(Un)patching Callbacks
======================

Livepatch (un)patch-callbacks provide a mechanism for livepatch modules
to execute callback functions before and after transitioning the system.
They can be considered a **power feature** that **extends livepatching
abilities** to include:

  - Safe updates to global data

  - "Patches" to init and probe functions

  - Patching otherwise unpatchable code (i.e. assembly)

In most cases, (un)patch callbacks will need to be used in conjunction
with memory barriers and kernel synchronization primitives, like
mutexes/spinlocks, or even stop_machine(), to avoid concurrency issues.


1. Callback types
=================

The pointers to the callbacks are stored in `struct klp_state_callbacks`.
This structure is bundled into `struct klp_state`. The connection with
the state helps to maintain the lifetime of the changes made by the callbacks,
see also Documentation/livepatch/system-state.rst

The `struct klp_state_callbacks` allows to define the following
callbacks. All of them are optional:

*pre_patch()*

  - Called only when the related state is being enabled at the beginning
    of the transition. This is the only callback with a return value.
    The livepatch module won't be loaded when it returns an error code.

*post_patch()*

  - Called only when the related state is being enabled at the end
    of the transition.

*pre_unpatch()*

  - Called only when the related state is being disabled at the beginning
    of the transition.

*post_patch()*

  - Called only when the related state is being disabled at the end
    of the transition.

*shadow_dtor()*

  - Destruct callback which is used for releasing obsolete shadow variables
    using the same *@id*. They are freed right after calling *post_unpatch()*
    callback.


3. How it works
===============

Each callback is optional, omitting one does not preclude specifying any
other.  However, the livepatching core executes the handlers in
symmetry: *pre_patch()* callbacks have a *post_unpatch()* counterpart and
*post_patch()* callbacks have a *pre_unpatch()* counterpart.  An unpatch
callback will only be executed if its corresponding patch callback was
executed. Typical use cases pair a patch handler that acquires and
configures resources with an unpatch handler tears down and releases
those same resources.

A callback is only executed when the related livepatch introduces or
removes the state. Specifically, the *pre_patch()* and *post_patch()*
callbacks are not called if any already enabled livepatch supports
the given state, regardless of whether atomic replacement is used or
livepatches are installed in parallel. Similarly, the *pre_unpatch()*
and *post_unpatch()* callbacks are called during atomic replacement
only for states from currently enabled livepatches that will no longer
be supported by the new livepatch.

The *pre_patch()* callback, if specified, is expected to return a status
code (0 for success, -ERRNO on error).  An error status code indicates
to the livepatching core that the requested state could not be enabled
a safe way and to stop the current patching request. (When no *pre_patch()*
callback is provided, the transition is assumed to be safe.)  If a
*pre_patch()* callback returns failure, the kernel's module loader will
refuse to load the livepatch.

If a patch transition is reversed, no *pre_unpatch()* handlers will be run.
This follows the previously mentioned symmetry -- *pre_unpatch() callbacks
will only occur if their corresponding *post_patch()* callback executed.


4. Expected usage
=================

The expected role of each callback is as follows:

*pre_patch()*

  - Allocate memory, using a shadow variable, when necessary. The allocation
    might fail and *pre_patch()* is the only callback that could stop loading
    of the livepatch.

  - Do any other preparatory action that is needed by the new code even
    before the transition gets finished. For example, initialize
    the allocated memory.

    The system state itself is typically modified in *post_patch()*
    when the entire system is able to handle it.

  - Clean up its own mess in case of error. It might be done by a custom
    code or by calling *post_unpatch()* explicitly.

*post_patch()*

  - Do the actual system state modification. Eventually allow
    the new code to use it.

*pre_unpatch()*

  - Prevent the code, added by the livepatch, relying on the system
    state change.

  - Revert the system state modification..

*post_unpatch()*

  - Remove any not longer needed setting or data. Note that all shadow
    variables using the same *@id* are freed automatically.


4. Use cases
============

Sample livepatch modules demonstrating the callback API can be found in
samples/livepatch/ directory. These samples were modified for use in
kselftests and can be found in the tools/testing/selftests/livepatch/
directory.

Global data update
------------------

A *pre_patch()* callback can be useful to update a global variable.  For
example, commit 75ff39ccc1bd ("tcp: make challenge acks less predictable")
changes a global sysctl, as well as patches the tcp_send_challenge_ack()
function.

In this case, if we're being super paranoid, it might make sense to
patch the data *after* patching is complete with a *post_patch()* callback,
so that tcp_send_challenge_ack() could first be changed to read
sysctl_tcp_challenge_ack_limit with READ_ONCE.

__init and probe function patches support
-----------------------------------------

Although __init and probe functions are not directly livepatch-able, it
may be possible to implement similar updates via *pre_patch()*/*post_patch()*
callbacks.

The commit 48900cb6af42 ("virtio-net: drop NETIF_F_FRAGLIST") change the way
that virtnet_probe() initialized its driver's net_device features.  A
*pre_patch()*/*post_patch()* callback could iterate over all such devices,
making a similar change to their hw_features value.  (Client functions of the
value may need to be updated accordingly.)
