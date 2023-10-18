/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright 2023 NXP.
 *
 * Author: Frank Li <Frank.Li@nxp.com>
 */

#ifndef I3C_SLAVE_H
#define I3C_SLAVE_H

#include <linux/device.h>

struct i3c_slave_func;
struct i3c_slave_ctrl;

/**
 * struct i3c_slave_func_ops - set of function pointers for performing i3c slave function operations
 * @bind: ops to perform when a controller device has been bound to function device
 * @unbind: ops to perform when a binding has been lost between a controller device and function
 *	    device
 */
struct i3c_slave_func_ops {
	int (*bind)(struct i3c_slave_func *func);
	void (*unbind)(struct i3c_slave_func *func);
};

/**
 * struct i3c_slave_func_driver - represents the I3C function driver
 * @probe: ops to perform when a new function device has been bound to the function driver
 * @remove: ops to perform when the binding between the function device and function driver is
 *	    broken
 * @driver: I3C Function driver
 * @ops: set of function pointers for performing function operations
 * @owner: the owner of the module that registers the I3C function driver
 * @epf_group: list of configfs group corresponding to the I3C function driver
 */
struct i3c_slave_func_driver {
	int (*probe)(struct i3c_slave_func *func);
	void (*remove)(struct i3c_slave_func *func);

	char *name;
	struct device_driver driver;
	struct i3c_slave_func_ops *ops;
	struct module *owner;
};

/**
 * struct i3c_slave_func - represents the I3C function device
 * @dev: the I3C function device
 * @name: the name of the I3C function device
 * @driver: the function driver to which this function device is bound
 * @group: configfs group associated with the EPF device
 * @lock: mutex to protect i3c_slave_func_ops
 * @ctrl: binded I3C controller device
 * @is_bound: indicates if bind notification to function driver has been invoked
 * @vednor_id: vendor id
 * @part_id: part id
 * @instance_id: instance id
 * @ext_id: ext id
 * @vendor_info: vendor info
 * @static_addr: static address for I2C. It is 0 for I3C.
 * @max_write_len: maximum write length
 * @max_read_len: maximum read length
 * @bcr: bus characteristics register (BCR)
 * @dcr: device characteristics register (DCR)
 */
struct i3c_slave_func {
	struct device dev;
	char *name;
	struct i3c_slave_func_driver *driver;
	struct config_group *group;
	/* mutex to protect against concurrent access of i3c_slave_func_ops */
	struct mutex lock;
	struct i3c_slave_ctrl *ctrl;
	bool is_bound;

	u16 vendor_id;
	u16 part_id;
	u8 instance_id;
	u16 ext_id;
	u8 vendor_info;
	u16 static_addr;
	u16 max_write_len;	//0 is hardware default max value
	u16 max_read_len;	//0 is hardware default max value
	u8 bcr;
	u8 dcr;
};

enum i3c_request_stat {
	I3C_REQUEST_OKAY,
	I3C_REQUEST_PARTIAL,
	I3C_REQUEST_ERR,
	I3C_REQUEST_CANCEL,
};

/**
 * struct i3c_request - represents the an I3C transfer request
 * @buf: data buffer
 * @length: data length
 * @complete: call back function when request finished or cancelled
 * @context: general data for complete callback function
 * @status: transfer status
 * @actual: how much actually transferred
 * @ctrl: I3C slave controller associate with this request
 * @tx: transfer direction, 1: slave to master, 0: master to slave
 */
struct i3c_request {
	void *buf;
	unsigned int length;

	void (*complete)(struct i3c_request *req);
	void *context;
	struct list_head list;

	enum i3c_request_stat status;
	unsigned int actual;
	struct i3c_slave_ctrl *ctrl;
	bool tx;
};

/**
 * struct i3c_slave_ctrl_features - represents I3C slave controller features.
 * @tx_fifo_sz: tx hardware fifo size
 * @rx_fifo_sz: rx hardware fifo size
 */
struct i3c_slave_ctrl_features {
	u32 tx_fifo_sz;
	u32 rx_fifo_sz;
};

/**
 * struct i3c_slave_ctrl_ops - set of function pointers for performing controller operations
 * @set_config: set I3C controller configuration
 * @enable: enable I3C controller
 * @disable: disable I3C controller
 * @raise_ibi: rasie IBI interrupt to master
 * @queue: queue an I3C transfer
 * @dequeue: dequeue an I3C transfer
 * @cancel_all_reqs: call all pending requests
 * @fifo_status: current FIFO status
 * @fifo_flush: flush hardware FIFO
 * @get_features: ops to get the features supported by the I3C slave controller
 * @owner: the module owner containing the ops
 */
struct i3c_slave_ctrl_ops {
	int (*set_config)(struct i3c_slave_ctrl *ctrl, struct i3c_slave_func *func);
	int (*enable)(struct i3c_slave_ctrl *ctrl);
	int (*disable)(struct i3c_slave_ctrl *ctrl);
	int (*raise_ibi)(struct i3c_slave_ctrl *ctrl, void *p, u8 size);

	struct i3c_request *(*alloc_request)(struct i3c_slave_ctrl *ctrl, gfp_t gfp_flags);
	void (*free_request)(struct i3c_request *req);

	int (*queue)(struct i3c_request *req, gfp_t gfp_flags);
	int (*dequeue)(struct i3c_request *req);

	void (*cancel_all_reqs)(struct i3c_slave_ctrl *ctrl, bool tx);

	int (*fifo_status)(struct i3c_slave_ctrl *ctrl, bool tx);
	void (*fifo_flush)(struct i3c_slave_ctrl *ctrl, bool tx);
	int (*hotjoin)(struct i3c_slave_ctrl *ctrl);
	int (*set_status_format1)(struct i3c_slave_ctrl *ctrl, u16 status);
	u16 (*get_status_format1)(struct i3c_slave_ctrl *ctrl);
	u8  (*get_addr)(struct i3c_slave_ctrl *ctrl);
	const struct i3c_slave_ctrl_features *(*get_features)(struct i3c_slave_ctrl *ctrl);
	struct module *owner;
};

/**
 * struct i3c_slave_ctrl - represents the I3C slave device
 * @dev: I3C slave device
 * @ops: function pointers for performing endpoint operations
 * @func: slave functions present in this controller device
 * @group: configfs group representing the I3C controller device
 */
struct i3c_slave_ctrl {
	struct device dev;
	const struct i3c_slave_ctrl_ops *ops;
	struct i3c_slave_func *func;
	struct config_group *group;
};

/**
 * i3c_slave_ctrl_raise_ibi() - Raise IBI to master
 * @ctrl: I3C slave controller
 * @p: optional data for IBI
 * @size: size of optional data
 *
 * Returns: Zero for success, or an error code in case of failure
 */
static inline int i3c_slave_ctrl_raise_ibi(struct i3c_slave_ctrl *ctrl, void *p, u8 size)
{
	if (ctrl && ctrl->ops && ctrl->ops->raise_ibi)
		return ctrl->ops->raise_ibi(ctrl, p, size);

	return -EINVAL;
}

/**
 * i3c_slave_ctrl_cancel_all_reqs() - Cancel all pending request
 * @ctrl: I3C slave controller
 * @tx: Transfer diretion queue
 * @size: size of optional data
 */
static inline void i3c_slave_ctrl_cancel_all_reqs(struct i3c_slave_ctrl *ctrl, bool tx)
{
	if (ctrl && ctrl->ops && ctrl->ops->cancel_all_reqs)
		ctrl->ops->cancel_all_reqs(ctrl, tx);
}

/**
 * i3c_slave_ctrl_set_config() - Set controller configuration
 * @ctrl: I3C slave controller device
 * @func: Function device
 *
 * Returns: Zero for success, or an error code in case of failure
 */
static inline int
i3c_slave_ctrl_set_config(struct i3c_slave_ctrl *ctrl, struct i3c_slave_func *func)
{
	if (ctrl && ctrl->ops && ctrl->ops->set_config)
		return ctrl->ops->set_config(ctrl, func);

	return -EINVAL;
}

/**
 * i3c_slave_ctrl_enable() - Enable I3C controller
 * @ctrl: I3C slave controller device
 *
 * Returns: Zero for success, or an error code in case of failure
 */
static inline int
i3c_slave_ctrl_enable(struct i3c_slave_ctrl *ctrl)
{
	if (ctrl && ctrl->ops && ctrl->ops->enable)
		return ctrl->ops->enable(ctrl);

	return -EINVAL;
}

/**
 * i3c_slave_ctrl_disable() - Disable I3C controller
 * @ctrl: I3C slave controller device
 *
 * Returns: Zero for success, or an error code in case of failure
 */
static inline int
i3c_slave_ctrl_disable(struct i3c_slave_ctrl *ctrl)
{
	if (ctrl && ctrl->ops && ctrl->ops->disable)
		return ctrl->ops->disable(ctrl);

	return -EINVAL;
}

/**
 * i3c_slave_ctrl_alloc_request() - Alloc an I3C transfer
 * @ctrl: I3C slave controller device
 * gfp_flags: additional gfp flags used when allocating the buffers
 *
 * Returns: Zero for success, or an error code in case of failure
 */
static inline struct i3c_request *
i3c_slave_ctrl_alloc_request(struct i3c_slave_ctrl *ctrl, gfp_t gfp_flags)
{
	struct i3c_request *req = NULL;

	if (ctrl && ctrl->ops && ctrl->ops->alloc_request)
		req = ctrl->ops->alloc_request(ctrl, gfp_flags);
	else
		req = kzalloc(sizeof(*req), gfp_flags);

	if (req)
		req->ctrl = ctrl;

	return req;
}

/**
 * i3c_slave_ctrl_free_request() - Free an I3C transfer
 * @ctrl: I3C slave controller device
 *
 * Returns: Zero for success, or an error code in case of failure
 */
static inline void
i3c_slave_ctrl_free_request(struct i3c_request *req)
{
	struct i3c_slave_ctrl *ctrl;

	if (!req)
		return;

	ctrl = req->ctrl;
	if (ctrl && ctrl->ops && ctrl->ops->free_request)
		ctrl->ops->free_request(req);
	else
		kfree(req);
}

/**
 * i3c_slave_ctrl_queue() - Queue an I3C transfer
 * @ctrl: I3C slave controller device
 * gfp_flags: additional gfp flags used when allocating the buffers
 *
 * Returns: Zero for success, or an error code in case of failure
 */
static inline int
i3c_slave_ctrl_queue(struct i3c_request *req, gfp_t gfp_flags)
{
	struct i3c_slave_ctrl *ctrl;
	int ret = -EINVAL;

	if (!req)
		return -EINVAL;

	ctrl = req->ctrl;

	req->actual = 0;
	req->status = 0;
	if (ctrl && ctrl->ops && ctrl->ops->queue)
		ret = ctrl->ops->queue(req, gfp_flags);

	return ret;
}

/**
 * i3c_slave_ctrl_dequeue() - Dequeue an I3C transfer
 * @ctrl: I3C slave controller device
 *
 * Returns: Zero for success, or an error code in case of failure
 */
static inline int
i3c_slave_ctrl_dequeue(struct i3c_request *req)
{
	struct i3c_slave_ctrl *ctrl;
	int ret = -EINVAL;

	if (!req)
		return -EINVAL;

	ctrl = req->ctrl;
	if (ctrl && ctrl->ops && ctrl->ops->dequeue)
		ret = ctrl->ops->dequeue(req);

	return ret;
}

/**
 * i3c_slave_ctrl_fifo_status() - Get controller FIFO status
 * @ctrl: I3C slave controller device
 * @tx: 1: Slave to master, 0: master to slave
 *
 * Returns: How much data in FIFO
 */
static inline int
i3c_slave_ctrl_fifo_status(struct i3c_slave_ctrl *ctrl, bool tx)
{
	if (ctrl && ctrl->ops && ctrl->ops->fifo_status)
		return ctrl->ops->fifo_status(ctrl, tx);

	return 0;
}

/**
 * i3c_slave_ctrl_fifo_flush() - Flush controller FIFO
 * @ctrl: I3C slave controller device
 * @tx: 1: Slave to master, 0: master to slave
 *
 */
static inline void
i3c_slave_ctrl_fifo_flush(struct i3c_slave_ctrl *ctrl, bool tx)
{
	if (ctrl && ctrl->ops && ctrl->ops->fifo_flush)
		return ctrl->ops->fifo_flush(ctrl, tx);
}

/**
 * i3c_slave_ctrl_get_features() - Get controller supported features
 * @ctrl: I3C slave controller device
 *
 * Returns: The pointer to struct i3c_slave_ctrl_features
 */
static inline const struct i3c_slave_ctrl_features*
i3c_slave_ctrl_get_features(struct i3c_slave_ctrl *ctrl)
{
	if (ctrl && ctrl->ops && ctrl->ops->get_features)
		return ctrl->ops->get_features(ctrl);

	return NULL;
}

/**
 * i3c_slave_set_status_format1() - Set controller supported features
 * &ctrl: I3C slave controller device
 * &status: I3C GETSTATUS format1
 *
 * Returns: Zero for success, or an error code in case of failure
 */
static inline int
i3c_slave_ctrl_set_status_format1(struct i3c_slave_ctrl *ctrl, u16 status)
{
	if (ctrl && ctrl->ops && ctrl->ops->set_status_format1)
		return ctrl->ops->set_status_format1(ctrl, status);

	return -EINVAL;
}

/**
 * i3c_slave_get_status_format1() - Get controller supported features
 * &ctrl: I3C slave controller device
 *
 * Return: I3C GETSTATUS format1
 */
static inline u16
i3c_slave_ctrl_get_status_format1(struct i3c_slave_ctrl *ctrl)
{
	if (ctrl && ctrl->ops && ctrl->ops->get_status_format1)
		return ctrl->ops->get_status_format1(ctrl);

	return 0;
}

/**
 * i3c_slave_ctrl_get_addr() - Get controller address
 * &ctrl: I3C slave controller device
 *
 * Return: address
 */
static inline u8 i3c_slave_ctrl_get_addr(struct i3c_slave_ctrl *ctrl)
{
	if (ctrl && ctrl->ops && ctrl->ops->get_addr)
		return ctrl->ops->get_addr(ctrl);

	return 0;
}

#define to_i3c_slave_ctrl(device) container_of((device), struct i3c_slave_ctrl, dev)

#define to_i3c_slave_func(func_dev) container_of((func_dev), struct i3c_slave_func, dev)
#define to_i3c_slave_func_driver(drv) (container_of((drv), struct i3c_slave_func_driver, driver))

#define i3c_slave_ctrl_create(dev, ops) \
		__i3c_slave_ctrl_create((dev), (ops), THIS_MODULE)
#define devm_i3c_slave_ctrl_create(dev, ops) \
		__devm_i3c_slave_ctrl_create((dev), (ops), THIS_MODULE)

struct i3c_slave_ctrl *
__devm_i3c_slave_ctrl_create(struct device *dev, const struct i3c_slave_ctrl_ops *ops,
			     struct module *owner);
struct i3c_slave_ctrl *
__i3c_slave_ctrl_create(struct device *dev, const struct i3c_slave_ctrl_ops *ops,
			struct module *owner);

void devm_i3c_slave_ctrl_destroy(struct device *dev, struct i3c_slave_ctrl *epc);
void i3c_slave_ctrl_destroy(struct i3c_slave_ctrl *epc);

int i3c_slave_ctrl_add_func(struct i3c_slave_ctrl *ctrl, struct i3c_slave_func *func);
void i3c_slave_ctrl_remove_func(struct i3c_slave_ctrl *ctrl, struct i3c_slave_func *func);
int i3c_slave_ctrl_hotjoin(struct i3c_slave_ctrl *ctrl);

struct config_group *i3c_slave_cfs_add_ctrl_group(struct i3c_slave_ctrl *ctrl);

void i3c_slave_cfs_remove_ctrl_group(struct config_group *group);
struct config_group *i3c_slave_cfs_add_func_group(const char *name);
void i3c_slave_cfs_remove_func_group(struct config_group *group);
struct i3c_slave_ctrl *i3c_slave_ctrl_get(const char *name);
void i3c_slave_ctrl_put(struct i3c_slave_ctrl *ctrl);

int i3c_slave_func_bind(struct i3c_slave_func *func);
void i3c_slave_func_unbind(struct i3c_slave_func *func);
struct i3c_slave_func *i3c_slave_func_create(const char *drv_name, const char *name);

#define i3c_slave_func_register_driver(drv) \
	__i3c_slave_func_register_driver(drv, THIS_MODULE)

int __i3c_slave_func_register_driver(struct i3c_slave_func_driver *drv, struct module *owner);
void i3c_slave_func_unregister_driver(struct i3c_slave_func_driver *drv);

#define DECLARE_I3C_SLAVE_FUNC(_name, _probe, _remove, _ops)			\
	static struct i3c_slave_func_driver _name ## i3c_func = {		\
		.driver.name = __stringify(_name),				\
		.owner  = THIS_MODULE,						\
		.probe = _probe,						\
		.remove = _remove,						\
		.ops = _ops							\
	};									\
	MODULE_ALIAS("i3cfunc:" __stringify(_name))

#define DECLARE_I3C_SLAVE_INIT(_name, _probe, _remove, _ops)			\
	DECLARE_I3C_SLAVE_FUNC(_name, _probe, _remove, _ops);			\
	static int __init _name ## mod_init(void)				\
	{									\
		return i3c_slave_func_register_driver(&_name ## i3c_func);	\
	}									\
	static void __exit _name ## mod_exit(void)				\
	{									\
		i3c_slave_func_unregister_driver(&_name ## i3c_func);		\
	}									\
	module_init(_name ## mod_init);						\
	module_exit(_name ## mod_exit)

#endif
