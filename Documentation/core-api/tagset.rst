.. SPDX-License-Identifier: GPL-2.0+

======
Tagset
======

Overview
========

A tagset is a set of strings, intended for resource tagging where
metadata about a resource is represented simply by a name. The public
API can be found in ``<linux/tagset.h>``.


Initialization and Destruction
==============================

A tagset must be initialized before use::

    struct tagset tags;
    tagset_init(&tags);

Before adding tags, allocate capacity::

    if (!tagset_alloc(&tags, num_tags, GFP_KERNEL))
        return -ENOMEM;

When finished, release all resources::

    tagset_destroy(&tags);

This frees all tag strings and the array. The tagset is reinitialized
and may be reused by calling ``tagset_alloc()`` again.


Adding Tags
===========

Two functions are provided for adding tags. Both require that
sufficient capacity has been allocated via ``tagset_alloc()``.
All functions that allocate memory return false on failure; callers
should check return values.

``tagset_add()``
    Adds a tag string that is already in kmalloc'd memory. On success,
    the tagset takes ownership of the string and will free it when
    the tagset is destroyed::

        char *tag = kstrdup("mytag", GFP_KERNEL);
        if (!tag || !tagset_add(&tags, tag))
            kfree(tag);  /* failed, caller must free */

``tagset_add_dup()``
    Duplicates the tag string internally. The caller retains ownership
    of the original string::

        if (!tagset_add_dup(&tags, "mytag", GFP_KERNEL))
            return -ENOMEM;

Both functions return true on success, false on failure. Tags must
not be added after ``tagset_finalize()`` has been called. Use
``GFP_ATOMIC`` when adding tags in atomic context.


Finalizing
==========

After adding all tags, the tagset must be finalized before querying::

    tagset_finalize(&tags);

This sorts the array to enable efficient binary search and removes
any duplicate tags. Calling ``tagset_is_member()`` or
``tagset_intersection()`` on a non-finalized tagset produces
undefined results.


Querying Tags
=============

``tagset_is_empty()``
    Returns true if the tagset contains no tags.

``tagset_count()``
    Returns the number of tags in the tagset. Before finalization,
    this is the number of tags added; after finalization, this is
    the number of unique tags.

``tagset_is_member()``
    Returns true if a tag is present in the tagset. Uses binary
    search for O(log N) complexity::

        if (tagset_is_member(&tags, "mytag"))
            pr_info("tag found\n");

``tagset_intersection()``
    Returns true if two tagsets share at least one common tag.
    Uses merge-style comparison for O(N+M) complexity::

        if (tagset_intersection(&tags1, &tags2))
            pr_info("sets overlap\n");


Iteration
=========

Use ``tagset_for_each()`` to iterate over all tags::

    unsigned int index;
    char *tag;

    tagset_for_each(&tags, index, tag)
        pr_info("tag: %s\n", tag);

Callers should not depend on the order in which tags are returned.
Modifying the tagset during iteration produces undefined behavior.


Copying
=======

``tagset_copy()`` duplicates all tags from one tagset to another::

    struct tagset copy;
    if (!tagset_copy(&copy, &original, GFP_KERNEL))
        pr_err("copy failed\n");

The source tagset should be finalized before copying. The destination
tagset is initialized and ready for queries after this function
returns (no separate ``tagset_finalize()`` call is needed). Each tag
string is duplicated, so the two tagsets are fully independent after
copying.


Typical Usage Pattern
=====================

A typical usage pattern for building a tagset::

    struct tagset tags;

    tagset_init(&tags);
    if (!tagset_alloc(&tags, count, GFP_KERNEL))
        return -ENOMEM;

    for (i = 0; i < count; i++) {
        if (!tagset_add_dup(&tags, strings[i], GFP_KERNEL)) {
            tagset_destroy(&tags);
            return -ENOMEM;
        }
    }
    tagset_finalize(&tags);

    /* Now safe to query */
    if (tagset_is_member(&tags, "target"))
        do_something();

    tagset_destroy(&tags);


Thread Safety
=============

Tagsets have no internal locking. Callers provide synchronization
between writers and readers.

The build-and-finalize phase mutates the tagset; the post-finalize
query phase reads from it. The two phases must be separated by a
publication boundary, because ``tagset_finalize()`` itself carries
no memory barrier. A reader that observes the finalized tagset
before the writer's stores have propagated may see a stale
``ts_count`` or a partially populated ``ts_tags[]`` array.

Three publication patterns are sufficient:

* Lock release after ``tagset_finalize()``, lock acquire before
  each query. The matching unlock/lock pair supplies release and
  acquire ordering.

* ``rcu_assign_pointer()`` of the tagset pointer after
  ``tagset_finalize()``, paired with ``rcu_dereference()`` inside
  an RCU read-side critical section on the reader.

* ``smp_store_release()`` of the tagset pointer after
  ``tagset_finalize()``, paired with ``smp_load_acquire()`` on the
  reader.

Once published, the tagset must remain immutable until no further
readers can observe it. ``tagset_destroy()`` is not safe against
concurrent readers, and ``tagset_finalize()`` must not be called
more than once. With RCU publication, callers typically defer
destruction to a grace period (``synchronize_rcu()`` before
``tagset_destroy()``, or ``tagset_destroy()`` from a
``call_rcu()`` callback) so that in-flight readers drain before
the storage is freed.


Implementation
==============

Each tagset is rooted on the following structure::

    struct tagset {
            char         **ts_tags;
            unsigned int   ts_count;
            unsigned int   ts_capacity;
            bool           ts_finalized;
    };

The implementation uses a sorted array of string pointers, providing
O(log N) membership testing and O(N+M) intersection operations.

+------------------------+-------------+
| Operation              | Complexity  |
+========================+=============+
| tagset_add             | O(1)        |
+------------------------+-------------+
| tagset_finalize        | O(N log N)  |
+------------------------+-------------+
| tagset_is_member       | O(log N)    |
+------------------------+-------------+
| tagset_intersection    | O(N + M)    |
+------------------------+-------------+
| tagset_copy            | O(N)        |
+------------------------+-------------+
