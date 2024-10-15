=====================================================
The Linux Remoteproc subsystem Driver Core kernel API
=====================================================

anish kumar <yesanishhere@gmail.com>

Introduction
------------
This document does not describe what a Remote processor subsystem
(RPROC) Driver or Device is. It also does not describe the API
which can be used by user space to communicate with a RPROC driver.
If you want to know this then please read the following
file: Documentation/remotproc/remoteproc-api.rst .

So what does this document describe? It describes the API that can be used by
remote processor Drivers that want to use the remote processor Driver Core
Framework. This framework provides all interfacing towards user space so that
the same code does not have to be reproduced each time. This also means that
a remote processor driver then only needs to provide the different routines
(operations) that control the remote processor.

The API
-------
Each remote processor driver that wants to use the remote processor Driver Core
must #include <linux/remoteproc.h> (you would have to do this anyway when
writing a rproc device driver). This include file contains following
register routine::

	int devm_rproc_add(struct device *dev, struct rproc *rproc)

The devm_rproc_add routine registers a remote processor device.
The parameter of this routine is a pointer to a rproc device structure.
This routine returns zero on success and a negative errno code for failure.

The rproc device structure looks like this::

  struct rproc {
	struct list_head node;
	struct iommu_domain *domain;
	const char *name;
	const char *firmware;
	void *priv;
	struct rproc_ops *ops;
	struct device dev;
	atomic_t power;
	unsigned int state;
	enum rproc_dump_mechanism dump_conf;
	struct mutex lock;
	struct dentry *dbg_dir;
	struct list_head traces;
	int num_traces;
	struct list_head carveouts;
	struct list_head mappings;
	u64 bootaddr;
	struct list_head rvdevs;
	struct list_head subdevs;
	struct idr notifyids;
	int index;
	struct work_struct crash_handler;
	unsigned int crash_cnt;
	bool recovery_disabled;
	int max_notifyid;
	struct resource_table *table_ptr;
	struct resource_table *clean_table;
	struct resource_table *cached_table;
	size_t table_sz;
	bool has_iommu;
	bool auto_boot;
	bool sysfs_read_only;
	struct list_head dump_segments;
	int nb_vdev;
	u8 elf_class;
	u16 elf_machine;
	struct cdev cdev;
	bool cdev_put_on_release;
	DECLARE_BITMAP(features, RPROC_MAX_FEATURES);
  };

It contains following fields:

* node: list node of this rproc object
* domain: iommu domain
* name: human readable name of the rproc
* firmware: name of firmware file to be loaded
* priv: private data which belongs to the platform-specific rproc module
* ops: platform-specific start/stop rproc handlers
* dev: virtual device for refcounting and common remoteproc behavior
* power: refcount of users who need this rproc powered up
* state: state of the device
* dump_conf: Currently selected coredump configuration
* lock: lock which protects concurrent manipulations of the rproc
* dbg_dir: debugfs directory of this rproc device
* traces: list of trace buffers
* num_traces: number of trace buffers
* carveouts: list of physically contiguous memory allocations
* mappings: list of iommu mappings we initiated, needed on shutdown
* bootaddr: address of first instruction to boot rproc with (optional)
* rvdevs: list of remote virtio devices
* subdevs: list of subdevices, to following the running state
* notifyids: idr for dynamically assigning rproc-wide unique notify ids
* index: index of this rproc device
* crash_handler: workqueue for handling a crash
* crash_cnt: crash counter
* recovery_disabled: flag that state if recovery was disabled
* max_notifyid: largest allocated notify id.
* table_ptr: pointer to the resource table in effect
* clean_table: copy of the resource table without modifications.  Used
*      	 when a remote processor is attached or detached from the core
* cached_table: copy of the resource table
* table_sz: size of @cached_table
* has_iommu: flag to indicate if remote processor is behind an MMU
* auto_boot: flag to indicate if remote processor should be auto-started
* sysfs_read_only: flag to make remoteproc sysfs files read only
* dump_segments: list of segments in the firmware
* nb_vdev: number of vdev currently handled by rproc
* elf_class: firmware ELF class
* elf_machine: firmware ELF machine
* cdev: character device of the rproc
* cdev_put_on_release: flag to indicate if remoteproc should be shutdown on @char_dev release
* features: indicate remoteproc features

The list of rproc operations is defined as::

  struct rproc_ops {
	int (*prepare)(struct rproc *rproc);
	int (*unprepare)(struct rproc *rproc);
	int (*start)(struct rproc *rproc);
	int (*stop)(struct rproc *rproc);
	int (*attach)(struct rproc *rproc);
	int (*detach)(struct rproc *rproc);
	void (*kick)(struct rproc *rproc, int vqid);
	void * (*da_to_va)(struct rproc *rproc, u64 da, size_t len, bool *is_iomem);
	int (*parse_fw)(struct rproc *rproc, const struct firmware *fw);
	int (*handle_rsc)(struct rproc *rproc, u32 rsc_type, void *rsc,
			  int offset, int avail);
	struct resource_table *(*find_loaded_rsc_table)(
				struct rproc *rproc, const struct firmware *fw);
	struct resource_table *(*get_loaded_rsc_table)(
				struct rproc *rproc, size_t *size);
	int (*load)(struct rproc *rproc, const struct firmware *fw);
	int (*sanity_check)(struct rproc *rproc, const struct firmware *fw);
	u64 (*get_boot_addr)(struct rproc *rproc, const struct firmware *fw);
	unsigned long (*panic)(struct rproc *rproc);
	void (*coredump)(struct rproc *rproc);
  };

Most of the operations are optional. Currently in the implementation
there are no mandatory operations, however from the practical standpoint
minimum ops are:

* start: this is a pointer to the routine that starts the remote processor
  device.
  The routine needs a pointer to the remote processor device structure as a
  parameter. It returns zero on success or a negative errno code for failure.

* stop: with this routine the remote processor device is being stopped.

  The routine needs a pointer to the remote processor device structure as a
  parameter. It returns zero on success or a negative errno code for failure.

* da_to_va: this is the routine that needs to translate device address to
  application processor virtual address that it can copy code to.

  The routine needs a pointer to the remote processor device structure as a
  parameter. It returns zero on success or a negative errno code for failure.

  The routine provides the device address it finds in the ELF firmware and asks
  the driver to convert that to virtual address.

All other callbacks are optional in case of ELF provided firmware.

* load: this is to load the firmware on to the remote device.

  The routine needs firmware file that it needs to load on to the remote processor.
  If the driver overrides this callback then default ELF loader will not get used.
  Otherwise default framework provided loader gets used.

  load = rproc_elf_load_segments;
  parse_fw = rproc_elf_load_rsc_table;
  find_loaded_rsc_table = rproc_elf_find_loaded_rsc_table;
  sanity_check = rproc_elf_sanity_check;
  get_boot_addr = rproc_elf_get_boot_addr;

* parse_fw: this routing parses the provided firmware. In case of ELF format,
  framework provided rproc_elf_load_rsc_table function can be used.

* sanity_check: Check the format of the firmware.

* coredump: If the driver prefers to manage coredumps independently, it can
  implement its own coredump handling. However, the framework offers a default
  implementation for the ELF format by assigning this callback to
  rproc_coredump, unless the driver has overridden it.

* get_boot_addr: In case the bootaddr defined in ELF firmware is different, driver
  can use this callback to set a different boot address for remote processor to
  starts its reset vector from.

* find_loaded_rsc_table: this routine gets the loaded resource table from the firmware.

  resource table should have a section named (.resource_table) for the framework
  to understand and interpret its content. Resource table is a way for remote
  processor to ask for resources such as memory for dumping and logging. Look
  at core documentation to know how to create the ELF section for the same.

* get_loaded_rsc_table: Driver can customize passing the resource table by overriding
  this callback. Framework doesn't provide any default implementation for the same.


The rproc_report_crash function allows you to report a crash when crash is
detected by the driver.

::

  void rproc_report_crash(struct rproc *rproc, enum rproc_crash_type type);

To add a subdev corresponding driver can call::

  void rproc_add_subdev(struct rproc *rproc, struct rproc_subdev *subdev);

To remove a subdev, driver can call.

::

  void rproc_remove_subdev(struct rproc *rproc, struct rproc_subdev *subdev);

To work with ELF coredump below function can be called::

  void rproc_coredump_cleanup(struct rproc *rproc);
  void rproc_coredump(struct rproc *rproc);
  void rproc_coredump_using_sections(struct rproc *rproc);
  int rproc_coredump_add_segment(struct rproc *rproc, dma_addr_t da, size_t size);
  int rproc_coredump_add_custom_segment(struct rproc *rproc,
                                        dma_addr_t da, size_t size,
                                        void (*dumpfn)(struct rproc *rproc,
                                           struct rproc_dump_segment *segment,
                                           void *dest, size_t offset,
                                           size_t size),

Remember that coredump functions provided by the framework only works with ELF format.
