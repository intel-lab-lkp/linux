// SPDX-License-Identifier: GPL-2.0-only
/*
 * Goodix fingerprint sensors behind an EC shared-memory mailbox.
 *
 * The tested GXFP5130 platform exposes the fingerprint transport through
 * ACPI-described MMIO and GPIO handshakes. The host exchanges opaque MP
 * payloads with the device through this mailbox.
 *
 * Sensor protocol policy deliberately stays in userspace. This driver does
 * not know about Goodix commands, checksums, TLS, configuration, capture, or
 * sensor power states.
 */

#include <linux/acpi.h>
#include <linux/align.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/capability.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/kfifo.h>
#include <linux/miscdevice.h>
#include <linux/poll.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/timekeeping.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>

#include "goodix_ec_mailbox.h"
#include <linux/goodix_ec.h>

#define GXFP5130_ACPI_HID "GXFP5130"

static void goodix_ec_device_release(struct kref *refcount)
{
	struct goodix_device *gdev =
		container_of(refcount, struct goodix_device, refcount);

	kfree(gdev);
}

bool goodix_ec_device_get(struct goodix_device *gdev)
{
	return kref_get_unless_zero(&gdev->refcount);
}

void goodix_ec_device_put(struct goodix_device *gdev)
{
	kref_put(&gdev->refcount, goodix_ec_device_release);
}

static void goodix_ec_device_put_action(void *data)
{
	goodix_ec_device_put(data);
}

/* EC mailbox transport. */

struct goodix_ec_acpi_gpio_state {
	unsigned int gpio_count;
	int irq_index;
	int done_index[2];
	u8 done_polarity[2];
	unsigned int done_count;
};

static int goodix_ec_acpi_gpio_resource(struct acpi_resource *resource,
					void *context)
{
	struct goodix_ec_acpi_gpio_state *state = context;
	struct acpi_resource_gpio *gpio;
	unsigned int index;

	if (resource->type != ACPI_RESOURCE_TYPE_GPIO)
		return 0;

	gpio = &resource->data.gpio;
	index = state->gpio_count++;
	if (!gpio->pin_table_length)
		return 0;

	if (gpio->connection_type == ACPI_RESOURCE_GPIO_TYPE_INT) {
		if (state->irq_index < 0)
			state->irq_index = index;
	} else if (gpio->connection_type == ACPI_RESOURCE_GPIO_TYPE_IO &&
		   state->done_count < ARRAY_SIZE(state->done_index)) {
		state->done_index[state->done_count] = index;
		state->done_polarity[state->done_count] = gpio->polarity;
		state->done_count++;
	}

	return 0;
}

static int goodix_ec_add_gpio_mappings(struct device *dev)
{
	struct goodix_ec_acpi_gpio_state state = {
		.irq_index = -1,
		.done_index = { -1, -1 },
	};
	struct acpi_gpio_mapping *mappings;
	struct acpi_gpio_params *params;
	struct acpi_device *adev = ACPI_COMPANION(dev);
	LIST_HEAD(resources);
	bool active_low;
	int ret;

	if (!adev)
		return -ENODEV;

	ret = acpi_dev_get_resources(adev, &resources,
				     goodix_ec_acpi_gpio_resource, &state);
	acpi_dev_free_resource_list(&resources);
	if (ret <= 0)
		return ret < 0 ? ret : -ENOENT;
	if (state.irq_index < 0 || state.done_count != 2)
		return dev_err_probe(dev, -ENOENT,
				     "ACPI _CRS does not describe one IRQ and two done GPIOs\n");
	if (state.done_polarity[0] != state.done_polarity[1])
		return dev_err_probe(dev, -EINVAL,
				     "ACPI done GPIO polarities differ\n");

	active_low = state.done_polarity[0] != 0;
	params = devm_kcalloc(dev, 3, sizeof(*params), GFP_KERNEL);
	mappings = devm_kcalloc(dev, 4, sizeof(*mappings), GFP_KERNEL);
	if (!params || !mappings)
		return -ENOMEM;

	params[0].crs_entry_index = state.irq_index;
	params[0].line_index = 0;
	params[0].active_low = false;
	mappings[0].name = "irq-gpios";
	mappings[0].data = &params[0];
	mappings[0].size = 1;

	params[1].crs_entry_index = state.done_index[0];
	params[1].line_index = 0;
	params[1].active_low = active_low;
	mappings[1].name = "write-done-gpios";
	mappings[1].data = &params[1];
	mappings[1].size = 1;

	params[2].crs_entry_index = state.done_index[1];
	params[2].line_index = 0;
	params[2].active_low = active_low;
	mappings[2].name = "read-done-gpios";
	mappings[2].data = &params[2];
	mappings[2].size = 1;

	ret = devm_acpi_dev_add_driver_gpios(dev, mappings);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to install ACPI GPIO mappings\n");

	dev_dbg(dev,
		"ACPI GPIO mappings: irq=%d write-done=%d read-done=%d active-low=%u\n",
		state.irq_index, state.done_index[0], state.done_index[1],
		active_low);
	return 0;
}

static int goodix_ec_get_gpio(struct goodix_device *gdev,
			      struct gpio_desc **gpio,
			      const char *name,
			      enum gpiod_flags flags)
{
	*gpio = devm_gpiod_get(gdev->dev, name, flags);
	if (IS_ERR(*gpio))
		return dev_err_probe(gdev->dev, PTR_ERR(*gpio),
				     "failed to acquire %s GPIO\n", name);

	if (!*gpio)
		return dev_err_probe(gdev->dev, -ENODEV,
				     "%s GPIO missing\n", name);

	return 0;
}

static void goodix_ec_mmio_write_tx(struct goodix_device *gdev, size_t len)
{
	size_t offset;

	for (offset = 0; offset < len; offset += sizeof(u64)) {
		u64 value = get_unaligned_le64(gdev->tx_buf + offset);

		writeq(value, gdev->mailbox + GOODIX_EC_TX_OFFSET + offset);
	}

	/* Publish every qword before asserting the write-done doorbell. */
	wmb();
}

static void goodix_ec_mmio_read_rx(struct goodix_device *gdev)
{
	size_t offset;

	for (offset = 0; offset < GOODIX_EC_RX_SIZE; offset += sizeof(u64)) {
		u64 value = readq(gdev->mailbox + GOODIX_EC_RX_OFFSET + offset);

		put_unaligned_le64(value, gdev->rx_buf + offset);
	}

	/* Complete the mailbox snapshot before parsing its headers. */
	rmb();
}

static int goodix_ec_pulse_write_done(struct goodix_device *gdev)
{
	if (!gdev->write_done_gpio)
		return -ENODEV;

	gpiod_set_value_cansleep(gdev->write_done_gpio, 0);
	fsleep(GOODIX_WRITE_DONE_PRE_US);

	gpiod_set_value_cansleep(gdev->write_done_gpio, 1);
	fsleep(GOODIX_WRITE_DONE_HIGH_US);

	gpiod_set_value_cansleep(gdev->write_done_gpio, 0);
	fsleep(GOODIX_WRITE_DONE_POST_US);

	return 0;
}

static int goodix_ec_pulse_read_done(struct goodix_device *gdev)
{
	if (!gdev->read_done_gpio)
		return -ENODEV;

	/* RX must be copied before acknowledging that the host consumed it. */
	gpiod_set_value_cansleep(gdev->read_done_gpio, 1);
	fsleep(GOODIX_READ_DONE_HIGH_US);

	gpiod_set_value_cansleep(gdev->read_done_gpio, 0);
	fsleep(GOODIX_READ_DONE_POST_US);

	return 0;
}

static int goodix_ec_build_packet(struct goodix_device *gdev,
				  const u8 *payload, size_t payload_len,
				  size_t *packet_len)
{
	struct goodix_ec_header *header;
	size_t total_len;

	if (!gdev || !payload || !payload_len || !packet_len)
		return -EINVAL;

	if (payload_len > U16_MAX)
		return -EOVERFLOW;

	total_len = ALIGN(sizeof(*header) + payload_len,
			  GOODIX_EC_PACKET_ALIGNMENT);
	if (total_len > GOODIX_EC_TX_SIZE)
		return -EMSGSIZE;

	memset(gdev->tx_buf, 0, total_len);

	header = (struct goodix_ec_header *)gdev->tx_buf;
	header->type = GOODIX_EC_PACKET_TYPE;
	header->payload_len = cpu_to_le16(payload_len);
	header->checksum = header->type + (payload_len & 0xff) +
			   ((payload_len >> 8) & 0xff);
	header->sequence = cpu_to_le16(++gdev->tx_sequence);

	memcpy(gdev->tx_buf + sizeof(*header), payload, payload_len);
	*packet_len = total_len;

	return 0;
}

int goodix_ec_sync_send(struct goodix_device *gdev,
			const u8 *tx, size_t tx_len)
{
	size_t packet_len;
	int ret;

	ret = goodix_ec_build_packet(gdev, tx, tx_len, &packet_len);
	if (ret)
		return ret;

	goodix_ec_mmio_write_tx(gdev, packet_len);

	ret = goodix_ec_pulse_write_done(gdev);
	if (ret)
		return ret;

	fsleep(GOODIX_SYNC_RX_DELAY_US);
	return 0;
}

/* MP packet transport. */
static void goodix_ec_rx_push(struct goodix_device *gdev, u8 mp_type,
			      const u8 *payload, size_t payload_len)
{
	struct goodix_ec_record_header header;
	unsigned long flags;
	size_t required;

	if (!gdev || !payload || !payload_len ||
	    payload_len > GOODIX_EC_UAPI_RX_MAX)
		return;

	memset(&header, 0, sizeof(header));
	header.len = payload_len;
	header.mp_type = mp_type >> 4;
	header.timestamp_ns = ktime_get_ns();
	required = sizeof(header) + payload_len;

	spin_lock_irqsave(&gdev->rx_fifo_lock, flags);
	if (!gdev->rx_fifo_ready || kfifo_avail(&gdev->rx_fifo) < required) {
		spin_unlock_irqrestore(&gdev->rx_fifo_lock, flags);
		dev_warn_ratelimited(gdev->dev,
				     "RX queue full; dropping %zu-byte packet\n",
				     payload_len);
		return;
	}

	kfifo_in(&gdev->rx_fifo, &header, sizeof(header));
	kfifo_in(&gdev->rx_fifo, payload, payload_len);
	spin_unlock_irqrestore(&gdev->rx_fifo_lock, flags);

	wake_up_interruptible(&gdev->rx_wait);
}

static irqreturn_t goodix_ec_irq_thread(int irq, void *data)
{
	struct goodix_device *gdev = data;
	size_t chunk_len;
	size_t remaining;
	u16 declared_len;
	u8 mp_type;
	int ret = 0;
	int ack_ret;

	mutex_lock(&gdev->transfer_lock);

	goodix_ec_mmio_read_rx(gdev);

	/*
	 * Continuation IRQs contain raw payload bytes without another
	 * MP header.
	 */
	if (gdev->rx_reassembly_active) {
		remaining = gdev->rx_reassembly_len -
			    gdev->rx_reassembly_received;

		chunk_len = min_t(size_t,
				  remaining,
				  GOODIX_EC_RX_SIZE);

		memcpy(gdev->rx_reassembly +
		       gdev->rx_reassembly_received,
		       gdev->rx_buf,
		       chunk_len);

		gdev->rx_reassembly_received += chunk_len;

		ack_ret = goodix_ec_pulse_read_done(gdev);
		if (ack_ret)
			ret = ack_ret;

		if (!ret &&
		    gdev->rx_reassembly_received ==
		    gdev->rx_reassembly_len) {
			dev_dbg(gdev->dev,
				"RX reassembly complete: MP=0x%02x payload=%zu bytes\n",
				 gdev->rx_reassembly_mp_type,
				 gdev->rx_reassembly_len);

			goodix_ec_rx_push(gdev,
					  gdev->rx_reassembly_mp_type,
					  gdev->rx_reassembly,
					  gdev->rx_reassembly_len);

			kfree(gdev->rx_reassembly);
			gdev->rx_reassembly = NULL;
			gdev->rx_reassembly_len = 0;
			gdev->rx_reassembly_received = 0;
			gdev->rx_reassembly_mp_type = 0;
			gdev->rx_reassembly_active = false;
		}

		mutex_unlock(&gdev->transfer_lock);

		if (ret)
			dev_warn_ratelimited(gdev->dev,
					     "IRQ %d continuation ACK failed: %d\n",
					     irq, ret);

		return IRQ_HANDLED;
	}

	mp_type = gdev->rx_buf[0];

	if ((mp_type >> 4) != 0x0a &&
	    (mp_type >> 4) != 0x0b &&
	    (mp_type >> 4) != 0x0c) {
		ret = -EBADMSG;
		goto acknowledge;
	}

	if (gdev->rx_buf[3] !=
	    gdev->rx_buf[0] +
	    gdev->rx_buf[1] +
	    gdev->rx_buf[2]) {
		ret = -EBADMSG;
		goto acknowledge;
	}

	declared_len = get_unaligned_le16(gdev->rx_buf + 1);

	/* The complete payload fits in one mailbox transaction. */
	if (declared_len <=
	    GOODIX_EC_RX_SIZE -
	    sizeof(struct goodix_mp_header)) {
		goodix_ec_rx_push(gdev,
				  mp_type,
				  gdev->rx_buf +
				  sizeof(struct goodix_mp_header),
				  declared_len);

		dev_dbg(gdev->dev,
			"IRQ %d RX queued: MP=0x%02x payload=%u bytes\n",
			irq,
			mp_type >> 4,
			declared_len);

		goto acknowledge;
	}

	/* Reassemble payloads delivered across multiple IRQs. */
	gdev->rx_reassembly = kmalloc(declared_len, GFP_KERNEL);
	if (!gdev->rx_reassembly) {
		ret = -ENOMEM;
		goto acknowledge;
	}

	chunk_len = GOODIX_EC_RX_SIZE -
		    sizeof(struct goodix_mp_header);

	memcpy(gdev->rx_reassembly,
	       gdev->rx_buf +
	       sizeof(struct goodix_mp_header),
	       chunk_len);

	gdev->rx_reassembly_len = declared_len;
	gdev->rx_reassembly_received = chunk_len;
	gdev->rx_reassembly_mp_type = mp_type;
	gdev->rx_reassembly_active = true;

	dev_dbg(gdev->dev,
		"RX reassembly started: MP=0x%02x total=%u first=%zu remaining=%zu\n",
		 mp_type >> 4,
		 declared_len,
		 chunk_len,
		 (size_t)declared_len - chunk_len);

acknowledge:
	ack_ret = goodix_ec_pulse_read_done(gdev);
	if (!ret && ack_ret)
		ret = ack_ret;

	mutex_unlock(&gdev->transfer_lock);

	if (ret)
		dev_warn_ratelimited(gdev->dev,
				     "IRQ %d RX handling failed: %d\n",
				     irq, ret);

	return IRQ_HANDLED;
}

static const struct goodix_model_data gxfp5130_model = {
	.name = "GXFP5130",
};

/* ACPI platform driver and model selection. */

static int goodix_ec_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct acpi_device_id *match;
	struct goodix_device *gdev;
	struct resource *resource;
	resource_size_t mailbox_size;
	unsigned long irq_flags;
	unsigned int irq_type;
	int ret;

	match = acpi_match_device(dev->driver->acpi_match_table, dev);
	if (!match || !match->driver_data)
		return -ENODEV;

	gdev = kzalloc_obj(*gdev);
	if (!gdev)
		return -ENOMEM;
	kref_init(&gdev->refcount);
	ret = devm_add_action_or_reset(dev, goodix_ec_device_put_action, gdev);
	if (ret)
		return ret;

	gdev->dev = dev;
	gdev->model = (const struct goodix_model_data *)match->driver_data;
	gdev->tx_sequence = GOODIX_EC_SEQUENCE_SEED;
	mutex_init(&gdev->transfer_lock);
	platform_set_drvdata(pdev, gdev);

	gdev->mailbox = devm_platform_get_and_ioremap_resource(pdev, 0,
							       &resource);
	if (IS_ERR(gdev->mailbox))
		return dev_err_probe(dev, PTR_ERR(gdev->mailbox),
				     "failed to map EC mailbox MMIO resource\n");

	mailbox_size = resource_size(resource);
	if (mailbox_size < GOODIX_EC_MMIO_SIZE)
		return dev_err_probe(dev, -EINVAL,
				     "EC mailbox resource too small: %#llx\n",
				     (unsigned long long)mailbox_size);

	gdev->mailbox_phys = resource->start;
	gdev->mailbox_size = mailbox_size;

	gdev->tx_buf = devm_kzalloc(dev, GOODIX_EC_TX_SIZE, GFP_KERNEL);
	if (!gdev->tx_buf)
		return -ENOMEM;

	gdev->rx_buf = devm_kzalloc(dev, GOODIX_EC_RX_SIZE, GFP_KERNEL);
	if (!gdev->rx_buf)
		return -ENOMEM;

	ret = goodix_ec_add_gpio_mappings(dev);
	if (ret)
		return ret;

	ret = goodix_ec_get_gpio(gdev, &gdev->write_done_gpio,
				 "write-done", GPIOD_OUT_LOW);
	if (ret)
		return ret;

	ret = goodix_ec_get_gpio(gdev, &gdev->read_done_gpio,
				 "read-done", GPIOD_OUT_LOW);
	if (ret)
		return ret;

	ret = goodix_ec_get_gpio(gdev, &gdev->irq_gpio,
				 "irq", GPIOD_IN);
	if (ret)
		return ret;

	gdev->irq = gpiod_to_irq(gdev->irq_gpio);
	if (gdev->irq < 0)
		return dev_err_probe(dev, gdev->irq,
				     "failed to map IRQ GPIO to an IRQ\n");

	irq_type = irq_get_trigger_type(gdev->irq);
	if (irq_type == IRQ_TYPE_NONE) {
		ret = irq_set_irq_type(gdev->irq, IRQ_TYPE_LEVEL_HIGH);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to set level-high IRQ trigger\n");
		irq_type = IRQ_TYPE_LEVEL_HIGH;
	}

	dev_info(dev, "bound %s: mailbox=%pa size=%#llx irq=%d\n",
		 gdev->model->name, &gdev->mailbox_phys,
		 (unsigned long long)gdev->mailbox_size, gdev->irq);

	irq_flags = IRQF_ONESHOT | IRQF_NO_AUTOEN;
	ret = devm_request_threaded_irq(dev,
					gdev->irq,
					NULL,
					goodix_ec_irq_thread,
					irq_flags,
					dev_name(dev),
					gdev);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to request IRQ %d\n",
				     gdev->irq);

	dev_dbg(dev, "IRQ %d registered: flags=%#lx trigger=%#x\n",
		gdev->irq, irq_flags, irq_type);

	ret = goodix_ec_uapi_register(gdev);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register userspace interface\n");

	enable_irq(gdev->irq);
	gdev->irq_enabled = true;
	dev_info(dev, "IRQ %d armed\n", gdev->irq);

	dev_info(dev, "EC mailbox transport ready\n");
	return 0;
}

static void goodix_ec_remove(struct platform_device *pdev)
{
	struct goodix_device *gdev = platform_get_drvdata(pdev);

	if (!gdev)
		return;

	WRITE_ONCE(gdev->disconnected, true);
	if (gdev->irq_enabled) {
		disable_irq(gdev->irq);
		synchronize_irq(gdev->irq);
		gdev->irq_enabled = false;
	}

	goodix_ec_uapi_unregister(gdev);
	mutex_lock(&gdev->transfer_lock);
	kfree(gdev->rx_reassembly);
	gdev->rx_reassembly = NULL;
	gdev->rx_reassembly_len = 0;
	gdev->rx_reassembly_received = 0;
	gdev->rx_reassembly_active = false;

	if (gdev->write_done_gpio)
		gpiod_set_value_cansleep(gdev->write_done_gpio, 0);
	if (gdev->read_done_gpio)
		gpiod_set_value_cansleep(gdev->read_done_gpio, 0);
	mutex_unlock(&gdev->transfer_lock);

	dev_info(gdev->dev, "%s detached\n", gdev->model->name);
}

static int goodix_ec_suspend(struct device *dev)
{
	struct goodix_device *gdev = dev_get_drvdata(dev);
	unsigned long flags;

	if (!gdev || READ_ONCE(gdev->disconnected))
		return 0;

	WRITE_ONCE(gdev->suspended, true);
	wake_up_interruptible_all(&gdev->rx_wait);
	if (gdev->irq_enabled) {
		disable_irq(gdev->irq);
		synchronize_irq(gdev->irq);
		gdev->irq_enabled = false;
	}

	mutex_lock(&gdev->transfer_lock);
	gpiod_set_value_cansleep(gdev->write_done_gpio, 0);
	gpiod_set_value_cansleep(gdev->read_done_gpio, 0);
	kfree(gdev->rx_reassembly);
	gdev->rx_reassembly = NULL;
	gdev->rx_reassembly_len = 0;
	gdev->rx_reassembly_received = 0;
	gdev->rx_reassembly_active = false;
	mutex_unlock(&gdev->transfer_lock);

	spin_lock_irqsave(&gdev->rx_fifo_lock, flags);
	if (gdev->rx_fifo_ready)
		kfifo_reset(&gdev->rx_fifo);
	spin_unlock_irqrestore(&gdev->rx_fifo_lock, flags);
	return 0;
}

static int goodix_ec_resume(struct device *dev)
{
	struct goodix_device *gdev = dev_get_drvdata(dev);

	if (!gdev || READ_ONCE(gdev->disconnected))
		return 0;

	gpiod_set_value_cansleep(gdev->write_done_gpio, 0);
	gpiod_set_value_cansleep(gdev->read_done_gpio, 0);
	if (!gdev->irq_enabled) {
		enable_irq(gdev->irq);
		gdev->irq_enabled = true;
	}
	/* Permit new writes only after the receive path is armed. */
	WRITE_ONCE(gdev->suspended, false);
	wake_up_interruptible_all(&gdev->rx_wait);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(goodix_ec_pm_ops, goodix_ec_suspend,
				goodix_ec_resume);

static const struct acpi_device_id goodix_ec_acpi_match[] = {
	{
		.id = GXFP5130_ACPI_HID,
		.driver_data = (kernel_ulong_t)&gxfp5130_model,
	},
	{ }
};
MODULE_DEVICE_TABLE(acpi, goodix_ec_acpi_match);

static struct platform_driver goodix_ec_driver = {
	.probe = goodix_ec_probe,
	.remove = goodix_ec_remove,
	.driver = {
		.name = GOODIX_EC_DRIVER_NAME,
		.acpi_match_table = ACPI_PTR(goodix_ec_acpi_match),
		.pm = pm_sleep_ptr(&goodix_ec_pm_ops),
	},
};
module_platform_driver(goodix_ec_driver);

MODULE_AUTHOR("Ertugrul Topcu <ertugtopcu0@gmail.com>");
MODULE_DESCRIPTION("Goodix fingerprint sensors over an EC mailbox");
MODULE_LICENSE("GPL");
