// SPDX-License-Identifier: GPL-2.0-only
/*
 * LSTP USB interface driver.
 *
 * Copyright (c) 2026, NVIDIA CORPORATION & AFFILIATES.  All rights reserved.
 */

#include <linux/acpi.h>
#include <linux/atomic.h>
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/build_bug.h>
#include <linux/cleanup.h>
#include <linux/delay.h>
#include <linux/idr.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/overflow.h>
#include <linux/property.h>
#include <linux/spinlock.h>
#include <linux/uio.h>
#include <linux/usb.h>
#include <linux/wait_bit.h>
#include <linux/workqueue.h>

#include <linux/usb/lstp.h>

#define LSTP_VERSION 1
#define LSTP_USB_REQUEST_TIMEOUT_MS 1000
#define LSTP_USB_EP_MAX_SIZE 512
#define LSTP_MAX_CHANNELS 256 /* Includes channel 0 */
#define LSTP_ANY_RX_LEN 0 /* "no minimum" -- callers without a known min payload length */
#define LSTP_READ_CONFIG_MAX U16_MAX /* sentinel: max ch_config bytes per USB RX packet */
#define LSTP_INTF_NAME_LEN (LSTP_CH_NAME_LEN + 1) /* ch0_name + '\0' */

#define LSTP_CH_FLAG_ENABLE BIT(0)

#define LSTP_BUFFER_LOCK_BIT 0 /* resp_buffer_lock, irq_buffer_lock, irq_resp_buffer_lock */

/* Fields of lstp_header.cmd and lstp_header.status */
#define LSTP_HDR_CODE GENMASK(6, 0)
#define LSTP_HDR_RESPONSE BIT(7)

enum lstp_channel_type {
	LSTP_CHANNEL_TYPE_MGMT = 0x00,
	LSTP_CHANNEL_TYPE_SPI = 0x01,
	LSTP_CHANNEL_TYPE_GPIO = 0x02,
	LSTP_CHANNEL_TYPE_I2C = 0x03,
	LSTP_CHANNEL_TYPE_UART = 0x04,
	LSTP_CHANNEL_TYPE_IPMI = 0x05,
	LSTP_CHANNEL_TYPE_MMIO = 0x06,
	LSTP_CHANNEL_TYPE_MAX, /* sentinel, not a wire value; keep last */
};

struct lstp_ch0_read_req {
	u8 ch_id;
	__le16 offset;
	__le16 length;
} __packed;

struct lstp_ch0_write_req {
	u8 ch_id;
	__le16 offset;
	struct lstp_ch_desc desc;
	u8 ch_config[];
} __packed;

/* Process-context resubmit path for RX completions whose usb_submit_urb() failed. */
struct lstp_rx_retry {
	struct delayed_work work;
	int last_err;
	unsigned int failures;
};

/**
 * struct lstp_usb_stats - Shared bulk-in endpoint counters, ``lstp/statistics/``.
 * @rx_packets:    Packets completed successfully on the endpoint.
 * @rx_errors:     Completions rejected by lstp_validate_rx_pkt().
 * @rx_no_channel: Well-formed packets naming an unregistered channel.
 * @rx_urb_errors: Error completions other than the teardown codes.
 * @rx_stalls:     Endpoint halts cleared by lstp_rx_retry_work().
 * @rx_retries:    Resubmit attempts made by lstp_rx_retry_work().
 */
struct lstp_usb_stats {
	atomic64_t rx_packets;
	atomic64_t rx_errors;
	atomic64_t rx_no_channel;
	atomic64_t rx_urb_errors;
	atomic64_t rx_stalls;
	atomic64_t rx_retries;
};

struct lstp_usb {
	struct usb_device *udev;
	struct usb_interface *intf;
	u8 bulk_in_ep;
	u8 bulk_out_ep;
	struct urb *bulk_rx_urb;
	struct lstp_rx_retry bulk_rx_retry;
	u8 *rx_buf;
	size_t bulk_tx_size;
	size_t bulk_rx_size;
	u8 lstp_version;
	char lstp_intf_name[LSTP_INTF_NAME_LEN];
	u8 max_ch_id;
	/*
	 * Published with smp_store_release() by lstp_create_channel(); the RX
	 * completion and the disconnect fence smp_load_acquire() before they
	 * dereference. Plain loads only where the read is already so ordered.
	 */
	struct lstp_channel *channels[LSTP_MAX_CHANNELS];
	/* Node the channel nodes hang off, or NULL; see lstp_find_intf_fwnode() */
	struct fwnode_handle *intf_fwnode;
	struct lstp_usb_stats stats;
	struct notifier_block aux_nb; /* auxiliary-bus bind/unbind hook (one per device) */
	const struct bus_type *aux_bus; /* bus @aux_nb is registered on */
};

static DEFINE_IDA(lstp_auxdev_ida);

/*
 * Channel containers and lstp/ kobjects not yet released. Their release runs
 * this module's code and can come after disconnect, so module exit waits until
 * none remain.
 */
static atomic_t lstp_live_objects = ATOMIC_INIT(0);

enum lstp_ch0_cmd {
	LSTP_CH0_CMD_READ_CONFIG = 0x08,
	LSTP_CH0_CMD_WRITE_CONFIG = 0x09,
	LSTP_CH0_CMD_LOCK = 0x0A,
};

struct lstp_ch0_config {
	u8 lstp_version;
	u8 num_channels; /* does not include ch0 */
} __packed;

/**
 * struct lstp_channel_stats - Counters for one channel's LSTP exchanges.
 * @tx_packets: Requests handed to the host controller.
 * @rx_packets: Responses accepted by lstp_validate_resp().
 * @rx_events:  Unsolicited requests delivered to the bound driver.
 * @timeouts:   Requests unanswered within LSTP_USB_RESPONSE_TIMEOUT_MS.
 * @errors:     Responses rejected by lstp_validate_resp().
 * @tx_errors:  Requests that failed to submit.
 * @rx_dropped: Inbound packets the channel could not accept.
 * @rx_late:    Responses that arrived with no waiter, or after the waiter's
 *              answer. A lower bound: nothing on the wire ties a response to
 *              its request.
 *
 * Monotonic; published read-only in the channel's ``statistics`` sysfs group.
 */
struct lstp_channel_stats {
	atomic64_t tx_packets;
	atomic64_t rx_packets;
	atomic64_t rx_events;
	atomic64_t timeouts;
	atomic64_t errors;
	atomic64_t tx_errors;
	atomic64_t rx_dropped;
	atomic64_t rx_late;
};

/**
 * enum lstp_auxdev_state - Lifecycle state of a channel's embedded auxdev.
 * @LSTP_AUXDEV_RAW:    Never auxiliary_device_init'd, owns no instance id: ch0,
 *                      an unsupported channel type, or a failure before init.
 * @LSTP_AUXDEV_INITED: auxiliary_device_init() done; not yet on the bus.
 * @LSTP_AUXDEV_ADDED:  auxiliary_device_add() done; live on the auxiliary bus.
 */
enum lstp_auxdev_state {
	LSTP_AUXDEV_RAW = 0,
	LSTP_AUXDEV_INITED,
	LSTP_AUXDEV_ADDED,
};

/**
 * struct lstp_channel_type_info - What the core needs to know of a channel type.
 * @name:           Auxdev name; the channel's auxdev is "lstp.<name>.<id>".
 * @compatible:     "compatible" of the channel's firmware node, or NULL if the
 *                  type has none.
 * @irq_needs_resp: The device waits for a response to each request it sends,
 *                  so the core NACKs one no driver takes.
 *
 * Kept in the core: channels are created and matched to firmware nodes
 * whether or not their driver is loaded.
 */
struct lstp_channel_type_info {
	const char *name;
	const char *compatible;
	bool irq_needs_resp;
};

/**
 * struct lstp_channel_priv - Framework-private extension of struct lstp_channel.
 * @pub:                  Public, subsystem-visible part. Must be first.
 * @auxdev:               Per-channel auxiliary_device; its refcount owns this
 *                        container, see lstp_channel_dev_release().
 * @auxdev_state:         Lifecycle state of @auxdev.
 * @ch_type:              LSTP channel type.
 * @type:                 @ch_type's entry in lstp_channel_types[], or NULL
 *                        for ch0 and for a channel that was skipped.
 * @usb:                  Back-pointer to the LSTP USB device.
 * @tx_mutex:             Serializes outbound requests, one per channel.
 * @cfg_lock:             Serializes ch0 descriptor read-modify-write.
 * @resp_buffer_lock:     Bit lock guarding @resp_buf.
 * @irq_buffer_lock:      Bit lock guarding @irq_buf.
 * @irq_resp_buffer_lock: Bit lock guarding the IRQ response slot.
 * @rx_ready:             Set when a solicited response is in @resp_buf.
 * @gone:                 Fence: the device has gone. Never cleared.
 * @unbound:              Fence: raised at unbind, cleared at the next bind.
 * @started:              Gates dispatch of unsolicited packets.
 * @irq_cb_lock:          Guards @irq_ops against the atomic RX dispatch reader.
 * @irq_ops:              Ops of the bound driver, NULL while unbound.
 * @tx_buf:               Bulk-out scratch buffer for requests.
 * @tx_resp_buf:          Bulk-out scratch buffer for IRQ responses.
 * @resp_buf:             Buffer for solicited responses, or NULL.
 * @irq_buf:              Buffer for unsolicited requests, or NULL.
 * @bulk_tx_urb:          URB for outbound requests.
 * @bulk_tx_resp_urb:     URB for IRQ responses.
 * @rx_wq:                Wait queue for solicited-response readers.
 * @stats:                Per-channel exchange counters.
 *
 * @gone and @unbound together are the channel's I/O fence; see
 * Documentation/driver-api/usb/lstp.rst.
 */
struct lstp_channel_priv {
	struct lstp_channel pub;
	struct auxiliary_device auxdev;
	enum lstp_auxdev_state auxdev_state;

	u8 ch_type;
	const struct lstp_channel_type_info *type;
	struct lstp_usb *usb;
	struct mutex tx_mutex; /* serializes outbound requests */
	struct mutex cfg_lock; /* serializes ch0 descriptor read-modify-write */
	unsigned long resp_buffer_lock;
	unsigned long irq_buffer_lock;
	unsigned long irq_resp_buffer_lock;
	bool rx_ready;
	bool gone;
	bool unbound;
	bool started;
	spinlock_t irq_cb_lock; /* guards @irq_ops vs atomic RX dispatch */
	const struct lstp_ops *irq_ops;
	u8 *tx_buf;
	u8 *tx_resp_buf;
	u8 *resp_buf;
	u8 *irq_buf;
	struct urb *bulk_tx_urb;
	struct urb *bulk_tx_resp_urb;
	wait_queue_head_t rx_wq;
	struct lstp_channel_stats stats;
};

#define to_lstp_priv(ch) container_of((ch), struct lstp_channel_priv, pub)

static_assert(offsetof(struct lstp_channel_priv, pub) == 0,
	      "struct lstp_channel must be the first member of struct lstp_channel_priv");

static inline struct lstp_channel_priv *to_lstp_priv_from_auxdev(struct auxiliary_device *auxdev)
{
	return container_of(auxdev, struct lstp_channel_priv, auxdev);
}

/* Valid only for drivers registered through lstp_auxiliary_driver_register(). */
static inline const struct lstp_auxiliary_driver *to_lstp_aux_drv(struct device_driver *drv)
{
	return container_of(to_auxiliary_drv(drv), struct lstp_auxiliary_driver, adrv);
}

/**
 * lstp_channel_from_auxdev() - Recover the channel handle from its auxdev.
 * @auxdev: Auxiliary device the framework registered for the channel.
 *
 * Context: Any context.
 * Return: The channel handle, or %NULL if @auxdev was %NULL.
 */
struct lstp_channel *lstp_channel_from_auxdev(struct auxiliary_device *auxdev)
{
	if (!auxdev)
		return NULL;

	return &to_lstp_priv_from_auxdev(auxdev)->pub;
}
EXPORT_SYMBOL_NS_GPL(lstp_channel_from_auxdev, "LSTP");

/*
 * True once usb_disconnect() has marked the device USB_STATE_NOTATTACHED, read
 * locklessly as usb_submit_urb() does, or once a channel that outlived its
 * interface has dropped @usb. Unlike the channel fence, false for an unbind
 * while the device is still attached.
 */
static inline bool lstp_usb_gone(const struct lstp_usb *usb)
{
	return !usb || !usb->udev || usb->udev->state == USB_STATE_NOTATTACHED;
}

/**
 * lstp_channel_get() - Pin a channel so it outlives USB disconnect.
 * @ch: LSTP channel to pin, or %NULL.
 *
 * Takes a reference on the channel's auxiliary_device, which owns the private
 * container; see Documentation/driver-api/usb/lstp.rst for when a subsystem
 * must pin. Only a channel offered to a subsystem can be pinned; ch0 and
 * unsupported channel types stay LSTP_AUXDEV_RAW.
 *
 * Context: Any context.
 * Return: @ch, or %NULL if @ch was %NULL or is not pinnable.
 */
struct lstp_channel *lstp_channel_get(struct lstp_channel *ch)
{
	if (!ch)
		return NULL;

	struct lstp_channel_priv *priv = to_lstp_priv(ch);

	if (WARN_ON_ONCE(priv->auxdev_state == LSTP_AUXDEV_RAW))
		return NULL;

	get_device(&priv->auxdev.dev);
	return ch;
}
EXPORT_SYMBOL_NS_GPL(lstp_channel_get, "LSTP");

/**
 * lstp_channel_put() - Drop a pin taken by lstp_channel_get().
 * @ch: LSTP channel to release, or %NULL.
 *
 * The last reference frees the container. Refuses what lstp_channel_get()
 * refuses, so a pin that was never taken is never dropped.
 *
 * Context: Process context. May sleep.
 */
void lstp_channel_put(struct lstp_channel *ch)
{
	if (!ch)
		return;

	struct lstp_channel_priv *priv = to_lstp_priv(ch);

	if (WARN_ON_ONCE(priv->auxdev_state == LSTP_AUXDEV_RAW))
		return;

	put_device(&priv->auxdev.dev);
}
EXPORT_SYMBOL_NS_GPL(lstp_channel_put, "LSTP");

static void lstp_usb_rx_callback(struct urb *urb);
static void lstp_rx_retry_work(struct work_struct *work);
static void lstp_teardown(struct lstp_usb *dev);
static void lstp_kobj_release(struct kobject *kobj);
static ssize_t lstp_name_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf);

/*
 * /sys/.../lstp. Allocated on its own because a kobject's memory must last
 * until its release, which can come after struct lstp_usb (devres) is gone.
 * @dev is only dereferenced by attribute callbacks, and kobject_del() waits
 * for those to finish before the devres teardown goes on.
 */
struct lstp_kobj {
	struct kobject kobj;
	struct lstp_usb *dev;
};

static struct lstp_usb *lstp_kobj_to_usb(struct kobject *kobj)
{
	return container_of(kobj, struct lstp_kobj, kobj)->dev;
}

/*
 * One lstp_request*() or lstp_send() is one LSTP exchange. Not exported:
 * subsystems serialise multi-exchange sequences at their own layer.
 */
DEFINE_GUARD(lstp_channel, struct lstp_channel *, mutex_lock(&to_lstp_priv(_T)->tx_mutex),
	     mutex_unlock(&to_lstp_priv(_T)->tx_mutex))

static struct kobj_attribute lstp_name_attr = __ATTR(name, 0444, lstp_name_show, NULL);

/* A named group's files get the owning kobject, so lstp_kobj_to_usb() applies. */
/* clang-format off */
#define LSTP_STAT_ATTR(field)						\
	static ssize_t lstp_stat_##field##_show(struct kobject *kobj,	\
						struct kobj_attribute *attr, \
						char *buf)		\
	{								\
		struct lstp_usb *lstp = lstp_kobj_to_usb(kobj);		\
									\
		return sysfs_emit(buf, "%lld\n",			\
				  atomic64_read(&lstp->stats.field));	\
	}								\
	static struct kobj_attribute lstp_stat_##field##_attr =		\
		__ATTR(field, 0444, lstp_stat_##field##_show, NULL)
/* clang-format on */

LSTP_STAT_ATTR(rx_packets);
LSTP_STAT_ATTR(rx_errors);
LSTP_STAT_ATTR(rx_no_channel);
LSTP_STAT_ATTR(rx_urb_errors);
LSTP_STAT_ATTR(rx_stalls);
LSTP_STAT_ATTR(rx_retries);

static struct attribute *lstp_attrs[] = {
	&lstp_name_attr.attr,
	NULL,
};

static struct attribute *lstp_stat_attrs[] = {
	&lstp_stat_rx_packets_attr.attr,
	&lstp_stat_rx_errors_attr.attr,
	&lstp_stat_rx_no_channel_attr.attr,
	&lstp_stat_rx_urb_errors_attr.attr,
	&lstp_stat_rx_stalls_attr.attr,
	&lstp_stat_rx_retries_attr.attr,
	NULL,
};

static const struct attribute_group lstp_group = {
	.attrs = lstp_attrs,
};

static const struct attribute_group lstp_stat_group = {
	.name = "statistics",
	.attrs = lstp_stat_attrs,
};

static const struct attribute_group *lstp_groups[] = {
	&lstp_group,
	&lstp_stat_group,
	NULL,
};

static const struct kobj_type lstp_ktype = {
	.release = lstp_kobj_release,
	.sysfs_ops = &kobj_sysfs_ops,
	.default_groups = lstp_groups,
};

/* clang-format off */
static const int lstp_errno_map[] = {
	[LSTP_SUCCESS] = 0,
	[LSTP_ERROR] = -EIO,
	[LSTP_TIMEOUT] = -ETIMEDOUT,
	[LSTP_BUSY] = -EBUSY,
	[LSTP_NACK] = -ENXIO,
	[LSTP_ARB_LOST] = -EAGAIN,
	[LSTP_NOT_SUPP] = -EOPNOTSUPP,
	[LSTP_TOO_LARGE] = -EFBIG,
}; /* clang-format on */

/*
 * Unmapped statuses are -EIO. A gap in the designated-initializer table reads
 * back as 0, so only LSTP_SUCCESS may return 0.
 */
static int lstp_status_to_errno(u8 status)
{
	if (status == LSTP_SUCCESS)
		return 0;

	if (status < ARRAY_SIZE(lstp_errno_map) && lstp_errno_map[status])
		return lstp_errno_map[status];

	return -EIO;
}

static int lstp_validate_rx_pkt(struct lstp_usb *dev, struct lstp_packet *rx_pkt,
				size_t actual_length)
{
	struct lstp_header *rx_hdr = &rx_pkt->hdr;

	if (actual_length < sizeof(struct lstp_header)) {
		dev_err_ratelimited(&dev->intf->dev,
				    "%s: Packet without header (got %zu, need >=%zu)\n", __func__,
				    actual_length, sizeof(struct lstp_header));
		return -EIO;
	}

	u8 ch_id = rx_hdr->ch_id;

	if (ch_id > dev->max_ch_id) {
		dev_err_ratelimited(&dev->intf->dev, "%s: Invalid channel ID (got %u, max %u)\n",
				    __func__, ch_id, dev->max_ch_id);
		return -EIO;
	}

	size_t claimed_payload_len = le16_to_cpu(rx_hdr->length);
	size_t max_payload_len = dev->bulk_rx_size - sizeof(struct lstp_header);
	/* Payloads are limited to one packet. */
	if (claimed_payload_len > max_payload_len) {
		dev_err_ratelimited(&dev->intf->dev,
				    "%s: ch_%u: Payload too large (claims %zu, max %zu)\n",
				    __func__, ch_id, claimed_payload_len, max_payload_len);
		return -EIO;
	}

	if (sizeof(struct lstp_header) + claimed_payload_len > actual_length) {
		dev_err_ratelimited(&dev->intf->dev,
				    "%s: ch_%u: Payload too short (claims %zu, got %zu)\n",
				    __func__, ch_id, claimed_payload_len,
				    actual_length - sizeof(struct lstp_header));
		return -EIO;
	}

	return 0;
}

/*
 * Validate a packet already accepted by lstp_validate_rx_pkt(): LSTP status
 * first, then payload against @min_payload_len (LSTP_ANY_RX_LEN for none).
 * Longer payloads pass; see lstp_request() for the length modes. @lstp_status,
 * when given, is set only if the returned errno came from the status byte.
 */
static int lstp_validate_resp(struct lstp_usb *dev, struct lstp_packet *rx_pkt,
			      size_t min_payload_len, int *lstp_status)
{
	struct lstp_header *rx_hdr = &rx_pkt->hdr;
	u8 ch_id = rx_hdr->ch_id;
	u8 status = FIELD_GET(LSTP_HDR_CODE, rx_hdr->status);

	int ret = lstp_status_to_errno(status);

	if (ret) {
		if (lstp_status)
			*lstp_status = status;
		dev_dbg(&dev->intf->dev, "%s: ch_%u: LSTP error status %d (errno=%d)\n", __func__,
			ch_id, status, ret);
		return ret;
	}

	size_t payload_len = le16_to_cpu(rx_hdr->length);

	if (payload_len < min_payload_len) {
		dev_err_ratelimited(&dev->intf->dev,
				    "%s: ch_%u: Payload too short (need at least %zu, got %zu)\n",
				    __func__, ch_id, min_payload_len, payload_len);
		return -EIO;
	}

	if (min_payload_len && payload_len > min_payload_len)
		dev_dbg(&dev->intf->dev,
			"%s: ch_%u: Payload longer than minimum (got %zu, min %zu) -- forward-compat extension?\n",
			__func__, ch_id, payload_len, min_payload_len);

	return 0;
}

/*
 * Bytes of ch_config[] one READ_CONFIG reply can carry, or 0 if ch0 is
 * unreachable. Every config read is served by ch0, so the window follows ch0's
 * RX payload rather than the target channel's: 42 with the 64-byte bulk
 * endpoint Full Speed mandates, 490 with a 512-byte one.
 */
static size_t lstp_usb_config_window(const struct lstp_usb *usb)
{
	/* Probe enforces bulk_rx_size >= LSTP_USB_EP_MIN_SIZE; anchor a nonzero window. */
	BUILD_BUG_ON(sizeof(struct lstp_header) + sizeof(struct lstp_ch0_read_resp) >=
		     LSTP_USB_EP_MIN_SIZE);

	if (!usb || !usb->channels[0])
		return 0;

	return usb->channels[0]->max_rx_payload - offsetof(struct lstp_ch0_read_resp, ch_config);
}

static size_t lstp_ch0_config_window(struct lstp_channel *ch)
{
	return lstp_usb_config_window(to_lstp_priv(ch)->usb);
}

/* lstp_ch0_read_config() by channel id, for discovery, before the channel exists. */
static int __lstp_ch0_read_config(struct lstp_usb *usb, u8 ch_id, u16 offset, u16 length,
				  struct lstp_ch0_read_resp **resp_out, size_t *len_out)
{
	struct lstp_ch0_read_resp *resp __free(kfree) = NULL;

	if (!usb || !usb->channels[0])
		return -ENODEV;
	if (!resp_out || !len_out)
		return -EINVAL;

	struct lstp_channel *ch0 = usb->channels[0];
	size_t max_payload_len = ch0->max_rx_payload;
	u16 max_config_len = lstp_usb_config_window(usb);
	u16 config_len = length;

	if (length == LSTP_READ_CONFIG_MAX)
		config_len = max_config_len;
	else if (length > max_config_len)
		return -EINVAL;

	resp = kzalloc(max_payload_len, GFP_KERNEL);
	if (!resp)
		return -ENOMEM;

	/* Probe enforces bulk_tx_size >= LSTP_USB_EP_MIN_SIZE; anchor the fixed request here. */
	BUILD_BUG_ON(sizeof(struct lstp_header) + sizeof(struct lstp_ch0_read_req) >
		     LSTP_USB_EP_MIN_SIZE);

	struct lstp_ch0_read_req req = {
		.ch_id = ch_id,
		.offset = cpu_to_le16(offset),
		.length = cpu_to_le16(config_len),
	};

	size_t actual;
	int ret = lstp_request(ch0, LSTP_CH0_CMD_READ_CONFIG, &req, sizeof(req), resp,
			       max_payload_len, &actual, NULL);
	if (ret) {
		dev_dbg(&usb->intf->dev, "%s: ch_%u: READ_CONFIG failed (%pe)\n", __func__, ch_id,
			ERR_PTR(ret));
		return ret;
	}

	size_t need = sizeof(*resp);

	if (length != LSTP_READ_CONFIG_MAX)
		need += length;
	if (actual < need) {
		dev_dbg(&usb->intf->dev, "%s: ch_%u: READ_CONFIG short reply: %zu < %zu\n",
			__func__, ch_id, actual, need);
		return -EIO;
	}

	*resp_out = no_free_ptr(resp);
	*len_out = actual;
	return 0;
}

/**
 * lstp_ch0_read_config() - Read a config slice via ch0 READ_CONFIG.
 * @ch:       Target LSTP channel to read config from
 * @offset:   Byte offset starting at ch_config[0]
 * @length:   Bytes of ch_config[] to read, at most what one reply can carry
 * @resp_out: On success, the kmalloc'd on-wire response (descriptor head plus
 *            config bytes); the caller owns it and must kfree() it
 * @len_out:  On success, response bytes in *@resp_out: at least the head plus
 *            @length config bytes
 *
 * Valid only while a driver is bound to @ch; does not honour a pin past
 * disconnect.
 *
 * Context: Process context. Performs blocking USB I/O.
 * Return: 0 on success, negative errno on failure.
 */
int lstp_ch0_read_config(struct lstp_channel *ch, u16 offset, u16 length,
			 struct lstp_ch0_read_resp **resp_out, size_t *len_out)
{
	if (!ch)
		return -ENODEV;

	return __lstp_ch0_read_config(to_lstp_priv(ch)->usb, ch->ch_id, offset, length, resp_out,
				      len_out);
}
EXPORT_SYMBOL_NS_GPL(lstp_ch0_read_config, "LSTP");

/**
 * lstp_ch0_read_config_bytes() - Read a byte range of ch_config[], reassembling packets.
 * @ch:     Target LSTP channel to read config from
 * @offset: Byte offset starting at ch_config[0]
 * @buf:    Destination buffer of at least @len bytes
 * @len:    Bytes to read
 *
 * Splits @len into reads of at most lstp_ch0_config_window() bytes each.
 *
 * Context: Process context. Performs blocking USB I/O, once per packet.
 * Return: 0 on success, negative errno on failure.
 */
static int lstp_ch0_read_config_bytes(struct lstp_channel *ch, u16 offset, void *buf, size_t len)
{
	if (!ch)
		return -ENODEV;
	if (!buf)
		return -EINVAL;

	size_t avail = lstp_ch0_config_window(ch);
	size_t end;

	if (!avail)
		return -ENODEV;
	if (check_add_overflow((size_t)offset, len, &end) || end > U16_MAX)
		return -EINVAL;

	for (size_t done = 0; done < len;) {
		struct lstp_ch0_read_resp *resp __free(kfree) = NULL;
		u16 length = min(len - done, avail);
		size_t resp_len;
		int ret = lstp_ch0_read_config(ch, offset + done, length, &resp, &resp_len);

		if (ret)
			return ret;

		/* lstp_ch0_read_config() rejects a reply short of @length config bytes. */
		memcpy(buf + done, resp->ch_config, length);
		done += length;
	}
	return 0;
}

/**
 * lstp_read_config_table() - Walk a channel's variable-length config entry array.
 * @ch:                Target LSTP channel to read config from
 * @config_head_size:  Bytes before the entry array starts (sizeof the fixed config head)
 * @config_entry_size: Size of one wire config entry
 * @total_entries:     Number of entries to walk
 * @entry_fn:          Per-entry callback; @entry points into a scratch buffer
 *                     and is valid only for the duration of the call
 * @entry_ctx:         Opaque context passed to @entry_fn
 *
 * The array is read as a byte stream; an entry straddling two responses is
 * carried across reads. Valid only while a driver is bound to @ch; does not
 * honour a pin past disconnect.
 *
 * Context: Process context. Performs blocking USB I/O; @entry_fn runs in the
 * same context.
 * Return: 0 on success or for an empty table, %-ENODEV for a %NULL @ch or
 *         unreachable ch0, %-EINVAL for a bad argument, else the first read
 *         or @entry_fn errno.
 */
int lstp_read_config_table(struct lstp_channel *ch, size_t config_head_size,
			   size_t config_entry_size, unsigned int total_entries,
			   lstp_config_entry_fn entry_fn, void *entry_ctx)
{
	if (!total_entries)
		return 0;
	if (!ch)
		return -ENODEV;
	if (!entry_fn || !config_entry_size)
		return -EINVAL;

	size_t avail = lstp_ch0_config_window(ch);
	size_t table_bytes;
	size_t table_end;

	if (!avail)
		return -ENODEV;
	if (check_mul_overflow((size_t)total_entries, config_entry_size, &table_bytes) ||
	    check_add_overflow(config_head_size, table_bytes, &table_end) || table_end > U16_MAX)
		return -EINVAL;

	/* One full window plus the largest partial entry that can carry over. */
	u8 *buf __free(kfree) = kmalloc(config_entry_size + avail, GFP_KERNEL);

	if (!buf)
		return -ENOMEM;

	unsigned int next_entry = 0;
	size_t carried = 0;

	for (size_t done = 0; done < table_bytes;) {
		size_t length = min(table_bytes - done, avail);
		int ret = lstp_ch0_read_config_bytes(ch, config_head_size + done, buf + carried,
						     length);

		if (ret) {
			dev_err(ch->dev, "%s: ch_%d: config read for entry %u failed (%pe)\n",
				__func__, ch->ch_id, next_entry, ERR_PTR(ret));
			return ret;
		}
		done += length;
		carried += length;

		size_t consumed = 0;

		while (carried - consumed >= config_entry_size) {
			ret = entry_fn(ch, next_entry++, buf + consumed, entry_ctx);
			if (ret)
				return ret;
			consumed += config_entry_size;
		}

		carried -= consumed;
		memmove(buf, buf + consumed, carried);
	}
	return 0;
}
EXPORT_SYMBOL_NS_GPL(lstp_read_config_table, "LSTP");

/*
 * Read @ch's descriptor head via ch0 READ_CONFIG. Read-modify-write callers
 * hold priv->cfg_lock across this and the write; see __lstp_ch0_write_config().
 */
static int lstp_ch0_read_desc(struct lstp_channel *ch, struct lstp_ch_desc *desc)
{
	if (!ch)
		return -ENODEV;
	struct lstp_channel_priv *priv = to_lstp_priv(ch);

	struct lstp_usb *usb = priv->usb;

	if (!usb || !usb->channels[0])
		return -ENODEV;
	struct lstp_channel *ch0 = usb->channels[0];

	struct lstp_ch0_read_req req = {
		.ch_id = ch->ch_id,
		.offset = cpu_to_le16(0),
		.length = cpu_to_le16(0),
	};

	/* Fixed-length read (see lstp_request()); firmware may append a config trailer. */
	int ret = lstp_request(ch0, LSTP_CH0_CMD_READ_CONFIG, &req, sizeof(req), desc,
			       sizeof(*desc), NULL, NULL);
	if (ret)
		dev_err(ch->dev, "%s: ch_%d: READ_CONFIG failed (%pe)\n", __func__, ch->ch_id,
			ERR_PTR(ret));
	return ret;
}

/*
 * Raw WRITE_CONFIG sender: @desc goes on the wire verbatim, @config_len bytes
 * of @config land at @offset in ch_config[] (@config is NULL exactly when
 * @config_len is 0).
 * Every WRITE_CONFIG re-sends the descriptor head, so a caller that changes
 * the descriptor must hold priv->cfg_lock across a re-read of @desc from the
 * device and this write, or it writes back a stale type/flags/name.
 */
static int __lstp_ch0_write_config(struct lstp_channel *ch, u16 offset,
				   const struct lstp_ch_desc *desc, const void *config,
				   u16 config_len)
{
	if (!ch || !desc)
		return -ENODEV;
	struct lstp_channel_priv *priv = to_lstp_priv(ch);

	struct lstp_usb *usb = priv->usb;

	if (!usb || !usb->channels[0])
		return -ENODEV;
	if (config_len && !config)
		return -EINVAL;

	struct lstp_channel *ch0 = usb->channels[0];
	size_t req_len;

	/* Probe enforces bulk_tx_size >= LSTP_USB_EP_MIN_SIZE; anchor the fixed head here. */
	BUILD_BUG_ON(sizeof(struct lstp_header) + offsetof(struct lstp_ch0_write_req, ch_config) >
		     LSTP_USB_EP_MIN_SIZE);

	if (check_add_overflow(offsetof(struct lstp_ch0_write_req, ch_config), config_len,
			       &req_len))
		return -EINVAL;

	struct lstp_ch0_write_req *req __free(kfree) = kmalloc(req_len, GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	req->ch_id = ch->ch_id;
	req->offset = cpu_to_le16(offset);
	req->desc = *desc;
	if (config_len)
		memcpy(req->ch_config, config, config_len);

	int ret = lstp_request(ch0, LSTP_CH0_CMD_WRITE_CONFIG, req, req_len, NULL, 0, NULL, NULL);

	if (ret)
		dev_warn_ratelimited(ch->dev,
				     "%s: ch_%d: WRITE_CONFIG failed (%pe). Config locked?\n",
				     __func__, ch->ch_id, ERR_PTR(ret));

	return ret;
}

static int lstp_ch0_set_enable(struct lstp_channel *ch, bool enable)
{
	struct lstp_ch_desc desc;

	if (!ch)
		return -ENODEV;

	guard(mutex)(&to_lstp_priv(ch)->cfg_lock);

	int ret = lstp_ch0_read_desc(ch, &desc);

	if (ret)
		return ret;

	if (enable)
		desc.ch_flags |= LSTP_CH_FLAG_ENABLE;
	else
		desc.ch_flags &= ~LSTP_CH_FLAG_ENABLE;

	return __lstp_ch0_write_config(ch, 0, &desc, NULL, 0);
}

static void lstp_put_usb_dev(void *data)
{
	usb_put_dev((struct usb_device *)data);
}

/*
 * Devres action: kill and free the URB in *(struct urb **)@data and NULL the
 * slot. @data must live at least as long as the devres node.
 */
static void lstp_release_urb_slot(void *data)
{
	struct urb **slot = data;

	usb_kill_urb(*slot);
	usb_free_urb(*slot);
	*slot = NULL;
}

static void lstp_kobj_del_put(void *data)
{
	struct kobject *kobj = data;

	kobject_del(kobj);
	kobject_put(kobj);
}

static void lstp_kobj_release(struct kobject *kobj)
{
	kfree(container_of(kobj, struct lstp_kobj, kobj));
	if (atomic_dec_and_test(&lstp_live_objects))
		wake_up_var(&lstp_live_objects);
}

static ssize_t lstp_name_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct lstp_usb *lstp = lstp_kobj_to_usb(kobj);

	return sysfs_emit(buf, "%s\n", lstp->lstp_intf_name);
}

/* <auxdev>/enable: LSTP_CH_FLAG_ENABLE, read back from the device. */
static ssize_t enable_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct lstp_channel *ch = lstp_channel_from_auxdev(to_auxiliary_dev(dev));
	struct lstp_ch_desc desc;
	int ret = lstp_ch0_read_desc(ch, &desc);

	if (ret)
		return ret;

	u8 enable_bit = (desc.ch_flags & LSTP_CH_FLAG_ENABLE) ? 1 : 0;

	return sysfs_emit(buf, "%u\n", enable_bit);
}

static ssize_t enable_store(struct device *dev, struct device_attribute *attr, const char *buf,
			    size_t count)
{
	struct lstp_channel *ch = lstp_channel_from_auxdev(to_auxiliary_dev(dev));
	unsigned int enable;
	int ret = kstrtouint(buf, 0, &enable);

	if (ret)
		return ret;

	if (enable > 1)
		return -EINVAL;

	ret = lstp_ch0_set_enable(ch, enable != 0);
	if (ret)
		return ret;

	return count;
}

static ssize_t name_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct lstp_channel *ch = lstp_channel_from_auxdev(to_auxiliary_dev(dev));

	return sysfs_emit(buf, "%s\n", ch->display_name);
}

/* <auxdev>/channel: ch_id, which the auxdev instance suffix does not track. */
static ssize_t channel_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct lstp_channel *ch = lstp_channel_from_auxdev(to_auxiliary_dev(dev));

	return sysfs_emit(buf, "%u\n", ch->ch_id);
}

/* clang-format off */
#define LSTP_CHANNEL_STAT_ATTR(field)						\
	static ssize_t field##_show(struct device *dev,				\
				    struct device_attribute *attr, char *buf)	\
	{									\
		struct lstp_channel_priv *priv =				\
			to_lstp_priv_from_auxdev(to_auxiliary_dev(dev));		\
										\
		return sysfs_emit(buf, "%lld\n",				\
				  atomic64_read(&priv->stats.field));		\
	}									\
	static DEVICE_ATTR_RO(field)
/* clang-format on */

LSTP_CHANNEL_STAT_ATTR(tx_packets);
LSTP_CHANNEL_STAT_ATTR(rx_packets);
LSTP_CHANNEL_STAT_ATTR(rx_events);
LSTP_CHANNEL_STAT_ATTR(timeouts);
LSTP_CHANNEL_STAT_ATTR(errors);
LSTP_CHANNEL_STAT_ATTR(tx_errors);
LSTP_CHANNEL_STAT_ATTR(rx_dropped);
LSTP_CHANNEL_STAT_ATTR(rx_late);

static DEVICE_ATTR_RO(name);
static DEVICE_ATTR_RO(channel);
static DEVICE_ATTR_RW(enable);

static struct attribute *lstp_channel_attrs[] = {
	&dev_attr_name.attr,
	&dev_attr_channel.attr,
	&dev_attr_enable.attr,
	NULL,
};

/* clang-format off */
static struct attribute *lstp_channel_stat_attrs[] = {
	&dev_attr_tx_packets.attr,
	&dev_attr_rx_packets.attr,
	&dev_attr_rx_events.attr,
	&dev_attr_timeouts.attr,
	&dev_attr_errors.attr,
	&dev_attr_tx_errors.attr,
	&dev_attr_rx_dropped.attr,
	&dev_attr_rx_late.attr,
	NULL,
}; /* clang-format on */

static const struct attribute_group lstp_channel_group = {
	.attrs = lstp_channel_attrs,
};

static const struct attribute_group lstp_channel_stat_group = {
	.name = "statistics",
	.attrs = lstp_channel_stat_attrs,
};

static const struct attribute_group *lstp_channel_groups[] = {
	&lstp_channel_group,
	&lstp_channel_stat_group,
	NULL,
};

static int lstp_create_sysfs_hierarchy(struct lstp_usb *dev)
{
	struct device *parent_dev = &dev->intf->dev;
	struct lstp_kobj *lk = kzalloc_obj(*lk);
	int ret;

	if (!lk)
		return -ENOMEM;
	atomic_inc(&lstp_live_objects);
	lk->dev = dev;

	ret = kobject_init_and_add(&lk->kobj, &lstp_ktype, &parent_dev->kobj, "lstp");
	if (ret) {
		kobject_put(&lk->kobj);
		return ret;
	}

	return devm_add_action_or_reset(parent_dev, lstp_kobj_del_put, &lk->kobj);
}

/*
 * Warn about an interface node with a one-cell "reg", and about an interface
 * node under a combined device node. The USB core attaches neither, so every
 * channel of the interface would come up undescribed.
 */
static void lstp_check_of_layout(struct lstp_usb *dev)
{
	struct device_node *udev_np = dev->udev->dev.of_node;
	struct device_node *intf_np = dev->intf->dev.of_node;
	u8 ifnum = dev->intf->cur_altsetting->desc.bInterfaceNumber;
	struct device_node *np;
	u32 reg;

	if (intf_np == udev_np) {
		for_each_child_of_node(udev_np, np) {
			if (!of_node_name_eq(np, "interface"))
				continue;
			dev_warn(&dev->intf->dev,
				 "%s: %pOF is a combined device and interface node; channel nodes go directly under it, not under %pOFP\n",
				 __func__, udev_np, np);
			of_node_put(np);
			break;
		}
		return;
	}

	if (intf_np)
		return;

	for_each_child_of_node(udev_np, np) {
		if (of_property_count_u32_elems(np, "reg") != 1 ||
		    of_property_read_u32(np, "reg", &reg) || reg != ifnum)
			continue;
		dev_warn(&dev->intf->dev,
			 "%s: %pOF has a one-cell reg; the USB core attaches an interface node only for a two-cell reg = <interface configuration>, so no channel of this interface is described\n",
			 __func__, np);
		of_node_put(np);
		break;
	}
}

#ifdef CONFIG_ACPI
/* The full 64-bit _ADR of an ACPI Device(); -ENODATA for any other node. */
static int lstp_acpi_read_adr(struct fwnode_handle *node, u64 *adr)
{
	if (!is_acpi_device_node(node))
		return -ENODATA;

	return acpi_get_local_u64_address(ACPI_HANDLE_FWNODE(node), adr);
}
#else
static int lstp_acpi_read_adr(struct fwnode_handle *node, u64 *adr)
{
	return -ENODATA;
}
#endif

/*
 * The address of an interface or channel node: on ACPI the _ADR of a Device(),
 * compared in full, so a value above the interface number or channel ID range
 * never matches; elsewhere its "reg". A _DSD data node has no address.
 */
static int lstp_fwnode_read_address(struct fwnode_handle *node, u64 *addr)
{
	u32 reg;
	int ret;

	if (is_acpi_node(node))
		return lstp_acpi_read_adr(node, addr);

	ret = fwnode_property_read_u32(node, "reg", &reg);
	if (!ret)
		*addr = reg;
	return ret;
}

/*
 * A _DSD data node has no _ADR, so it never matches an interface or channel;
 * warn rather than leave the table's mistake silent.
 */
static void lstp_check_acpi_data_node(struct lstp_usb *dev, struct fwnode_handle *node)
{
	if (is_acpi_data_node(node))
		dev_warn(&dev->intf->dev,
			 "%s: %pfw: ACPI _DSD data node, not a Device(); interface and channel objects must be Device() objects with _ADR\n",
			 __func__, node);
}

/*
 * The node the channel nodes hang off, with a reference, or NULL; see
 * Documentation/driver-api/usb/lstp.rst.
 */
static struct fwnode_handle *lstp_find_intf_fwnode(struct lstp_usb *dev)
{
	struct fwnode_handle *udev_node = dev_fwnode(&dev->udev->dev);
	u8 ifnum = dev->intf->cur_altsetting->desc.bInterfaceNumber;
	struct fwnode_handle *child;
	u64 addr;

	if (!udev_node)
		return NULL;

	if (is_of_node(udev_node)) {
		lstp_check_of_layout(dev);
		return fwnode_handle_get(dev_fwnode(&dev->intf->dev));
	}

	fwnode_for_each_child_node(udev_node, child) {
		if (!lstp_fwnode_read_address(child, &addr) && addr == ifnum)
			return child;
	}

	/* Only once nothing matched: other children may describe other interfaces. */
	fwnode_for_each_child_node(udev_node, child)
		lstp_check_acpi_data_node(dev, child);

	return NULL;
}

static void lstp_put_fwnode(void *data)
{
	fwnode_handle_put(data);
}

static int lstp_init_intf_fwnode(struct lstp_usb *dev)
{
	struct fwnode_handle *child;

	dev->intf_fwnode = lstp_find_intf_fwnode(dev);
	if (!dev->intf_fwnode)
		return 0;

	if (is_acpi_node(dev->intf_fwnode)) {
		fwnode_for_each_child_node(dev->intf_fwnode, child)
			lstp_check_acpi_data_node(dev, child);
	}

	return devm_add_action_or_reset(&dev->intf->dev, lstp_put_fwnode, dev->intf_fwnode);
}

static bool lstp_channel_fwnode_matches(struct fwnode_handle *node, u8 channel_id,
					const char *compatible)
{
	u64 addr;

	return !lstp_fwnode_read_address(node, &addr) && addr == channel_id &&
	       fwnode_property_match_string(node, "compatible", compatible) >= 0;
}

/*
 * The child of @parent matching @channel_id and @compatible, with a reference,
 * or NULL. Disabled OF nodes are matched too, so that they remove the channel;
 * see Documentation/driver-api/usb/lstp.rst for the iterator choice.
 */
static struct fwnode_handle *lstp_find_channel_fwnode(struct fwnode_handle *parent, u8 channel_id,
						      const char *compatible)
{
	struct fwnode_handle *ch_node;

	if (is_of_node(parent)) {
		for_each_child_of_node_scoped(to_of_node(parent), np) {
			if (lstp_channel_fwnode_matches(of_fwnode_handle(np), channel_id,
							compatible))
				return of_fwnode_handle(of_node_get(np));
		}
		return NULL;
	}

	fwnode_for_each_child_node(parent, ch_node) {
		if (lstp_channel_fwnode_matches(ch_node, channel_id, compatible))
			return ch_node;
	}

	return NULL;
}

/*
 * Store @ch's firmware node, if any, in @ch->fwnode. The channel owns the one
 * reference and drops it in lstp_channel_free(); a subsystem that points
 * another device at @ch->fwnode must not put it. Returns false for a disabled
 * device tree node, whose channel is then not created. See
 * Documentation/driver-api/usb/lstp.rst.
 */
static bool lstp_init_channel_fwnode(struct lstp_channel *ch, const char *compatible)
{
	struct lstp_channel_priv *priv = to_lstp_priv(ch);

	if (!compatible)
		return true;

	ch->fwnode = lstp_find_channel_fwnode(priv->usb->intf_fwnode, ch->ch_id, compatible);

	if (is_of_node(ch->fwnode) && !fwnode_device_is_available(ch->fwnode)) {
		dev_info(&priv->usb->intf->dev, "%s: ch_%d: skipping disabled channel %pfw\n",
			 __func__, ch->ch_id, ch->fwnode);
		fwnode_handle_put(ch->fwnode);
		ch->fwnode = NULL;
		return false;
	}

	dev_dbg(&priv->usb->intf->dev, "%s: ch_%d: %s firmware node for \"%s\"\n", __func__,
		ch->ch_id, ch->fwnode ? "matched" : "no", compatible);

	return true;
}

static int lstp_aux_notifier_register(struct lstp_usb *dev, struct auxiliary_device *auxdev);

/*
 * Allocate @resp_buf and the IRQ response slot (@tx_resp_buf,
 * @bulk_tx_resp_urb). Devm on the USB interface, not on ch->dev, so the auxdev
 * stays devres-clean for its driver; see lstp_channel_quiesce_unbound(). ch0
 * allocates its own @resp_buf.
 */
static int lstp_alloc_channel_bufs(struct lstp_channel *ch)
{
	struct lstp_channel_priv *priv = to_lstp_priv(ch);
	struct device *idev = &priv->usb->intf->dev;

	priv->resp_buf = devm_kzalloc(idev, priv->usb->bulk_rx_size, GFP_KERNEL);
	if (!priv->resp_buf)
		return -ENOMEM;

	priv->tx_resp_buf = devm_kzalloc(idev, sizeof(struct lstp_header), GFP_KERNEL);
	if (!priv->tx_resp_buf)
		return -ENOMEM;

	priv->bulk_tx_resp_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!priv->bulk_tx_resp_urb)
		return -ENOMEM;

	return devm_add_action_or_reset(idev, lstp_release_urb_slot, &priv->bulk_tx_resp_urb);
}

/*
 * Allocate @irq_buf at the first bind of a driver that takes unsolicited
 * packets; a rebind reuses it. Owned by the container and freed with it, so
 * an unbind cannot free it under lstp_usb_rx_callback(), and an interface
 * unwind running beside a first bind cannot strand it.
 */
static int lstp_alloc_channel_irq_bufs(struct lstp_channel_priv *priv)
{
	void *irq_buf;

	/* Bind paths only, serialised by device_lock, so the load is unordered. */
	if (priv->irq_buf)
		return 0;

	irq_buf = kzalloc(priv->usb->bulk_rx_size, GFP_KERNEL);
	if (!irq_buf)
		return -ENOMEM;

	/* Pairs with smp_load_acquire() in lstp_usb_rx_callback(): publish it zeroed. */
	smp_store_release(&priv->irq_buf, irq_buf);

	return 0;
}

/*
 * Free the instance id, mutexes, firmware-node reference, @irq_buf,
 * platform_data carrier and the container. Both URBs are interface devres,
 * already gone, and RX is stopped, so nothing can still read @irq_buf.
 */
static void lstp_channel_free(struct lstp_channel_priv *priv)
{
	/* Here, not at bus removal, so a live device's id cannot be recycled. */
	if (priv->auxdev_state != LSTP_AUXDEV_RAW)
		ida_free(&lstp_auxdev_ida, priv->auxdev.id);
	mutex_destroy(&priv->tx_mutex);
	mutex_destroy(&priv->cfg_lock);
	fwnode_handle_put(priv->pub.fwnode);
	kfree(priv->irq_buf);
	kfree(priv->auxdev.dev.platform_data);
	kfree(priv);
	if (atomic_dec_and_test(&lstp_live_objects))
		wake_up_var(&lstp_live_objects);
}

/* Sole free site for an init'd container, on its auxdev's last put. */
static void lstp_channel_dev_release(struct device *dev)
{
	lstp_channel_free(to_lstp_priv_from_auxdev(to_auxiliary_dev(dev)));
}

static void lstp_channel_destroy_action(void *data)
{
	struct lstp_channel_priv *priv = data;

	switch (priv->auxdev_state) {
	case LSTP_AUXDEV_ADDED:
		auxiliary_device_delete(&priv->auxdev);
		priv->usb = NULL;
		fallthrough;
	case LSTP_AUXDEV_INITED:
		/* Drops the init ref; release -> lstp_channel_dev_release(). */
		auxiliary_device_uninit(&priv->auxdev);
		break;
	case LSTP_AUXDEV_RAW:
		lstp_channel_free(priv);
		break;
	}
}

/*
 * Allocate a channel container and publish it in dev->channels[]. Its auxdev
 * is set up once the channel type is known. Returns NULL on failure.
 */
static struct lstp_channel *lstp_create_channel(struct lstp_usb *dev, u8 ch_id)
{
	struct lstp_channel_priv *priv = kzalloc_obj(*priv);

	if (!priv)
		return NULL;
	atomic_inc(&lstp_live_objects);
	struct lstp_channel *ch = &priv->pub;

	priv->usb = dev;
	priv->auxdev_state = LSTP_AUXDEV_RAW;
	mutex_init(&priv->tx_mutex);
	mutex_init(&priv->cfg_lock);
	spin_lock_init(&priv->irq_cb_lock);
	init_waitqueue_head(&priv->rx_wq);

	/* Registered first so it unwinds last, after actions that use @priv. */
	if (devm_add_action_or_reset(&dev->intf->dev, lstp_channel_destroy_action, priv))
		return NULL;

	priv->tx_buf = devm_kzalloc(&dev->intf->dev, dev->bulk_tx_size, GFP_KERNEL);
	if (!priv->tx_buf)
		return NULL;

	priv->bulk_tx_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!priv->bulk_tx_urb)
		return NULL;
	if (devm_add_action_or_reset(&dev->intf->dev, lstp_release_urb_slot, &priv->bulk_tx_urb))
		return NULL;

	ch->ch_id = ch_id;
	ch->dev = &dev->intf->dev;
	ch->max_tx_payload = dev->bulk_tx_size - sizeof(struct lstp_header);
	ch->max_rx_payload = dev->bulk_rx_size - sizeof(struct lstp_header);
	/* Pairs with smp_load_acquire() in lstp_usb_rx_callback(). */
	smp_store_release(&dev->channels[ch_id], ch);
	return ch;
}

static int lstp_ch0_init(struct lstp_usb *dev)
{
	struct lstp_channel *ch0 = lstp_create_channel(dev, 0);

	if (!ch0) {
		dev_err(&dev->intf->dev, "%s: Could not allocate channel 0\n", __func__);
		return -ENOMEM;
	}
	struct lstp_channel_priv *priv0 = to_lstp_priv(ch0);

	priv0->ch_type = LSTP_CHANNEL_TYPE_MGMT;
	priv0->resp_buf = devm_kzalloc(&dev->intf->dev, dev->bulk_rx_size, GFP_KERNEL);
	if (!priv0->resp_buf)
		return -ENOMEM;
	return 0;
}

/* Each discovery pass thrown away costs at least one response timeout. */
#define LSTP_DISCOVERY_PASSES 3

/* Every channel's READ_CONFIG reply from one pass, read before any is offered. */
struct lstp_discovery {
	struct lstp_ch0_read_resp *resp[LSTP_MAX_CHANNELS];
	size_t resp_len[LSTP_MAX_CHANNELS];
	int err[LSTP_MAX_CHANNELS];
};

static void lstp_discovery_reset(struct lstp_discovery *d)
{
	for (int i = 0; i < LSTP_MAX_CHANNELS; i++)
		kfree(d->resp[i]);
	memset(d, 0, sizeof(*d));
}

static void lstp_discovery_free(struct lstp_discovery *d)
{
	if (d)
		lstp_discovery_reset(d);
	kfree(d);
}

DEFINE_FREE(lstp_discovery, struct lstp_discovery *, lstp_discovery_free(_T))

/* A reply that cannot answer its read (see lstp_discover()); positive, unlike an errno. */
static int lstp_discovery_stale(struct lstp_usb *dev, u8 ch_id, const char *why)
{
	dev_warn(&dev->intf->dev, "%s: ch_%u: Stale reply during discovery (%s), resynchronizing\n",
		 __func__, ch_id, why);
	return 1;
}

/* Drops while only ch0 exists: ch0 replies in its rx_late, others in rx_no_channel. */
static s64 lstp_discovery_dropped(struct lstp_usb *dev)
{
	struct lstp_channel_priv *priv0 = to_lstp_priv(dev->channels[0]);

	return atomic64_read(&priv0->stats.rx_late) + atomic64_read(&dev->stats.rx_no_channel);
}

/*
 * Let the replies to requests an earlier binding abandoned arrive and be
 * dropped. Returns once a whole response timeout passes without one, after
 * LSTP_DISCOVERY_PASSES timeouts, or when the device is gone.
 */
static void lstp_discovery_drain(struct lstp_usb *dev)
{
	for (int i = 0; i < LSTP_DISCOVERY_PASSES && !lstp_usb_gone(dev); i++) {
		s64 seen = lstp_discovery_dropped(dev);

		msleep(LSTP_USB_RESPONSE_TIMEOUT_MS);
		if (lstp_discovery_dropped(dev) == seen)
			return;
	}
}

/*
 * Read ch0's descriptor into @dev. Returns 0, 1 for a stale reply unless this
 * is the @last pass, or a negative errno.
 */
static int lstp_discover_ch0(struct lstp_usb *dev, bool last)
{
	struct lstp_ch0_read_resp *resp __free(kfree) = NULL;
	struct lstp_channel *ch0 = dev->channels[0];
	size_t need = sizeof(*resp) + sizeof(struct lstp_ch0_config);
	const char *stale = NULL;
	size_t actual;
	int ret = __lstp_ch0_read_config(dev, 0, 0, LSTP_READ_CONFIG_MAX, &resp, &actual);

	if (ret == -ETIMEDOUT)
		stale = "no reply";
	else if (!ret && actual < need)
		stale = "short reply";
	else if (!ret && resp->desc.ch_type != LSTP_CHANNEL_TYPE_MGMT)
		stale = "not the management channel";
	if (stale && !last)
		return lstp_discovery_stale(dev, 0, stale);
	if (ret)
		return ret;

	if (actual < need) {
		dev_err(&dev->intf->dev, "%s: ch_0: READ_CONFIG short reply: %zu < %zu\n", __func__,
			actual, need);
		return -EIO;
	}
	const struct lstp_ch0_config *config = (const struct lstp_ch0_config *)resp->ch_config;

	if (resp->desc.ch_type != LSTP_CHANNEL_TYPE_MGMT) {
		dev_err(&dev->intf->dev, "%s: ch_0: Received wrong channel 0 type (got %d)\n",
			__func__, resp->desc.ch_type);
		return -EINVAL;
	}

	if (resp->desc.ch_name[0] == '\0') {
		dev_warn(&dev->intf->dev,
			 "%s: ch_0: Empty LSTP interface name, consider fixing it! Using default 'LSTP'\n",
			 __func__);
		strscpy(dev->lstp_intf_name, "LSTP", sizeof(dev->lstp_intf_name));
	} else {
		snprintf(dev->lstp_intf_name, sizeof(dev->lstp_intf_name), "%.*s", LSTP_CH_NAME_LEN,
			 resp->desc.ch_name);
	}
	strscpy(ch0->display_name, dev->lstp_intf_name, sizeof(ch0->display_name));

	if (config->lstp_version != LSTP_VERSION) {
		dev_err(&dev->intf->dev, "%s: ch_0: Invalid LSTP version\n", __func__);
		return -EINVAL;
	}
	dev->lstp_version = config->lstp_version;

	BUILD_BUG_ON(type_max(typeof_member(struct lstp_ch0_config, num_channels)) >=
		     LSTP_MAX_CHANNELS);
	if (config->num_channels == 0) {
		dev_err(&dev->intf->dev, "%s: ch_0: Invalid channel count %d (min 1, max %d)\n",
			__func__, config->num_channels, LSTP_MAX_CHANNELS - 1);
		return -EINVAL;
	}
	dev->max_ch_id = config->num_channels;
	return 0;
}

/*
 * Read channels 1..max_ch_id into @d. A failed read is kept in @d->err and its
 * channel skipped; 1 for a stale reply as for lstp_discover_ch0().
 */
static int lstp_discover_channels(struct lstp_usb *dev, struct lstp_discovery *d, bool last)
{
	for (int ch_id = 1; ch_id <= dev->max_ch_id; ch_id++) {
		const char *stale = NULL;
		int ret = __lstp_ch0_read_config(dev, ch_id, 0, LSTP_READ_CONFIG_MAX,
						 &d->resp[ch_id], &d->resp_len[ch_id]);

		if (ret == -ETIMEDOUT)
			stale = "no reply";
		else if (!ret && d->resp[ch_id]->desc.ch_type == LSTP_CHANNEL_TYPE_MGMT)
			stale = "management channel descriptor";
		if (stale && last) {
			dev_err(&dev->intf->dev,
				"%s: ch_%u: Stale reply on the last discovery pass (%s)\n",
				__func__, ch_id, stale);
			return ret ?: -EIO;
		}
		if (stale)
			return lstp_discovery_stale(dev, ch_id, stale);
		d->err[ch_id] = ret;
	}
	return 0;
}

/*
 * Read every channel descriptor, resynchronizing first if a reply is stale.
 *
 * The device still answers requests an earlier binding abandoned, and no
 * reply names its request (see struct lstp_header). ch0 answers in order, so
 * a pass is thrown away, the stale replies drained and discovery restarted
 * from ch0 when a reply cannot be its read's own or a read gets none. No
 * channel is created before a pass completes. The last pass fails instead.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int lstp_discover(struct lstp_usb *dev, struct lstp_discovery *d)
{
	for (int pass = 1;; pass++) {
		bool last = pass == LSTP_DISCOVERY_PASSES;
		int ret = lstp_discover_ch0(dev, last);

		if (!ret)
			ret = lstp_discover_channels(dev, d, last);
		if (ret <= 0)
			return ret;
		lstp_discovery_reset(d);
		lstp_discovery_drain(dev);
	}
}

/* clang-format off */
static const char * const lstp_ch_type_tags[] = {
	[LSTP_CHANNEL_TYPE_SPI] = "SPI",
	[LSTP_CHANNEL_TYPE_GPIO] = "GPIO",
	[LSTP_CHANNEL_TYPE_I2C] = "I2C",
	[LSTP_CHANNEL_TYPE_UART] = "UART",
	[LSTP_CHANNEL_TYPE_IPMI] = "IPMI",
	[LSTP_CHANNEL_TYPE_MMIO] = "MMIO",
}; /* clang-format on */

/*
 * Set ch->display_name from the firmware @ch_name (not NUL-terminated), or the
 * userspace-visible fallback "<intf_name>_<TYPE>_CH<id>". Needs ch_id, ch_type.
 */
static void lstp_set_display_name(struct lstp_channel *ch, const char *ch_name, size_t ch_name_len)
{
	struct lstp_channel_priv *priv = to_lstp_priv(ch);
	size_t name_len = strnlen(ch_name, ch_name_len);

	if (name_len) {
		snprintf(ch->display_name, sizeof(ch->display_name), "%s_%.*s",
			 priv->usb->lstp_intf_name, (int)name_len, ch_name);
		return;
	}

	const char *tag = (priv->ch_type < ARRAY_SIZE(lstp_ch_type_tags)) ?
				  lstp_ch_type_tags[priv->ch_type] :
				  NULL;
	if (!tag)
		tag = "UNKNOWN";

	snprintf(ch->display_name, sizeof(ch->display_name), "%s_%s_CH%u",
		 priv->usb->lstp_intf_name, tag, ch->ch_id);
	dev_warn(ch->dev, "%s: ch_%d: Empty channel name, consider fixing it! Using default '%s'\n",
		 __func__, ch->ch_id, ch->display_name);
}

/*
 * Set up the channel's auxdev and take its instance id (LSTP_AUXDEV_INITED).
 * @config is copied into a container-owned platform_data carrier, so it
 * outlives the discovery scratch and every rebind.
 */
static int lstp_init_channel_auxdev(struct lstp_channel_priv *priv, const void *config,
				    size_t config_len)
{
	struct lstp_channel_init_info *info;
	void *blob;
	int ret;

	priv->auxdev.dev.parent = &priv->usb->intf->dev;
	priv->auxdev.dev.release = lstp_channel_dev_release;
	priv->auxdev.dev.groups = lstp_channel_groups;
	priv->auxdev.name = priv->type->name;

	/* Carrier and blob in one allocation, freed with the container. */
	info = kzalloc(size_add(sizeof(*info), config_len), GFP_KERNEL);
	if (!info)
		return -ENOMEM;
	blob = info + 1;
	memcpy(blob, config, config_len);
	info->config = blob;
	info->config_len = config_len;
	priv->auxdev.dev.platform_data = info;

	ret = ida_alloc(&lstp_auxdev_ida, GFP_KERNEL);
	if (ret < 0)
		return ret;
	priv->auxdev.id = ret;

	ret = auxiliary_device_init(&priv->auxdev);
	if (ret) {
		ida_free(&lstp_auxdev_ida, priv->auxdev.id);
		return ret;
	}
	/* lstp_nack_undelivered() reads it locklessly once the channel is published. */
	WRITE_ONCE(priv->auxdev_state, LSTP_AUXDEV_INITED);

	return 0;
}

static const struct lstp_channel_type_info lstp_channel_types[LSTP_CHANNEL_TYPE_MAX] = {
	[LSTP_CHANNEL_TYPE_SPI] = {
		.name = "spi",
		.compatible = "nvidia,lstp-spi",
	},
	[LSTP_CHANNEL_TYPE_GPIO] = {
		.name = "gpio",
		.compatible = "nvidia,lstp-gpio",
	},
	[LSTP_CHANNEL_TYPE_I2C] = {
		.name = "i2c",
		.compatible = "nvidia,lstp-i2c",
	},
	[LSTP_CHANNEL_TYPE_UART] = {
		.name = "uart",
		.compatible = "nvidia,lstp-uart",
		.irq_needs_resp = true,
	},
	[LSTP_CHANNEL_TYPE_IPMI] = {
		.name = "ipmi",
		.compatible = "nvidia,lstp-ipmi",
	},
	/* No compatible: a miscdevice has no firmware children or consumers. */
	[LSTP_CHANNEL_TYPE_MMIO] = {
		.name = "mmio",
	},
};

/* @ch_type is firmware-reported; NULL for ch0's type and any type not listed. */
static const struct lstp_channel_type_info *lstp_channel_type_lookup(u8 ch_type)
{
	if (ch_type >= ARRAY_SIZE(lstp_channel_types) || !lstp_channel_types[ch_type].name)
		return NULL;

	return &lstp_channel_types[ch_type];
}

/**
 * lstp_init_subsys_channel() - Bring up a single subsystem channel (1..N).
 * @dev:      LSTP USB device
 * @ch_id:    Channel id to initialise
 * @resp:     The channel's READ_CONFIG reply, from lstp_discover()
 * @resp_len: Bytes in @resp
 *
 * Creates the channel and offers its auxiliary device to the bus. An
 * unimplemented channel type, or a disabled device tree node, is skipped.
 *
 * Return: 1 if offered to the bus, 0 if skipped, negative errno.
 */
static int lstp_init_subsys_channel(struct lstp_usb *dev, u8 ch_id,
				    const struct lstp_ch0_read_resp *resp, size_t resp_len)
{
	struct lstp_channel *ch = lstp_create_channel(dev, ch_id);

	if (!ch)
		return -ENOMEM;
	struct lstp_channel_priv *priv = to_lstp_priv(ch);
	int ret;

	priv->ch_type = resp->desc.ch_type;
	lstp_set_display_name(ch, resp->desc.ch_name, LSTP_CH_NAME_LEN);

	priv->type = lstp_channel_type_lookup(priv->ch_type);
	if (!priv->type) {
		dev_warn(&dev->intf->dev, "%s: ch_%d: Unsupported channel type %d\n", __func__,
			 ch->ch_id, priv->ch_type);
		return 0;
	}

	/* Before the auxdev takes an instance id, so a skipped channel holds none. */
	if (!lstp_init_channel_fwnode(ch, priv->type->compatible)) {
		priv->type = NULL;
		return 0;
	}

	ret = lstp_init_channel_auxdev(priv, resp->ch_config, resp_len - sizeof(*resp));
	if (ret)
		goto err_clear_type;

	/* The bus exists once an auxdev is init'd; hook it before the first add. */
	ret = lstp_aux_notifier_register(dev, &priv->auxdev);
	if (ret)
		goto err_clear_type;

	/* From here ch->dev is the auxdev; framework I/O stays on the interface. */
	ch->dev = &priv->auxdev.dev;
	device_set_node(&priv->auxdev.dev, ch->fwnode);

	ret = lstp_alloc_channel_bufs(ch);
	if (ret)
		goto err_clear_type;

	ret = auxiliary_device_add(&priv->auxdev);
	if (ret)
		goto err_clear_type;
	/* Pairs with smp_load_acquire() in lstp_nack_undelivered(): @type and bufs. */
	smp_store_release(&priv->auxdev_state, LSTP_AUXDEV_ADDED);

	/* Not an error: the driver may be a module not yet loaded, or deferred. */
	if (!priv->auxdev.dev.driver)
		dev_dbg(ch->dev, "%s: ch_%d: No %s driver bound yet\n", __func__, ch->ch_id,
			priv->type->name);

	return 1;

err_clear_type:
	priv->type = NULL;
	return ret;
}

/*
 * Discover the device and offer channels 1..max_ch_id to the auxiliary bus.
 * Returns 0, or a negative errno if ch0 fails; a failed channel is skipped.
 */
static int lstp_init_channels(struct lstp_usb *dev)
{
	struct lstp_discovery *d __free(lstp_discovery) = kzalloc_obj(*d);

	if (!d)
		return -ENOMEM;

	int ret = lstp_ch0_init(dev);

	if (ret)
		return ret;
	ret = lstp_discover(dev, d);
	if (ret)
		return ret;

	dev_info(&dev->intf->dev, "%s: LSTP v%d: device %s discovered with %d channels\n", __func__,
		 dev->lstp_version, dev->lstp_intf_name, dev->max_ch_id);

	unsigned int offered = 0;

	for (int ch_id = 1; ch_id <= dev->max_ch_id; ch_id++) {
		ret = d->err[ch_id];
		if (!ret)
			ret = lstp_init_subsys_channel(dev, ch_id, d->resp[ch_id],
						       d->resp_len[ch_id]);
		if (ret < 0) {
			dev_err(&dev->intf->dev,
				"%s: ch_%d: Channel initialization failed. (%pe)\n", __func__,
				ch_id, ERR_PTR(ret));
			continue;
		}
		offered += ret;
	}

	dev_info(&dev->intf->dev, "%s: offered %u of %u advertised channels to the bus\n", __func__,
		 offered, dev->max_ch_id);

	return 0;
}

/**
 * lstp_probe() - USB driver probe function.
 * @intf: USB interface being probed
 * @id:   USB device ID that matched
 *
 * Creates lstp/ before discovery, so a consumer reacting to a new channel
 * finds lstp/name. Succeeds with no channel bound.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int lstp_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *udev = usb_get_dev(interface_to_usbdev(intf));
	struct lstp_usb *dev = devm_kzalloc(&intf->dev, sizeof(*dev), GFP_KERNEL);
	struct usb_endpoint_descriptor *ep_in, *ep_out;

	if (!dev) {
		usb_put_dev(udev);
		return -ENOMEM;
	}

	int ret = devm_add_action_or_reset(&intf->dev, lstp_put_usb_dev, udev);

	if (ret)
		return ret;

	ret = usb_find_common_endpoints(intf->cur_altsetting, &ep_in, &ep_out, NULL, NULL);
	if (ret) {
		dev_err(&intf->dev, "%s: Could not find bulk endpoints\n", __func__);
		return ret;
	}

	dev->udev = udev;
	dev->intf = intf;
	dev->max_ch_id = 255; /* Maximum possible before device discovery */
	dev->bulk_in_ep = ep_in->bEndpointAddress;
	dev->bulk_out_ep = ep_out->bEndpointAddress;
	dev->bulk_tx_size = min_t(size_t, usb_endpoint_maxp(ep_out), LSTP_USB_EP_MAX_SIZE);
	dev->bulk_rx_size = min_t(size_t, usb_endpoint_maxp(ep_in), LSTP_USB_EP_MAX_SIZE);

	if (dev->bulk_tx_size < LSTP_USB_EP_MIN_SIZE || dev->bulk_rx_size < LSTP_USB_EP_MIN_SIZE) {
		dev_err(&intf->dev, "%s: Invalid endpoint sizes (tx=%zu, rx=%zu, min=%u)\n",
			__func__, dev->bulk_tx_size, dev->bulk_rx_size, LSTP_USB_EP_MIN_SIZE);
		return -EINVAL;
	}

	ret = lstp_init_intf_fwnode(dev);
	if (ret)
		return ret;

	dev->rx_buf = devm_kzalloc(&intf->dev, dev->bulk_rx_size, GFP_KERNEL);
	if (!dev->rx_buf)
		return -ENOMEM;

	dev->bulk_rx_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!dev->bulk_rx_urb)
		return -ENOMEM;
	ret = devm_add_action_or_reset(&intf->dev, lstp_release_urb_slot, &dev->bulk_rx_urb);
	if (ret)
		return ret;

	usb_set_intfdata(intf, dev);

	INIT_DELAYED_WORK(&dev->bulk_rx_retry.work, lstp_rx_retry_work);

	usb_fill_bulk_urb(dev->bulk_rx_urb, dev->udev, usb_rcvbulkpipe(dev->udev, dev->bulk_in_ep),
			  dev->rx_buf, dev->bulk_rx_size, lstp_usb_rx_callback, dev);
	ret = usb_submit_urb(dev->bulk_rx_urb, GFP_KERNEL);
	if (ret) {
		dev_err(&intf->dev, "%s: Could not submit bulk RX URB (%pe)\n", __func__,
			ERR_PTR(ret));
		return ret;
	}

	ret = lstp_create_sysfs_hierarchy(dev);
	if (ret) {
		dev_err(&intf->dev, "%s: Failed to create sysfs hierarchy (%pe)\n", __func__,
			ERR_PTR(ret));
		goto err_teardown;
	}

	ret = lstp_init_channels(dev);
	if (ret)
		goto err_teardown;

	dev_info(&intf->dev, "%s: LSTP device initialized successfully\n", __func__);
	return 0;

err_teardown:
	dev_err(&intf->dev, "%s: Probe failed (%pe)\n", __func__, ERR_PTR(ret));
	lstp_teardown(dev);
	return ret;
}

/* Raise the permanent half of the I/O fence; nothing ever clears it. */
static void lstp_channel_mark_gone(struct lstp_channel *ch)
{
	struct lstp_channel_priv *priv = to_lstp_priv(ch);

	/* Release; pairs with smp_load_acquire() in lstp_channel_disconnected(). */
	smp_store_release(&priv->gone, true);
	wake_up_all(&priv->rx_wq);
}

/* Raise the half of the I/O fence that the next bind clears; see @unbound. */
static void lstp_channel_mark_unbound(struct lstp_channel *ch)
{
	struct lstp_channel_priv *priv = to_lstp_priv(ch);

	/* Pairs with smp_load_acquire() in lstp_channel_disconnected(). */
	smp_store_release(&priv->unbound, true);
	wake_up_all(&priv->rx_wq);
}

static void lstp_signal_disconnect(struct lstp_usb *dev)
{
	for (int i = 0; i < LSTP_MAX_CHANNELS; i++) {
		/* Pairs with smp_store_release() in lstp_create_channel(). */
		struct lstp_channel *ch = smp_load_acquire(&dev->channels[i]);

		if (ch)
			lstp_channel_mark_gone(ch);
	}
}

/* @aux_nb.notifier_call doubles as the "already registered" flag; clear it. */
static void lstp_aux_notifier_unregister(void *data)
{
	struct lstp_usb *dev = data;

	bus_unregister_notifier(dev->aux_bus, &dev->aux_nb);
	dev->aux_nb.notifier_call = NULL;
	dev->aux_bus = NULL;
}

static const struct lstp_ops *lstp_bound_driver_ops(struct device_driver *drv);

/*
 * Fence a channel and reclaim its TX URBs, at the tail of the driver's own
 * unwind: the BIND arm books this as devres on the channel auxdev. Killed,
 * not poisoned, so a rebind can submit again. Once @gone is up the URBs
 * belong to interface teardown, which frees them without this auxdev's lock.
 * See Documentation/driver-api/usb/lstp.rst.
 */
static void lstp_channel_quiesce_unbound(void *data)
{
	struct lstp_channel_priv *priv = data;

	lstp_channel_mark_unbound(&priv->pub);

	/* A request that passed the fence finishes and drops tx_mutex here. */
	guard(mutex)(&priv->tx_mutex);
	/* lstp_drain_inflight() takes tx_mutex after raising @gone. */
	if (smp_load_acquire(&priv->gone))
		return;

	usb_kill_urb(priv->bulk_tx_urb);
	usb_kill_urb(priv->bulk_tx_resp_urb);
}

/**
 * lstp_aux_bus_notify() - One auxiliary-bus notifier per lstp_usb.
 * @nb:     The @aux_nb embedded in the owning lstp_usb.
 * @action: The BUS_NOTIFY_* edge the driver core is reporting.
 * @data:   The struct device the edge is about.
 *
 * Handles every bind/unbind edge of this device's channels. The edges the bus
 * does not report are listed in Documentation/driver-api/usb/lstp.rst.
 *
 * Context: Process context, with the notified device's device_lock held by the
 * driver core.
 * Return: %NOTIFY_DONE always; this notifier observes, it does not veto.
 */
static int lstp_aux_bus_notify(struct notifier_block *nb, unsigned long action, void *data)
{
	struct lstp_usb *dev = container_of(nb, struct lstp_usb, aux_nb);
	struct device *cdev = data;
	struct lstp_channel_priv *priv;
	const struct lstp_ops *ops;

	if (cdev->release != lstp_channel_dev_release)
		return NOTIFY_DONE;
	priv = to_lstp_priv_from_auxdev(to_auxiliary_dev(cdev));
	if (priv->usb != dev)
		return NOTIFY_DONE;

	switch (action) {
	case BUS_NOTIFY_BIND_DRIVER:
		/*
		 * Before .probe(): book the re-fence. Devres is LIFO, so it
		 * fires after everything the driver goes on to register.
		 */
		if (devm_add_action(cdev, lstp_channel_quiesce_unbound, priv)) {
			/* lstp_aux_probe() fails the bind on the raised fence. */
			lstp_channel_mark_unbound(&priv->pub);
			dev_warn(cdev, "%s: ch_%d: Could not book the unbind fence\n", __func__,
				 priv->pub.ch_id);
			break;
		}
		/* Lift the fence before .probe(); @gone is not touched. */
		smp_store_release(&priv->unbound, false);
		break;
	case BUS_NOTIFY_BOUND_DRIVER:
		/* After .probe(): publish the dispatch handle. */
		ops = lstp_bound_driver_ops(cdev->driver);
		scoped_guard(spinlock_irqsave, &priv->irq_cb_lock)
			priv->irq_ops = ops;
		/* Open RX dispatch; lstp_usb_rx_callback() load-acquires. */
		smp_store_release(&priv->started, true);
		break;
	case BUS_NOTIFY_UNBIND_DRIVER:
		/*
		 * Before .remove(): retract dispatch, so no IRQ reaches what
		 * .remove() frees. The fence stays down for .remove() and the
		 * driver's devres unwind.
		 */
		smp_store_release(&priv->started, false);
		scoped_guard(spinlock_irqsave, &priv->irq_cb_lock)
			priv->irq_ops = NULL;
		break;
	case BUS_NOTIFY_DRIVER_NOT_BOUND:
	case BUS_NOTIFY_UNBOUND_DRIVER:
		/*
		 * UNBOUND follows .remove() and the auxdev's devres; NOT_BOUND
		 * precedes a failed probe's devres unwind, which must therefore
		 * not reach the bind's state through @priv.
		 */
		priv->pub.priv = NULL;
		break;
	}
	return NOTIFY_DONE;
}

/*
 * Register the per-device aux-bus notifier once, before the first
 * auxiliary_device_add(), so an already-registered driver's first BOUND is
 * seen. The bus comes from the auxdev: auxiliary_bus_type is not exported.
 */
static int lstp_aux_notifier_register(struct lstp_usb *dev, struct auxiliary_device *auxdev)
{
	int ret;

	if (dev->aux_nb.notifier_call)
		return 0;
	dev->aux_bus = auxdev->dev.bus;
	dev->aux_nb.notifier_call = lstp_aux_bus_notify;
	ret = bus_register_notifier(dev->aux_bus, &dev->aux_nb);
	if (ret) {
		dev->aux_nb.notifier_call = NULL;
		dev->aux_bus = NULL;
		return ret;
	}
	return devm_add_action_or_reset(&dev->intf->dev, lstp_aux_notifier_unregister, dev);
}

static void lstp_ch_signal_response(struct lstp_channel *ch)
{
	struct lstp_channel_priv *priv = to_lstp_priv(ch);

	/* Pairs with smp_load_acquire() in lstp_ch_got_response(): @resp_buf first. */
	smp_store_release(&priv->rx_ready, true);
	wake_up_all(&priv->rx_wq);
}

static inline bool lstp_ch_got_response(struct lstp_channel *ch)
{
	/* Acquire pairs with the smp_store_release() in lstp_ch_signal_response(). */
	return smp_load_acquire(&to_lstp_priv(ch)->rx_ready);
}

static inline bool lstp_ch_should_wake(struct lstp_channel *ch)
{
	return lstp_ch_got_response(ch) || lstp_channel_disconnected(ch);
}

/*
 * Poison first: a resubmit racing in from the RX completion or the retry
 * worker then takes -EPERM, which lstp_rx_handle_err() routes to the
 * disconnect arm without rescheduling, so the cancel below cannot lose a
 * self-requeue.
 */
static void lstp_stop_rx(struct lstp_usb *dev)
{
	usb_poison_urb(dev->bulk_rx_urb);
	cancel_delayed_work_sync(&dev->bulk_rx_retry.work);
}

/* Poison every channel's TX URBs, so a racing usb_submit_urb() gets -EPERM. */
static void lstp_stop_tx(struct lstp_usb *dev)
{
	for (int i = 0; i < LSTP_MAX_CHANNELS; i++) {
		struct lstp_channel *ch = dev->channels[i];

		if (!ch)
			continue;
		struct lstp_channel_priv *priv = to_lstp_priv(ch);

		usb_poison_urb(priv->bulk_tx_urb);
		usb_poison_urb(priv->bulk_tx_resp_urb);
	}
}

/*
 * Wait out in-flight channel I/O. The fence is already raised, so a caller
 * mid-submission finishes and drops tx_mutex, and every later one sees the
 * fence under it. Nothing then touches @tx_buf or @resp_buf when devres frees
 * them.
 */
static void lstp_drain_inflight(struct lstp_usb *dev)
{
	for (int i = 0; i < LSTP_MAX_CHANNELS; i++) {
		struct lstp_channel *ch = dev->channels[i];

		if (!ch)
			continue;
		struct lstp_channel_priv *priv = to_lstp_priv(ch);

		mutex_lock(&priv->tx_mutex);
		mutex_unlock(&priv->tx_mutex);
	}
}

/**
 * lstp_teardown() - Ordered driver-level teardown of the LSTP device.
 * @dev: LSTP USB device structure
 *
 * Shared with the lstp_probe() error path, since USB core does not call
 * .disconnect() on probe failure. Subsystem teardown runs later, against freed
 * framework buffers; the fence raised here, not the drain, keeps it off them.
 * See Documentation/driver-api/usb/lstp.rst.
 */
static void lstp_teardown(struct lstp_usb *dev)
{
	/* RX must stop before aux_driver .remove() runs, or dispatch can race it. */
	lstp_stop_rx(dev);

	lstp_signal_disconnect(dev);
	lstp_drain_inflight(dev);
	lstp_stop_tx(dev);
}

/* USB .disconnect(); the rest unwinds via devres on interface unbind. */
static void lstp_disconnect(struct usb_interface *intf)
{
	struct lstp_usb *dev = usb_get_intfdata(intf);

	dev_info(&intf->dev, "%s: LSTP device disconnected\n", __func__);

	if (!dev)
		return;

	lstp_teardown(dev);
}

static void lstp_usb_tx_callback(struct urb *urb)
{
	struct lstp_channel *ch = urb->context;

	if (urb->status != 0 && urb->status != -ENOENT && urb->status != -ECONNRESET &&
	    urb->status != -ESHUTDOWN) {
		dev_warn_ratelimited(ch->dev, "%s: ch_%d: TX URB error (%pe)\n", __func__,
				     ch->ch_id, ERR_PTR(urb->status));
	}
}

/*
 * Completion for @bulk_tx_resp_urb. Clears @irq_resp_buffer_lock on every
 * path, kills included, so the bit cannot stay set across a re-probe.
 */
static void lstp_usb_resp_tx_callback(struct urb *urb)
{
	struct lstp_channel *ch = urb->context;

	if (urb->status != 0 && urb->status != -ENOENT && urb->status != -ECONNRESET &&
	    urb->status != -ESHUTDOWN) {
		dev_warn_ratelimited(ch->dev, "%s: ch_%d: RX response TX URB error (%pe)\n",
				     __func__, ch->ch_id, ERR_PTR(urb->status));
	}

	clear_bit(LSTP_BUFFER_LOCK_BIT, &to_lstp_priv(ch)->irq_resp_buffer_lock);
}

/**
 * lstp_send_irq_resp() - Post a zero-payload response for an unsolicited RX packet.
 * @ch:     LSTP channel that received the IRQ packet
 * @status: Status byte to return to firmware (bit 7 is forced to 1 = response)
 *
 * Failures are logged and swallowed. A response posted while the previous one
 * is in flight is dropped; firmware keeps one request in flight, so that
 * should not happen.
 *
 * Context: Atomic, from an ops->irq_packet handler on the USB RX completion
 *          path, or from the core's RX completion.
 */
void lstp_send_irq_resp(struct lstp_channel *ch, u8 status)
{
	if (!ch)
		return;
	struct lstp_channel_priv *priv = to_lstp_priv(ch);

	if (!priv->usb || !priv->usb->udev)
		return;

	/*
	 * NULL during interface unwind, and before the channel is offered,
	 * when lstp_nack_undelivered() does not call; see
	 * lstp_alloc_channel_bufs().
	 */
	if (!priv->tx_resp_buf || !priv->bulk_tx_resp_urb)
		return;

	if (test_and_set_bit(LSTP_BUFFER_LOCK_BIT, &priv->irq_resp_buffer_lock)) {
		dev_warn_ratelimited(ch->dev,
				     "%s: ch_%d: Dropping response - prior URB in flight\n",
				     __func__, ch->ch_id);
		return;
	}

	struct lstp_packet *tx_pkt = (struct lstp_packet *)priv->tx_resp_buf;

	tx_pkt->hdr.ch_id = ch->ch_id;
	tx_pkt->hdr.length = cpu_to_le16(0);
	tx_pkt->hdr.status = LSTP_HDR_RESPONSE | FIELD_PREP(LSTP_HDR_CODE, status);

	usb_fill_bulk_urb(priv->bulk_tx_resp_urb, priv->usb->udev,
			  usb_sndbulkpipe(priv->usb->udev, priv->usb->bulk_out_ep), tx_pkt,
			  sizeof(struct lstp_header), lstp_usb_resp_tx_callback, ch);

	int ret = usb_submit_urb(priv->bulk_tx_resp_urb, GFP_ATOMIC);

	if (ret) {
		clear_bit(LSTP_BUFFER_LOCK_BIT, &priv->irq_resp_buffer_lock);
		/* Unbind kill, unplug or shutdown: expected, not an error. */
		if (ret == -EPERM || ret == -ENODEV || ret == -ESHUTDOWN)
			dev_dbg_ratelimited(ch->dev, "%s: ch_%d: Failed to submit TX URB (%pe)\n",
					    __func__, ch->ch_id, ERR_PTR(ret));
		else
			dev_err_ratelimited(ch->dev, "%s: ch_%d: Failed to submit TX URB (%pe)\n",
					    __func__, ch->ch_id, ERR_PTR(ret));
	}
}
EXPORT_SYMBOL_NS_GPL(lstp_send_irq_resp, "LSTP");

/*
 * RX resubmits retry forever with bounded exponential backoff: an attempt
 * budget would kill RX for good on a transient -ENOMEM. BASE_DELAY also
 * throttles completion errors from a babbling device, and stays well under
 * LSTP_USB_RESPONSE_TIMEOUT_MS.
 */
#define LSTP_RX_RETRY_BASE_DELAY_MS 50
#define LSTP_RX_RETRY_MAX_DELAY_MS 1000
#define LSTP_RX_RETRY_MAX_BACKOFF_SHIFT 5U

static unsigned long lstp_rx_retry_backoff_ms(unsigned int failures)
{
	unsigned int shift = failures ? min(failures - 1, LSTP_RX_RETRY_MAX_BACKOFF_SHIFT) : 0;
	unsigned long ms = (unsigned long)LSTP_RX_RETRY_BASE_DELAY_MS << shift;

	return min_t(unsigned long, ms, LSTP_RX_RETRY_MAX_DELAY_MS);
}

/*
 * Record @ret for lstp_rx_retry_work() and schedule it after @delay_ms, or
 * signal disconnect on a terminal errno.
 */
static void lstp_rx_handle_err(struct lstp_usb *dev, int ret, unsigned long delay_ms)
{
	struct lstp_rx_retry *rxr = &dev->bulk_rx_retry;

	WRITE_ONCE(rxr->last_err, ret);

	/*
	 * lstp_stop_rx()'s poison (-EPERM), or the device or HCD gone: the
	 * expected disconnect, already logged by lstp_disconnect().
	 */
	if (ret == -ENODEV || ret == -ESHUTDOWN || ret == -EPERM) {
		lstp_signal_disconnect(dev);
		return;
	}

	/* Wire-side errors can fire back-to-back; submit-side ones are rare. */
	if (ret == -EPROTO || ret == -EOVERFLOW || ret == -EILSEQ)
		dev_warn_ratelimited(&dev->intf->dev,
				     "%s: RX URB transient error (%pe), retry in %lums\n", __func__,
				     ERR_PTR(ret), delay_ms);
	else
		dev_warn(&dev->intf->dev, "%s: RX URB transient error (%pe), retry in %lums\n",
			 __func__, ERR_PTR(ret), delay_ms);

	schedule_delayed_work(&rxr->work, msecs_to_jiffies(delay_ms));
}

/* Process context: clears an endpoint halt and resubmits with GFP_KERNEL. */
static void lstp_rx_retry_work(struct work_struct *work)
{
	struct lstp_rx_retry *rxr = container_of(to_delayed_work(work), struct lstp_rx_retry, work);
	struct lstp_usb *dev = container_of(rxr, struct lstp_usb, bulk_rx_retry);
	int last_err = READ_ONCE(rxr->last_err);

	if (last_err == -EPIPE) {
		atomic64_inc(&dev->stats.rx_stalls);

		int ret = usb_clear_halt(dev->udev, usb_rcvbulkpipe(dev->udev, dev->bulk_in_ep));

		if (ret)
			dev_warn(&dev->intf->dev, "%s: usb_clear_halt failed (%pe)\n", __func__,
				 ERR_PTR(ret));
	}

	atomic64_inc(&dev->stats.rx_retries);

	int ret = usb_submit_urb(dev->bulk_rx_urb, GFP_KERNEL);

	if (ret == 0) {
		if (rxr->failures)
			dev_info(&dev->intf->dev, "%s: RX URB resubmitted after %u failure(s)\n",
				 __func__, rxr->failures);
		rxr->failures = 0;
		return;
	}

	rxr->failures++;
	lstp_rx_handle_err(dev, ret, lstp_rx_retry_backoff_ms(rxr->failures));
}

/**
 * lstp_dispatch_irq_packet() - Deliver an unsolicited packet to the bound driver.
 * @ch:  Channel that received the unsolicited request.
 * @pkt: Validated packet, valid only for this call.
 *
 * The only path from the RX completion into a subsystem. Reads @irq_ops under
 * @irq_cb_lock, under which the notifier publishes and retracts it, so an
 * unbind cannot free driver state under the upcall. The lock is held with IRQs
 * off across ops->irq_packet.
 *
 * Context: Atomic, USB RX completion.
 * Return: true if a handler ran; false if none did, and the caller books a drop.
 */
static bool lstp_dispatch_irq_packet(struct lstp_channel *ch, struct lstp_packet *pkt)
{
	struct lstp_channel_priv *priv = to_lstp_priv(ch);

	guard(spinlock_irqsave)(&priv->irq_cb_lock);
	if (!priv->irq_ops || !priv->irq_ops->irq_packet)
		return false;

	priv->irq_ops->irq_packet(ch, pkt);
	return true;
}

/*
 * NACK a device-initiated request that no driver took, on a channel type
 * whose device waits for the response, so it retries instead of stalling
 * the channel. Context: Atomic, USB RX completion.
 */
static void lstp_nack_undelivered(struct lstp_channel_priv *priv)
{
	/*
	 * Pairs with smp_store_release() in lstp_init_subsys_channel(): once
	 * ADDED, @type is set for good and the response slot is in place.
	 */
	if (smp_load_acquire(&priv->auxdev_state) != LSTP_AUXDEV_ADDED)
		return;
	if (lstp_usb_gone(priv->usb))
		return;
	if (priv->type->irq_needs_resp)
		lstp_send_irq_resp(&priv->pub, LSTP_NACK);
}

/* RX completion: requests (bit7=0) go to lstp_dispatch_irq_packet(), responses wake waiters. */
static void lstp_usb_rx_callback(struct urb *urb)
{
	struct lstp_usb *dev = urb->context;
	int ret;

	if (urb->status == -ENOENT || urb->status == -ECONNRESET || urb->status == -ESHUTDOWN) {
		lstp_signal_disconnect(dev);
		return;
	}

	if (urb->status) {
		atomic64_inc(&dev->stats.rx_urb_errors);
		lstp_rx_handle_err(dev, urb->status, LSTP_RX_RETRY_BASE_DELAY_MS);
		return;
	}

	atomic64_inc(&dev->stats.rx_packets);

	struct lstp_packet *rx_pkt = (struct lstp_packet *)dev->rx_buf;

	if (lstp_validate_rx_pkt(dev, rx_pkt, urb->actual_length)) {
		atomic64_inc(&dev->stats.rx_errors);
		goto resubmit;
	}

	/* ch_id range-checked by lstp_validate_rx_pkt(). */
	u8 ch_id = rx_pkt->hdr.ch_id;
	/* Pairs with smp_store_release() in lstp_create_channel(). */
	struct lstp_channel *ch = smp_load_acquire(&dev->channels[ch_id]);

	if (!ch) {
		atomic64_inc(&dev->stats.rx_no_channel);
		dev_err_ratelimited(&dev->intf->dev, "%s: ch_%d: Channel not registered\n",
				    __func__, ch_id);
		goto resubmit;
	}
	struct lstp_channel_priv *priv = to_lstp_priv(ch);

	/* Pairs with smp_store_release() in the BOUND arm of lstp_aux_bus_notify(). */
	bool in_service = smp_load_acquire(&priv->started);

	/* Pairs with smp_store_release() in lstp_alloc_channel_irq_bufs(). */
	void *irq_buf = smp_load_acquire(&priv->irq_buf);

	if (in_service && !FIELD_GET(LSTP_HDR_RESPONSE, rx_pkt->hdr.status) && irq_buf) {
		if (test_and_set_bit(LSTP_BUFFER_LOCK_BIT, &priv->irq_buffer_lock)) {
			atomic64_inc(&priv->stats.rx_dropped);
			dev_warn_ratelimited(ch->dev,
					     "%s: ch_%d: Dropping request - irq buffer busy\n",
					     __func__, ch_id);
			lstp_nack_undelivered(priv);
		} else {
			struct lstp_packet *irq_pkt = (struct lstp_packet *)irq_buf;
			bool delivered;

			dev_dbg(ch->dev, "%s: ch_%d: Received unsolicited request (IRQ event)\n",
				__func__, ch_id);
			memcpy(irq_buf, dev->rx_buf, urb->actual_length);
			delivered = lstp_dispatch_irq_packet(ch, irq_pkt);
			clear_bit(LSTP_BUFFER_LOCK_BIT, &priv->irq_buffer_lock);

			/* rx_events counts delivery, not arrival. */
			if (delivered) {
				atomic64_inc(&priv->stats.rx_events);
			} else {
				atomic64_inc(&priv->stats.rx_dropped);
				lstp_nack_undelivered(priv);
			}
		}
	} else if (FIELD_GET(LSTP_HDR_RESPONSE, rx_pkt->hdr.status) && priv->resp_buf) {
		/*
		 * The first reply after a request answers it; the waiter may still
		 * be reading @resp_buf, so a second one must not overwrite it.
		 */
		if (!test_bit(LSTP_BUFFER_LOCK_BIT, &priv->resp_buffer_lock) ||
		    lstp_ch_got_response(ch)) {
			atomic64_inc(&priv->stats.rx_late);
			dev_warn_ratelimited(ch->dev,
					     "%s: ch_%d: Dropping response - no one waiting\n",
					     __func__, ch_id);
		} else {
			memcpy(priv->resp_buf, dev->rx_buf, urb->actual_length);
			lstp_ch_signal_response(ch);
		}
	} else if (!in_service && priv->type) {
		/*
		 * Implemented but unclaimed: the driver may not be loaded yet.
		 * @type keeps ch0 and failed channels in the warn arm below.
		 */
		atomic64_inc(&priv->stats.rx_dropped);
		dev_dbg_ratelimited(ch->dev, "%s: ch_%d: Dropping packet - no driver bound\n",
				    __func__, ch_id);
		if (!FIELD_GET(LSTP_HDR_RESPONSE, rx_pkt->hdr.status))
			lstp_nack_undelivered(priv);
	} else {
		atomic64_inc(&priv->stats.rx_dropped);
		dev_warn_ratelimited(ch->dev, "%s: ch_%d: Dropping packet - no handler for it\n",
				     __func__, ch_id);
		if (!FIELD_GET(LSTP_HDR_RESPONSE, rx_pkt->hdr.status))
			lstp_nack_undelivered(priv);
	}

resubmit:
	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret)
		lstp_rx_handle_err(dev, ret, LSTP_RX_RETRY_BASE_DELAY_MS);
}

/**
 * lstp_channel_disconnected() - Check whether the channel can carry traffic.
 * @ch: LSTP channel to check; a NULL @ch counts as unable.
 *
 * Context: Any context.
 * Return: true if the channel is fenced (see
 * Documentation/driver-api/usb/lstp.rst). lstp_send(), lstp_request() and
 * lstp_requestv() on it then fail with -ENODEV, so the caller must stop
 * issuing traffic and unwind.
 */
bool lstp_channel_disconnected(const struct lstp_channel *ch)
{
	if (!ch)
		return true;

	const struct lstp_channel_priv *priv = to_lstp_priv(ch);

	/* Pairs with smp_store_release() in the two mark helpers. */
	return smp_load_acquire(&priv->gone) || smp_load_acquire(&priv->unbound);
}
EXPORT_SYMBOL_NS_GPL(lstp_channel_disconnected, "LSTP");

/**
 * lstp_send() - Send a one-way LSTP packet on the bulk-out endpoint.
 * @ch:      LSTP channel to send on
 * @cmd:     LSTP command byte. Bit 7 is forced to 0 (request direction)
 * @payload: Payload bytes to copy into the channel's TX buffer; may be NULL
 *           if @len == 0
 * @len:     Payload length in bytes
 *
 * Fire-and-forget: no response is read. The TX mutex covers the fence test and
 * the submission, which lstp_drain_inflight() relies on.
 *
 * Context: Process context. May sleep.
 *
 * Return: 0 on success; -ENODEV if @ch is NULL, not initialized for I/O, or
 *         fenced; -EINVAL if @payload is NULL with @len > 0; -EMSGSIZE if
 *         @len exceeds the channel's TX payload limit; otherwise the errno
 *         from usb_bulk_msg().
 */
int lstp_send(struct lstp_channel *ch, u8 cmd, const void *payload, size_t len)
{
	size_t total;

	if (!ch)
		return -ENODEV;
	struct lstp_channel_priv *priv = to_lstp_priv(ch);

	if (!priv->usb || !priv->tx_buf)
		return -ENODEV;

	if (len && !payload)
		return -EINVAL;

	if (len > ch->max_tx_payload || check_add_overflow(sizeof(struct lstp_header), len, &total))
		return -EMSGSIZE;

	guard(lstp_channel)(ch);
	if (lstp_channel_disconnected(ch))
		return -ENODEV;

	struct lstp_packet *tx_pkt = (struct lstp_packet *)priv->tx_buf;

	tx_pkt->hdr.ch_id = ch->ch_id;
	tx_pkt->hdr.cmd = FIELD_PREP(LSTP_HDR_CODE, cmd); /* request: LSTP_HDR_RESPONSE clear */
	tx_pkt->hdr.length = cpu_to_le16(len);
	if (len)
		memcpy(tx_pkt->payload, payload, len);

	int ret = usb_bulk_msg(priv->usb->udev,
			       usb_sndbulkpipe(priv->usb->udev, priv->usb->bulk_out_ep),
			       priv->tx_buf, total, NULL, LSTP_USB_REQUEST_TIMEOUT_MS);

	/* -EPERM/-ESHUTDOWN/-ENODEV are disconnect outcomes, not failures. */
	if (!ret)
		atomic64_inc(&priv->stats.tx_packets);
	else if (ret != -EPERM && ret != -ESHUTDOWN && ret != -ENODEV)
		atomic64_inc(&priv->stats.tx_errors);

	return ret;
}
EXPORT_SYMBOL_NS_GPL(lstp_send, "LSTP");

/*
 * One request/response exchange on @ch, the request already in priv->tx_buf;
 * see lstp_request() for the modes. The caller holds priv->tx_mutex and has
 * tested the fence under it, so a TX URB a fenced timeout left queued is not
 * resubmitted. Holding priv->resp_buffer_lock is what tells
 * lstp_usb_rx_callback() a waiter exists.
 */
static int lstp_request_xfer(struct lstp_channel *ch, u8 cmd, u16 req_len, void *resp,
			     size_t resp_cap, size_t *resp_len, int *lstp_status)
{
	struct lstp_channel_priv *priv = to_lstp_priv(ch);
	struct lstp_packet *tx_pkt = (struct lstp_packet *)priv->tx_buf;
	struct lstp_packet *rx_pkt = (struct lstp_packet *)priv->resp_buf;
	/* Strict mode makes @resp_cap the wire floor; variable mode has none. */
	u16 wire_min = resp_len ? LSTP_ANY_RX_LEN : (u16)resp_cap;
	size_t to_copy = 0;

	if (test_and_set_bit(LSTP_BUFFER_LOCK_BIT, &priv->resp_buffer_lock)) {
		dev_err(ch->dev, "%s: ch_%d: BUG - rx buffer already in use\n", __func__,
			ch->ch_id);
		return -EBUSY;
	}

	tx_pkt->hdr.ch_id = ch->ch_id;
	tx_pkt->hdr.cmd = FIELD_PREP(LSTP_HDR_CODE, cmd); /* request: LSTP_HDR_RESPONSE clear */
	tx_pkt->hdr.length = cpu_to_le16(req_len);

	if (req_len > 0)
		dev_dbg(ch->dev, "%s: ch_%d: TX cmd=0x%02x, tx_len=%u, rx_min=%u, payload=0x%*ph\n",
			__func__, ch->ch_id, cmd, req_len, wire_min, req_len, tx_pkt->payload);
	else
		dev_dbg(ch->dev, "%s: ch_%d: TX cmd=0x%02x, tx_len=0, rx_min=%u\n", __func__,
			ch->ch_id, cmd, wire_min);

	usb_fill_bulk_urb(priv->bulk_tx_urb, priv->usb->udev,
			  usb_sndbulkpipe(priv->usb->udev, priv->usb->bulk_out_ep), priv->tx_buf,
			  sizeof(struct lstp_header) + req_len, lstp_usb_tx_callback, ch);

	/* Plain store: the usb_submit_urb() below orders it before any response. */
	WRITE_ONCE(priv->rx_ready, false);

	int ret = usb_submit_urb(priv->bulk_tx_urb, GFP_KERNEL);

	if (ret) {
		/* Teardown raced the submit; report a disconnect, not -EPERM. */
		if (ret == -EPERM || ret == -ESHUTDOWN)
			ret = -ENODEV;

		/* -ENODEV here is a disconnect outcome, not a failure. */
		if (ret != -ENODEV)
			atomic64_inc(&priv->stats.tx_errors);
		goto out_release;
	}

	atomic64_inc(&priv->stats.tx_packets);

	wait_event_timeout(priv->rx_wq, lstp_ch_should_wake(ch),
			   msecs_to_jiffies(LSTP_USB_RESPONSE_TIMEOUT_MS));

	if (!lstp_ch_got_response(ch)) {
		if (lstp_channel_disconnected(ch)) {
			/*
			 * Fenced: the queued URB is left to teardown, see
			 * out_release. The fence, not lstp_usb_gone(), is
			 * what stops the next request from resubmitting it.
			 */
			ret = -ENODEV;
		} else {
			atomic64_inc(&priv->stats.timeouts);
			/* Removed device: -ENODEV, not a retryable timeout. */
			ret = lstp_usb_gone(priv->usb) ? -ENODEV : -ETIMEDOUT;
		}
		dev_err_ratelimited(ch->dev, "%s: ch_%d: No response (%pe)\n", __func__, ch->ch_id,
				    ERR_PTR(ret));
		goto out_release;
	}

	ret = lstp_validate_resp(priv->usb, rx_pkt, wire_min, lstp_status);
	if (ret) {
		atomic64_inc(&priv->stats.errors);
		goto out_release;
	}

	atomic64_inc(&priv->stats.rx_packets);

	size_t actual = le16_to_cpu(rx_pkt->hdr.length);

	dev_dbg(ch->dev, "%s: ch_%d: RX rx_len=%zu\n", __func__, ch->ch_id, actual);

	if (resp_len) {
		if (actual > resp_cap) {
			ret = -EMSGSIZE;
			goto out_release;
		}
		to_copy = actual;
		*resp_len = actual;
	} else {
		/* Fixed mode: a longer reply is truncated to @resp_cap. */
		to_copy = resp_cap;
	}

	if (resp && to_copy)
		memcpy(resp, rx_pkt->payload, to_copy);

	if (lstp_status)
		*lstp_status = LSTP_SUCCESS;

out_release:
	/*
	 * Retire the TX URB before returning, even on success: a stale reply
	 * (see struct lstp_header) can answer this request before its transfer
	 * completes, and the next request reuses the URB and @tx_buf. Cheap once
	 * retired, and @tx_mutex keeps any submit off the @reject this raises.
	 * Skipped when fenced; see Documentation/driver-api/usb/lstp.rst.
	 */
	if (!lstp_channel_disconnected(ch))
		usb_kill_urb(priv->bulk_tx_urb);

	clear_bit(LSTP_BUFFER_LOCK_BIT, &priv->resp_buffer_lock);
	return ret;
}

/**
 * lstp_request() - Issue an LSTP request/response exchange.
 * @ch:       LSTP channel
 * @cmd:      LSTP command byte. Bit 7 is forced to 0 (request direction)
 * @req:      Request payload to copy into the channel's TX buffer; may be
 *            NULL if @req_len == 0
 * @req_len:  Request payload length in bytes
 * @resp:     Caller buffer that receives the response payload; may be NULL
 *            if @resp_cap == 0, or when @resp_len is non-NULL to discard
 *            the response payload
 * @resp_cap: Capacity of @resp in bytes. The exact expected response length
 *            in fixed mode; an upper bound on the device-reported length in
 *            variable mode
 * @resp_len: Out-param for the on-wire response length, or NULL. The pointer
 *            doubles as the mode selector:
 *
 *            * NULL (fixed): the device must reply with at least @resp_cap
 *              bytes or the call fails with -EIO. Longer replies are
 *              accepted for forward compatibility and only the first
 *              @resp_cap bytes are copied into @resp
 *
 *            * non-NULL (variable): no lower bound. A reply longer than
 *              @resp_cap fails with -EMSGSIZE; otherwise the reported length
 *              is stored in *@resp_len and that many bytes are copied into
 *              @resp
 * @lstp_status: Optional out-param for the raw LSTP response status. Set to
 *            the device status when the exchange succeeds or when the
 *            returned errno is derived from that status, and to
 *            LSTP_NO_RESPONSE_STATUS otherwise. Unmodified if NULL
 *
 * One exchange per call, under the channel TX mutex, so at most one request is
 * in flight per channel. Use lstp_requestv() for a payload spread over several
 * buffers.
 *
 * Context: Process context. May sleep.
 *
 * Return: 0 on success; -ENODEV if @ch is NULL, not initialized for I/O, or
 *         fenced; -EINVAL for a NULL @req or fixed-mode @resp with a non-zero
 *         length; -EMSGSIZE if a length exceeds the channel's payload limits
 *         or a variable-mode reply exceeds @resp_cap; -EIO if a fixed-mode
 *         reply is short; -ETIMEDOUT if the device does not answer; otherwise
 *         the errno from the LSTP status or the USB I/O.
 */
int lstp_request(struct lstp_channel *ch, u8 cmd, const void *req, size_t req_len, void *resp,
		 size_t resp_cap, size_t *resp_len, int *lstp_status)
{
	return lstp_requestv(ch, cmd,
			     &(const struct kvec){
				     .iov_base = (void *)req,
				     .iov_len = req_len,
			     },
			     1, resp, resp_cap, resp_len, lstp_status);
}
EXPORT_SYMBOL_NS_GPL(lstp_request, "LSTP");

/**
 * lstp_requestv() - Vectored variant of lstp_request().
 * @ch:       LSTP channel
 * @cmd:      LSTP command byte; see lstp_request()
 * @vecs:     Input kvec vector, concatenated into the TX payload in order.
 *            An entry's @iov_base may be NULL only when its @iov_len is 0.
 *            May be NULL if @nvecs is 0
 * @nvecs:    Number of entries in @vecs; may be 0 for an empty request
 * @resp:     see lstp_request()
 * @resp_cap: see lstp_request()
 * @resp_len: see lstp_request()
 * @lstp_status: see lstp_request()
 *
 * As lstp_request(), with the request payload gathered from @vecs.
 *
 * Context: Process context. May sleep.
 *
 * Return: see lstp_request().
 */
int lstp_requestv(struct lstp_channel *ch, u8 cmd, const struct kvec *vecs, size_t nvecs,
		  void *resp, size_t resp_cap, size_t *resp_len, int *lstp_status)
{
	size_t req_total = 0;

	/* Fail-safe default: only the xfer/validation publish a real status. */
	if (lstp_status)
		*lstp_status = LSTP_NO_RESPONSE_STATUS;

	if (!ch)
		return -ENODEV;
	struct lstp_channel_priv *priv = to_lstp_priv(ch);

	if (!priv->usb || !priv->tx_buf || !priv->resp_buf)
		return -ENODEV;

	if (nvecs && !vecs)
		return -EINVAL;
	if (!resp_len && resp_cap && !resp)
		return -EINVAL;

	for (size_t i = 0; i < nvecs; i++) {
		if (vecs[i].iov_len && !vecs[i].iov_base)
			return -EINVAL;
		if (check_add_overflow(req_total, vecs[i].iov_len, &req_total))
			return -EMSGSIZE;
	}

	if (req_total > ch->max_tx_payload || resp_cap > ch->max_rx_payload)
		return -EMSGSIZE;

	guard(lstp_channel)(ch);
	/* Fence first: on a channel pinned past disconnect, tx_buf is freed. */
	if (lstp_channel_disconnected(ch))
		return -ENODEV;

	struct lstp_packet *tx_pkt = (struct lstp_packet *)priv->tx_buf;
	size_t off = 0;

	for (size_t i = 0; i < nvecs; i++) {
		if (vecs[i].iov_len) {
			memcpy(tx_pkt->payload + off, vecs[i].iov_base, vecs[i].iov_len);
			off += vecs[i].iov_len;
		}
	}

	return lstp_request_xfer(ch, cmd, (u16)req_total, resp, resp_cap, resp_len, lstp_status);
}
EXPORT_SYMBOL_NS_GPL(lstp_requestv, "LSTP");

#define LSTP_USB_VID_NVIDIA 0x0955
#define LSTP_USB_VID_MICROCHIP 0x0424
#define LSTP_USB_VID_ASMEDIA 0x174c

#define LSTP_USB_SUBCLASS 0x3f

#define LSTP_USB_DEVICE(vid, pid)                                                         \
	USB_DEVICE_AND_INTERFACE_INFO(vid, pid, USB_CLASS_VENDOR_SPEC, LSTP_USB_SUBCLASS, \
				      LSTP_VERSION)

static const struct usb_device_id lstp_id_table[] = {
	{ LSTP_USB_DEVICE(LSTP_USB_VID_NVIDIA, 0xcf11) },
	{ LSTP_USB_DEVICE(LSTP_USB_VID_MICROCHIP, 0x4940) },
	{ LSTP_USB_DEVICE(LSTP_USB_VID_ASMEDIA, 0x2214) },
	{}
};
MODULE_DEVICE_TABLE(usb, lstp_id_table);

static struct usb_driver lstp_usb_driver = {
	.name = "lstp",
	.probe = lstp_probe,
	.disconnect = lstp_disconnect,
	.id_table = lstp_id_table,
};

/*
 * Interposed ahead of the subsystem's .probe(): set up what the matched driver
 * needs, then delegate. A notifier cannot refuse a bind; this can.
 */
static int lstp_aux_probe(struct auxiliary_device *auxdev, const struct auxiliary_device_id *id)
{
	const struct lstp_auxiliary_driver *drv = to_lstp_aux_drv(auxdev->dev.driver);
	struct lstp_channel_priv *priv = to_lstp_priv_from_auxdev(auxdev);
	int ret;

	/*
	 * Refuse a fenced channel: its device is gone and the interface's
	 * devres, which holds the channel's framework buffers, may be
	 * unwinding. Also refuses a bind whose BIND arm could not book its
	 * unwind. Nothing below is booked on the interface.
	 */
	if (lstp_channel_disconnected(&priv->pub))
		return -ENODEV;

	if (drv->ops && drv->ops->irq_packet) {
		ret = lstp_alloc_channel_irq_bufs(priv);
		if (ret)
			return ret;
	}

	return drv->subsys_probe(auxdev, id);
}

/*
 * The LSTP ops of a driver we registered, or NULL. The bus matches on the
 * device-name prefix, so a foreign driver can bind our channel, and
 * container_of() on it would be type confusion reached from URB completion.
 * Only our drivers carry lstp_aux_probe() as .probe(): that is the test.
 */
static const struct lstp_ops *lstp_bound_driver_ops(struct device_driver *drv)
{
	if (!drv || to_auxiliary_drv(drv)->probe != lstp_aux_probe)
		return NULL;

	return to_lstp_aux_drv(drv)->ops;
}

/**
 * __lstp_auxiliary_driver_register() - Register an LSTP subsystem aux_driver.
 * @drv:     LSTP auxiliary driver to register.
 * @owner:   Module owning @drv.
 * @modname: Name of that module.
 *
 * Interposes lstp_aux_probe() ahead of the subsystem's .probe(), then
 * registers the embedded adrv. Call it through lstp_auxiliary_driver_register().
 *
 * Context: Process context. May sleep.
 * Return: 0 on success, negative errno on failure.
 */
int __lstp_auxiliary_driver_register(struct lstp_auxiliary_driver *drv, struct module *owner,
				     const char *modname)
{
	if (WARN_ON(!drv->adrv.probe))
		return -EINVAL;

	/* Idempotent: an unregister/register cycle must not chain the wrapper. */
	if (drv->adrv.probe != lstp_aux_probe) {
		drv->subsys_probe = drv->adrv.probe;
		drv->adrv.probe = lstp_aux_probe;
	}

	int ret = __auxiliary_driver_register(&drv->adrv, owner, modname);

	/* A driver that failed to register is never unregistered: undo it here. */
	if (ret && drv->subsys_probe) {
		drv->adrv.probe = drv->subsys_probe;
		drv->subsys_probe = NULL;
	}

	return ret;
}
EXPORT_SYMBOL_NS_GPL(__lstp_auxiliary_driver_register, "LSTP");

/**
 * lstp_auxiliary_driver_unregister() - Unregister an LSTP subsystem aux_driver.
 * @drv: LSTP auxiliary driver to unregister.
 *
 * Restores the subsystem's own .probe(), so the interposed wrapper does not
 * outlive its registration in a struct owned by the child module.
 *
 * Context: Process context. May sleep.
 */
void lstp_auxiliary_driver_unregister(struct lstp_auxiliary_driver *drv)
{
	auxiliary_driver_unregister(&drv->adrv);

	if (drv->subsys_probe) {
		drv->adrv.probe = drv->subsys_probe;
		drv->subsys_probe = NULL;
	}
}
EXPORT_SYMBOL_NS_GPL(lstp_auxiliary_driver_unregister, "LSTP");

static int __init lstp_module_init(void)
{
	return usb_register(&lstp_usb_driver);
}

static void __exit lstp_module_exit(void)
{
	usb_deregister(&lstp_usb_driver);
	/* Before ida_destroy() and unload: these releases run module code, free ids. */
	wait_var_event(&lstp_live_objects, !atomic_read(&lstp_live_objects));

	ida_destroy(&lstp_auxdev_ida);
}

module_init(lstp_module_init);
module_exit(lstp_module_exit);

MODULE_DESCRIPTION("LSTP USB device driver");
MODULE_LICENSE("GPL");
