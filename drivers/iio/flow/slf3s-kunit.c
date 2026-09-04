// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the Sensirion SLF3S driver's power management.
 *
 * The tests bind the real driver to a fake I2C device backed by a
 * programmable regulator, so that every regulator_enable() and
 * regulator_disable() the driver issues can be counted.  The point of the
 * suite is the balance of those calls across suspend/resume cycles,
 * including the cycles in which the sensor or the supply fails.
 *
 * Copyright (c) 2026
 */

#include <kunit/device.h>
#include <kunit/resource.h>
#include <kunit/test.h>

#include <linux/cleanup.h>
#include <linux/crc8.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/sprintf.h>
#include <linux/string.h>

#define SLF3S_TEST_ADDR			0x08
#define SLF3S_TEST_CRC8_POLY		0x31
#define SLF3S_TEST_CRC8_INIT		0xff
#define SLF3S_TEST_PID_LEN		18
/* "%d-%04x" for an adapter number and a 16-bit address, plus the nul. */
#define SLF3S_TEST_CONSUMER_LEN		16

/*
 * Family byte 0x03 and sub-type byte 0x03 make slf3s_detect_variant()
 * settle on the SLF3S-0600F.  The bytes sit at offsets 1 and 3 of the
 * product-info block, i.e. in the low half of the first word and the high
 * half of the second one.
 */
#define SLF3S_TEST_PID_WORD0		0x0003
#define SLF3S_TEST_PID_WORD1		0x0300

enum slf3s_fake_state {
	SLF3S_FAKE_OFF,
	SLF3S_FAKE_IDLE,
	SLF3S_FAKE_MEASURING,
};

enum slf3s_fake_cmd {
	SLF3S_FAKE_CMD_NONE,
	SLF3S_FAKE_CMD_PREP_PID,
	SLF3S_FAKE_CMD_READ_PID,
	SLF3S_FAKE_CMD_START_WATER,
	SLF3S_FAKE_CMD_START_IPA,
	SLF3S_FAKE_CMD_STOP,
	SLF3S_FAKE_CMD_UNKNOWN,
	SLF3S_FAKE_CMD_COUNT,
};

/**
 * struct slf3s_fake - protocol-level emulation of an SLF3S sensor
 * @adap:	I2C adapter the emulated sensor answers on
 * @lock:	serialises state against the driver's transfers
 * @state:	power/measurement state of the emulated part
 * @pid_armed:	a product-id read command was accepted, a read may follow
 * @fail_cmd:	command whose next occurrence is rejected
 * @fail_err:	error returned for @fail_cmd
 * @n_cmd:	per-command counters, indexed by enum slf3s_fake_cmd
 * @crc_table:	CRC-8 table used to sign the emulated responses
 */
struct slf3s_fake {
	struct i2c_adapter adap;
	struct mutex lock; /* serialises state against the driver's transfers */
	enum slf3s_fake_state state;
	bool pid_armed;
	enum slf3s_fake_cmd fail_cmd;
	int fail_err;
	unsigned int n_cmd[SLF3S_FAKE_CMD_COUNT];
	u8 crc_table[CRC8_TABLE_SIZE];
};

/**
 * struct slf3s_test_reg - regulator that counts what the driver does to it
 * @rdev:	registered regulator device
 * @lock:	protects the counters against concurrent callbacks
 * @n_enable:	successful regulator_enable() calls seen
 * @n_disable:	successful regulator_disable() calls seen
 * @enabled:	current state of the emulated supply
 * @fail_enable: error returned by the next enable, then cleared
 */
struct slf3s_test_reg {
	struct regulator_dev *rdev;
	struct mutex lock; /* protects the counters against concurrent callbacks */
	unsigned int n_enable;
	unsigned int n_disable;
	bool enabled;
	int fail_enable;
};

/**
 * struct slf3s_test_ctx - per-test fixture
 * @fake:	emulated sensor
 * @reg:	emulated supply
 * @client:	I2C client the driver is bound to
 * @suspended:	mirrors dev->power.is_suspended for the PM sequencer
 */
struct slf3s_test_ctx {
	struct slf3s_fake fake;
	struct slf3s_test_reg reg;
	struct i2c_client *client;
	bool suspended;
};

/* --- emulated sensor ---------------------------------------------------- */

static enum slf3s_fake_cmd slf3s_fake_decode(const u8 *buf)
{
	static const struct {
		u8 bytes[2];
		enum slf3s_fake_cmd cmd;
	} cmds[] = {
		{ { 0x36, 0x7c }, SLF3S_FAKE_CMD_PREP_PID },
		{ { 0xe1, 0x02 }, SLF3S_FAKE_CMD_READ_PID },
		{ { 0x36, 0x08 }, SLF3S_FAKE_CMD_START_WATER },
		{ { 0x36, 0x15 }, SLF3S_FAKE_CMD_START_IPA },
		{ { 0x3f, 0xf9 }, SLF3S_FAKE_CMD_STOP },
	};

	for (unsigned int i = 0; i < ARRAY_SIZE(cmds); i++) {
		if (!memcmp(buf, cmds[i].bytes, sizeof(cmds[i].bytes)))
			return cmds[i].cmd;
	}

	return SLF3S_FAKE_CMD_UNKNOWN;
}

static int slf3s_fake_write(struct slf3s_fake *f, struct i2c_msg *msg)
{
	enum slf3s_fake_cmd cmd;

	if (msg->len != 2)
		return -EIO;

	cmd = slf3s_fake_decode(msg->buf);
	f->n_cmd[cmd]++;

	if (f->fail_cmd == cmd) {
		f->fail_cmd = SLF3S_FAKE_CMD_NONE;
		return f->fail_err;
	}

	switch (cmd) {
	case SLF3S_FAKE_CMD_PREP_PID:
	case SLF3S_FAKE_CMD_READ_PID:
		if (f->state != SLF3S_FAKE_IDLE)
			return -ENXIO;
		f->pid_armed = cmd == SLF3S_FAKE_CMD_READ_PID;
		return 0;
	case SLF3S_FAKE_CMD_START_WATER:
	case SLF3S_FAKE_CMD_START_IPA:
		if (f->state != SLF3S_FAKE_IDLE)
			return -ENXIO;
		f->state = SLF3S_FAKE_MEASURING;
		return 0;
	case SLF3S_FAKE_CMD_STOP:
		/* The real part NACKs a stop when it is already idle. */
		if (f->state != SLF3S_FAKE_MEASURING)
			return -ENXIO;
		f->state = SLF3S_FAKE_IDLE;
		return 0;
	default:
		return -ENXIO;
	}
}

static void slf3s_fake_put_word(struct slf3s_fake *f, u8 *dst, u16 val)
{
	dst[0] = val >> 8;
	dst[1] = val & 0xff;
	dst[2] = crc8(f->crc_table, dst, 2, SLF3S_TEST_CRC8_INIT);
}

/*
 * Only the product-info block is emulated.  Sample reads are not part of
 * these tests, and the driver never issues one on its own.
 */
static int slf3s_fake_read(struct slf3s_fake *f, struct i2c_msg *msg)
{
	u8 buf[SLF3S_TEST_PID_LEN];

	if (!f->pid_armed)
		return -ENXIO;

	f->pid_armed = false;

	if (msg->len != sizeof(buf))
		return -EIO;

	slf3s_fake_put_word(f, &buf[0], SLF3S_TEST_PID_WORD0);
	slf3s_fake_put_word(f, &buf[3], SLF3S_TEST_PID_WORD1);
	for (unsigned int i = 6; i < sizeof(buf); i += 3)
		slf3s_fake_put_word(f, &buf[i], 0);

	memcpy(msg->buf, buf, sizeof(buf));

	return 0;
}

static int slf3s_fake_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs,
			   int num)
{
	struct slf3s_fake *f = i2c_get_adapdata(adap);

	guard(mutex)(&f->lock);

	for (int i = 0; i < num; i++) {
		int ret;

		if (msgs[i].addr != SLF3S_TEST_ADDR)
			return -ENXIO;

		if (f->state == SLF3S_FAKE_OFF)
			return -ENXIO;

		if (msgs[i].flags & I2C_M_RD)
			ret = slf3s_fake_read(f, &msgs[i]);
		else
			ret = slf3s_fake_write(f, &msgs[i]);

		if (ret)
			return ret;
	}

	return num;
}

static u32 slf3s_fake_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C;
}

static const struct i2c_algorithm slf3s_fake_algo = {
	.xfer = slf3s_fake_xfer,
	.functionality = slf3s_fake_func,
};

static void slf3s_fake_fail_next(struct slf3s_fake *f,
				 enum slf3s_fake_cmd cmd, int err)
{
	guard(mutex)(&f->lock);

	f->fail_cmd = cmd;
	f->fail_err = err;
}

static enum slf3s_fake_state slf3s_fake_state(struct slf3s_fake *f)
{
	guard(mutex)(&f->lock);

	return f->state;
}

/* --- emulated supply ---------------------------------------------------- */

static int slf3s_test_reg_enable(struct regulator_dev *rdev)
{
	struct slf3s_test_reg *reg = rdev_get_drvdata(rdev);

	guard(mutex)(&reg->lock);

	if (reg->fail_enable) {
		int err = reg->fail_enable;

		reg->fail_enable = 0;
		return err;
	}

	reg->n_enable++;
	reg->enabled = true;

	return 0;
}

static int slf3s_test_reg_disable(struct regulator_dev *rdev)
{
	struct slf3s_test_reg *reg = rdev_get_drvdata(rdev);

	guard(mutex)(&reg->lock);

	reg->n_disable++;
	reg->enabled = false;

	return 0;
}

static int slf3s_test_reg_is_enabled(struct regulator_dev *rdev)
{
	struct slf3s_test_reg *reg = rdev_get_drvdata(rdev);

	guard(mutex)(&reg->lock);

	return reg->enabled;
}

static void slf3s_test_reg_fail_enable(struct slf3s_test_reg *reg, int err)
{
	guard(mutex)(&reg->lock);

	reg->fail_enable = err;
}

static const struct regulator_ops slf3s_test_reg_ops = {
	.enable = slf3s_test_reg_enable,
	.disable = slf3s_test_reg_disable,
	.is_enabled = slf3s_test_reg_is_enabled,
};

static const struct regulator_desc slf3s_test_reg_desc = {
	.name = "slf3s-test-vdd",
	.id = -1,
	.type = REGULATOR_VOLTAGE,
	.owner = THIS_MODULE,
	.ops = &slf3s_test_reg_ops,
};

/* --- PM sequencer ------------------------------------------------------- */

/*
 * device_suspend() sets dev->power.is_suspended only when the callback
 * returned 0, and device_resume() bails out before running any callback
 * when the flag is clear.  The resume callback therefore runs if and only
 * if the immediately preceding suspend callback succeeded.  The helpers
 * below reproduce that rule so the tests cannot construct a sequence the
 * PM core never produces.
 */
static int slf3s_test_suspend(struct kunit *test, struct slf3s_test_ctx *ctx)
{
	struct device *dev = &ctx->client->dev;
	int ret;

	KUNIT_ASSERT_FALSE_MSG(test, ctx->suspended,
			       "suspend called twice without a resume");
	KUNIT_ASSERT_NOT_NULL(test, dev->driver);
	KUNIT_ASSERT_NOT_NULL(test, dev->driver->pm);
	KUNIT_ASSERT_NOT_NULL(test, dev->driver->pm->suspend);

	ret = dev->driver->pm->suspend(dev);
	ctx->suspended = ret == 0;

	return ret;
}

static int slf3s_test_resume(struct kunit *test, struct slf3s_test_ctx *ctx)
{
	struct device *dev = &ctx->client->dev;

	KUNIT_ASSERT_TRUE_MSG(test, ctx->suspended,
			      "resume after a failed suspend: the PM core does not do this");
	KUNIT_ASSERT_NOT_NULL(test, dev->driver->pm->resume);

	ctx->suspended = false;

	return dev->driver->pm->resume(dev);
}

/*
 * After a resume whose start command failed, the sensor is powered but
 * idle.  A test that wants to continue cycling has to put it back into a
 * measuring state; doing so through the fake rather than through the
 * driver keeps the driver's call counts untouched.
 */
static void slf3s_test_resume_sensor(struct kunit *test,
				     struct slf3s_test_ctx *ctx)
{
	guard(mutex)(&ctx->fake.lock);

	if (ctx->fake.state == SLF3S_FAKE_IDLE)
		ctx->fake.state = SLF3S_FAKE_MEASURING;
}

#define KUNIT_EXPECT_REG_BALANCED(test, ctx) do {			\
	KUNIT_EXPECT_EQ((test), (ctx)->reg.n_enable,			\
			(ctx)->reg.n_disable);				\
	KUNIT_EXPECT_FALSE((test), (ctx)->reg.enabled);		\
} while (0)

/* --- fixture ------------------------------------------------------------ */

static void slf3s_test_del_adapter(void *ptr)
{
	i2c_del_adapter(ptr);
}

static void slf3s_test_unregister_reg(void *ptr)
{
	regulator_unregister(ptr);
}

static void slf3s_test_unregister_client(void *ptr)
{
	i2c_unregister_device(ptr);
}

static void slf3s_test_destroy_mutex(void *data)
{
	mutex_destroy(data);
}

static int slf3s_test_init(struct kunit *test)
{
	struct regulator_consumer_supply *supply;
	struct regulator_init_data *init_data;
	struct i2c_board_info info = { };
	struct regulator_config config = { };
	struct slf3s_test_ctx *ctx;
	struct device *parent;
	char *consumer;
	int ret;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	test->priv = ctx;

	/* Actions run in reverse, so destroy after the unbind takes the locks. */
	mutex_init(&ctx->fake.lock);
	ret = kunit_add_action_or_reset(test, slf3s_test_destroy_mutex,
					&ctx->fake.lock);
	KUNIT_ASSERT_EQ(test, ret, 0);

	mutex_init(&ctx->reg.lock);
	ret = kunit_add_action_or_reset(test, slf3s_test_destroy_mutex,
					&ctx->reg.lock);
	KUNIT_ASSERT_EQ(test, ret, 0);

	crc8_populate_msb(ctx->fake.crc_table, SLF3S_TEST_CRC8_POLY);
	ctx->fake.state = SLF3S_FAKE_OFF;

	parent = kunit_device_register(test, "slf3s-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, parent);

	strscpy(ctx->fake.adap.name, "slf3s-test-adapter",
		sizeof(ctx->fake.adap.name));
	ctx->fake.adap.owner = THIS_MODULE;
	ctx->fake.adap.algo = &slf3s_fake_algo;
	ctx->fake.adap.dev.parent = parent;
	i2c_set_adapdata(&ctx->fake.adap, &ctx->fake);

	ret = i2c_add_adapter(&ctx->fake.adap);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = kunit_add_action_or_reset(test, slf3s_test_del_adapter,
					&ctx->fake.adap);
	KUNIT_ASSERT_EQ(test, ret, 0);

	/*
	 * The supply has to be resolvable by the time the client probes, and
	 * without a device tree that means a consumer map keyed on the client
	 * name.  The name is "<bus>-<addr>", so the adapter has to exist
	 * first.
	 */
	consumer = kunit_kzalloc(test, SLF3S_TEST_CONSUMER_LEN, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, consumer);
	scnprintf(consumer, SLF3S_TEST_CONSUMER_LEN, "%d-%04x",
		  ctx->fake.adap.nr, SLF3S_TEST_ADDR);

	supply = kunit_kzalloc(test, sizeof(*supply), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, supply);
	supply->supply = "vdd";
	supply->dev_name = consumer;

	init_data = kunit_kzalloc(test, sizeof(*init_data), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, init_data);
	init_data->constraints.valid_ops_mask = REGULATOR_CHANGE_STATUS;
	init_data->num_consumer_supplies = 1;
	init_data->consumer_supplies = supply;

	config.dev = parent;
	config.init_data = init_data;
	config.driver_data = &ctx->reg;

	ctx->reg.rdev = regulator_register(parent, &slf3s_test_reg_desc,
					   &config);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx->reg.rdev);
	ret = kunit_add_action_or_reset(test, slf3s_test_unregister_reg,
					ctx->reg.rdev);
	KUNIT_ASSERT_EQ(test, ret, 0);

	/* Powered but idle, like a part that has just been given its supply. */
	ctx->fake.state = SLF3S_FAKE_IDLE;

	strscpy(info.type, "slf3s-0600f", sizeof(info.type));
	info.addr = SLF3S_TEST_ADDR;

	ctx->client = i2c_new_client_device(&ctx->fake.adap, &info);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx->client);
	ret = kunit_add_action_or_reset(test, slf3s_test_unregister_client,
					ctx->client);
	KUNIT_ASSERT_EQ(test, ret, 0);

	KUNIT_ASSERT_NOT_NULL_MSG(test, ctx->client->dev.driver,
				  "the slf3s driver did not bind");
	/* One enable proves we got this regulator, not the dummy one. */
	KUNIT_ASSERT_EQ_MSG(test, ctx->reg.n_enable, 1,
			    "driver did not enable the test regulator");

	return 0;
}

/* --- tests -------------------------------------------------------------- */

/* T1: a clean cycle pairs every enable with a disable. */
static void slf3s_test_cycle_balanced(struct kunit *test)
{
	struct slf3s_test_ctx *ctx = test->priv;

	KUNIT_EXPECT_EQ(test, slf3s_fake_state(&ctx->fake),
			SLF3S_FAKE_MEASURING);

	KUNIT_EXPECT_EQ(test, slf3s_test_suspend(test, ctx), 0);
	KUNIT_EXPECT_EQ(test, slf3s_fake_state(&ctx->fake), SLF3S_FAKE_IDLE);
	KUNIT_EXPECT_FALSE(test, ctx->reg.enabled);
	KUNIT_EXPECT_EQ(test, ctx->reg.n_disable, 1);

	KUNIT_EXPECT_EQ(test, slf3s_test_resume(test, ctx), 0);
	KUNIT_EXPECT_EQ(test, slf3s_fake_state(&ctx->fake),
			SLF3S_FAKE_MEASURING);
	KUNIT_EXPECT_TRUE(test, ctx->reg.enabled);
	KUNIT_EXPECT_EQ(test, ctx->reg.n_enable, 2);

	kunit_release_action(test, slf3s_test_unregister_client, ctx->client);

	KUNIT_EXPECT_EQ(test, ctx->reg.n_enable, 2);
	KUNIT_EXPECT_EQ(test, ctx->reg.n_disable, 2);
	KUNIT_EXPECT_REG_BALANCED(test, ctx);
}

/*
 * T5: the proof for the question raised on the list.  The PM core never
 * runs two resumes in a row, so the driver's enable count can never climb
 * above one, no matter how often the sensor fails.  The sequencer refuses
 * illegal orderings, so this loop is the legal worst case.
 */
static void slf3s_test_cycles_never_leak(struct kunit *test)
{
	struct slf3s_test_ctx *ctx = test->priv;

	for (unsigned int i = 0; i < 10; i++) {
		int ret;

		switch (i % 3) {
		case 1:
			slf3s_fake_fail_next(&ctx->fake,
					     SLF3S_FAKE_CMD_START_WATER,
					     -ENXIO);
			break;
		case 2:
			slf3s_fake_fail_next(&ctx->fake, SLF3S_FAKE_CMD_STOP,
					     -ENXIO);
			break;
		default:
			break;
		}

		ret = slf3s_test_suspend(test, ctx);
		KUNIT_EXPECT_LE_MSG(test, ctx->reg.n_enable - ctx->reg.n_disable,
				    1, "supply enabled more than once, cycle %u",
				    i);
		if (ret)
			continue;

		slf3s_test_resume(test, ctx);
		KUNIT_EXPECT_LE_MSG(test, ctx->reg.n_enable - ctx->reg.n_disable,
				    1, "supply enabled more than once, cycle %u",
				    i);

		/* Put the sensor back into a measuring state for the next round. */
		if (slf3s_fake_state(&ctx->fake) != SLF3S_FAKE_MEASURING &&
		    !ctx->suspended)
			slf3s_test_resume_sensor(test, ctx);
	}
}

/* T6: unbinding after a failed resume must not leave the supply on. */
static void slf3s_test_unbind_after_failed_resume(struct kunit *test)
{
	struct slf3s_test_ctx *ctx = test->priv;

	KUNIT_EXPECT_EQ(test, slf3s_test_suspend(test, ctx), 0);

	slf3s_fake_fail_next(&ctx->fake, SLF3S_FAKE_CMD_START_WATER, -ENXIO);
	KUNIT_EXPECT_LT(test, slf3s_test_resume(test, ctx), 0);

	kunit_release_action(test, slf3s_test_unregister_client, ctx->client);

	KUNIT_EXPECT_REG_BALANCED(test, ctx);
}

/* T7: a probe that fails must not leave the supply enabled. */
static void slf3s_test_probe_failure(struct kunit *test)
{
	struct slf3s_test_ctx *ctx = test->priv;
	struct i2c_board_info info = { };
	struct i2c_client *client;

	/* Start from a clean slate: drop the client the fixture bound. */
	kunit_release_action(test, slf3s_test_unregister_client, ctx->client);
	ctx->client = NULL;
	KUNIT_ASSERT_FALSE(test, ctx->reg.enabled);

	slf3s_fake_fail_next(&ctx->fake, SLF3S_FAKE_CMD_PREP_PID, -ENXIO);

	strscpy(info.type, "slf3s-0600f", sizeof(info.type));
	info.addr = SLF3S_TEST_ADDR;

	client = i2c_new_client_device(&ctx->fake.adap, &info);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, client);

	KUNIT_EXPECT_NULL_MSG(test, client->dev.driver,
			      "probe was expected to fail");
	KUNIT_EXPECT_REG_BALANCED(test, ctx);

	i2c_unregister_device(client);
}

/* T2: a resume whose start command fails must switch the supply back off. */
static void slf3s_test_resume_start_failure(struct kunit *test)
{
	struct slf3s_test_ctx *ctx = test->priv;

	KUNIT_ASSERT_EQ(test, slf3s_test_suspend(test, ctx), 0);

	slf3s_fake_fail_next(&ctx->fake, SLF3S_FAKE_CMD_START_WATER, -ENXIO);
	KUNIT_EXPECT_LT(test, slf3s_test_resume(test, ctx), 0);

	KUNIT_EXPECT_FALSE_MSG(test, ctx->reg.enabled,
			       "supply left on after a failed resume");
	KUNIT_EXPECT_REG_BALANCED(test, ctx);
}

/* T3: a stop command that fails is no reason to keep the supply on. */
static void slf3s_test_suspend_stop_failure(struct kunit *test)
{
	struct slf3s_test_ctx *ctx = test->priv;

	slf3s_fake_fail_next(&ctx->fake, SLF3S_FAKE_CMD_STOP, -ENXIO);
	slf3s_test_suspend(test, ctx);

	KUNIT_EXPECT_FALSE_MSG(test, ctx->reg.enabled,
			       "supply left on after a failed stop command");
	KUNIT_EXPECT_REG_BALANCED(test, ctx);
}

/*
 * T4: if the supply itself refuses to come up, the next suspend must not
 * call regulator_disable() on a supply that was never enabled.  Doing so
 * trips "unbalanced disables" in the regulator core and aborts the whole
 * system suspend with -EIO.
 */
static void slf3s_test_resume_enable_failure(struct kunit *test)
{
	struct slf3s_test_ctx *ctx = test->priv;
	unsigned int n_disable;

	KUNIT_ASSERT_EQ(test, slf3s_test_suspend(test, ctx), 0);
	n_disable = ctx->reg.n_disable;

	slf3s_test_reg_fail_enable(&ctx->reg, -EIO);
	KUNIT_EXPECT_EQ(test, slf3s_test_resume(test, ctx), -EIO);
	KUNIT_EXPECT_FALSE(test, ctx->reg.enabled);

	/* The sensor answers even though the driver's supply never came up. */
	slf3s_test_resume_sensor(test, ctx);

	KUNIT_EXPECT_EQ_MSG(test, slf3s_test_suspend(test, ctx), 0,
			    "suspend failed after a resume that could not enable the supply");
	KUNIT_EXPECT_EQ_MSG(test, ctx->reg.n_disable, n_disable,
			    "driver disabled a supply it never enabled");
	KUNIT_EXPECT_REG_BALANCED(test, ctx);
}

/*
 * T8: the devm cleanup must not disable a supply that is already off.
 *
 * An unbalanced regulator_disable() WARNs and returns before the fake's
 * disable op runs, so n_disable cannot tell buggy from correct here.
 * Count the WARN() instead.
 */
static void slf3s_test_unbind_after_enable_failure(struct kunit *test)
{
	struct slf3s_test_ctx *ctx = test->priv;
	unsigned int n_disable;

	KUNIT_ASSERT_EQ(test, slf3s_test_suspend(test, ctx), 0);
	n_disable = ctx->reg.n_disable;

	slf3s_test_reg_fail_enable(&ctx->reg, -EIO);
	KUNIT_EXPECT_EQ(test, slf3s_test_resume(test, ctx), -EIO);

	kunit_warning_suppress(test) {
		kunit_release_action(test, slf3s_test_unregister_client,
				     ctx->client);
		KUNIT_EXPECT_EQ_MSG(test, KUNIT_SUPPRESSED_WARNING_COUNT(), 0,
				    "unbind triggered \"unbalanced disables\" for a supply that was already off");
	}

	KUNIT_EXPECT_EQ_MSG(test, ctx->reg.n_disable, n_disable,
			    "unbind disabled a supply that was already off");
	KUNIT_EXPECT_REG_BALANCED(test, ctx);
}

static struct kunit_case slf3s_test_cases[] = {
	KUNIT_CASE(slf3s_test_cycle_balanced),
	KUNIT_CASE(slf3s_test_cycles_never_leak),
	KUNIT_CASE(slf3s_test_unbind_after_failed_resume),
	KUNIT_CASE(slf3s_test_probe_failure),
	KUNIT_CASE(slf3s_test_resume_start_failure),
	KUNIT_CASE(slf3s_test_suspend_stop_failure),
	KUNIT_CASE(slf3s_test_resume_enable_failure),
	KUNIT_CASE(slf3s_test_unbind_after_enable_failure),
	{ }
};

static struct kunit_suite slf3s_test_suite = {
	.name = "slf3s-pm",
	.init = slf3s_test_init,
	.test_cases = slf3s_test_cases,
};

kunit_test_suite(slf3s_test_suite);

MODULE_DESCRIPTION("KUnit tests for the Sensirion SLF3S driver");
MODULE_LICENSE("GPL");
