// SPDX-License-Identifier: GPL-2.0-only
/*
 * SPI bus driver for the Virtio SPI controller
 * Copyright (C) 2023 OpenSynergy GmbH
 */

#include <linux/completion.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/stddef.h>
#include <linux/virtio.h>
#include <linux/virtio_ring.h>
#include <linux/version.h>
#include <linux/of.h>
#include <linux/spi/spi.h>
#include <linux/virtio_spi.h>

/* virtio_spi private data structure */
struct virtio_spi_priv {
	/* The virtio device we're associated with */
	struct virtio_device *vdev;
	/* Pointer to the virtqueue */
	struct virtqueue *vq;
	/* Copy of config space mode_func_supported */
	u32 mode_func_supported;
	/* Copy of config space max_freq_hz */
	u32 max_freq_hz;
};

struct virtio_spi_req {
	struct completion completion;
	struct spi_transfer_head transfer_head	____cacheline_aligned;
	const uint8_t *tx_buf			____cacheline_aligned;
	uint8_t *rx_buf				____cacheline_aligned;
	struct spi_transfer_result result	____cacheline_aligned;
};

static struct spi_board_info board_info = {
	.modalias = "spi-virtio",
};

static void virtio_spi_msg_done(struct virtqueue *vq)
{
	struct virtio_spi_req *req;
	unsigned int len;

	while ((req = virtqueue_get_buf(vq, &len)))
		complete(&req->completion);
}

static int virtio_spi_one_transfer(struct virtio_spi_req *spi_req,
				   struct spi_controller *ctrl,
				   struct spi_message *msg,
				   struct spi_transfer *xfer)
{
	struct virtio_spi_priv *priv = spi_controller_get_devdata(ctrl);
	struct device *dev = &priv->vdev->dev;
	struct spi_device *spi = msg->spi;
	struct spi_transfer_head *th;
	struct scatterlist sg_out_head, sg_out_payload;
	struct scatterlist sg_in_result, sg_in_payload;
	struct scatterlist *sgs[4];
	unsigned int outcnt = 0u;
	unsigned int incnt = 0u;
	int ret;

	th = &spi_req->transfer_head;

	/* Fill struct spi_transfer_head */
	th->chip_select_id = spi_get_chipselect(spi, 0);
	th->bits_per_word = spi->bits_per_word;
	/*
	 * Got comment: "The virtio spec for cs_change is *not* what the Linux
	 * cs_change field does, this will not do the right thing."
	 * TODO: Understand/discuss this, still unclear what may be wrong here
	 */
	th->cs_change = xfer->cs_change;
	th->tx_nbits = xfer->tx_nbits;
	th->rx_nbits = xfer->rx_nbits;
	th->reserved[0] = 0;
	th->reserved[1] = 0;
	th->reserved[2] = 0;

	BUILD_BUG_ON(VIRTIO_SPI_CPHA != SPI_CPHA);
	BUILD_BUG_ON(VIRTIO_SPI_CPOL != SPI_CPOL);
	BUILD_BUG_ON(VIRTIO_SPI_CS_HIGH != SPI_CS_HIGH);
	BUILD_BUG_ON(VIRTIO_SPI_MODE_LSB_FIRST != SPI_LSB_FIRST);

	th->mode = cpu_to_le32(spi->mode & (SPI_LSB_FIRST | SPI_CS_HIGH |
					    SPI_CPOL | SPI_CPHA));
	if ((spi->mode & SPI_LOOP) != 0)
		th->mode |= cpu_to_le32(VIRTIO_SPI_MODE_LOOP);

	th->freq = cpu_to_le32(xfer->speed_hz);

	ret = spi_delay_to_ns(&xfer->word_delay, xfer);
	if (ret < 0) {
		dev_warn(dev, "Cannot convert word_delay\n");
		goto msg_done;
	}
	th->word_delay_ns = cpu_to_le32((u32)ret);

	ret = spi_delay_to_ns(&xfer->delay, xfer);
	if (ret < 0) {
		dev_warn(dev, "Cannot convert delay\n");
		goto msg_done;
	}
	th->cs_setup_ns = cpu_to_le32((u32)ret);
	th->cs_delay_hold_ns = cpu_to_le32((u32)ret);

	/* This is the "off" time when CS has to be deasserted for a moment */
	ret = spi_delay_to_ns(&xfer->cs_change_delay, xfer);
	if (ret < 0) {
		dev_warn(dev, "Cannot convert cs_change_delay\n");
		goto msg_done;
	}
	th->cs_change_delay_inactive_ns = cpu_to_le32((u32)ret);

	/* Set buffers */
	spi_req->tx_buf = xfer->tx_buf;
	spi_req->rx_buf = xfer->rx_buf;

	/* Prepare sending of virtio message */
	init_completion(&spi_req->completion);

	sg_init_one(&sg_out_head, &spi_req->transfer_head,
		    sizeof(struct spi_transfer_head));
	sgs[outcnt] = &sg_out_head;
	outcnt++;

	if (spi_req->tx_buf) {
		sg_init_one(&sg_out_payload, spi_req->tx_buf, xfer->len);
		sgs[outcnt] = &sg_out_payload;
		outcnt++;
	}

	if (spi_req->rx_buf) {
		sg_init_one(&sg_in_payload, spi_req->rx_buf, xfer->len);
		sgs[outcnt + incnt] = &sg_in_payload;
		incnt++;
	}

	sg_init_one(&sg_in_result, &spi_req->result,
		    sizeof(struct spi_transfer_result));
	sgs[outcnt + incnt] = &sg_in_result;
	incnt++;

	ret = virtqueue_add_sgs(priv->vq, sgs, outcnt, incnt, spi_req,
				GFP_KERNEL);

msg_done:
	if (ret)
		msg->status = ret;

	return ret;
}

static int virtio_spi_transfer_one_message(struct spi_controller *ctrl,
					   struct spi_message *msg)
{
	struct virtio_spi_priv *priv = spi_controller_get_devdata(ctrl);
	struct virtio_spi_req *spi_req;
	struct spi_transfer *xfer;
	int ret = 0;

	spi_req = kzalloc(sizeof(*spi_req), GFP_KERNEL);
	if (!spi_req) {
		ret = -ENOMEM;
		goto no_mem;
	}

	/*
	 * Simple implementation: Process message by message and wait for each
	 * message to be completed by the device side.
	 */
	list_for_each_entry(xfer, &msg->transfers, transfer_list) {
		ret = virtio_spi_one_transfer(spi_req, ctrl, msg, xfer);
		if (ret)
			goto msg_done;

		virtqueue_kick(priv->vq);

		wait_for_completion(&spi_req->completion);

		/* Read result from message */
		ret = (int)spi_req->result.result;
		if (ret)
			goto msg_done;
	}

msg_done:
	kfree(spi_req);
no_mem:
	msg->status = ret;
	spi_finalize_current_message(ctrl);

	return ret;
}

static void virtio_spi_read_config(struct virtio_device *vdev)
{
	struct spi_controller *ctrl = dev_get_drvdata(&vdev->dev);
	struct virtio_spi_priv *priv = vdev->priv;
	u8 cs_max_number;
	u8 tx_nbits_supported;
	u8 rx_nbits_supported;

	cs_max_number = virtio_cread8(vdev, offsetof(struct virtio_spi_config,
						     cs_max_number));
	ctrl->num_chipselect = cs_max_number;

	/* Set the mode bits which are understood by this driver */
	priv->mode_func_supported =
		virtio_cread32(vdev, offsetof(struct virtio_spi_config,
					      mode_func_supported));
	ctrl->mode_bits = priv->mode_func_supported &
			  (VIRTIO_SPI_CS_HIGH | VIRTIO_SPI_MODE_LSB_FIRST);
	if ((priv->mode_func_supported & VIRTIO_SPI_MF_SUPPORT_CPHA_1) != 0)
		ctrl->mode_bits |= VIRTIO_SPI_CPHA;
	if ((priv->mode_func_supported & VIRTIO_SPI_MF_SUPPORT_CPOL_1) != 0)
		ctrl->mode_bits |= VIRTIO_SPI_CPOL;
	if ((priv->mode_func_supported & VIRTIO_SPI_MF_SUPPORT_LSB_FIRST) != 0)
		ctrl->mode_bits |= SPI_LSB_FIRST;
	if ((priv->mode_func_supported & VIRTIO_SPI_MF_SUPPORT_LOOPBACK) != 0)
		ctrl->mode_bits |= SPI_LOOP;
	tx_nbits_supported =
		virtio_cread8(vdev, offsetof(struct virtio_spi_config,
					     tx_nbits_supported));
	if ((tx_nbits_supported & VIRTIO_SPI_RX_TX_SUPPORT_DUAL) != 0)
		ctrl->mode_bits |= SPI_TX_DUAL;
	if ((tx_nbits_supported & VIRTIO_SPI_RX_TX_SUPPORT_QUAD) != 0)
		ctrl->mode_bits |= SPI_TX_QUAD;
	if ((tx_nbits_supported & VIRTIO_SPI_RX_TX_SUPPORT_OCTAL) != 0)
		ctrl->mode_bits |= SPI_TX_OCTAL;
	rx_nbits_supported =
		virtio_cread8(vdev, offsetof(struct virtio_spi_config,
					     rx_nbits_supported));
	if ((rx_nbits_supported & VIRTIO_SPI_RX_TX_SUPPORT_DUAL) != 0)
		ctrl->mode_bits |= SPI_RX_DUAL;
	if ((rx_nbits_supported & VIRTIO_SPI_RX_TX_SUPPORT_QUAD) != 0)
		ctrl->mode_bits |= SPI_RX_QUAD;
	if ((rx_nbits_supported & VIRTIO_SPI_RX_TX_SUPPORT_OCTAL) != 0)
		ctrl->mode_bits |= SPI_RX_OCTAL;

	ctrl->bits_per_word_mask =
		virtio_cread32(vdev, offsetof(struct virtio_spi_config,
					      bits_per_word_mask));

	priv->max_freq_hz =
		virtio_cread32(vdev, offsetof(struct virtio_spi_config,
					      max_freq_hz));
}

static int virtio_spi_find_vqs(struct virtio_spi_priv *priv)
{
	struct virtqueue *vq;

	vq = virtio_find_single_vq(priv->vdev, virtio_spi_msg_done, "spi-rq");
	if (IS_ERR(vq))
		return (int)PTR_ERR(vq);
	priv->vq = vq;
	return 0;
}

/* Function must not be called before virtio_spi_find_vqs() has been run */
static void virtio_spi_del_vq(struct virtio_device *vdev)
{
	virtio_reset_device(vdev);
	vdev->config->del_vqs(vdev);
}

static int virtio_spi_validate(struct virtio_device *vdev)
{
	/*
	 * SPI needs always access to the config space.
	 * Check that the driver can access the config space
	 */
	if (!vdev->config->get) {
		dev_err(&vdev->dev, "%s failure: config access disabled\n",
			__func__);
		return -EINVAL;
	}

	if (!virtio_has_feature(vdev, VIRTIO_F_VERSION_1)) {
		dev_err(&vdev->dev,
			"device does not comply with spec version 1.x\n");
		return -EINVAL;
	}

	return 0;
}

static int virtio_spi_probe(struct virtio_device *vdev)
{
	struct device_node *np = vdev->dev.parent->of_node;
	struct virtio_spi_priv *priv;
	struct spi_controller *ctrl;
	int err;
	u32 bus_num;
	u16 csi;

	ctrl = devm_spi_alloc_host(&vdev->dev, sizeof(*priv));
	if (!ctrl) {
		dev_err(&vdev->dev, "Kernel memory exhausted in %s()\n",
			__func__);
		err = -ENOMEM;
		goto err_return;
	}

	priv = spi_controller_get_devdata(ctrl);
	priv->vdev = vdev;
	vdev->priv = priv;
	dev_set_drvdata(&vdev->dev, ctrl);

	err = of_property_read_u32(np, "spi,bus-num", &bus_num);
	if (!err && bus_num <= S16_MAX)
		ctrl->bus_num = (s16)bus_num;

	virtio_spi_read_config(vdev);

	/* Method to transfer a single SPI message */
	ctrl->transfer_one_message = virtio_spi_transfer_one_message;

	/* Initialize virtqueues */
	err = virtio_spi_find_vqs(priv);
	if (err) {
		dev_err(&vdev->dev, "Cannot setup virtqueues\n");
		goto err_return;
	}

	err = spi_register_controller(ctrl);
	if (err) {
		dev_err(&vdev->dev, "Cannot register controller\n");
		goto err_return;
	}

	board_info.max_speed_hz = priv->max_freq_hz;
	/* spi_new_device() currently does not use bus_num but better set it */
	board_info.bus_num = (u16)ctrl->bus_num;

	/* Add chip selects to controller */
	for (csi = 0; csi < ctrl->num_chipselect; csi++) {
		dev_dbg(&vdev->dev, "Setting up CS=%u\n", csi);
		board_info.chip_select = csi;
		/* TODO: Discuss setting of board_info.mode */
		if (!(priv->mode_func_supported & VIRTIO_SPI_CS_HIGH))
			board_info.mode = SPI_MODE_0;
		else
			board_info.mode = SPI_MODE_0 | SPI_CS_HIGH;
		if (!spi_new_device(ctrl, &board_info)) {
			dev_err(&vdev->dev, "Cannot setup device %u\n", csi);
			err = -ENODEV;
			goto err_return;
		}
	}

	return 0;

err_return:
	return err;
}

static void virtio_spi_remove(struct virtio_device *vdev)
{
	struct spi_controller *ctrl = dev_get_drvdata(&vdev->dev);

	spi_unregister_controller(ctrl);
	virtio_spi_del_vq(vdev);
}

static int virtio_spi_freeze(struct virtio_device *vdev)
{
	struct device *dev = &vdev->dev;
	struct spi_controller *ctrl = dev_get_drvdata(dev);
	int ret;

	/* Stop the queue running */
	ret = spi_controller_suspend(ctrl);
	if (ret) {
		dev_warn(dev, "cannot suspend controller (%d)\n", ret);
		return ret;
	}

	virtio_spi_del_vq(vdev);
	return 0;
}

static int virtio_spi_restore(struct virtio_device *vdev)
{
	struct device *dev = &vdev->dev;
	struct spi_controller *ctrl = dev_get_drvdata(dev);
	int ret;

	ret = virtio_spi_find_vqs(vdev->priv);
	if (ret) {
		dev_err(dev, "problem starting vqueue (%d)\n", ret);
		return ret;
	}

	ret = spi_controller_resume(ctrl);
	if (ret)
		dev_err(dev, "problem resuming controller (%d)\n", ret);

	return ret;
}

static struct virtio_device_id virtio_spi_id_table[] = {
	{ VIRTIO_ID_SPI, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static struct virtio_driver virtio_spi_driver = {
	.driver.name = KBUILD_MODNAME,
	.driver.owner = THIS_MODULE,
	.id_table = virtio_spi_id_table,
	.validate = virtio_spi_validate,
	.probe = virtio_spi_probe,
	.remove = virtio_spi_remove,
	.freeze = pm_sleep_ptr(virtio_spi_freeze),
	.restore = pm_sleep_ptr(virtio_spi_restore),
};

module_virtio_driver(virtio_spi_driver);
MODULE_DEVICE_TABLE(virtio, virtio_spi_id_table);

MODULE_AUTHOR("OpenSynergy GmbH");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Virtio SPI bus driver");
