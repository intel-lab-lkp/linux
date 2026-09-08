// SPDX-License-Identifier: GPL-2.0-only
/* Included by qcom-hamoa-ec.c to test its private transport and callbacks. */

#include <kunit/resource.h>
#include <kunit/test.h>
#include <linux/of.h>

struct qcom_ec_test_context {
	struct i2c_adapter adapter;
	struct qcom_ec ec;
	u32 functionality;
	int transfer_result;
	int byte_result;
	u8 reply[4];
	u8 block_length;
	unsigned int transactions;
	unsigned int writes;
};

static int qcom_ec_test_transfer(struct i2c_adapter *adapter,
				 struct i2c_msg *messages, int count)
{
	struct qcom_ec_test_context *ctx = i2c_get_adapdata(adapter);

	ctx->transactions++;
	/* Only the RPM query's combined write/read transaction is allowed. */
	if (count != 2 || messages[0].addr != 0x76 || messages[1].addr != 0x76 ||
	    messages[0].flags || messages[1].flags != I2C_M_RD ||
	    messages[0].len != 2 || messages[1].len != 3 ||
	    messages[0].buf[0] != 0x22 || messages[0].buf[1] != 1) {
		ctx->writes++;
		return -EPROTO;
	}

	if (ctx->transfer_result == 2)
		memcpy(messages[1].buf, ctx->reply, messages[1].len);

	return ctx->transfer_result;
}

static s32 qcom_ec_test_smbus(struct i2c_adapter *adapter, u16 address,
			      unsigned short flags, char read_write, u8 command,
			      int size, union i2c_smbus_data *data)
{
	struct qcom_ec_test_context *ctx = i2c_get_adapdata(adapter);

	ctx->transactions++;
	if (read_write != I2C_SMBUS_READ) {
		ctx->writes++;
		return -EPROTO;
	}
	if (address != 0x76 || flags)
		return -EPROTO;

	if (command == 0x29 && size == I2C_SMBUS_BYTE_DATA) {
		if (ctx->byte_result < 0)
			return ctx->byte_result;
		data->byte = ctx->byte_result;
		return 0;
	}

	if ((command == 0x0e || command == 0x42) && size == I2C_SMBUS_I2C_BLOCK_DATA) {
		data->block[0] = ctx->block_length;
		memcpy(&data->block[1], ctx->reply, ctx->block_length);
		return 0;
	}

	return -EPROTO;
}

static u32 qcom_ec_test_functionality(struct i2c_adapter *adapter)
{
	struct qcom_ec_test_context *ctx = i2c_get_adapdata(adapter);

	return ctx->functionality;
}

static const struct i2c_algorithm qcom_ec_test_algorithm = {
	.master_xfer = qcom_ec_test_transfer,
	.smbus_xfer = qcom_ec_test_smbus,
	.functionality = qcom_ec_test_functionality,
};

static void qcom_ec_test_delete_adapter(void *data)
{
	i2c_del_adapter(data);
}

static void qcom_ec_test_unregister_client(void *data)
{
	i2c_unregister_device(data);
}

static int qcom_ec_test_init(struct kunit *test)
{
	struct qcom_ec_test_context *ctx;
	int ret;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	test->priv = ctx;
	ctx->functionality = I2C_FUNC_I2C | I2C_FUNC_SMBUS_READ_BYTE_DATA |
			     I2C_FUNC_SMBUS_READ_I2C_BLOCK;
	ctx->transfer_result = 2;
	ctx->reply[0] = 2;
	ctx->byte_result = 36;
	ctx->adapter.owner = THIS_MODULE;
	ctx->adapter.algo = &qcom_ec_test_algorithm;
	strscpy(ctx->adapter.name, "qcom-ec-kunit", sizeof(ctx->adapter.name));
	i2c_set_adapdata(&ctx->adapter, ctx);
	ret = i2c_add_adapter(&ctx->adapter);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = kunit_add_action_or_reset(test, qcom_ec_test_delete_adapter, &ctx->adapter);
	KUNIT_ASSERT_EQ(test, ret, 0);

	ctx->ec.client = i2c_new_dummy_device(&ctx->adapter, 0x76);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx->ec.client);
	ret = kunit_add_action_or_reset(test, qcom_ec_test_unregister_client, ctx->ec.client);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ctx->ec.variant = &qcom_ec_slim7x;
	i2c_set_clientdata(ctx->ec.client, &ctx->ec);

	return 0;
}

static void qcom_ec_rpm_test(struct kunit *test)
{
	struct qcom_ec_test_context *ctx = test->priv;
	static const struct {
		int transfer_result;
		u8 count;
		int error;
	} failures[] = {
		{ 0, 2, -EIO },
		{ 1, 2, -EIO },
		{ -EREMOTEIO, 2, -EREMOTEIO },
		{ -ETIMEDOUT, 2, -ETIMEDOUT },
		{ 2, 0, -EPROTO },
		{ 2, 1, -EPROTO },
		{ 2, 3, -EPROTO },
		{ 2, 0xff, -EPROTO },
	};
	long value = -1;
	int i;

	KUNIT_EXPECT_EQ(test, qcom_ec_read_fan_rpm(&ctx->ec, &value), 0);
	KUNIT_EXPECT_EQ(test, value, 0);
	ctx->reply[1] = 0xef;
	ctx->reply[2] = 0xbe;
	KUNIT_EXPECT_EQ(test, qcom_ec_read_fan_rpm(&ctx->ec, &value), 0);
	KUNIT_EXPECT_EQ(test, value, 0xbeef);

	for (i = 0; i < ARRAY_SIZE(failures); i++) {
		value = -1;
		ctx->transfer_result = failures[i].transfer_result;
		ctx->reply[0] = failures[i].count;
		KUNIT_EXPECT_EQ_MSG(test, qcom_ec_read_fan_rpm(&ctx->ec, &value),
				    failures[i].error, "case %d", i);
		KUNIT_EXPECT_EQ(test, value, -1);
	}
	KUNIT_EXPECT_EQ(test, ctx->writes, 0);
}

static void qcom_ec_temperature_test(struct kunit *test)
{
	struct qcom_ec_test_context *ctx = test->priv;
	static const int temperatures[] = { 0, 36, 254, 255, -EREMOTEIO };
	long value;
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(temperatures); i++) {
		value = -1;
		ctx->byte_result = temperatures[i];
		ret = qcom_ec_hwmon_read(&ctx->ec.client->dev, hwmon_temp,
					 hwmon_temp_input, 0, &value);
		if (temperatures[i] < 0) {
			KUNIT_EXPECT_EQ(test, ret, temperatures[i]);
			KUNIT_EXPECT_EQ(test, value, -1);
		} else if (temperatures[i] == 255) {
			KUNIT_EXPECT_EQ(test, ret, -ENODATA);
			KUNIT_EXPECT_EQ(test, value, -1);
		} else {
			KUNIT_EXPECT_EQ(test, ret, 0);
			KUNIT_EXPECT_EQ(test, value, temperatures[i] * 1000);
		}
	}
	KUNIT_EXPECT_EQ(test, ctx->writes, 0);
}

static void qcom_ec_read_only_test(struct kunit *test)
{
	struct qcom_ec_test_context *ctx = test->priv;
	struct device *dev = &ctx->ec.client->dev;
	long value = -1;

	KUNIT_EXPECT_EQ(test, qcom_ec_hwmon_is_visible(NULL, hwmon_fan, hwmon_fan_input, 0),
			0444);
	KUNIT_EXPECT_EQ(test, qcom_ec_hwmon_is_visible(NULL, hwmon_temp, hwmon_temp_input, 0),
			0444);
	KUNIT_EXPECT_EQ(test, qcom_ec_hwmon_is_visible(NULL, hwmon_fan, hwmon_fan_input, 1), 0);
	KUNIT_EXPECT_EQ(test, qcom_ec_hwmon_is_visible(NULL, hwmon_pwm, hwmon_pwm_input, 0), 0);
	KUNIT_EXPECT_EQ(test, qcom_ec_hwmon_read(dev, hwmon_fan, hwmon_fan_input, 1, &value),
			-EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, qcom_ec_hwmon_read(dev, hwmon_pwm, hwmon_pwm_input, 0, &value),
			-EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, value, -1);
	KUNIT_EXPECT_EQ(test, qcom_ec_suspend(dev), 0);
	KUNIT_EXPECT_EQ(test, qcom_ec_resume(dev), 0);
	qcom_ec_remove(ctx->ec.client);
	KUNIT_EXPECT_EQ(test, ctx->transactions, 0);
}

static void qcom_ec_probe_transport_test(struct kunit *test)
{
	struct qcom_ec_test_context *ctx = test->priv;

	ctx->functionality = I2C_FUNC_I2C;
	KUNIT_EXPECT_EQ(test, qcom_ec_hwmon_probe(&ctx->ec), -EOPNOTSUPP);
	ctx->functionality = I2C_FUNC_SMBUS_READ_BYTE_DATA;
	KUNIT_EXPECT_EQ(test, qcom_ec_hwmon_probe(&ctx->ec), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, ctx->transactions, 0);
}

static void qcom_ec_probe_sensor_test(struct kunit *test)
{
	struct qcom_ec_test_context *ctx = test->priv;

	KUNIT_EXPECT_EQ(test, qcom_ec_hwmon_probe(&ctx->ec), 0);
	KUNIT_EXPECT_EQ(test, ctx->transactions, 1);
	KUNIT_EXPECT_EQ(test, ctx->writes, 0);
	ctx->reply[0] = 0;
	KUNIT_EXPECT_EQ(test, qcom_ec_hwmon_probe(&ctx->ec), -EPROTO);
	KUNIT_EXPECT_EQ(test, ctx->writes, 0);
}

static void qcom_ec_reference_validation_test(struct kunit *test)
{
	struct qcom_ec_test_context *ctx = test->priv;
	u8 response[3];

	ctx->block_length = 3;
	KUNIT_EXPECT_EQ(test, qcom_ec_read(&ctx->ec, EC_THERMAL_CAP_CMD, 3, response), 0);
	ctx->block_length = 2;
	KUNIT_EXPECT_EQ(test, qcom_ec_read(&ctx->ec, EC_THERMAL_CAP_CMD, 3, response), -EIO);
	ctx->block_length = 0;
	KUNIT_EXPECT_EQ(test, qcom_ec_read(&ctx->ec, EC_THERMAL_CAP_CMD, 3, response),
			-EOPNOTSUPP);
	ctx->block_length = 3;
	ctx->reply[0] = 0;
	KUNIT_EXPECT_EQ(test, qcom_ec_read(&ctx->ec, EC_THERMAL_CAP_CMD, 3, response), -EINVAL);
	ctx->reply[0] = 1;
	KUNIT_EXPECT_EQ(test, qcom_ec_read(&ctx->ec, EC_THERMAL_CAP_CMD, 3, response), -EINVAL);
	KUNIT_EXPECT_EQ(test, ctx->writes, 0);
}

static void qcom_ec_variant_test(struct kunit *test)
{
	static const char compatible[] = "lenovo,yoga-slim7x-ec\0qcom,hamoa-crd-ec";
	struct property property = {
		.name = "compatible",
		.length = sizeof(compatible),
		.value = (void *)compatible,
	};
	struct device_node node = { .properties = &property };
	const struct of_device_id *match;

	match = of_match_node(qcom_ec_of_match, &node);
	KUNIT_ASSERT_NOT_NULL(test, match);
	KUNIT_EXPECT_PTR_EQ(test, match->data, &qcom_ec_slim7x);
	property.value = "qcom,hamoa-crd-ec";
	property.length = sizeof("qcom,hamoa-crd-ec");
	match = of_match_node(qcom_ec_of_match, &node);
	KUNIT_ASSERT_NOT_NULL(test, match);
	KUNIT_EXPECT_PTR_EQ(test, match->data, &qcom_ec_reference);
}

static struct kunit_case qcom_ec_test_cases[] = {
	KUNIT_CASE(qcom_ec_rpm_test),
	KUNIT_CASE(qcom_ec_temperature_test),
	KUNIT_CASE(qcom_ec_read_only_test),
	KUNIT_CASE(qcom_ec_probe_transport_test),
	KUNIT_CASE(qcom_ec_probe_sensor_test),
	KUNIT_CASE(qcom_ec_reference_validation_test),
	KUNIT_CASE(qcom_ec_variant_test),
	{}
};

static struct kunit_suite qcom_ec_test_suite = {
	.name = "qcom-hamoa-ec",
	.init = qcom_ec_test_init,
	.test_cases = qcom_ec_test_cases,
};

kunit_test_suite(qcom_ec_test_suite);
