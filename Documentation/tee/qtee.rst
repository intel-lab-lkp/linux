.. SPDX-License-Identifier: GPL-2.0

=============================================
QTEE (Qualcomm Trusted Execution Environment)
=============================================

The QTEE driver handles communication with Qualcomm TEE [1].

The lowest level of communication with QTEE builds on the ARM SMC Calling
Convention (SMCCC) [2], which is the foundation for QTEE's Secure Channel
Manager (SCM) [3] used internally by the driver [4].

In a QTEE-based system, services are represented as objects with a series of
operations that can be called to produce results, including other objects.

When an object is hosted within QTEE, executing its operations is referred
to as direct invocation. QTEE can invoke objects hosted in the kernel or
userspace using a method known as callback requests.

The SCM provides two functions for direct invocation and callback request:

- QCOM_SCM_SMCINVOKE_INVOKE for direct invocation. It can return either
  a result or a callback request.
- QCOM_SCM_SMCINVOKE_CB_RSP submits a response for a previous callback request.

The QTEE Transport Message [5] is stacked on top of the SCM driver functions.

A message consists of two buffers shared with QTEE: inbound and outbound
buffers. The inbound buffer is used for direct invocation, and the outbound
buffer is used to make callback requests. This picture shows the contents of
a QTEE transport message::

                                      +---------------------+
                                      |                     v
    +-----------------+-------+-------+------+--------------------------+
    | qcomtee_msg_    |object | buffer       |                          |
    |  object_invoke  |  id   | offset, size |                          | (inbound buffer)
    +-----------------+-------+--------------+--------------------------+
    <---- header -----><---- arguments ------><- in/out buffer payload ->

                                      +-----------+
                                      |           v
    +-----------------+-------+-------+------+----------------------+
    | qcomtee_msg_    |object | buffer       |                      |
    |  callback       |  id   | offset, size |                      | (outbound buffer)
    +-----------------+-------+--------------+----------------------+

Each buffer is started with a header and array of arguments.

QTEE Transport Message supports four types of arguments:

- Input Object (IO) is an object parameter to the current invocation
  or callback request.
- Output Object (OO) is an object parameter from the current invocation
  or callback request.
- Input Buffer (IB) is (offset, size) pair to the inbound or outbound region
  to store parameter to the current invocation or callback request.
- Output Buffer (OB) is (offset, size) pair to the inbound or outbound region
  to store parameter from the current invocation or callback request.

The QTEE driver provides the qcomtee_object, which represents an object within
both QTEE and the kernel. To access any service in QTEE, a client needs to
invoke an instance of this object. Any structure intended to represent a service
for export to QTEE should include an instance of qcomtee_object::

	struct driver_service {
		struct qcomtee_object object;
		...
	};

	#define to_driver_service_object(o) container_of((o), struct driver_service, object)

	static int driver_service_dispatch(struct qcomtee_object *object, u32 op,
					   struct qcomtee_arg *args)
	{
		struct driver_service *so = to_driver_service_object(object);

		switch(op) {
		case OBJECT_OP1:
			...
			break;
		default:
			return -EINVAL;
		}
	}

	static void driver_service_object_release(struct si_object *object)
	{
		struct driver_service *so = to_driver_service_object(object);
		kfree(so);
	}

	struct si_object_operations driver_service_ops = {
		.release = driver_service_object_release;
		.dispatch = driver_service_dispatch;
	};

	void service_init(void)
	{
		struct driver_service *so = kzalloc(sizeof(*so), GFP_KERNEL);

		/* Initialize so->object as a callback object. */
		qcomtee_object_user_init(&so->object, QCOMTEE_OBJECT_TYPE_CB_OBJECT,
					 &driver_service_ops, "driver_service_object");

		/* Invoke a QTEE object and pass/register 'so->object' with QTEE. */
		...
	}
	module_init(service_init);

The QTEE driver utilizes qcomtee_object to encapsulate userspace objects. When
a callback request is made, it translates into calling the dispatch operation.
For userspace objects, this is converted into requests accessible to callback
servers and available through generic TEE API IOCTLs.

Picture of the relationship between the different components in the QTEE
architecture::

         User space               Kernel                     Secure world
         ~~~~~~~~~~               ~~~~~~                     ~~~~~~~~~~~~
   +--------+   +----------+                                +--------------+
   | Client |   |callback  |                                | Trusted      |
   +--------+   |server    |                                | Application  |
      /\        +----------+                                +--------------+
      ||  +----------+ /\                                          /\
      ||  |callback  | ||                                          ||
      ||  |server    | ||                                          \/
      ||  +----------+ ||                                   +--------------+
      \/       /\      ||                                   | TEE Internal |
   +-------+   ||      ||                                   | API          |
   | TEE   |   ||      ||   +--------+--------+             +--------------+
   | Client|   ||      ||   | TEE    | QTEE   |             | QTEE         |
   | API   |   \/      \/   | subsys | driver |             | Trusted OS   |
   +-------+----------------+----+-------+----+-------------+--------------+
   |      Generic TEE API        |       |   QTEE MSG                      |
   |      IOCTL (TEE_IOC_*)      |       |   SMCCC (QCOM_SCM_SMCINVOKE_*)  |
   +-----------------------------+       +---------------------------------+

References
==========

[1] https://docs.qualcomm.com/bundle/publicresource/topics/80-70015-11/qualcomm-trusted-execution-environment.html

[2] http://infocenter.arm.com/help/topic/com.arm.doc.den0028a/index.html

[3] drivers/firmware/qcom/qcom_scm.c

[4] drivers/tee/qcomtee/qcom_scm.c

[5] drivers/tee/qcomtee/qcomtee_msg.h
