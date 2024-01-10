============================
AppArmor Refcount Management
============================

Introduction
============

AppArmor task confinement is based on task profiles. Task profiles
specify the access rules - list of files which the task is allowed
to open/read/write, socket bind, mount, signal, ptrace and other
capabilities of the task.

A sample raw task profile (typically present in /etc/apparmor.d/)
would look like this:

::

    1. profile test_app /usr/bin/test_app {
    2.    @{sys}/devices/**                 r,
    3.    /var/log/testapp/access.log       w,
    4.    /var/log/testapp/error.log        w,
    5.
    6.    /lib/ld-*.so*                     mrix,
    7.
    8.    ^hat {
    9.        /dev/pts/*     rw,
    10.    }
    11.
    12.    change_profile -> restricted_access_profile,
    13.
    14.    /usr/*bash cx -> local_profile,
    15.
    16.    profile local_profile {
    17.        ...
    18.    }
    19. }


Above example shows a sample profile. A quick description of
each line is given below:

1  Defines a profile with name ``test_app`` and attachment specification
   ``/usr/bin/test_app``. The attachment specification is used for
   associating the application with a profile, during launch.

2,3, 4
   Specifies read and write access to various paths.

6  Read access for the so and inherit profile transition specification.

8  Hat profile. Used for running a portion of the program with different
   permissions, compared to the other portions of the program. For example,
   to run unauthenticated traffic and authenticated traffic in separate
   profiles in OpenSSH; running user supplied CGI scripts in separate
   profile in Apache.

12 Change profile rules, to switch child process to a profile, different
   from the parent process, on exec.

14 Profile transition for processes started from the current procees. For
   example, transition to a different profile for ``ls``, which is invoked
   from a shell program.


Objects and their Refcount Management
=====================================

There are various object resources within AppArmor

- Namespaces

  There is a root namespace associated for apparmorfs. This is the default
  namespace, to which all profiles are associated with.

  Profiles can be associated with a different namespaces (for chroot,
  containers env).

  Namespaces are represented using ``struct aa_ns``. Some of the relevant
  fields are::

      struct aa_policy base
      struct aa_profile *unconfined
      struct list_head sub_ns
      struct aa_ns *parent

  ``struct aa_policy`` contains a list of profiles associated with this ns.

  ``unconfined`` profile manages refcount for this namespace. It is also
  used as the default profile for tasks in this namespace and a proxy label,
  when profiles are removed.

  ``sub_ns`` is the list of child namespaces.

  ``parent`` Parent namespace, for this namespace.

  A parent and its child sub namespaces keep reference to each other::

    +---------------------+
    |                     |
    |      root_ns        |
    |                     |
    +---------------------+
            ^  /  \    ^
           /  /    \    \
   parent /  /      \    \ parent
         /  / sub_ns \    \
        /  /          \    \
       /  /            \    \
      /  v              v    \
   +-----------+    +-----------+
   |           |    |           |
   |    ns1    |    |    ns2    |
   |           |    |           |
   +-----------+    +-----------+

  Here, ``root_ns`` is the root apparmor namespace. It maintains a
  reference to all child namespace which are present in ``->sub_ns``.
  The child namespaces ``ns1``, ``ns2`` keep a reference to their
  ``parent``, which is the ``root_ns``.


- Profiles

  Profiles are represented as ``struct aa_profile``

  Some of the fields of interest are::

      struct aa_policy base
      struct aa_profile __rcu *parent
      struct aa_ns *ns
      struct aa_loaddata *rawdata
      struct aa_label label

   ``base`` - Maintains the list of child subprofiles - hats

   ``parent`` - If subprofile, pointer to the parent profile

   ``ns`` - Parent namespace

   ``rawdata`` - Used for matching profile data, for profile updates

   ``label`` - Refcount object

  A profile keeps a reference to the namespace it is associated with.
  In addition, there is a reference kept for all profiles in
  ``base.profiles`` of a namespace::

      +-----------------------------+
      |                             |
      |      root_ns                |
      |                             |
      +-----------------------------+
                ^  /       ^   |
               /  /     ns |   |
       parent /  /         |   |
             /  / sub_ns   |   |base.profiles
            /  /           |   |
           /  /            |   |
          /  v             |   v
       +-----------+    +-----------+
       |           |    |           |
       |    ns1    |    |     A     |
       |           |    |           |
       +-----------+    +-----------+
      base     |    ^
      .profiles|    | parent
               v    |
       +-----------+
       |           |
       |    P      |
       |           |
       +-----------+

  For subprofiles, a refcount is kept for the ``->parent`` profile.
  For each child in ``->base.profiles``, a refcount is kept::

            +--------------+
            |              |
            |    root_ns   |
            |              |
            +-------^------+
       base.    |   |
       profiles v   |ns
             +---------------+
             |               |
            ^|      A        |^
   parent  / |               | \parent
          /  +---------------+  \
         /    /  base.profiles\  \
        /    /                 v  \
     +------v-+               +----\---+
     |        |               |        |
     |  B     |               |    C   |
     |        |               |        |
     +--------+               +--------+


- Labels

  Label manages refcount for the ``aa_profile`` objects. It is
  represented as ``struct aa_label``. Some of the fields are::

      struct kref count
      struct aa_proxy *proxy
      long flags
      int size
      struct aa_profile *vec[]

  ``count`` - Refcounting for the enclosing object.
  ``proxy`` - Redirection of stale profiles
  ``flags`` - state (STALE), type (NS, PROFILE)
  ``vec``   - if ``size`` > 1, for compound labels (for stacked profiles)


  For compound/stack labels, there is a reference kept, for all
  the stack profiles::

   +--------+     +---------+      +-------+
   |   A    |     |   B     |      |  C    |
   |        |     |         |      |       |
   +-----^--+     +---------+      +-------+
      ^  \            ^           ^
       \  \           |           |
        \  \  +---------------+   |
         \  \ |    A//&:ns1:B |   |
          \  \|               |   |
           \  +---------------+   |
            \                     |
             \                    |
              \ +-------------------+
               \|A//&:ns1:B//&:ns2:C|
                |                   |
                +-------------------+

- Proxy

  A proxy is associated with a label, and is used for redirecting
  running tasks to new profile on profile change. Proxies are
  represented as ``struct aa_proxy``::

    struct aa_label __rcu *label

  ``label`` - Redirect to the latest profile label.

  While a label is not stale, its proxy points to the same label.
  On profile updates, the proxy, the label is marked as stale,
  and label is redirected to the new profile label::

   +------------+          +-----------+
   |            |          |           |
   |   old      | -------->|    P1     |
   |            | <--------|           |
   +------------+          +-----------+


   +------------+         +------------+
   |            |         |            |
   |    old     |-------->|    P1      |
   |            |         |            |
   +------------+         +--^---------+
                             |  |
   +------------+            |  |
   |            |-----------/   |
   |    new     |<-------------/
   |            |
   +------------+

Lifecycle of the Apparmor Kernel Objects
========================================

#. Init

  #. During AppArmor init, root ns (RNS:1) and its unconfined
     profile are created (RNS:1). If initialization completes
     successfully, the ``root_ns`` initial ref is never destroyed
     (?).

  #. Usespace init scripts load the current set of defined profiles
     into kernel, typically through ``apparmor_parser`` command.

     The loaded profiles have an initial refcount (P1:1 , P2:1).
     A profile P1, which is in default namespace keeps a reference
     to root ns (RNS:2). If a profile P2 is in a different namespace,
     NS1, that namespace object is allocated (NS1:1) and the namespace
     is added to ``sub_ns`` of ``root_ns`` (NS1:2). The child namespace
     NS1 keeps a reference to parent ``root_ns`` (RNS:3). P2 keeps a
     reference to NS1 (NS1:2). The root ns keeps a reference to P1 in
     ``->profiles`` list (P1:2). NS1 keeps a reference to P2 in its
     ``->profiles`` list (P2:2). In addition, label proxies keep
     reference to P1 and P2 (P1:3, P2:3).

#. Runtime

  #. As part of the bprm cred updates (``apparmor_bprm_creds_for_exec()``),
     the current task T1 is attached to a profile (P1), based on the best
     attachment match rule. T1 keeps a refcount for P1, while the current
     ``cred`` is active (P1:4).

  #. If P1 is replaced with a new profile P3, P1 is removed from the root
     ns profiles list (P1:3), proxy is redirected to P3 (P1:2), and the
     initial label is released (P1:1) and P1's label is marked stale.

  #. Any T1 accesses, which have a apparmor hook, would reference the
     current task's cred label::

         __begin_current_label_crit_section()
             struct aa_label *label = cred_label(cred);

             if (label_is_stale(label))
                 label = aa_get_newest_label(label);

             return label;

         aa_get_newest_label(struct aa_label __rcu **l)
             return aa_get_label_rcu(&l->proxy->label);

         aa_get_label_rcu(struct aa_label __rcu **l)
             rcu_read_lock();
             do {
                c = rcu_dereference(*l);
             } while (c && !kref_get_unless_zero(&c->count));
             rcu_read_unlock();

  #. On task exit and cref freeing, the last reference for P1 is
     released (P1:0).

#. Release

  Below is the set of release operations, based on the label's
  parent object type.

  #. If ns is not assigned (early init error exit), do not wait for
     RCU grace period. Otherwise use ``call_rcu()``

  #. If label is associated with a namespace (unconfined label)
      #. Drop Parent ns reference.

  #. If label is associated with a profile
      #. Drop parent profile reference.
      #. Drop ns reference.

  #. Drop all vector profile references for stacked profiles.


Links
=====

Userspace tool - https://gitlab.com/apparmor/apparmor
    Profile syntax      - parser/apparmor.d.pod
    Sample change hats  - changehat/
    Other documentation - libraries/libapparmor/doc
