.. SPDX-License-Identifier: GPL-2.0+

=====================
Linked Lists in Linux
=====================

:Author: Nicolas Frattaroli <nicolas.frattaroli@collabora.com>

.. contents::

Introduction
============

Linked lists are one of the most basic data structures used in many programs.
The Linux kernel implements several different flavours of linked lists. The
purpose of this document is not to explain linked lists in general, but to show
new kernel developers how to use the Linux kernel implementations of linked
lists.

Please note that while linked lists certainly are ubiquitous, they may not
always be the best data structure to use in cases where a simple array doesn't
already suffice. Familiarizing oneself with other in-kernel generic data
structures, especially for concurrent accesses, is highly encouraged.

Linux implementation of doubly linked lists
===========================================

Linux's linked list implementations can be used by including the header file
``<linux/list.h>``.

The doubly-linked list will likely be the most familiar to many readers. It's a
list that can efficiently be traversed forwards and backwards.

The Linux kernel's doubly-linked list is circular in nature. This means that to
get from the head node to the tail, we can just travel one edge backwards.
Similarly, to get from the tail node to the head, we can simply travel forwards
"beyond" the tail and arrive back at the head.

Declaring a node
----------------

A node in a doubly-linked list is declared by adding a ``struct list_head``
member to the struct you wish to be contained in the list:

.. code-block:: c

  struct clown {
          unsigned long long shoe_size;
          const char *name;
          struct list_head node;  /* the aforementioned member */
  };

This may be an unfamiliar approach to some, as the classical explanation of a
linked list is a list node struct with pointers to the previous and next list
node, as well the payload data. Linux chooses this approach because it allows
for generic list modification code regardless of what data struct is contained
within the list. Since the ``struct list_head`` member is not a pointer but part
of the struct proper, the ``container_of`` pattern can be used by the list
implementation to access the payload data regardless of its type, while staying
oblivious to what said type actually is.

Declaring and initializing a list
---------------------------------

A doubly-linked list can then be declared as just another ``struct list_head``,
and initialised with the LIST_HEAD_INIT() macro during initial assignment, or
with the INIT_LIST_HEAD() function later:

.. code-block:: c

  struct clown_car {
          int tyre_pressure[4];
          struct list_head clowns;        /* Looks like a node! */
  };

  /* ... Somewhere later in our driver ... */

  static int circus_init(struct circus_priv *circus)
  {
          struct clown_car other_car = {
                .tyre_pressure = {10, 12, 11, 9},
                .clowns = LIST_HEAD_INIT(other_car.clowns)
          };

          circus->car.clowns = INIT_LIST_HEAD(&circus->car.clowns);

          return 0;
  }

A further point of confusion to some may be that the list itself doesn't really
have its own type. The concept of the entire linked list and a
``struct list_head`` member that points to other entries in the list are one and
the same.

Adding nodes to the list
------------------------

Adding a node to the linked list is done through the list_add() function.

We'll return to our clown car example to illustrate how nodes get added to the
list:

.. code-block:: c

  static int circus_fill_car(struct circus_priv *circus)
  {
          struct clown_car *car = &circus->car;
          struct clown *grock;
          struct clown *dimitri;

          /* State 1 */

          grock = kzalloc(sizeof(*grock), GFP_KERNEL);
          if (!grock)
                  return -ENOMEM;
          grock->name = "Grock";
          grock->shoe_size = 1000;

          /* Note that we're adding the "node" member */
          list_add(&grock->node, &car->clowns);

          /* State 2 */

          dimitri = kzalloc(sizeof(*dimitri), GFP_KERNEL);
          if (!dimitri)
                  return -ENOMEM;
          dimitri->name = "Dimitri";
          dimitri->shoe_size = 50;

          list_add(&dimitri->node, &car->clowns);

          /* State 3 */

          return 0;
  }

In State 1, our list of clowns is still empty:

.. kernel-render:: DOT
   :alt: Visualization of clowns list in State 1
   :caption: clowns list in State 1, grey dashed lines are "prev" edges

   digraph c {
    rankdir="LR";
    node [shape=box];
    "clowns" -> "clowns";
    "clowns" -> "clowns" [color=gray, style=dashed, constraint=false, arrowsize=0.5];
   }

In State 2, we've added Grock after the list head:

.. kernel-render:: DOT
   :alt: Visualization of clowns list in State 2
   :caption: clowns list in State 2, grey dashed lines are "prev" edges

   digraph c {
    rankdir="LR";
    node [shape=box];
    "clowns" -> "Grock";
    "Grock" -> "clowns" [constraint=false];
    "clowns" -> "Grock" -> "clowns" [color=gray, style=dashed, constraint=false,
                                     arrowsize=0.5];
   }

In State 3, we've added Dimitri after the list head, resulting in the following:

.. kernel-render:: DOT
   :alt: Visualization of clowns list in State 3
   :caption: clowns list in State 3, grey dashed lines are "prev" edges

   digraph c {
    rankdir="LR";
    node [shape=box];
    "clowns" -> "Dimitri" -> "Grock";
    "Grock" -> "clowns" [constraint=false];
    "clowns" -> "Grock" -> "Dimitri" -> "clowns" [color=gray, style=dashed,
                                                  constraint=false, arrowsize=0.5];
   }

If we wanted to have Dimitri inserted at the end of the list instead, we'd use
list_add_tail(). Our code would then look like this:

.. code-block:: c

  static int circus_fill_car(struct circus_priv *circus)
  {
          /* ... */

          list_add_tail(&dimitri->node, &car->clowns);

          /* State 3b */

          return 0;
  }

This results in the following list:

.. kernel-render:: DOT
   :alt: Visualization of clowns list in State 3b
   :caption: clowns list in State 3b, grey dashed lines are "prev" edges

   digraph c {
    rankdir="LR";
    node [shape=box];
    "clowns" -> "Grock" -> "Dimitri";
    "Dimitri" -> "clowns" [constraint=false];
    "clowns" -> "Dimitri" -> "Grock" -> "clowns" [color=gray, style=dashed,
                                                  constraint=false, arrowsize=0.5];
   }

Traversing the list
-------------------

To iterate the list, we can loop through all nodes within the list with
list_for_each().

In our clown example, this results in the following somewhat awkward code:

.. code-block:: c

  static unsigned long long circus_get_max_shoe_size(struct circus_priv *circus)
  {
          unsigned long long res = 0;
          struct clown *e;
          struct list_head *cur;

          list_for_each(cur, &circus->car.clowns) {
                  e = list_entry(cur, struct clown, node);
                  if (e->shoe_size > res)
                          res = e->shoe_size;
          }

          return res;
  }

Note how the additional ``list_entry`` call is a little awkward here. It's only
there because we're iterating through the ``node`` members, but we really want
to iterate through the payload, i.e. the ``struct clown`` that contains each
node's ``struct list_head``. For this reason, there is a second macro:
list_for_each_entry()

Using it would change our code to something like this:

.. code-block:: c

  static unsigned long long circus_get_max_shoe_size(struct circus_priv *circus)
  {
          unsigned long long res = 0;
          struct clown *e;

          list_for_each_entry(e, &circus->car.clowns, node) {
                  if (e->shoe_size > res)
                          res = e->shoe_size;
          }

          return res;
  }

This eliminates the need for the ``list_entry`` step, and our loop cursor is now
of the type of our payload. The macro is given the member name that corresponds
to the list's ``struct list_head`` within the clown struct so that it can still
walk the list.

Removing nodes from the list
----------------------------

The list_del() function can be used to remove entries from the list. It not only
removes the given entry from the list, but poisons the entry's ``prev`` and
``next`` pointers, so that unintended use of the entry after removal does not
go unnoticed.

We can extend our previous example to remove one of the entries:

.. code-block:: c

  static int circus_fill_car(struct circus_priv *circus)
  {
          /* ... */

          list_add(&dimitri->node, &car->clowns);

          /* State 3 */

          list_del(&dimitri->node);

          /* State 4 */

          return 0;
  }

The result of this would be this:

.. kernel-render:: DOT
   :alt: Visualization of clowns list in State 4
   :caption: clowns list in State 4, grey dashed lines are "prev" edges. Note
             how the Dimitri node no longer points to itself.

   digraph c {
    rankdir="LR";
    node [shape=box];
    "Dimitri";
    "clowns" -> "Grock";
    "Grock" -> "clowns" [constraint=false];
    "clowns" -> "Grock" -> "clowns" [color=gray, style=dashed, constraint=false,
                                     arrowsize=0.5];
   }

If we wanted to reinitialize the removed node instead to make it point at itself
again like an empty list head, we can use list_del_init() instead:

.. code-block:: c

  static int circus_fill_car(struct circus_priv *circus)
  {
          /* ... */

          list_add(&dimitri->node, &car->clowns);

          /* State 3 */

          list_del_init(&dimitri->node);

          /* State 4b */

          return 0;
  }

This results in the deleted node pointing to itself again:

.. kernel-render:: DOT
   :alt: Visualization of clowns list in State 4b
   :caption: clowns list in State 4b, grey dashed lines are "prev" edges. Note
             how the Dimitri node points to itself again.

   digraph c {
    rankdir="LR";
    node [shape=box];
    "Dimitri" -> "Dimitri";
    "Dimitri" -> "Dimitri" [color=gray, style=dashed, constraint=false, arrowsize=0.5];
    "clowns" -> "Grock";
    "Grock" -> "clowns" [constraint=false];
    "clowns" -> "Grock" -> "clowns" [color=gray, style=dashed, constraint=false,
                                     arrowsize=0.5];
   }

Traversing whilst removing nodes
--------------------------------

Deleting entries while we're traversing the list will cause problems if we use
list_for_each() and list_for_each_entry(), as deleting the current entry would
modify the ``next`` pointer of it, which means the traversal can't properly
advance to the next list entry.

There is a solution to this however: list_for_each_safe() and
list_for_each_entry_safe(). These take an additional parameter of a pointer to
a ``struct list_head`` to use as temporary storage for the next entry during,
iteration, solving the issue.

An example of how to use it:

.. code-block:: c

  static void circus_eject_insufficient_clowns(struct circus_priv *circus)
  {
          struct clown *e;
          struct clown *n;      /* temporary storage for safe iteration */

          list_for_each_entry_safe(e, n, &circus->car.clowns, node) {
                if (e->shoe_size < 500)
                        list_del(&e->node);
          }
  }

Proper memory management (i.e. freeing the deleted node while making sure
nothing still references it) in this case is left up as an exercise to the
reader.

Concurrency considerations
--------------------------

Concurrent access and modification of a list needs to be protected with a lock
in most cases. Alternatively and preferably, one may use the RCU primitives for
lists in read-mostly use-cases, where read accesses to the list are common but
modifications to the list less so. See Documentation/RCU/listRCU.rst for more
details.

Further reading
---------------

* `How does the kernel implements Linked Lists? - KernelNewbies <https://kernelnewbies.org/FAQ/LinkedLists>`_
