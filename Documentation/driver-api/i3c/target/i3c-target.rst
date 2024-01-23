.. SPDX-License-Identifier: GPL-2.0

:Author: Frank Li <Frank.Li@nxp.com>

This document is a guide to use the I3C Target Framework in order to create
target controller driver, target function driver, and using configfs interface
to bind the function driver to the controller driver.

Introduction
============

Linux has a comprehensive I3C subsystem to support I3C controllers that
operates in master mode. The subsystem has capability to scan I3C bus,assign
i3c device address, load I3C driver (based on Manufacturer ID, part ID),
support other services like hot-join, In-Band Interrupt(IBI).

However the I3C controller IP integrated in some SoCs is capable of operating
either in Master mode or Target mode. I3C Target Framework will add target mode
support in Linux. This will help to run Linux in an target system which can
have a wide variety of use cases from testing or validation, co-processor
accelerator, etc.

I3C Target Core
=================

The I3C Target Core layer comprises 3 components: the Target Controller
library, the Target Function library, and the configfs layer to bind the target
function with the target controller.

I3C Target Controller Library
------------------------------------

The Controller library provides APIs to be used by the controller that can
operate in target mode. It also provides APIs to be used by function
driver/library in order to implement a particular target function.

APIs for the I3C Target controller Driver
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

This section lists the APIs that the I3C Target core provides to be used by the
I3C controller driver.

* devm_i3c_target_ctrl_create()/i3c_target_ctrl_create()

   The I3C controller driver should implement the following ops:

	* set_config: ops to set i3c configuration
	* enable: ops to enable controller
	* disable: ops to disable controller
	* raise_ibi: ops to raise IBI to master controller
	* alloc_request: ops to alloc a transfer request
	* free_request: ops to free a transfer request
	* queue: ops to queue a request to transfer queue
	* dequeue: ops to dequeue a request from transfer queue
	* cancel_all_reqs: ops to cancel all request from transfer queue
        * fifo_status: ops to get fifo status
        * fifo_flush: ops to flush hardware fifo
	* get_features: ops to get controller supported features

   The I3C controller driver can then create a new Controller device by
   invoking devm_i3c_target_ctrl_create()/i3c_target_ctrl_create().

* devm_i3c_target_ctrl_destroy()/i3c_target_ctrl_destroy()

   The I3C controller driver can destroy the Controller device created by
   either devm_i3c_target_ctrl_create() or i3c_target_ctrl_create() using
   devm_i3c_target_ctrl_destroy() or i3c_target_ctrl_destroy().

I3C Target Controller APIs for the I3C Target Function Driver
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

This section lists the APIs that the I3C Target core provides to be used by the
I3C target function driver.

* i3c_target_ctrl_set_config()

   The I3C target function driver should use i3c_target_ctrl_set_config() to
   write i3c configuration to the target controller.

* i3c_target_ctrl_enable()/i3c_target_ctrl_disable()

   The I3C target function driver should use i3c_target_ctrl_enable()/
   i3c_target_ctrl_disable() to enable/disable i3c target controller.

* i3c_target_ctrl_alloc_request()/i3c_target_ctrl_free_request()

   The I3C target function driver should usei3c_target_ctrl_alloc_request() /
   i3c_target_ctrl_free_request() to alloc/free a i3c request.

* i3c_target_ctrl_raise_ibi()

   The I3C target function driver should use i3c_target_ctrl_raise_ibi() to
   raise IBI.

* i3c_target_ctrl_queue()/i3c_target_ctrl_dequeue()

   The I3C target function driver should use i3c_target_ctrl_queue()/
   i3c_target_ctrl_dequeue(), to queue/dequeue I3C transfer to/from transfer
   queue.

* i3c_target_ctrl_get_features()

   The I3C target function driver should use i3c_target_ctrl_get_features() to
   get I3C target controller supported features.

Other I3C Target Controller APIs
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

There are other APIs provided by the Controller library. These are used for
binding the I3C Target Function device with Controlller device. i3c-cfs.c can
be used as reference for using these APIs.

* i3c_target_ctrl_get()

   Get a reference to the I3C target controller based on the device name of
   the controller.

* i3c_target_ctrl_put()

   Release the reference to the I3C target controller obtained using
   i3c_target_ctrl_get()

* i3c_target_ctrl_add_func()

   Add a I3C target function to a I3C target controller.

* i3c_target_ctrl_remove_func()

   Remove the I3C target function from I3C target controller.

I3C Target Function Library
----------------------------------

The I3C Target Function library provides APIs to be used by the function driver
and the Controller library to provide target mode functionality.

I3C Target Function APIs for the I3C Target Function Driver
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

This section lists the APIs that the I3C Target core provides to be used
by the I3C target function driver.

* i3c_target_func_register_driver()

   The I3C Target Function driver should implement the following ops:
	 * bind: ops to perform when a Controller device has been bound to
	   Function device
	 * unbind: ops to perform when a binding has been lost between a
	   Controller device and Function device

  The I3C Function driver can then register the I3C Function driver by using
  i3c_target_func_register_driver().

* i3c_target_func_unregister_driver()

  The I3C Function driver can unregister the I3C Function driver by using
  i3c_epf_unregister_driver().

APIs for the I3C Target Controller Library
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

This section lists the APIs that the I3C Target core provides to be used by the
I3C target controller library.

Other I3C Target APIs
~~~~~~~~~~~~~~~~~~~~

There are other APIs provided by the Function library. These are used to notify
the function driver when the Function device is bound to the EPC device.
i3c-cfs.c can be used as reference for using these APIs.

* i3c_target_func_create()

   Create a new I3C Function device by passing the name of the I3C EPF device.
   This name will be used to bind the Function device to a Function driver.

* i3c_target_func_destroy()

   Destroy the created I3C Function device.

* i3c_target_func_bind()

   i3c_target_func_bind() should be invoked when the EPF device has been bound
   to a Controller device.

* i3c_target_func_unbind()

   i3c_target_func_unbind() should be invoked when the binding between EPC
   device and function device is lost.
