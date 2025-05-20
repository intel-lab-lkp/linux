// SPDX-License-Identifier: GPL-2.0
/*
 * Kunit tests for ChromeOS Embedded Controller I2C interface.
 */
#include <kunit/test.h>
#include <kunit/static_stub.h>

#include <linux/i2c.h>
#include <linux/platform_data/cros_ec_proto.h>
#include <linux/sched.h>

#include "cros_ec.h"

#define BUFSIZE 128
#define I2C_ADDR 0x06

struct cros_ec_i2c_test_priv {
	struct i2c_adapter *fake_adap;
	struct i2c_client *client;

	int fake_cros_ec_register_called;
	struct cros_ec_device *ec_dev;

	int fake_cros_ec_unregister_called;

	struct i2c_msg *i2c_msgs;
	int i2c_msg_num;
	int fake_i2c_xfer_res;
	int fake_i2c_xfer_len;
	u8 *fake_i2c_xfer_data;
	u8 fake_i2c_xfer_sum;
	int fake_i2c_xfer_ret;
};

static int fake_cros_ec_register(struct cros_ec_device *ec_dev)
{
	struct kunit *test = current->kunit_test;
	struct cros_ec_i2c_test_priv *priv = test->priv;

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
	struct cros_ec_i2c_test_priv *priv = test->priv;

	priv->fake_cros_ec_unregister_called += 1;
}

static int fake_cros_ec_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg msgs[], int num)
{
	struct kunit *test = current->kunit_test;
	struct cros_ec_i2c_test_priv *priv = test->priv;
	int i;

	priv->i2c_msgs = kunit_kmalloc_array(test, sizeof(struct i2c_msg), num, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, priv->i2c_msgs);

	for (i = 0; i < num; ++i) {
		memcpy(priv->i2c_msgs + i, msgs + i, sizeof(struct i2c_msg));

		priv->i2c_msgs[i].buf = kunit_kmalloc(test, msgs[i].len, GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, priv->i2c_msgs[i].buf);
		memcpy(priv->i2c_msgs[i].buf, msgs[i].buf, msgs[i].len);

		if (msgs[i].flags == I2C_M_RD) {
			msgs[i].buf[0] = priv->fake_i2c_xfer_res;
			msgs[i].buf[1] = priv->fake_i2c_xfer_len;

			if (priv->fake_i2c_xfer_data)
				memcpy(&msgs[i].buf[2], priv->fake_i2c_xfer_data,
				       priv->fake_i2c_xfer_len);

			msgs[i].buf[priv->fake_i2c_xfer_len + 2] = priv->fake_i2c_xfer_sum;
		}
	}
	priv->i2c_msg_num = num;

	return priv->fake_i2c_xfer_ret;
}

static u32 fake_cros_ec_i2c_functionality(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm fake_cros_ec_i2c_algorithm = {
	.master_xfer = fake_cros_ec_i2c_xfer,
	.functionality = fake_cros_ec_i2c_functionality,
};

static int cros_ec_i2c_test_init(struct kunit *test)
{
	struct cros_ec_i2c_test_priv *priv;
	static const struct i2c_board_info board_info = {
		I2C_BOARD_INFO("cros-ec-i2c", I2C_ADDR),
	};

	kunit_activate_static_stub(test, cros_ec_register, fake_cros_ec_register);
	kunit_activate_static_stub(test, cros_ec_unregister, fake_cros_ec_unregister);

	priv = kunit_kzalloc(test, sizeof(*priv), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, priv);
	test->priv = priv;

	priv->fake_adap = kunit_kzalloc(test, sizeof(*priv->fake_adap), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, priv->fake_adap);

	priv->fake_adap->owner = THIS_MODULE;
	strscpy(priv->fake_adap->name, "cros-ec-i2c-test");
	priv->fake_adap->algo = &fake_cros_ec_i2c_algorithm;
	priv->fake_adap->retries = 3;

	KUNIT_ASSERT_EQ(test, i2c_add_adapter(priv->fake_adap), 0);

	priv->client = i2c_new_client_device(priv->fake_adap, &board_info);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, priv->client);
	KUNIT_EXPECT_EQ(test, priv->client->addr, I2C_ADDR);

	KUNIT_EXPECT_EQ(test, priv->fake_cros_ec_register_called, 1);
	KUNIT_ASSERT_NOT_NULL(test, priv->ec_dev);
	KUNIT_EXPECT_EQ(test, priv->fake_cros_ec_unregister_called, 0);
	return 0;
}

static void cros_ec_i2c_test_exit(struct kunit *test)
{
	struct cros_ec_i2c_test_priv *priv = test->priv;

	i2c_unregister_device(priv->client);
	KUNIT_EXPECT_EQ(test, priv->fake_cros_ec_unregister_called, 1);

	i2c_del_adapter(priv->fake_adap);

	kunit_deactivate_static_stub(test, cros_ec_register);
	kunit_deactivate_static_stub(test, cros_ec_unregister);
}

static int cros_ec_i2c_test_cmd_xfer_init(struct kunit *test)
{
	struct cros_ec_i2c_test_priv *priv;

	cros_ec_i2c_test_init(test);
	priv = test->priv;
	priv->ec_dev->proto_version = 2;
	return 0;
}

static void cros_ec_i2c_test_cmd_xfer_normal(struct kunit *test)
{
	struct cros_ec_i2c_test_priv *priv = test->priv;
	struct cros_ec_command *msg;
	int ret, i;
	u8 sum;

	msg = kunit_kmalloc(test, sizeof(*msg) + 2 /* max(outsize, insize) */, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, msg);
	msg->version = 0x1000;
	msg->command = 0x1001;
	msg->outsize = 2;
	msg->insize = 1;
	msg->data[0] = 0xbe;
	msg->data[1] = 0xef;

	priv->fake_i2c_xfer_res = 0;
	priv->fake_i2c_xfer_data = kunit_kmalloc(test, msg->insize, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, priv->fake_i2c_xfer_data);
	priv->fake_i2c_xfer_data[0] = 0xaa;
	priv->fake_i2c_xfer_len = msg->insize;
	priv->fake_i2c_xfer_ret = 2 /* # of i2c_msg */;
	sum = priv->fake_i2c_xfer_res + priv->fake_i2c_xfer_len + priv->fake_i2c_xfer_data[0];
	priv->fake_i2c_xfer_sum = sum;

	ret = priv->ec_dev->cmd_xfer(priv->ec_dev, msg);
	KUNIT_EXPECT_EQ(test, ret, 1 /* insize */);
	KUNIT_EXPECT_EQ(test, msg->result, 0);
	KUNIT_EXPECT_EQ(test, msg->data[0], 0xaa);

	KUNIT_EXPECT_EQ(test, priv->i2c_msg_num, 2);
	/*
	 * Validate output message only which is supposed to be received by EC devices.
	 */
	KUNIT_EXPECT_EQ(test, priv->i2c_msgs[0].addr, I2C_ADDR);
	KUNIT_EXPECT_EQ(test, priv->i2c_msgs[0].flags, 0);
	/*
	 * Total length of the message: EC_MSG_TX_HEADER_BYTES (version, command, length) +
	 * outsize + EC_MSG_TX_TRAILER_BYTES (checksum).
	 */
	KUNIT_EXPECT_EQ(test, priv->i2c_msgs[0].len, EC_MSG_TX_PROTO_BYTES + 2 /* outsize */);
	KUNIT_EXPECT_EQ(test, priv->i2c_msgs[0].buf[0], (u8)(EC_CMD_VERSION0 + 0x1000));
	KUNIT_EXPECT_EQ(test, priv->i2c_msgs[0].buf[1], (u8)0x1001);
	KUNIT_EXPECT_EQ(test, priv->i2c_msgs[0].buf[2], 2 /* outsize */);
	for (sum = 0, i = 0; i < EC_MSG_TX_HEADER_BYTES + 2 /* outsize */; ++i)
		sum += priv->i2c_msgs[0].buf[i];
	KUNIT_EXPECT_EQ(test,
			priv->i2c_msgs[0].buf[EC_MSG_TX_HEADER_BYTES + 2 /* outsize */], sum);
}

static void cros_ec_i2c_test_cmd_xfer_error(struct kunit *test)
{
	struct cros_ec_i2c_test_priv *priv = test->priv;
	struct cros_ec_command msg = {};
	int ret;

	priv->fake_i2c_xfer_ret = -EBUSY;
	ret = priv->ec_dev->cmd_xfer(priv->ec_dev, &msg);
	KUNIT_EXPECT_EQ(test, ret, -EBUSY);

	priv->fake_i2c_xfer_ret = 1;
	ret = priv->ec_dev->cmd_xfer(priv->ec_dev, &msg);
	KUNIT_EXPECT_EQ(test, ret, -EIO);

	priv->fake_i2c_xfer_ret = 2 /* # of i2c_msg */;
	priv->fake_i2c_xfer_res = EC_RES_IN_PROGRESS;
	ret = priv->ec_dev->cmd_xfer(priv->ec_dev, &msg);
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
}

static void cros_ec_i2c_test_cmd_xfer_response_too_long(struct kunit *test)
{
	struct cros_ec_i2c_test_priv *priv = test->priv;
	struct cros_ec_command msg = {};
	int ret;

	priv->fake_i2c_xfer_res = 0;
	priv->fake_i2c_xfer_data = kunit_kmalloc(test, 1, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, priv->fake_i2c_xfer_data);
	priv->fake_i2c_xfer_len = msg.insize + 1; /* make it greater than msg.insize */
	priv->fake_i2c_xfer_ret = 2 /* # of i2c_msg */;

	ret = priv->ec_dev->cmd_xfer(priv->ec_dev, &msg);
	KUNIT_EXPECT_EQ(test, ret, -ENOSPC);
}

static void cros_ec_i2c_test_cmd_xfer_response_bad_checksum(struct kunit *test)
{
	struct cros_ec_i2c_test_priv *priv = test->priv;
	struct cros_ec_command msg = {};
	int ret;

	priv->fake_i2c_xfer_ret = 2 /* # of i2c_msg */;
	priv->fake_i2c_xfer_sum = (u8)0xbad;

	ret = priv->ec_dev->cmd_xfer(priv->ec_dev, &msg);
	KUNIT_EXPECT_EQ(test, ret, -EBADMSG);
}

static struct kunit_case cros_ec_i2c_test_cmd_xfer_cases[] = {
	KUNIT_CASE(cros_ec_i2c_test_cmd_xfer_normal),
	KUNIT_CASE(cros_ec_i2c_test_cmd_xfer_error),
	KUNIT_CASE(cros_ec_i2c_test_cmd_xfer_response_too_long),
	KUNIT_CASE(cros_ec_i2c_test_cmd_xfer_response_bad_checksum),
	{},
};

static struct kunit_suite cros_ec_i2c_test_cmd_xfer_suite = {
	.name = "cros_ec_i2c_test_cmd_xfer_suite",
	.init = cros_ec_i2c_test_cmd_xfer_init,
	.exit = cros_ec_i2c_test_exit,
	.test_cases = cros_ec_i2c_test_cmd_xfer_cases,
};

static int cros_ec_i2c_test_pkt_xfer_init(struct kunit *test)
{
	struct cros_ec_i2c_test_priv *priv;

	cros_ec_i2c_test_init(test);
	priv = test->priv;
	priv->ec_dev->proto_version = 3;
	return 0;
}

static void cros_ec_i2c_test_pkt_xfer_normal(struct kunit *test)
{
	struct cros_ec_i2c_test_priv *priv = test->priv;
	struct cros_ec_command *msg;
	struct ec_host_request *request;
	struct ec_host_response *response;
	int ret, i;
	u8 sum;

	msg = kunit_kmalloc(test, sizeof(*msg) + 2 /* max(outsize, insize) */, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, msg);
	msg->version = 0x1000;
	msg->command = 0x1001;
	msg->outsize = 2;
	msg->insize = 1;
	msg->data[0] = 0xbe;
	msg->data[1] = 0xef;

	priv->fake_i2c_xfer_res = 0;
	priv->fake_i2c_xfer_len = sizeof(*response) + msg->insize;
	priv->fake_i2c_xfer_data = kunit_kzalloc(test, priv->fake_i2c_xfer_len, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, priv->fake_i2c_xfer_data);
	response = (struct ec_host_response *)priv->fake_i2c_xfer_data;
	response->struct_version = EC_HOST_RESPONSE_VERSION;
	response->data_len = msg->insize;
	priv->fake_i2c_xfer_data[sizeof(*response)] = 0xaa;
	priv->fake_i2c_xfer_ret = 2 /* # of i2c_msg */;
	for (sum = 0, i = 0; i < priv->fake_i2c_xfer_len; ++i)
		sum += priv->fake_i2c_xfer_data[i];
	response->checksum = -sum;

	ret = priv->ec_dev->pkt_xfer(priv->ec_dev, msg);
	KUNIT_EXPECT_EQ(test, ret, 1 /* insize */);
	KUNIT_EXPECT_EQ(test, msg->result, 0);
	KUNIT_EXPECT_EQ(test, msg->data[0], 0xaa);

	KUNIT_EXPECT_EQ(test, priv->i2c_msg_num, 2);
	/*
	 * Validate output message only which is supposed to be received by EC devices.
	 */
	KUNIT_EXPECT_EQ(test, priv->i2c_msgs[0].addr, I2C_ADDR);
	KUNIT_EXPECT_EQ(test, priv->i2c_msgs[0].flags, 0);
	/*
	 * Total length of the message: sizeof(struct ec_host_request_i2c) + outsize.
	 * The test can't access struct ec_host_request_i2c directly (in cros_ec_i2c.c).
	 * However, it is a {u8 + struct ec_host_request} struct.
	 */
	KUNIT_EXPECT_EQ(test, priv->i2c_msgs[0].len, 1 + sizeof(*request) + 2 /* outsize */);
	/*
	 * i2c_msgs[0].buf is a struct ec_host_request_i2c.  Again, the test can't access the
	 * struct directly.  Access the u8 and struct ec_host_request separately.
	 */
	KUNIT_EXPECT_EQ(test, priv->i2c_msgs[0].buf[0], EC_COMMAND_PROTOCOL_3);
	request = (struct ec_host_request *)&priv->i2c_msgs[0].buf[1];
	KUNIT_EXPECT_EQ(test, request->struct_version, EC_HOST_REQUEST_VERSION);
	KUNIT_EXPECT_EQ(test, request->command, 0x1001);
	KUNIT_EXPECT_EQ(test, request->command_version, (u8)0x1000);
	KUNIT_EXPECT_EQ(test, request->data_len, 2 /* outsize */);
	for (sum = 0, i = 0; i < sizeof(*request); ++i)
		sum += ((u8 *)request)[i];
	sum += 0xbe + 0xef;
	KUNIT_EXPECT_EQ(test, sum, 0);
}

static void cros_ec_i2c_test_pkt_xfer_msg_too_long(struct kunit *test)
{
	struct cros_ec_i2c_test_priv *priv = test->priv;
	struct cros_ec_command msg = {
		.insize = 10,
		.outsize = 10,
	};
	int ret;

	priv->ec_dev->din_size = 0;
	ret = priv->ec_dev->pkt_xfer(priv->ec_dev, &msg);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	priv->ec_dev->din_size = BUFSIZE;
	priv->ec_dev->dout_size = 0;
	ret = priv->ec_dev->pkt_xfer(priv->ec_dev, &msg);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
}

static void cros_ec_i2c_test_pkt_xfer_error(struct kunit *test)
{
	struct cros_ec_i2c_test_priv *priv = test->priv;
	struct cros_ec_command msg = {};
	int ret;

	priv->fake_i2c_xfer_ret = -EBUSY;
	ret = priv->ec_dev->pkt_xfer(priv->ec_dev, &msg);
	KUNIT_EXPECT_EQ(test, ret, -EBUSY);

	priv->fake_i2c_xfer_ret = 1;
	ret = priv->ec_dev->pkt_xfer(priv->ec_dev, &msg);
	KUNIT_EXPECT_EQ(test, ret, -EIO);
}

static void cros_ec_i2c_test_pkt_xfer_in_progress(struct kunit *test)
{
	struct cros_ec_i2c_test_priv *priv = test->priv;
	struct cros_ec_command msg = {};
	int ret;

	priv->fake_i2c_xfer_res = EC_RES_IN_PROGRESS;
	priv->fake_i2c_xfer_ret = 2 /* # of i2c_msg */;
	ret = priv->ec_dev->pkt_xfer(priv->ec_dev, &msg);
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
}

static void cros_ec_i2c_test_pkt_xfer_to_v2_proto(struct kunit *test)
{
	struct cros_ec_i2c_test_priv *priv = test->priv;
	struct cros_ec_command msg = {};
	int ret;

	priv->fake_i2c_xfer_res = EC_RES_INVALID_COMMAND;
	priv->fake_i2c_xfer_ret = 2 /* # of i2c_msg */;
	ret = priv->ec_dev->pkt_xfer(priv->ec_dev, &msg);
	KUNIT_EXPECT_EQ(test, ret, -EPROTONOSUPPORT);
}

static void cros_ec_i2c_test_pkt_xfer_response_too_short(struct kunit *test)
{
	struct cros_ec_i2c_test_priv *priv = test->priv;
	struct cros_ec_command msg = {};
	int ret;

	priv->fake_i2c_xfer_len = sizeof(struct ec_host_response) - 1; /* make it shorter */
	priv->fake_i2c_xfer_ret = 2 /* # of i2c_msg */;
	ret = priv->ec_dev->pkt_xfer(priv->ec_dev, &msg);
	KUNIT_EXPECT_EQ(test, ret, -EBADMSG);
}

static void cros_ec_i2c_test_pkt_xfer_response_too_long(struct kunit *test)
{
	struct cros_ec_i2c_test_priv *priv = test->priv;
	struct cros_ec_command msg = {};
	struct ec_host_response *response;
	int ret;

	priv->fake_i2c_xfer_len = sizeof(*response);
	priv->fake_i2c_xfer_data = kunit_kzalloc(test, priv->fake_i2c_xfer_len, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, priv->fake_i2c_xfer_data);
	response = (struct ec_host_response *)priv->fake_i2c_xfer_data;
	response->struct_version = EC_HOST_RESPONSE_VERSION;
	response->data_len = 100;
	priv->fake_i2c_xfer_ret = 2 /* # of i2c_msg */;

	ret = priv->ec_dev->pkt_xfer(priv->ec_dev, &msg);
	KUNIT_EXPECT_EQ(test, ret, -EMSGSIZE);
}

static void cros_ec_i2c_test_pkt_xfer_bad_checksum(struct kunit *test)
{
	struct cros_ec_i2c_test_priv *priv = test->priv;
	struct cros_ec_command msg = {};
	struct ec_host_response *response;
	int ret;

	priv->fake_i2c_xfer_len = sizeof(*response);
	priv->fake_i2c_xfer_data = kunit_kzalloc(test, priv->fake_i2c_xfer_len, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, priv->fake_i2c_xfer_data);
	response = (struct ec_host_response *)priv->fake_i2c_xfer_data;
	response->struct_version = EC_HOST_RESPONSE_VERSION;
	response->checksum = (u8)0xbad;
	priv->fake_i2c_xfer_ret = 2 /* # of i2c_msg */;

	ret = priv->ec_dev->pkt_xfer(priv->ec_dev, &msg);
	KUNIT_EXPECT_EQ(test, ret, -EBADMSG);
}

static struct kunit_case cros_ec_i2c_test_pkt_xfer_cases[] = {
	KUNIT_CASE(cros_ec_i2c_test_pkt_xfer_normal),
	KUNIT_CASE(cros_ec_i2c_test_pkt_xfer_msg_too_long),
	KUNIT_CASE(cros_ec_i2c_test_pkt_xfer_error),
	KUNIT_CASE(cros_ec_i2c_test_pkt_xfer_in_progress),
	KUNIT_CASE(cros_ec_i2c_test_pkt_xfer_to_v2_proto),
	KUNIT_CASE(cros_ec_i2c_test_pkt_xfer_response_too_short),
	KUNIT_CASE(cros_ec_i2c_test_pkt_xfer_response_too_long),
	KUNIT_CASE(cros_ec_i2c_test_pkt_xfer_bad_checksum),
	{},
};

static struct kunit_suite cros_ec_i2c_test_pkt_xfer_suite = {
	.name = "cros_ec_i2c_test_pkt_xfer_suite",
	.init = cros_ec_i2c_test_pkt_xfer_init,
	.exit = cros_ec_i2c_test_exit,
	.test_cases = cros_ec_i2c_test_pkt_xfer_cases,
};

kunit_test_suites(
	&cros_ec_i2c_test_cmd_xfer_suite,
	&cros_ec_i2c_test_pkt_xfer_suite,
);

MODULE_LICENSE("GPL");
