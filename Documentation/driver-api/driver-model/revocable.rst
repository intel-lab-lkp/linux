.. SPDX-License-Identifier: GPL-2.0

==============================
Revocable Resource Management
==============================

Overview
========

.. kernel-doc:: drivers/base/revocable.c
   :doc: Overview

Revocable vs. Devres (devm)
===========================

Revocable and Devres address different problems in resource management:

*   **Devres:** Primarily addresses **resource leaks**.  The lifetime of the
    resources is tied to the lifetime of the device.  The resource is
    automatically freed when the device is unbound.  This cleanup happens
    irrespective of any potential active users.

*   **Revocable:** Primarily addresses **invalid memory access**,
    such as Use-After-Free (UAF).  It's an independent synchronization
    primitive that decouples consumer access from the resource's actual
    presence.  Consumers interact with a "revocable object" (an intermediary),
    not the underlying resource directly.  This revocable object persists as
    long as there are active references to it from consumer handles.

**Key Distinctions & How They Complement Each Other:**

1.  **Reference Target:** Consumers hold a reference to the *revocable object*,
    not the encapsulated resource itself.

2.  **Resource Lifetime vs. Access:** The underlying resource's lifetime is
    independent of the number of references to the revocable object.  The
    resource can be freed at any point.  A common scenario is the resource
    being freed by `devres` when the providing device is unbound.

3.  **Safe Access:** Revocable provides a safe way to attempt access.  Before
    using the resource, a consumer uses the Revocable API (e.g.,
    revocable_try_access()).  This function checks if the resource is still
    valid.  It returns a pointer to the resource only if it hasn't been
    revoked; otherwise, it returns NULL.  This prevents UAF by providing a
    clear signal that the resource is gone.

4.  **Complementary Usage:** `devres` and Revocable work well together.
    `devres` can handle the automatic allocation and deallocation of a
    resource tied to a device.  The Revocable mechanism can be layered on top
    to provide safe access for consumers whose lifetimes might extend beyond
    the provider device's lifetime.  For instance, a userspace program might
    keep a character device file open even after the physical device has been
    removed.  In this case:

    *   `devres` frees the device-specific resource upon unbinding.
    *   The Revocable mechanism ensures that any subsequent operations on the
        open file handle, which attempt to access the now-freed resource,
        will fail gracefully (e.g., revocable_try_access() returns NULL)
        instead of causing a UAF.

In summary, `devres` ensures resources are *released* to prevent leaks, while
the Revocable mechanism ensures that attempts to *access* these resources are
done safely, even if the resource has been released.

API and Usage
=============

For Resource Providers
----------------------

There are two ways to manage the resource provider handle (``struct revocable``):

Dynamic Allocation
~~~~~~~~~~~~~~~~~~

If the lifetime of the ``struct revocable`` is not tied to another specific
kernel object, or if multiple independent consumers need to hold references,
dynamic allocation should be used.

*   **Creation:** Use revocable_alloc() to allocate and initialize.
*   **Ownership:** The caller receives a reference, and the provider holds
    another.
*   **Revocation:** Call revocable_revoke() when the resource is going away.
    This drops the provider's reference.
*   **Cleanup:** The caller *must* call revocable_put() to release its reference
    when it no longer needs the handle.  The memory is freed automatically when
    the last reference is dropped.

Embedded Allocation
~~~~~~~~~~~~~~~~~~~

If the ``struct revocable`` can be embedded within a parent kernel object
(e.g., a foo_device struct), this method can be simpler as the lifetime is
inherently tied to the parent.

*   **Initialization:** Declare a ``struct revocable`` within your parent
    structure and initialize it with revocable_init().
*   **Ownership:** The caller receives a reference, and the provider holds
    another.
*   **Revocation:** Call revocable_revoke() when the resource is going away.
    This drops the provider's reference.
*   **Cleanup:** The owner *must* call revocable_put() during the parent
    object's teardown process and ensuring no more consumers can access
    it.  This cleans up internal resources like the SRCU domain.  The memory
    for the ``struct revocable`` is freed when the parent object is freed.

.. kernel-doc:: include/linux/revocable.h
   :identifiers: revocable

.. kernel-doc:: drivers/base/revocable.c
   :identifiers: revocable_alloc

.. kernel-doc:: drivers/base/revocable.c
   :identifiers: revocable_init

.. kernel-doc:: drivers/base/revocable.c
   :identifiers: revocable_revoke

.. kernel-doc:: drivers/base/revocable.c
   :identifiers: revocable_get

.. kernel-doc:: drivers/base/revocable.c
   :identifiers: revocable_put

Example Usage (Dynamic Allocation)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: c

    struct foo_device {
        struct revocable *rev;
        ...
    };

    int foo_device_probe(struct device *dev)
    {
        struct foo_device *foo_dev;
        void *res;
        int ret;

        foo_dev = devm_kzalloc(dev, sizeof(*foo_dev), GFP_KERNEL);
        if (!foo_dev)
            return -ENOMEM;

        // Acquire the actual resource.
        res = ...(...);

        // Allocate the revocable handle.
        foo_dev->rev = revocable_alloc(res);
        if (!foo_dev->rev)
            return -ENOMEM;

        dev_set_drvdata(dev, foo_dev);
        // ... further device setup ...
        return 0;
    }

    void foo_device_remove(struct device *dev)
    {
        struct foo_device *foo_dev = dev_get_drvdata(dev);

        // Drop the reference.
        revocable_put(foo_dev->rev);
    }

    // Provider side would use revocable_revoke() on foo_dev->rev.
    // Consumer side would use revocable_try_access_* macros on foo_dev->rev.

Example Usage (Embedded Allocation)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: c

    struct foo_device {
        struct revocable rev;
        ...
    };

    int foo_device_probe(struct device *dev)
    {
        struct foo_device *foo_dev;
        void *res;
        int ret;

        foo_dev = devm_kzalloc(dev, sizeof(*foo_dev), GFP_KERNEL);
        if (!foo_dev)
            return -ENOMEM;

        // Acquire the actual resource.
        res = ...(...);

        // Initialize the embedded revocable.
        ret = revocable_init(&foo_dev->rev, res);
        if (ret)
            return ret;

        dev_set_drvdata(dev, foo_dev);
        // ... further device setup ...
        return 0;
    }

    void foo_device_remove(struct device *dev)
    {
        struct foo_device *foo_dev = dev_get_drvdata(dev);

        // Cleanup the embedded revocable internal state.
        revocable_put(&foo_dev->rev);
    }

    // Provider side would use revocable_revoke() on &foo_dev->rev.
    // Consumer side would use revocable_try_access_* macros on &foo_dev->rev.

For Resource Consumers
----------------------
.. kernel-doc:: include/linux/revocable.h
   :identifiers: revocable_handle

.. kernel-doc:: drivers/base/revocable.c
   :identifiers: revocable_handle_init

.. kernel-doc:: drivers/base/revocable.c
   :identifiers: revocable_handle_deinit

.. kernel-doc:: drivers/base/revocable.c
   :identifiers: revocable_try_access

.. kernel-doc:: drivers/base/revocable.c
   :identifiers: revocable_withdraw_access

.. kernel-doc:: include/linux/revocable.h
   :identifiers: revocable_try_access_with

Example Usage
~~~~~~~~~~~~~

.. code-block:: c

    int consumer_use_resource(struct revocable *rev)
    {
        struct foo_resource *res;

        revocable_try_access_with(rev, res);
        // Always check if the resource is valid.
        if (!res) {
            pr_warn("Resource is not available\n");
            return -EAGAIN;
        }

        // 'res' is guaranteed to be valid until this function exits.
        do_something_with(res);
        return 0;
    } // revocable_withdraw_access() is automatically called here.

.. kernel-doc:: include/linux/revocable.h
   :identifiers: revocable_try_access_or_return_err

Example Usage
~~~~~~~~~~~~~

.. code-block:: c

    int consumer_use_resource(struct revocable *rev)
    {
        struct foo_resource *res;

        // Returns -ENXIO if access fails.
        revocable_try_access_or_return_err(rev, res, -ENXIO);

        // 'res' is guaranteed to be valid if we reach here.
        do_something_with(res);
        return 0;
    } // revocable_withdraw_access() is automatically called here.

.. kernel-doc:: include/linux/revocable.h
   :identifiers: revocable_try_access_or_return

Example Usage
~~~~~~~~~~~~~

.. code-block:: c

    int consumer_use_resource(struct revocable *rev)
    {
        struct foo_resource *res;

        // Returns -ENODEV if access fails.
        revocable_try_access_or_return(rev, res);

        // 'res' is guaranteed to be valid if we reach here.
        do_something_with(res);
        return 0;
    } // revocable_withdraw_access() is automatically called here.

.. kernel-doc:: include/linux/revocable.h
   :identifiers: revocable_try_access_with_scoped

Example Usage
~~~~~~~~~~~~~

.. code-block:: c

    int consumer_use_resource(struct revocable *rev)
    {
        struct foo_resource *res;

        revocable_try_access_with_scoped(rev, res) {
            // Always check if the resource is valid.
            if (!res) {
                pr_warn("Resource is not available\n");
                return -EAGAIN;
            }

            // 'res' is valid for the rest of this block.
            do_something_with(res);
        }
        // revocable_withdraw_access() is automatically called here.

        return 0;
    }

.. kernel-doc:: include/linux/revocable.h
   :identifiers: revocable_try_access_or_return_err_scoped

Example Usage
~~~~~~~~~~~~~

.. code-block:: c

    int consumer_use_resource(struct revocable *rev)
    {
        struct foo_resource *res;

        // Returns -ENXIO if access fails.
        revocable_try_access_or_return_err_scoped(rev, res, -ENXIO) {
            // 'res' is guaranteed to be valid in this block.
            do_something_with(res);
        }
        // revocable_withdraw_access() is automatically called here.

        return 0; // Only reached if resource was accessed.
    }

.. kernel-doc:: include/linux/revocable.h
   :identifiers: revocable_try_access_or_return_scoped

Example Usage
~~~~~~~~~~~~~

.. code-block:: c

    int consumer_use_resource(struct revocable *rev)
    {
        struct foo_resource *res;

        // Returns -ENODEV if access fails.
        revocable_try_access_or_return_scoped(rev, res) {
            // 'res' is guaranteed to be valid in this block.
            do_something_with(res);
        }
        // revocable_withdraw_access() is automatically called here.

        return 0; // Only reached if resource was accessed.
    }

.. kernel-doc:: include/linux/revocable.h
   :identifiers: revocable_try_access_or_skip_scoped

Example Usage
~~~~~~~~~~~~~

.. code-block:: c

    int consumer_use_resource(struct revocable *rev)
    {
        struct foo_resource *res;

        revocable_try_access_or_skip_scoped(rev, res) {
            // This block is ONLY entered if 'res' is not NULL.
            do_something_with(res);
        }
        // revocable_withdraw_access() is automatically called here.

        return 0;
    }
