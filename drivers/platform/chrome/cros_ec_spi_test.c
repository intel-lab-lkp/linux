// SPDX-License-Identifier: GPL-2.0
/*
 * Kunit tests for ChromeOS Embedded Controller SPI interface.
 */
#include <kunit/test.h>
#include <kunit/kprobes_stub.h>

#include <linux/platform_data/cros_ec_commands.h>
#include <linux/platform_data/cros_ec_proto.h>
#include <linux/sched.h>
#include <linux/spi/spi.h>

#include "cros_ec.h"

#define BUFSIZE 128
#define SPI_BUS_NUM 0x6

struct fake_xt {
	u8 *buf;
	size_t len;

	struct list_head list;
};

struct cros_ec_spi_test_priv {
	struct class *fake_class;
	struct device dev;
	struct spi_controller *fake_ctlr;
	struct spi_device *fake_spi_device;
	struct spi_driver *spi_drv;

	int fake_cros_ec_register_called;
	struct cros_ec_device *ec_dev;

	int fake_cros_ec_unregister_called;

	struct list_head tx_head;
	struct list_head rx_head;
};

static struct fake_xt *queue_fake_xt(struct kunit *test, struct list_head *head, size_t len)
{
	struct fake_xt *xt = kunit_kmalloc(test, sizeof(*xt), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, xt);

	xt->buf = kunit_kzalloc(test, len, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, xt->buf);
	xt->len = len;
	list_add_tail(&xt->list, head);
	return xt;
}

static struct fake_xt *dequeue_fake_xt(struct list_head *head)
{
	struct fake_xt *xt = list_first_entry(head, struct fake_xt, list);
	list_del(&xt->list);
	return xt;
}

static int fake_cros_ec_register(struct cros_ec_device *ec_dev)
{
	struct kunit *test = current->kunit_test;
	struct cros_ec_spi_test_priv *priv = test->priv;

	priv->fake_cros_ec_register_called += 1;

	priv->ec_dev = ec_dev;
	priv->ec_dev->din_size = BUFSIZE;
	priv->ec_dev->din = kunit_kmalloc(test, priv->ec_dev->din_size, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, priv->ec_dev->din);
	priv->ec_dev->dout_size = BUFSIZE;
	priv->ec_dev->dout = kunit_kmalloc(test, priv->ec_dev->dout_size, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, priv->ec_dev->dout);
	return 0;
}

static void fake_cros_ec_unregister(struct cros_ec_device *ec_dev)
{
	struct kunit *test = current->kunit_test;
	struct cros_ec_spi_test_priv *priv = test->priv;

	priv->fake_cros_ec_unregister_called += 1;
}

static int fake_spi_transfer_one_message(struct spi_controller *ctlr, struct spi_message *msg)
{
	/* Note: the function's context is another kernel thread. */
	struct kunit *test = spi_controller_get_devdata(ctlr);
	struct cros_ec_spi_test_priv *priv = test->priv;
	struct spi_transfer *t;

	list_for_each_entry(t, &msg->transfers, transfer_list) {
		if (t->tx_buf) {
			struct fake_xt *tx = queue_fake_xt(test, &priv->tx_head, t->len);
			memcpy(tx->buf, t->tx_buf, t->len);
		}

		if (t->rx_buf && !list_empty(&priv->rx_head)) {
			struct fake_xt *rx = dequeue_fake_xt(&priv->rx_head);
			memcpy(t->rx_buf, rx->buf, min(t->len, rx->len));
		}
	}

	msg->status = 0;
	spi_finalize_current_message(ctlr);
	return 0;
}

static int find_target_driver(struct device_driver *drv, void *data)
{
	struct device_driver **pdrv = data;

	if (strcmp(drv->name, "cros-ec-spi") == 0)
		*pdrv = drv;
	return 0;
}

static int cros_ec_spi_test_init(struct kunit *test)
{
	struct cros_ec_spi_test_priv *priv;
	int ret;
	struct device_driver *drv;

	kunit_activate_kprobes_stub(test, cros_ec_register, fake_cros_ec_register);
	kunit_activate_kprobes_stub(test, cros_ec_unregister, fake_cros_ec_unregister);

	priv = kunit_kzalloc(test, sizeof(*priv), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, priv);
	test->priv = priv;

	priv->fake_class = class_create("fake-class");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, priv->fake_class);

	priv->dev.class = priv->fake_class;
	priv->dev.parent = NULL;
	dev_set_name(&priv->dev, "cros-ec-spi-test-dev");
	ret = device_register(&priv->dev);
	if (ret)
		put_device(&priv->dev);
	KUNIT_ASSERT_EQ(test, ret, 0);

	priv->fake_ctlr = spi_alloc_host(&priv->dev, 0);
	KUNIT_ASSERT_NOT_NULL(test, priv->fake_ctlr);
	spi_controller_set_devdata(priv->fake_ctlr, test);

	priv->fake_ctlr->transfer_one_message = fake_spi_transfer_one_message;
	priv->fake_ctlr->bus_num = SPI_BUS_NUM;
	ret = spi_register_controller(priv->fake_ctlr);
	KUNIT_ASSERT_EQ(test, ret, 0);

	ret = bus_for_each_drv(&spi_bus_type, NULL, &drv, find_target_driver);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_NOT_NULL(test, drv);

	priv->fake_spi_device = spi_alloc_device(priv->fake_ctlr);
	KUNIT_ASSERT_NOT_NULL(test, priv->fake_spi_device);

	priv->spi_drv = container_of(drv, struct spi_driver, driver);
	ret = priv->spi_drv->probe(priv->fake_spi_device);
	KUNIT_ASSERT_EQ(test, ret, 0);

	KUNIT_EXPECT_EQ(test, priv->fake_cros_ec_register_called, 1);
	KUNIT_ASSERT_NOT_NULL(test, priv->ec_dev);
	KUNIT_EXPECT_EQ(test, priv->fake_cros_ec_unregister_called, 0);

	INIT_LIST_HEAD(&priv->tx_head);
	INIT_LIST_HEAD(&priv->rx_head);

	return 0;
}

static void cros_ec_spi_test_exit(struct kunit *test)
{
	struct cros_ec_spi_test_priv *priv = test->priv;

	priv->spi_drv->remove(priv->fake_spi_device);
	KUNIT_EXPECT_EQ(test, priv->fake_cros_ec_unregister_called, 1);

	spi_dev_put(priv->fake_spi_device);
	spi_unregister_controller(priv->fake_ctlr);
	device_del(&priv->dev);
	class_destroy(priv->fake_class);

	kunit_deactivate_kprobes_stub(test, cros_ec_register);
	kunit_deactivate_kprobes_stub(test, cros_ec_unregister);
}

static int cros_ec_spi_test_cmd_xfer_init(struct kunit *test)
{
	struct cros_ec_spi_test_priv *priv;

	cros_ec_spi_test_init(test);
	priv = test->priv;
	priv->ec_dev->proto_version = 2;
	return 0;
}

static void cros_ec_spi_test_cmd_xfer_normal(struct kunit *test)
{
	struct cros_ec_spi_test_priv *priv = test->priv;
	struct cros_ec_command *msg;
	struct fake_xt *xt;
	int ret, i, len;
	u8 sum;

	msg = kunit_kzalloc(test, sizeof(*msg) + 2 /* max(outsize, insize) */, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, msg);
	msg->version = 0x1000;
	msg->command = 0x1001;
	msg->outsize = 2;
	msg->insize = 1;
	msg->data[0] = 0xbe;
	msg->data[1] = 0xef;

	/*
	 * cros_ec_spi sends out the msg and tries to read further EC_MSG_TX_PROTO_BYTES +
	 * msg->outsize bytes to verify if the EC can proceed the command by looking for
	 * EC_SPI_PAST_END, EC_SPI_RX_BAD_DATA, or EC_SPI_NOT_READY.  It's fine even if we
	 * prepare 0 length data.
	 */
	queue_fake_xt(test, &priv->rx_head, 0);

	len = 1 /* for EC_SPI_FRAME_START */ + EC_MSG_RX_PROTO_BYTES + msg->insize;
	xt = queue_fake_xt(test, &priv->rx_head, len);
	xt->buf[0] = EC_SPI_FRAME_START;
	xt->buf[1] = 0 /* result */;
	xt->buf[2] = msg->insize;
	xt->buf[3] = 0xaa;
	for (sum = 0, i = 1; i < len - 1; ++i)
		sum += xt->buf[i];
	xt->buf[len - 1] = sum;

	ret = priv->ec_dev->cmd_xfer(priv->ec_dev, msg);
	KUNIT_EXPECT_EQ(test, ret, 1 /* insize */);
	KUNIT_EXPECT_EQ(test, msg->result, 0);
	KUNIT_EXPECT_EQ(test, msg->data[0], 0xaa);

	KUNIT_EXPECT_FALSE(test, list_empty(&priv->tx_head));
	xt = dequeue_fake_xt(&priv->tx_head);
	KUNIT_EXPECT_TRUE(test, list_empty(&priv->tx_head));
	KUNIT_EXPECT_NOT_NULL(test, xt);
	KUNIT_EXPECT_NOT_NULL(test, xt->buf);
	KUNIT_EXPECT_EQ(test, xt->len, EC_MSG_TX_PROTO_BYTES + 2 /* outsize */);
	KUNIT_EXPECT_EQ(test, xt->buf[0], (u8)(EC_CMD_VERSION0 + 0x1000));
	KUNIT_EXPECT_EQ(test, xt->buf[1], (u8)0x1001);
	KUNIT_EXPECT_EQ(test, xt->buf[2], 2 /* outsize */);
	KUNIT_EXPECT_EQ(test, xt->buf[3], 0xbe);
	KUNIT_EXPECT_EQ(test, xt->buf[4], 0xef);
	for (sum = 0, i = 0; i < EC_MSG_TX_HEADER_BYTES + 2 /* outsize */; ++i)
		sum += xt->buf[i];
	KUNIT_EXPECT_EQ(test, xt->buf[EC_MSG_TX_HEADER_BYTES + 2 /* outsize */], sum);
}

static struct kunit_case cros_ec_spi_test_cmd_xfer_cases[] = {
	KUNIT_CASE(cros_ec_spi_test_cmd_xfer_normal),
	{}
};

static struct kunit_suite cros_ec_spi_test_cmd_xfer_suite = {
	.name = "cros_ec_spi_test_cmd_xfer_suite",
	.init = cros_ec_spi_test_cmd_xfer_init,
	.exit = cros_ec_spi_test_exit,
	.test_cases = cros_ec_spi_test_cmd_xfer_cases,
};

static int cros_ec_spi_test_pkt_xfer_init(struct kunit *test)
{
	struct cros_ec_spi_test_priv *priv;

	cros_ec_spi_test_init(test);
	priv = test->priv;
	priv->ec_dev->proto_version = 3;
	return 0;
}

static void cros_ec_spi_test_pkt_xfer_normal(struct kunit *test)
{
	struct cros_ec_spi_test_priv *priv = test->priv;
	struct cros_ec_command *msg;
	struct fake_xt *xt;
	struct ec_host_request *request;
	struct ec_host_response *response;
	int ret, i, len;
	u8 sum;

	msg = kunit_kzalloc(test, sizeof(*msg) + 2 /* max(outsize, insize) */, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, msg);
	msg->version = 0x1000;
	msg->command = 0x1001;
	msg->outsize = 2;
	msg->insize = 1;
	msg->data[0] = 0xbe;
	msg->data[1] = 0xef;

	/*
	 * cros_ec_spi sends out the msg and tries to read further EC_MSG_TX_PROTO_BYTES +
	 * msg->outsize bytes to verify if the EC can proceed the command by looking for
	 * EC_SPI_PAST_END, EC_SPI_RX_BAD_DATA, or EC_SPI_NOT_READY.  It's fine even if we
	 * prepare 0 length data.
	 */
	queue_fake_xt(test, &priv->rx_head, 0);

	len = 1 /* for EC_SPI_FRAME_START */ + sizeof(*response) + msg->insize;
	xt = queue_fake_xt(test, &priv->rx_head, len);
	xt->buf[0] = EC_SPI_FRAME_START;
	response = (struct ec_host_response *)&xt->buf[1];
	response->struct_version = EC_HOST_RESPONSE_VERSION;
	response->result = 0;
	response->data_len = msg->insize;
	xt->buf[sizeof(*response) + msg->insize] = 0xaa;
	for (sum = 0, i = 1; i < len; ++i)
		sum += xt->buf[i];
	response->checksum = -sum;

	ret = priv->ec_dev->pkt_xfer(priv->ec_dev, msg);
	KUNIT_EXPECT_EQ(test, ret, 1 /* insize */);
	KUNIT_EXPECT_EQ(test, msg->result, 0);
	KUNIT_EXPECT_EQ(test, msg->data[0], 0xaa);

	KUNIT_EXPECT_FALSE(test, list_empty(&priv->tx_head));
	xt = dequeue_fake_xt(&priv->tx_head);
	KUNIT_EXPECT_TRUE(test, list_empty(&priv->tx_head));
	KUNIT_EXPECT_NOT_NULL(test, xt);
	KUNIT_EXPECT_NOT_NULL(test, xt->buf);
	KUNIT_EXPECT_EQ(test, xt->len, sizeof(*request) + 2 /* outsize */);
	request = (struct ec_host_request *)xt->buf;
	KUNIT_EXPECT_EQ(test, request->struct_version, EC_HOST_REQUEST_VERSION);
	KUNIT_EXPECT_EQ(test, request->command_version, (u8)0x1000);
	KUNIT_EXPECT_EQ(test, request->command, 0x1001);
	KUNIT_EXPECT_EQ(test, request->data_len, 2 /* outsize */);
	KUNIT_EXPECT_EQ(test, xt->buf[sizeof(*request)], 0xbe);
	KUNIT_EXPECT_EQ(test, xt->buf[sizeof(*request) + 1], 0xef);
	for (sum = 0, i = 0; i < sizeof(*request) + 2 /* outsize */; ++i)
		sum += xt->buf[i];
	KUNIT_EXPECT_EQ(test, sum, 0);
}

static struct kunit_case cros_ec_spi_test_pkt_xfer_cases[] = {
	KUNIT_CASE(cros_ec_spi_test_pkt_xfer_normal),
	{}
};

static struct kunit_suite cros_ec_spi_test_pkt_xfer_suite = {
	.name = "cros_ec_spi_test_pkt_xfer_suite",
	.init = cros_ec_spi_test_pkt_xfer_init,
	.exit = cros_ec_spi_test_exit,
	.test_cases = cros_ec_spi_test_pkt_xfer_cases,
};

kunit_test_suites(
	&cros_ec_spi_test_cmd_xfer_suite,
	&cros_ec_spi_test_pkt_xfer_suite,
);

MODULE_LICENSE("GPL");
