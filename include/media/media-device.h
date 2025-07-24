/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Media device
 *
 * Copyright (C) 2010 Nokia Corporation
 *
 * Contacts: Laurent Pinchart <laurent.pinchart@ideasonboard.com>
 *	     Sakari Ailus <sakari.ailus@iki.fi>
 */

#ifndef _MEDIA_DEVICE_H
#define _MEDIA_DEVICE_H

#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/platform_device.h>

#include <media/media-devnode.h>
#include <media/media-entity.h>
#include <media/media-fh.h>

struct ida;
struct media_device;

/**
 * struct media_device_context - Media device context
 * @mdev: The media device this context is associated with
 * @refcount: The kref reference counter
 * @lock: Protects the entities contexts list
 * @contexts: List of entity contexts associated with this media device context
 * @initialized: Flag set to true by media_device_init_context()
 *
 * A media device context is created every time the media device gets opened by
 * userspace. It is then uniquely identified for applications by the numerical
 * file descriptor returned by a successful call to open() and is associated
 * with an instance of :c:type:`media_device_fh`.
 *
 * Media device contexts are ref-counted and thus freed once the last reference
 * to them is released.
 *
 * A media device context groups together the media entity contexts registered
 * on a video device or v4l2 subdevice that has been associated with a media
 * device context. The association between a media entity context and media
 * device context is called 'bounding', and the result of bounding is to create
 * an 'execution context' independent from other execution contexts.
 *
 * An entity context is bound to a media device context by a call to the
 * VIDIOC_BIND_CONTEXT ioctl on video devices and by a call to
 * VIDIOC_SUBDEV_BIND_CONTEXT on subdevices by userspace. The bounding operation
 * groups together entity contexts to the same media device context. As video
 * devices and v4l2 subdevices devnodes can be opened multiple times, each file
 * descriptor resulting from a successful open() call can be bound to a
 * different media device context.
 *
 * Creating execution contexts by bounding video entity contexts to a media
 * device context allows userspace to effectively multiplex the usage of a
 * media graph and of the device nodes that are part of it.
 *
 * In order to create an execution context userspace should:
 *
 * 1) Open the media device to create a media device context identified by the
 * file descriptor returned by a successful 'open()' call
 * 2) Open the video device or v4l2 subdevice and bind the file descriptors to
 * the media device context by calling the VIDIOC_BIND_CONTEXT and
 * VIDIOC_SUBDEV_BIND_CONTEXT ioctls
 *
 * All devices bound to the same media device context are now part of the same
 * execution context. From this point on all the operations performed on a file
 * descriptor bound to a media device context are independent from operations
 * performed on a file descriptor bound to a different execution context.
 *
 * Binding an entity context to a media device context increases the media
 * device context reference count. This guarantees that references to media
 * device context are valid as long as there are valid entity contexts that
 * refers to it. Symmetrically, unbinding an entity context from a media
 * device context decreases the media device context reference count.
 */
struct media_device_context {
	struct media_device *mdev;
	struct kref refcount;
	/* Protects the 'contexts' list */
	struct mutex lock;
	struct list_head contexts;
	bool initialized;
};

/**
 * struct media_entity_notify - Media Entity Notify
 *
 * @list: List head
 * @notify_data: Input data to invoke the callback
 * @notify: Callback function pointer
 *
 * Drivers may register a callback to take action when new entities get
 * registered with the media device. This handler is intended for creating
 * links between existing entities and should not create entities and register
 * them.
 */
struct media_entity_notify {
	struct list_head list;
	void *notify_data;
	void (*notify)(struct media_entity *entity, void *notify_data);
};

/**
 * struct media_device_ops - Media device operations
 * @link_notify: Link state change notification callback. This callback is
 *		 called with the graph_mutex held.
 * @req_alloc: Allocate a request. Set this if you need to allocate a struct
 *	       larger then struct media_request. @req_alloc and @req_free must
 *	       either both be set or both be NULL.
 * @req_free: Free a request. Set this if @req_alloc was set as well, leave
 *	      to NULL otherwise.
 * @req_validate: Validate a request, but do not queue yet. The req_queue_mutex
 *	          lock is held when this op is called.
 * @req_queue: Queue a validated request, cannot fail. If something goes
 *	       wrong when queueing this request then it should be marked
 *	       as such internally in the driver and any related buffers
 *	       must eventually return to vb2 with state VB2_BUF_STATE_ERROR.
 *	       The req_queue_mutex lock is held when this op is called.
 *	       It is important that vb2 buffer objects are queued last after
 *	       all other object types are queued: queueing a buffer kickstarts
 *	       the request processing, so all other objects related to the
 *	       request (and thus the buffer) must be available to the driver.
 *	       And once a buffer is queued, then the driver can complete
 *	       or delete objects from the request before req_queue exits.
 * @alloc_context: Allocate a media device context. The operation allows drivers to
 *		   allocate a driver-specific structure that embeds a
 *		   media_device_context instance as first member where to store
 *		   driver-specific information that are global to all device
 *		   contexts part of media device context. Returns 0 on success a
 *		   negative error code otherwise.
 * @destroy_context: Release a media device context.
 */
struct media_device_ops {
	int (*link_notify)(struct media_link *link, u32 flags,
			   unsigned int notification);
	struct media_request *(*req_alloc)(struct media_device *mdev);
	void (*req_free)(struct media_request *req);
	int (*req_validate)(struct media_request *req);
	void (*req_queue)(struct media_request *req);
	int (*alloc_context)(struct media_device *mdev,
			     struct media_device_context **ctx);
	void (*destroy_context)(struct media_device_context *ctx);
};

/**
 * struct media_device - Media device
 * @dev:	Parent device
 * @devnode:	Media device node
 * @driver_name: Optional device driver name. If not set, calls to
 *		%MEDIA_IOC_DEVICE_INFO will return ``dev->driver->name``.
 *		This is needed for USB drivers for example, as otherwise
 *		they'll all appear as if the driver name was "usb".
 * @model:	Device model name
 * @serial:	Device serial number (optional)
 * @bus_info:	Unique and stable device location identifier
 * @hw_revision: Hardware device revision
 * @flags:	Media device flags
 * @topology_version: Monotonic counter for storing the version of the graph
 *		topology. Should be incremented each time the topology changes.
 * @id:		Unique ID used on the last registered graph object
 * @entity_internal_idx: Unique internal entity ID used by the graph traversal
 *		algorithms
 * @entity_internal_idx_max: Allocated internal entity indices
 * @entities:	List of registered entities
 * @interfaces:	List of registered interfaces
 * @pads:	List of registered pads
 * @links:	List of registered links
 * @entity_notify: List of registered entity_notify callbacks
 * @graph_mutex: Protects access to struct media_device data
 * @pm_count_walk: Graph walk for power state walk. Access serialised using
 *		   graph_mutex.
 *
 * @source_priv: Driver Private data for enable/disable source handlers
 * @enable_source: Enable Source Handler function pointer
 * @disable_source: Disable Source Handler function pointer
 *
 * @ops:	Operation handler callbacks
 * @req_queue_mutex: Serialise the MEDIA_REQUEST_IOC_QUEUE ioctl w.r.t.
 *		     other operations that stop or start streaming.
 * @request_id: Used to generate unique request IDs
 *
 * @fh_list:	List of file handles in the media device
 *		(struct media_device_fh.mdev_list).
 * @fh_list_lock: Serialise access to fh_list list.
 * @default_context: The default video device context. Used by drivers that
 *		     support multi-context operation when operated by a
 *		     non-context aware userspace.
 *
 * This structure represents an abstract high-level media device. It allows easy
 * access to entities and provides basic media device-level support. The
 * structure can be allocated directly or embedded in a larger structure.
 *
 * The parent @dev is a physical device. It must be set before registering the
 * media device.
 *
 * @model is a descriptive model name exported through sysfs. It doesn't have to
 * be unique.
 *
 * @enable_source is a handler to find source entity for the
 * sink entity  and activate the link between them if source
 * entity is free. Drivers should call this handler before
 * accessing the source.
 *
 * @disable_source is a handler to find source entity for the
 * sink entity  and deactivate the link between them. Drivers
 * should call this handler to release the source.
 *
 * Use-case: find tuner entity connected to the decoder
 * entity and check if it is available, and activate the
 * link between them from @enable_source and deactivate
 * from @disable_source.
 *
 * .. note::
 *
 *    Bridge driver is expected to implement and set the
 *    handler when &media_device is registered or when
 *    bridge driver finds the media_device during probe.
 *    Bridge driver sets source_priv with information
 *    necessary to run @enable_source and @disable_source handlers.
 *    Callers should hold graph_mutex to access and call @enable_source
 *    and @disable_source handlers.
 *
 * The @flags field reports the media device capabilities:
 *
 * %MEDIA_DEVICE_FL_CONTEXT
 *    indicates that the media device supports multi-context operations.
 */
struct media_device {
	/* dev->driver_data points to this struct. */
	struct device *dev;
	struct media_devnode *devnode;

	char model[32];
	char driver_name[32];
	char serial[40];
	char bus_info[32];
	u32 hw_revision;
	u32 flags;

	u64 topology_version;

	u32 id;
	struct ida entity_internal_idx;
	int entity_internal_idx_max;

	struct list_head entities;
	struct list_head interfaces;
	struct list_head pads;
	struct list_head links;

	/* notify callback list invoked when a new entity is registered */
	struct list_head entity_notify;

	/* Serializes graph operations. */
	struct mutex graph_mutex;
	struct media_graph pm_count_walk;

	void *source_priv;
	int (*enable_source)(struct media_entity *entity,
			     struct media_pipeline *pipe);
	void (*disable_source)(struct media_entity *entity);

	const struct media_device_ops *ops;

	struct mutex req_queue_mutex;
	atomic_t request_id;

	struct list_head fh_list;
	spinlock_t fh_list_lock;

	struct media_device_context *default_context;
};

/* We don't need to include usb.h here */
struct usb_device;

#ifdef CONFIG_MEDIA_CONTROLLER

/* Supported link_notify @notification values. */
#define MEDIA_DEV_NOTIFY_PRE_LINK_CH	0
#define MEDIA_DEV_NOTIFY_POST_LINK_CH	1

/**
 * media_device_init() - Initializes a media device element
 *
 * @mdev:	pointer to struct &media_device
 *
 * This function initializes the media device prior to its registration.
 * The media device initialization and registration is split in two functions
 * to avoid race conditions and make the media device available to user-space
 * before the media graph has been completed.
 *
 * So drivers need to first initialize the media device, register any entity
 * within the media device, create pad to pad links and then finally register
 * the media device by calling media_device_register() as a final step.
 *
 * The caller is responsible for initializing the media device before
 * registration. The following fields must be set:
 *
 * - dev must point to the parent device
 * - model must be filled with the device model name
 *
 * The bus_info field is set by media_device_init() for PCI and platform devices
 * if the field begins with '\0'.
 */
void media_device_init(struct media_device *mdev);

/**
 * media_device_cleanup() - Cleanups a media device element
 *
 * @mdev:	pointer to struct &media_device
 *
 * This function that will destroy the graph_mutex that is
 * initialized in media_device_init().
 */
void media_device_cleanup(struct media_device *mdev);

/**
 * __media_device_register() - Registers a media device element
 *
 * @mdev:	pointer to struct &media_device
 * @owner:	should be filled with %THIS_MODULE
 *
 * Users, should, instead, call the media_device_register() macro.
 *
 * The caller is responsible for initializing the &media_device structure
 * before registration. The following fields of &media_device must be set:
 *
 *  - &media_device.model must be filled with the device model name as a
 *    NUL-terminated UTF-8 string. The device/model revision must not be
 *    stored in this field.
 *
 * The following fields are optional:
 *
 *  - &media_device.serial is a unique serial number stored as a
 *    NUL-terminated ASCII string. The field is big enough to store a GUID
 *    in text form. If the hardware doesn't provide a unique serial number
 *    this field must be left empty.
 *
 *  - &media_device.bus_info represents the location of the device in the
 *    system as a NUL-terminated ASCII string. For PCI/PCIe devices
 *    &media_device.bus_info must be set to "PCI:" (or "PCIe:") followed by
 *    the value of pci_name(). For USB devices,the usb_make_path() function
 *    must be used. This field is used by applications to distinguish between
 *    otherwise identical devices that don't provide a serial number.
 *
 *  - &media_device.hw_revision is the hardware device revision in a
 *    driver-specific format. When possible the revision should be formatted
 *    with the KERNEL_VERSION() macro.
 *
 * .. note::
 *
 *    #) Upon successful registration a character device named media[0-9]+ is created. The device major and minor numbers are dynamic. The model name is exported as a sysfs attribute.
 *
 *    #) Unregistering a media device that hasn't been registered is **NOT** safe.
 *
 * Return: returns zero on success or a negative error code.
 */
int __must_check __media_device_register(struct media_device *mdev,
					 struct module *owner);


/**
 * media_device_register() - Registers a media device element
 *
 * @mdev:	pointer to struct &media_device
 *
 * This macro calls __media_device_register() passing %THIS_MODULE as
 * the __media_device_register() second argument (**owner**).
 */
#define media_device_register(mdev) __media_device_register(mdev, THIS_MODULE)

/**
 * media_device_unregister() - Unregisters a media device element
 *
 * @mdev:	pointer to struct &media_device
 *
 * It is safe to call this function on an unregistered (but initialised)
 * media device.
 */
void media_device_unregister(struct media_device *mdev);

/* -----------------------------------------------------------------------------
 * media device context handling
 */

/**
 * media_device_context_get - Increase the media device context reference count
 *			      and return a reference to it
 * @ctx: The media device context
 */
struct media_device_context *
media_device_context_get(struct media_device_context *ctx);

/**
 * media_device_context_put - Decrease the media device context reference count
 * @ctx: The media device context
 */
void media_device_context_put(struct media_device_context *ctx);

/**
 * media_device_context_get_from_fd - Get the media device context associated with a
 *				      numerical file descriptor
 *
 * @fd: the numerical file descriptor
 *
 * A media device context is created whenever the media device devnode is opened
 * by userspace. It is then associated uniquely with a numerical file descriptor
 * which is unique in the userspace process context.
 *
 * This function allows to retrieve the media device associated with such
 * numerical file descriptor and increases the media device context reference
 * count to guarantee the returned reference stays valid at least until the
 * caller does not call media_device_context_put().
 *
 * Caller of this function are required to put the returned media device context
 * once they are done with it.
 *
 * The intended caller of this function is the VIDIOC_BIND_CONTEXT ioctl handler
 * which need to get the media device contexts associated to a numerical file
 * descriptor.
 */
struct media_device_context *media_device_context_get_from_fd(unsigned int fd);

/**
 * media_device_init_context - Initialize the media device context
 *
 * @mdev: The media device this context belongs to
 * @ctx: The media device context to initialize
 *
 * Initialize the fields of a media device context. Device drivers that support
 * multi context operations shall call this function in their implementation of
 * media_device_operations.alloc_context()
 */
int media_device_init_context(struct media_device *mdev,
			      struct media_device_context *ctx);

/**
 * media_device_cleanup_context - Cleanup the media device context
 *
 * @ctx: The media device context to clean up
 *
 * Cleanup a media device context. Device drivers that support multi context
 * operations shall call this function in their implementation of
 * media_device_operations.destroy_context() before releasing the memory allocated
 * by media_device_operations.alloc_context().
 */
void media_device_cleanup_context(struct media_device_context *ctx);

/**
 * media_device_bind_context - Bind an entity context to a media device context
 *
 * @mdev_context: pointer to struct &media_device_context
 * @context: the entity context to bind
 *
 * This function creates a mapping entry in the media device context that
 * associates an entity context to the media entity it belongs to and stores it
 * in a linked list so that they can be retrieved later.
 *
 * Binding an entity context to a media device context increases the media
 * device context refcount.
 *
 * The intended caller of this function is the VIDIOC_BIND_CONTEXT ioctl handler
 * that binds a newly created context to a media device context.
 */
int media_device_bind_context(struct media_device_context *mdev_context,
			      struct media_entity_context *context);

/**
 * media_device_unbind_context - Unbind an entity context from a media device
 *				 context
 *
 * @context: the entity context to unbind
 *
 * An entity context is unbound from a media device context when the file handle
 * it is associated with gets closed.
 *
 * Unbinding an entity context from a media device context decreases the media
 * device context refcount.
 *
 * Returns 0 if the context was bound to a media device context, -EINVAL
 * otherwise.
 */
int media_device_unbind_context(struct media_entity_context *context);

/**
 * media_device_get_entity_context - Get the entity context associated with
 *				     a media entity in a media device context
 *
 * @mdev_context: pointer to struct &media_device_context
 * @entity: pointer to struct &media_entity that the entity context is
 *	    associated with
 *
 * An entity context is uniquely associated with a media device context after it
 * has been bound to it by a call to the VIDIOC_BIND_CONTEXT ioctl. This helper
 * function retrieves the entity context associated with a media device context
 * for a specific entity that represents a video device or a v4l2 subdevice.
 *
 * The reference count of the returned entity context is increased to guarantee
 * the returned reference stays valid until the caller does not call
 * media_entity_context_put().
 *
 * Drivers are not expected to call this function directly but should instead
 * use the helpers provided by the video_device and v4l2_subdevice layers,
 * video_device_context_get() and v4l2_subdev_get_context() respectively.
 * Drivers are always required to decrease the returned context reference count
 * by calling video_device_context_put() and v4l2_subdev_put_context().
 *
 * If no entity context has been associated with the media device context
 * provided as first argument an error  pointer is returned. Drivers are
 * required to always check the value returned by this function.
 */
struct media_entity_context *
media_device_get_entity_context(struct media_device_context *mdev_context,
				struct media_entity *entity);

/*------------------------------------------------------------------------------
 * Media entity handling
 */

/**
 * media_device_register_entity() - registers a media entity inside a
 *	previously registered media device.
 *
 * @mdev:	pointer to struct &media_device
 * @entity:	pointer to struct &media_entity to be registered
 *
 * Entities are identified by a unique positive integer ID. The media
 * controller framework will such ID automatically. IDs are not guaranteed
 * to be contiguous, and the ID number can change on newer Kernel versions.
 * So, neither the driver nor userspace should hardcode ID numbers to refer
 * to the entities, but, instead, use the framework to find the ID, when
 * needed.
 *
 * The media_entity name, type and flags fields should be initialized before
 * calling media_device_register_entity(). Entities embedded in higher-level
 * standard structures can have some of those fields set by the higher-level
 * framework.
 *
 * If the device has pads, media_entity_pads_init() should be called before
 * this function. Otherwise, the &media_entity.pad and &media_entity.num_pads
 * should be zeroed before calling this function.
 *
 * Entities have flags that describe the entity capabilities and state:
 *
 * %MEDIA_ENT_FL_DEFAULT
 *    indicates the default entity for a given type.
 *    This can be used to report the default audio and video devices or the
 *    default camera sensor.
 *
 * .. note::
 *
 *    Drivers should set the entity function before calling this function.
 *    Please notice that the values %MEDIA_ENT_F_V4L2_SUBDEV_UNKNOWN and
 *    %MEDIA_ENT_F_UNKNOWN should not be used by the drivers.
 */
int __must_check media_device_register_entity(struct media_device *mdev,
					      struct media_entity *entity);

/**
 * media_device_unregister_entity() - unregisters a media entity.
 *
 * @entity:	pointer to struct &media_entity to be unregistered
 *
 * All links associated with the entity and all PADs are automatically
 * unregistered from the media_device when this function is called.
 *
 * Unregistering an entity will not change the IDs of the other entities and
 * the previoully used ID will never be reused for a newly registered entities.
 *
 * When a media device is unregistered, all its entities are unregistered
 * automatically. No manual entities unregistration is then required.
 *
 * .. note::
 *
 *    The media_entity instance itself must be freed explicitly by
 *    the driver if required.
 */
void media_device_unregister_entity(struct media_entity *entity);

/**
 * media_device_register_entity_notify() - Registers a media entity_notify
 *					   callback
 *
 * @mdev:      The media device
 * @nptr:      The media_entity_notify
 *
 * .. note::
 *
 *    When a new entity is registered, all the registered
 *    media_entity_notify callbacks are invoked.
 */

void media_device_register_entity_notify(struct media_device *mdev,
					struct media_entity_notify *nptr);

/**
 * media_device_unregister_entity_notify() - Unregister a media entity notify
 *					     callback
 *
 * @mdev:      The media device
 * @nptr:      The media_entity_notify
 *
 */
void media_device_unregister_entity_notify(struct media_device *mdev,
					struct media_entity_notify *nptr);

/* Iterate over all entities. */
#define media_device_for_each_entity(entity, mdev)			\
	list_for_each_entry(entity, &(mdev)->entities, graph_obj.list)

/* Iterate over all interfaces. */
#define media_device_for_each_intf(intf, mdev)			\
	list_for_each_entry(intf, &(mdev)->interfaces, graph_obj.list)

/* Iterate over all pads. */
#define media_device_for_each_pad(pad, mdev)			\
	list_for_each_entry(pad, &(mdev)->pads, graph_obj.list)

/* Iterate over all links. */
#define media_device_for_each_link(link, mdev)			\
	list_for_each_entry(link, &(mdev)->links, graph_obj.list)

/**
 * media_device_pci_init() - create and initialize a
 *	struct &media_device from a PCI device.
 *
 * @mdev:	pointer to struct &media_device
 * @pci_dev:	pointer to struct pci_dev
 * @name:	media device name. If %NULL, the routine will use the default
 *		name for the pci device, given by pci_name() macro.
 */
void media_device_pci_init(struct media_device *mdev,
			   struct pci_dev *pci_dev,
			   const char *name);
/**
 * __media_device_usb_init() - create and initialize a
 *	struct &media_device from a PCI device.
 *
 * @mdev:	pointer to struct &media_device
 * @udev:	pointer to struct usb_device
 * @board_name:	media device name. If %NULL, the routine will use the usb
 *		product name, if available.
 * @driver_name: name of the driver. if %NULL, the routine will use the name
 *		given by ``udev->dev->driver->name``, with is usually the wrong
 *		thing to do.
 *
 * .. note::
 *
 *    It is better to call media_device_usb_init() instead, as
 *    such macro fills driver_name with %KBUILD_MODNAME.
 */
void __media_device_usb_init(struct media_device *mdev,
			     struct usb_device *udev,
			     const char *board_name,
			     const char *driver_name);

#else
static inline void media_device_init(struct media_device *mdev)
{
}
static inline int media_device_register(struct media_device *mdev)
{
	return 0;
}
static inline void media_device_unregister(struct media_device *mdev)
{
}
static inline void media_device_cleanup(struct media_device *mdev)
{
}
static inline int media_device_register_entity(struct media_device *mdev,
						struct media_entity *entity)
{
	return 0;
}
static inline void media_device_unregister_entity(struct media_entity *entity)
{
}
static inline void media_device_register_entity_notify(
					struct media_device *mdev,
					struct media_entity_notify *nptr)
{
}
static inline void media_device_unregister_entity_notify(
					struct media_device *mdev,
					struct media_entity_notify *nptr)
{
}

static inline void media_device_pci_init(struct media_device *mdev,
					 struct pci_dev *pci_dev,
					 char *name)
{
}

static inline void __media_device_usb_init(struct media_device *mdev,
					   struct usb_device *udev,
					   char *board_name,
					   char *driver_name)
{
}

#endif /* CONFIG_MEDIA_CONTROLLER */

/**
 * media_device_usb_init() - create and initialize a
 *	struct &media_device from a PCI device.
 *
 * @mdev:	pointer to struct &media_device
 * @udev:	pointer to struct usb_device
 * @name:	media device name. If %NULL, the routine will use the usb
 *		product name, if available.
 *
 * This macro calls media_device_usb_init() passing the
 * media_device_usb_init() **driver_name** parameter filled with
 * %KBUILD_MODNAME.
 */
#define media_device_usb_init(mdev, udev, name) \
	__media_device_usb_init(mdev, udev, name, KBUILD_MODNAME)

/**
 * media_set_bus_info() - Set bus_info field
 *
 * @bus_info:		Variable where to write the bus info (char array)
 * @bus_info_size:	Length of the bus_info
 * @dev:		Related struct device
 *
 * Sets bus information based on &dev. This is currently done for PCI and
 * platform devices. dev is required to be non-NULL for this to happen.
 *
 * This function is not meant to be called from drivers.
 */
static inline void
media_set_bus_info(char *bus_info, size_t bus_info_size, struct device *dev)
{
	if (!dev)
		strscpy(bus_info, "no bus info", bus_info_size);
	else if (dev_is_platform(dev))
		snprintf(bus_info, bus_info_size, "platform:%s", dev_name(dev));
	else if (dev_is_pci(dev))
		snprintf(bus_info, bus_info_size, "PCI:%s", dev_name(dev));
}

#endif
