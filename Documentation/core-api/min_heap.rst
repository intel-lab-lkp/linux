.. SPDX-License-Identifier: GPL-2.0

============
Min Heap API
============

Introduction
============

The Min Heap API provides a set of functions and macros for managing min-heaps in the Linux kernel.
A min-heap is a binary tree structure where the value of each node is less than or equal to the
values of its children, ensuring that the smallest element is always at the root.

This API supports efficient insertion, deletion, and access to the minimum element. It is optimized
for use in systems with performance constraints and is suitable for scenarios where the minimum
element needs to be accessed or updated frequently.

This document provides a guide to the Min Heap API, detailing how to define and use min-heaps.
Please note that users should not directly call functions with **__min_heap_*()** names, but should
instead use the provided macro wrappers.

In addition to the standard version of the functions, the API also includes a set of inline
versions for performance-critical scenarios. These inline functions have the same names as their
non-inline counterparts but include an **_inline** suffix. For example, **__min_heap_init_inline**
and its corresponding macro wrapper **min_heap_init_inline**. As with the non-inline versions, it
is important to use the macro wrappers for inline functions instead of directly calling the
functions themselves.

Data Structures
===============

Min-Heap Definition
-------------------

The core data structure for representing a min-heap is defined using the **MIN_HEAP_PREALLOCATED**
and **DEFINE_MIN_HEAP** macros. These macros allow you to define a min-heap with a preallocated
buffer or dynamically allocated memory.

Example:

.. code-block:: c

    #define MIN_HEAP_PREALLOCATED(_type, _name, _nr)
    struct _name {
        int nr;         /* Number of elements in the heap */
        int size;       /* Maximum number of elements that can be held */
        _type *data;    /* Pointer to the heap data */
        _type preallocated[_nr];  /* Static preallocated array */
    }

    #define DEFINE_MIN_HEAP(_type, _name) MIN_HEAP_PREALLOCATED(_type, _name, 0)

A typical heap structure will include a counter for the number of elements (`nr`), the maximum
capacity of the heap (`size`), and a pointer to an array of elements (`data`). Optionally, you can
specify a static array for preallocated heap storage using **MIN_HEAP_PREALLOCATED**.

Min Heap Callbacks
------------------

The **struct min_heap_callbacks** provides customization options for ordering
elements in the heap and swapping them. It contains two function pointers:

.. code-block:: c

    struct min_heap_callbacks {
        bool (*less)(const void *lhs, const void *rhs, void *args);
        void (*swp)(void *lhs, void *rhs, void *args);
    };

- **less** is the comparison function used to establish the order of elements.
- **swp** is a function for swapping elements in the heap.

Macro Wrappers
==============

The following macro wrappers are provided for interacting with the heap in a user-friendly manner.
Each macro corresponds to a function that operates on the heap, and they abstract away direct calls
to internal functions.

Each macro accepts various parameters that are detailed below.

Heap Initialization
--------------------

.. code-block:: c

    min_heap_init(heap, data, size);

- **heap**: A pointer to the min-heap structure to be initialized.
- **data**: A pointer to the buffer where the heap elements will be stored. If `NULL`, the preallocated buffer within the heap structure will be used.
- **size**: The maximum number of elements the heap can hold.

This macro initializes the heap, setting its initial state. If `data` is `NULL`, the preallocated
memory inside the heap structure will be used for storage. Otherwise, the user-provided buffer is
used. The operation is **O(1)**.

**Inline Version:** min_heap_init_inline(heap, data, size)

Accessing the Top Element
-------------------------

.. code-block:: c

    element = min_heap_peek(heap);

- **heap**: A pointer to the min-heap from which to retrieve the smallest element.

This macro returns a pointer to the smallest element (the root) of the heap, or `NULL` if the heap
is empty. The operation is **O(1)**.

**Inline Version:** min_heap_peek_inline(heap)

Heap Insertion
--------------

.. code-block:: c

    success = min_heap_push(heap, element, callbacks, args);

- **heap**: A pointer to the min-heap into which the element should be inserted.
- **element**: A pointer to the element to be inserted into the heap.
- **callbacks**: A pointer to a `struct min_heap_callbacks` providing the `less` and `swp` functions.
- **args**: Optional arguments passed to the `less` and `swp` functions.

This macro inserts an element into the heap. It returns `true` if the insertion was successful and
`false` if the heap is full. The operation is **O(log n)**.

**Inline Version:** min_heap_push_inline(heap, element, callbacks, args)

Heap Removal
------------

.. code-block:: c

    success = min_heap_pop(heap, callbacks, args);

- **heap**: A pointer to the min-heap from which to remove the smallest element.
- **callbacks**: A pointer to a `struct min_heap_callbacks` providing the `less` and `swp` functions.
- **args**: Optional arguments passed to the `less` and `swp` functions.

This macro removes the smallest element (the root) from the heap. It returns `true` if the element
was successfully removed, or `false` if the heap is empty. The operation is **O(log n)**.

**Inline Version:** min_heap_pop_inline(heap, callbacks, args)

Heap Maintenance
----------------

You can use the following macros to maintain the heap's structure:

.. code-block:: c

    min_heap_sift_down(heap, pos, callbacks, args);

- **heap**: A pointer to the min-heap.
- **pos**: The index from which to start sifting down.
- **callbacks**: A pointer to a `struct min_heap_callbacks` providing the `less` and `swp` functions.
- **args**: Optional arguments passed to the `less` and `swp` functions.

This macro restores the heap property by moving the element at the specified index (`pos`) down the
heap until it is in the correct position. The operation is **O(log n)**.

**Inline Version:** min_heap_sift_down_inline(heap, pos, callbacks, args)

.. code-block:: c

    min_heap_sift_up(heap, idx, callbacks, args);

- **heap**: A pointer to the min-heap.
- **idx**: The index of the element to sift up.
- **callbacks**: A pointer to a `struct min_heap_callbacks` providing the `less` and `swp` functions.
- **args**: Optional arguments passed to the `less` and `swp` functions.

This macro restores the heap property by moving the element at the specified index (`idx`) up the
heap. The operation is **O(log n)**.

**Inline Version:** min_heap_sift_up_inline(heap, idx, callbacks, args)

.. code-block:: c

    min_heapify_all(heap, callbacks, args);

- **heap**: A pointer to the min-heap.
- **callbacks**: A pointer to a `struct min_heap_callbacks` providing the `less` and `swp` functions.
- **args**: Optional arguments passed to the `less` and `swp` functions.

This macro ensures that the entire heap satisfies the heap property. It is called when the heap is
built from scratch or after many modifications. The  operation is **O(n)**.

**Inline Version:** min_heapify_all_inline(heap, callbacks, args)

Removing Specific Elements
--------------------------

.. code-block:: c

    success = min_heap_del(heap, idx, callbacks, args);

- **heap**: A pointer to the min-heap.
- **idx**: The index of the element to delete.
- **callbacks**: A pointer to a `struct min_heap_callbacks` providing the `less` and `swp` functions.
- **args**: Optional arguments passed to the `less` and `swp` functions.

This macro removes an element at the specified index (`idx`) from the heap and restores the heap
property. The operation is **O(log n)**.

**Inline Version:** min_heap_del_inline(heap, idx, callbacks, args)

Other Utilities
===============

- **min_heap_full(heap)**: Checks whether the heap is full. Complexity: **O(1)**.

.. code-block:: c

    bool full = min_heap_full(heap);

- `heap`: A pointer to the min-heap to check.

This macro returns `true` if the heap is full, otherwise `false`.

**Inline Version:** min_heap_full_inline(heap)

- **min_heap_empty(heap)**: Checks whether the heap is empty. Complexity: **O(1)**.

.. code-block:: c

    bool empty = min_heap_empty(heap);

- `heap`: A pointer to the min-heap to check.

This macro returns `true` if the heap is empty, otherwise `false`.

**Inline Version:** min_heap_empty_inline(heap)

Example Usage
=============

An example usage of the min-heap API would involve defining a heap structure,
initializing it, and inserting and removing elements as needed.

.. code-block:: c

    /* Define a preallocated heap for storing up to 10 elements */
    MIN_HEAP_PREALLOCATED(int, my_heap, 10);

    struct min_heap_callbacks callbacks = {
        .less = my_less,  /* Custom comparison function */
        .swp = my_swap,   /* Custom swap function */
    };

    /* Initialize the heap using the preallocated buffer */
    min_heap_init(&my_heap, NULL, ARRAY_SIZE(my_heap.preallocated));

    /* If we have an external buffer, we can use it instead */
    int external_buffer[20];
    min_heap_init(&my_heap, external_buffer, ARRAY_SIZE(external_buffer));

    /* Insert elements into the heap */
    int new_element = 5;
    if (!min_heap_full(&my_heap)) {
        min_heap_push(&my_heap, &new_element, &callbacks, NULL);
    }

    /* Peek at the minimum element (without removing it) */
    int *min_element = min_heap_peek(&my_heap);

    /* Replace the root of the heap with a new element */
    int replacement_element = 3;
    min_heap_pop_push(&my_heap, &replacement_element, &callbacks, NULL);

    /* Reorder the heap by sifting down from a given position */
    min_heap_sift_down(&my_heap, 0, &callbacks, NULL);

    /* Remove the minimum element from the heap */
    if (!min_heap_empty(&my_heap)) {
        min_heap_pop(&my_heap, &callbacks, NULL);
    }

    /* Insert more elements into the heap */
    new_element = 8;
    if (!min_heap_full(&my_heap)) {
        min_heap_push(&my_heap, &new_element, &callbacks, NULL);
    }

    /* Delete an element from the heap at a specific index */
    int idx_to_delete = 2;
    min_heap_del(&my_heap, idx_to_delete, &callbacks, NULL);

    /* Ensure the entire heap maintains heap order */
    min_heapify_all(&my_heap, &callbacks, NULL);
