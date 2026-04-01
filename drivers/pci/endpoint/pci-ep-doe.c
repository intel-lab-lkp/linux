// SPDX-License-Identifier: GPL-2.0-only or MIT
/*
 * Data Object Exchange for PCIe Endpoint
 *	PCIe r7.0, sec 6.30 DOE
 *
 * Copyright (C) 2026 Texas Instruments Incorporated - https://www.ti.com
 *	Aksh Garg <a-garg7@ti.com>
 *	Siddharth Vadapalli <s-vadapalli@ti.com>
 */

#define dev_fmt(fmt) "DOE EP: " fmt

#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/pci.h>
#include <linux/pci-epc.h>
#include <linux/pci-doe.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>

#include "../pci.h"

/* Forward declaration of discovery protocol handler */
static int pci_ep_doe_handle_discovery(const void *request, size_t request_sz,
				       void **response, size_t *response_sz);

/**
 * struct pci_doe_protocol - DOE protocol handler entry
 * @vid: Vendor ID
 * @type: Protocol type
 * @handler: Handler function pointer
 */
struct pci_doe_protocol {
	u16 vid;
	u8 type;
	pci_doe_protocol_handler_t handler;
};

/**
 * struct pci_ep_doe_mb - State for a single DOE mailbox on EP
 *
 * This state is used to manage a single DOE mailbox capability on the
 * endpoint side.
 *
 * @epc: PCI endpoint controller this mailbox belongs to
 * @func_no: Physical function number of the function this mailbox belongs to
 * @cap_offset: Capability offset
 * @work_queue: Queue of work items
 * @flags: Bit array of PCI_DOE_FLAG_* flags
 */
struct pci_ep_doe_mb {
	struct pci_epc *epc;
	u8 func_no;
	u16 cap_offset;
	struct workqueue_struct *work_queue;
	unsigned long flags;
};

/**
 * struct pci_ep_doe_task - Represents a single DOE request/response task
 *
 * @feat: DOE feature (vendor ID and type)
 * @request_pl: Request payload
 * @request_pl_sz: Size of request payload in bytes
 * @response_pl: Response buffer
 * @response_pl_sz: Size of response buffer in bytes
 * @complete: Completion callback
 * @work: Work structure for workqueue
 * @doe_mb: DOE mailbox handling this task
 */
struct pci_ep_doe_task {
	struct pci_doe_feature feat;
	const void *request_pl;
	size_t request_pl_sz;
	void *response_pl;
	size_t response_pl_sz;
	pci_ep_doe_complete_t complete;

	/* Initialized by pci_ep_doe_submit_task() */
	struct work_struct work;
	struct pci_ep_doe_mb *doe_mb;
};

/*
 * Global registry of protocol handlers.
 * When a new DOE protocol, library is added, add an entry to this array.
 */
static const struct pci_doe_protocol pci_doe_protocols[] = {
	{
		.vid = PCI_VENDOR_ID_PCI_SIG,
		.type = PCI_DOE_FEATURE_DISCOVERY,
		.handler = pci_ep_doe_handle_discovery,
	},
};

/*
 * Combines function number and capability offset into a unique lookup key
 * for storing/retrieving DOE mailboxes in an xarray.
 */
#define PCI_DOE_MB_KEY(func, offset) \
	(((unsigned long)(func) << 16) | (offset))
#define PCI_DOE_PROTOCOL_COUNT        ARRAY_SIZE(pci_doe_protocols)

/**
 * pci_ep_doe_init() - Initialize the DOE framework for a controller in EP mode
 * @epc: PCI endpoint controller
 *
 * Initialize the DOE framework data structures. This only initializes
 * the xarray that will hold the mailboxes.
 *
 * RETURNS: 0 on success, -errno on failure
 */
int pci_ep_doe_init(struct pci_epc *epc)
{
	if (!epc)
		return -EINVAL;

	xa_init(&epc->doe_mbs);
	return 0;
}
EXPORT_SYMBOL_GPL(pci_ep_doe_init);

/**
 * pci_ep_doe_add_mailbox() - Add a DOE mailbox for a physical function
 * @epc: PCI endpoint controller
 * @func_no: Physical function number
 * @cap_offset: Offset of the DOE capability
 *
 * Create and register a DOE mailbox for the specified physical function
 * and capability offset.
 *
 * EPC core driver calls this for each DOE capability discovered in the config
 * space of each endpoint function through an API. The API is invoked by the
 * controller driver during initialization if DOE support is available.
 *
 * RETURNS: 0 on success, -errno on failure
 */
int pci_ep_doe_add_mailbox(struct pci_epc *epc, u8 func_no, u16 cap_offset)
{
	struct pci_ep_doe_mb *doe_mb;
	unsigned long key;
	int ret;

	if (!epc)
		return -EINVAL;

	doe_mb = kzalloc_obj(*doe_mb, GFP_KERNEL);
	if (!doe_mb)
		return -ENOMEM;

	doe_mb->epc = epc;
	doe_mb->func_no = func_no;
	doe_mb->cap_offset = cap_offset;

	doe_mb->work_queue = alloc_ordered_workqueue("pci_ep_doe[%s:pf%d:offset%x]", 0,
						     dev_name(&epc->dev),
						     func_no, cap_offset);
	if (!doe_mb->work_queue) {
		dev_err(epc->dev.parent,
			"[pf%d:offset%x] failed to allocate work queue\n",
			func_no, cap_offset);
		ret = -ENOMEM;
		goto err_free;
	}

	/* Add to xarray with composite key */
	key = PCI_DOE_MB_KEY(func_no, cap_offset);
	ret = xa_insert(&epc->doe_mbs, key, doe_mb, GFP_KERNEL);
	if (ret) {
		dev_err(epc->dev.parent,
			"[pf%d:offset%x] failed to insert mailbox: %d\n",
			func_no, cap_offset, ret);
		goto err_destroy;
	}

	dev_dbg(epc->dev.parent,
		"DOE mailbox added: pf%d offset 0x%x\n",
		func_no, cap_offset);

	return 0;

err_destroy:
	destroy_workqueue(doe_mb->work_queue);
err_free:
	kfree(doe_mb);
	return ret;
}
EXPORT_SYMBOL_GPL(pci_ep_doe_add_mailbox);

/**
 * pci_ep_doe_cancel_tasks() - Cancel all pending tasks
 * @doe_mb: DOE mailbox
 *
 * Cancel all pending tasks in the mailbox. Mark the mailbox as dead
 * so no new tasks can be submitted.
 */
static void pci_ep_doe_cancel_tasks(struct pci_ep_doe_mb *doe_mb)
{
	if (!doe_mb)
		return;

	/* Mark the mailbox as dead */
	set_bit(PCI_DOE_FLAG_DEAD, &doe_mb->flags);

	/* Stop all pending work items from starting */
	set_bit(PCI_DOE_FLAG_CANCEL, &doe_mb->flags);
}

/**
 * pci_ep_doe_get_mailbox() - Get DOE mailbox by function and offset
 * @epc: PCI endpoint controller
 * @func_no: Physical function number
 * @cap_offset: Offset of the DOE capability
 *
 * Internal helper to look up a DOE mailbox by its function number and
 * capability offset.
 *
 * RETURNS: Pointer to the mailbox or NULL if not found
 */
static struct pci_ep_doe_mb *pci_ep_doe_get_mailbox(struct pci_epc *epc,
						    u8 func_no, u16 cap_offset)
{
	unsigned long key;

	if (!epc)
		return NULL;

	key = PCI_DOE_MB_KEY(func_no, cap_offset);
	return xa_load(&epc->doe_mbs, key);
}

/**
 * pci_ep_doe_find_protocol() - Find protocol handler in static array
 * @vendor: Vendor ID
 * @type: Protocol type
 *
 * Look up a protocol handler in the static protocol array by matching vendor ID
 * and protocol type.
 *
 * RETURNS: Handler function pointer or NULL if not found
 */
static pci_doe_protocol_handler_t pci_ep_doe_find_protocol(u16 vendor, u8 type)
{
	int i;

	/* Search static protocol array */
	for (i = 0; i < PCI_DOE_PROTOCOL_COUNT; i++) {
		if (pci_doe_protocols[i].vid == vendor &&
		    pci_doe_protocols[i].type == type)
			return pci_doe_protocols[i].handler;
	}

	return NULL;
}

/**
 * pci_ep_doe_handle_discovery() - Handle Discovery protocol request
 * @request: Request payload
 * @request_sz: Request size
 * @response: Output pointer for response buffer
 * @response_sz: Output pointer for response size
 *
 * Handle the DOE Discovery protocol. The request contains an index specifying
 * which protocol to query. This function creates a response containing the
 * vendor ID and protocol type for the requested index, along with the next
 * index value for further discovery:
 *
 * - next_index = 0: Signals this is the last protocol supported
 * - next_index = n (non-zero): Signals more protocols available,
 *   query index n next
 *
 * RETURNS: 0 on success, -errno on failure
 */
static int pci_ep_doe_handle_discovery(const void *request, size_t request_sz,
				       void **response, size_t *response_sz)
{
	struct pci_doe_protocol protocol;
	u8 requested_index, next_index;
	u32 *response_pl;
	u32 request_pl;
	u16 vendor;
	u8 type;

	if (request_sz != sizeof(u32))
		return -EINVAL;

	request_pl = *(u32 *)request;
	requested_index = FIELD_GET(PCI_DOE_DATA_OBJECT_DISC_REQ_3_INDEX, request_pl);

	if (requested_index >= PCI_DOE_PROTOCOL_COUNT)
		return -EINVAL;

	/* Get protocol from array at requested_index */
	protocol = pci_doe_protocols[requested_index];
	vendor = protocol.vid;
	type = protocol.type;

	/* Calculate next index */
	next_index = (requested_index + 1 < PCI_DOE_PROTOCOL_COUNT) ? requested_index + 1 : 0;

	response_pl = kzalloc_obj(*response_pl, GFP_KERNEL);
	if (!response_pl)
		return -ENOMEM;

	/* Build response */
	*response_pl = FIELD_PREP(PCI_DOE_DATA_OBJECT_DISC_RSP_3_VID, vendor) |
		       FIELD_PREP(PCI_DOE_DATA_OBJECT_DISC_RSP_3_TYPE, type) |
		       FIELD_PREP(PCI_DOE_DATA_OBJECT_DISC_RSP_3_NEXT_INDEX, next_index);

	*response = response_pl;
	*response_sz = sizeof(*response_pl);

	return 0;
}

static void signal_task_complete(struct pci_ep_doe_task *task, int status)
{
	kfree(task->request_pl);
	task->complete(task->doe_mb->func_no, task->doe_mb->cap_offset, status,
		       task->feat.vid, task->feat.type,
		       task->response_pl, task->response_pl_sz);
	kfree(task);
}

/**
 * doe_ep_task_work() - Work function for processing DOE EP tasks
 * @work: Work structure
 *
 * Process a DOE request by calling the appropriate protocol handler.
 */
static void doe_ep_task_work(struct work_struct *work)
{
	struct pci_ep_doe_task *task = container_of(work, struct pci_ep_doe_task,
						    work);
	struct pci_ep_doe_mb *doe_mb = task->doe_mb;
	pci_doe_protocol_handler_t handler;
	int rc;

	if (test_bit(PCI_DOE_FLAG_DEAD, &doe_mb->flags)) {
		signal_task_complete(task, -EIO);
		return;
	}

	/* Check if request was aborted */
	if (test_bit(PCI_DOE_FLAG_CANCEL, &doe_mb->flags)) {
		signal_task_complete(task, -ECANCELED);
		return;
	}

	/* Find protocol handler in the array */
	handler = pci_ep_doe_find_protocol(task->feat.vid, task->feat.type);
	if (!handler) {
		dev_warn(doe_mb->epc->dev.parent,
			 "[%d:%x] Unsupported protocol VID=%04x TYPE=%02x\n",
			 doe_mb->func_no, doe_mb->cap_offset,
			 task->feat.vid, task->feat.type);
		signal_task_complete(task, -EOPNOTSUPP);
		return;
	}

	/* Call protocol handler */
	rc = handler(task->request_pl, task->request_pl_sz,
		     &task->response_pl, &task->response_pl_sz);

	signal_task_complete(task, rc);
}

/**
 * pci_ep_doe_submit_task() - Submit a task to be processed
 * @doe_mb: DOE mailbox
 * @task: Task to submit
 *
 * Submit a DOE task to the workqueue for asynchronous processing.
 *
 * RETURNS: 0 on success, -errno on failure
 */
static int pci_ep_doe_submit_task(struct pci_ep_doe_mb *doe_mb,
				  struct pci_ep_doe_task *task)
{
	if (test_bit(PCI_DOE_FLAG_DEAD, &doe_mb->flags))
		return -EIO;

	task->doe_mb = doe_mb;
	INIT_WORK(&task->work, doe_ep_task_work);
	queue_work(doe_mb->work_queue, &task->work);
	return 0;
}

/**
 * pci_ep_doe_process_request() - Process DOE request on endpoint
 * @epc: PCI endpoint controller
 * @func_no: Physical function number
 * @cap_offset: DOE capability offset
 * @vendor: Vendor ID from request header
 * @type: Protocol type from request header
 * @request: Request payload in CPU-native format
 * @request_sz: Size of request payload (bytes)
 * @complete: Callback to invoke upon completion
 *
 * Asynchronously process a DOE request received on the endpoint. The request
 * payload should not include the DOE header (vendor/type/length). The protocol
 * handler will allocate the response buffer, which the caller (controller driver)
 * must free after use.
 *
 * This function returns immediately after queuing the request. The completion
 * callback will be invoked asynchronously from workqueue context once the
 * request is processed. The callback receives the function number and capability
 * offset to identify the mailbox, along with a status code (0 on success, -errno
 * on failure), and other required arguments.
 *
 * As per DOE specification, a mailbox processes one request at a time.
 * Therefore, this function will never be called concurrently for the same
 * mailbox by different callers.
 *
 * The caller is responsible for the conversion of the received DOE request
 * with le32_to_cpu() before calling this function.
 * Similarly, it is responsible for converting the response payload with
 * cpu_to_le32() before sending it back over the DOE mailbox.
 *
 * The caller is also responsible for ensuring that the request size
 * is within the limits defined by PCI_DOE_MAX_LENGTH.
 *
 * RETURNS: 0 if the request was successfully queued, -errno on failure
 */
int pci_ep_doe_process_request(struct pci_epc *epc, u8 func_no, u16 cap_offset,
			       u16 vendor, u8 type, const void *request, size_t request_sz,
			       pci_ep_doe_complete_t complete)
{
	struct pci_ep_doe_mb *doe_mb;
	struct pci_ep_doe_task *task;
	int rc;

	doe_mb = pci_ep_doe_get_mailbox(epc, func_no, cap_offset);
	if (!doe_mb) {
		kfree(request);
		return -ENODEV;
	}

	task = kzalloc_obj(*task, GFP_KERNEL);
	if (!task) {
		kfree(request);
		return -ENOMEM;
	}

	task->feat.vid = vendor;
	task->feat.type = type;
	task->request_pl = request;
	task->request_pl_sz = request_sz;
	task->response_pl = NULL;
	task->response_pl_sz = 0;
	task->complete = complete;

	rc = pci_ep_doe_submit_task(doe_mb, task);
	if (rc) {
		kfree(request);
		kfree(task);
		return rc;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(pci_ep_doe_process_request);

/**
 * pci_ep_doe_abort() - Abort DOE operations on a mailbox
 * @epc: PCI endpoint controller
 * @func_no: Physical function number
 * @cap_offset: DOE capability offset
 *
 * Abort all queued and wait for in-flight DOE operations to complete for the
 * specified mailbox. This function is called by the EP controller driver
 * when the RC sets the ABORT bit in the DOE Control register.
 *
 * The function will:
 *
 * - Set CANCEL flag to prevent new requests in the queue from starting
 * - Wait for the currently executing handler to complete (cannot interrupt)
 * - Flush the workqueue to wait for all requests to be handled appropriately
 * - Clear CANCEL flag to prepare for new requests
 *
 * RETURNS: 0 on success, -errno on failure
 */
int pci_ep_doe_abort(struct pci_epc *epc, u8 func_no, u16 cap_offset)
{
	struct pci_ep_doe_mb *doe_mb;

	if (!epc)
		return -EINVAL;

	doe_mb = pci_ep_doe_get_mailbox(epc, func_no, cap_offset);
	if (!doe_mb)
		return -ENODEV;

	/* Set CANCEL flag - worker will abort queued requests */
	set_bit(PCI_DOE_FLAG_CANCEL, &doe_mb->flags);
	flush_workqueue(doe_mb->work_queue);

	/* Clear CANCEL flag - mailbox ready for new requests */
	clear_bit(PCI_DOE_FLAG_CANCEL, &doe_mb->flags);

	dev_dbg(epc->dev.parent,
		"DOE mailbox aborted: PF%d offset 0x%x\n",
		func_no, cap_offset);

	return 0;
}
EXPORT_SYMBOL_GPL(pci_ep_doe_abort);

/**
 * pci_ep_doe_destroy_mb() - Destroy a single DOE mailbox
 * @doe_mb: DOE mailbox to destroy
 *
 * Internal function to destroy a mailbox and free its resources.
 */
static void pci_ep_doe_destroy_mb(struct pci_ep_doe_mb *doe_mb)
{
	if (!doe_mb)
		return;

	pci_ep_doe_cancel_tasks(doe_mb);

	if (doe_mb->work_queue)
		destroy_workqueue(doe_mb->work_queue);

	kfree(doe_mb);
}

/**
 * pci_ep_doe_destroy() - Destroy all DOE mailboxes
 * @epc: PCI endpoint controller
 *
 * Destroy all DOE mailboxes and free associated resources.
 *
 * The EPC core driver calls this through an API, invoked by the controller
 * driver during controller cleanup to free all DOE resources,
 * if DOE support is available.
 */
void pci_ep_doe_destroy(struct pci_epc *epc)
{
	struct pci_ep_doe_mb *doe_mb;
	unsigned long index;

	if (!epc)
		return;

	xa_for_each(&epc->doe_mbs, index, doe_mb)
		pci_ep_doe_destroy_mb(doe_mb);

	xa_destroy(&epc->doe_mbs);
}
EXPORT_SYMBOL_GPL(pci_ep_doe_destroy);
