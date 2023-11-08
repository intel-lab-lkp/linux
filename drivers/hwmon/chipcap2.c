// SPDX-License-Identifier: GPL-2.0+
/*
 * cc2.c - Support for the Amphenol ChipCap 2 relative humidity, temperature sensor
 *
 * Part numbers supported:
 * - CC2DXX: digital, non-sleep mode (CC2D23, CC2D25, CC2D33, CC2D35)
 * - CC2DXXS: digital, sleep mode (CC2D23S, CC2D25S, CC2D33S, CC2D35S)
 *
 * Author: Javier Carrasco <javier.carrasco.cruz@gmail.com>
 *
 * Datasheet and application notes:
 * https://www.amphenol-sensors.com/en/telaire/humidity/527-humidity-sensors/3095-chipcap-2
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/hwmon.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>

#define CC2_DEV_NAME "chipcap2"

#define CC2_START_CM			0xA0
#define CC2_START_NOM			0x80
#define CC2_R_ALARM_HIGH_ON		0x18
#define CC2_R_ALARM_HIGH_OFF		0x19
#define CC2_R_ALARM_LOW_ON		0x1A
#define CC2_R_ALARM_LOW_OFF		0x1B
#define CC2_RW_OFFSET			0x40
#define CC2_W_ALARM_HIGH_ON		(CC2_R_ALARM_HIGH_ON + CC2_RW_OFFSET)
#define CC2_W_ALARM_HIGH_OFF		(CC2_R_ALARM_HIGH_OFF + CC2_RW_OFFSET)
#define CC2_W_ALARM_LOW_ON		(CC2_R_ALARM_LOW_ON + CC2_RW_OFFSET)
#define CC2_W_ALARM_LOW_OFF		(CC2_R_ALARM_LOW_OFF + CC2_RW_OFFSET)

#define CC2_STATUS_FIELD		GENMASK(7, 6)
#define CC2_STATUS_VALID_DATA		0x00
#define CC2_STATUS_STALE_DATA		0x01
#define CC2_STATUS_CMD_MODE		0x02

#define CC2_RESPONSE_FIELD		GENMASK(1, 0)
#define CC2_RESPONSE_BUSY		0x00
#define CC2_RESPONSE_ACK		0x01
#define CC2_RESPONSE_NACK		0x02

#define CC2_ERR_CORR_EEPROM		BIT(2)
#define CC2_ERR_UNCORR_EEPROM		BIT(3)
#define CC2_ERR_RAM_PARITY		BIT(4)
#define CC2_ERR_CONFIG_LOAD		BIT(5)

#define CC2_EEPROM_SIZE			10
#define CC2_EEPROM_DATA_LEN		3
#define CC2_MEAS_DATA_LEN		4

#define CC2_RH_DATA_FIELD		GENMASK(13, 0)

#define CC2_POWER_CYCLE_MS		150
#define CC2_RESP_SLEEP_MS		45
#define CC2_RESP_START_CM_US		100
#define CC2_RESP_EEPROM_R_US		100
#define CC2_RESP_EEPROM_W_MS		12
#define CC2_STARTUP_TIME_US		1250
#define CC2_UPDATE_RATE_MS		165

#define CC2_RH_MIN			0
#define CC2_RH_MAX			(100 * 1000U)

struct cc2_data;

enum cc2_ids {cc2dxx, cc2dxxs};

struct cc2_config {
	int (*measurement)(struct cc2_data *data,
			   enum hwmon_sensor_types type,
			   long *val);
};

struct cc2_alarm_info {
	long min;
	long min_hyst;
	long max;
	long max_hyst;
	bool rh_low;
	bool rh_high;
	bool rh_low_visible;
	bool rh_high_visible;
};

struct cc2_data {
	const struct cc2_config *config;
	struct cc2_alarm_info alarm;
	struct mutex alarm_lock; /* alarm status lock */
	struct completion complete;
	struct device *hwmon;
	struct i2c_client *client;
	struct mutex i2c_lock; /* i2c communication lock */
	struct regulator *regulator;
	const char *name;
	int irq_ready;
	int irq_low;
	int irq_high;
	bool ignore_irqs;
};

enum cc2_chan_addr {
	CC2_CHAN_TEMP = 0,
	CC2_CHAN_HUMIDITY,
};

static long cc2_rh_convert(u16 data)
{
	unsigned long tmp = (data & CC2_RH_DATA_FIELD) * CC2_RH_MAX;

	return tmp / ((1 << 14) - 1);
}

static u16 cc2_rh_to_reg(long data)
{
	return data * ((1 << 14) - 1) / CC2_RH_MAX;
}

static long cc2_rh_in_device_resolution(long data)
{
	u16 reg_val;

	reg_val = cc2_rh_to_reg(data);

	return cc2_rh_convert(reg_val);
}

static long cc2_temp_convert(u16 data)
{
	unsigned long tmp = ((data >> 2) * 165 * 1000U) / ((1 << 14) - 1);

	return tmp - 40 * 1000U;
}

static int cc2_analyze_cmd_response(struct device *dev, u8 status)
{
	int resp;
	int ret;

	if (FIELD_GET(CC2_STATUS_FIELD, status) != CC2_STATUS_CMD_MODE) {
		dev_dbg(dev, "Command sent out of command window\n");
		return -ETIMEDOUT;
	}

	resp = FIELD_GET(CC2_RESPONSE_FIELD, status);
	switch (resp) {
	case CC2_RESPONSE_ACK:
		ret = 0;
		break;
	case CC2_RESPONSE_BUSY:
		ret = -EBUSY;
		break;
	case CC2_RESPONSE_NACK:
		if (resp & CC2_ERR_CORR_EEPROM)
			dev_dbg(dev, "Command failed: corrected EEPROM\n");
		if (resp & CC2_ERR_UNCORR_EEPROM)
			dev_dbg(dev, "Command failed: uncorrected EEPROM\n");
		if (resp & CC2_ERR_RAM_PARITY)
			dev_dbg(dev, "Command failed: RAM parity\n");
		if (resp & CC2_ERR_RAM_PARITY)
			dev_dbg(dev, "Command failed: configuration error\n");
		ret = -ENODATA;
		break;
	default:
		dev_dbg(dev, "Unknown command reply\n");
		ret = -ENODATA;
		break;
	}

	return ret;
}

static int cc2_read_command_status(struct i2c_client *client)
{
	u8 status;
	int ret;

	ret = i2c_master_recv(client, &status, 1);
	if (ret != 1) {
		ret = ret < 0 ? ret : -EIO;
		return ret;
	}

	return cc2_analyze_cmd_response(&client->dev, status);
}

static int cc2_clear_data_ready(struct i2c_client *client)
{
	u8 data[CC2_MEAS_DATA_LEN];
	u8 status;
	int ret;

	ret = i2c_master_recv(client, data, CC2_MEAS_DATA_LEN);
	if (ret != CC2_MEAS_DATA_LEN) {
		ret = ret < 0 ? ret : -EIO;
		return ret;
	}
	status = FIELD_GET(CC2_STATUS_FIELD, data[0]);
	if (status == CC2_STATUS_STALE_DATA)
		return -EBUSY;

	if (status != CC2_STATUS_VALID_DATA)
		return -EIO;

	return 0;
}

/*
 * The command mode is only accessible after sending the START_CM command in the
 * first 10 ms after power-up.
 */
static int cc2_command_mode_start(struct cc2_data *data)
{
	unsigned long timeout;
	int ret;

	ret = i2c_smbus_write_word_data(data->client, CC2_START_CM, 0);
	if (ret < 0)
		return ret;

	if (data->irq_ready > 0) {
		timeout = usecs_to_jiffies(2 * CC2_RESP_START_CM_US);
		ret = wait_for_completion_timeout(&data->complete, timeout);
		if (!ret)
			return -ETIMEDOUT;
	} else {
		usleep_range(CC2_RESP_START_CM_US, 2 * CC2_RESP_START_CM_US);
	}

	return cc2_read_command_status(data->client);
}

/* Sending a Start_NOM command finishes the command mode immediately with no
 * reply and the device enters normal operation mode
 */
static int cc2_command_mode_finish(struct cc2_data *data)
{
	int ret;

	ret = i2c_smbus_write_word_data(data->client, CC2_START_NOM, 0);
	if (ret < 0)
		return ret;

	return 0;
}

/* EEPROM registers can only be read in command mode */
static int cc2_read_eeprom_reg(struct cc2_data *data, u8 cmd, u16 *val)
{
	u8 buf[CC2_EEPROM_DATA_LEN];
	unsigned long timeout;
	int ret;

	ret = i2c_smbus_write_word_data(data->client, cmd, 0);
	if (ret < 0)
		return ret;

	if (data->irq_ready > 0) {
		timeout = usecs_to_jiffies(2 * CC2_RESP_EEPROM_R_US);
		ret = wait_for_completion_timeout(&data->complete, timeout);
		if (!ret)
			return -ETIMEDOUT;
	} else {
		usleep_range(CC2_RESP_EEPROM_R_US, CC2_RESP_EEPROM_R_US + 10);
	}
	ret = i2c_master_recv(data->client, buf, CC2_EEPROM_DATA_LEN);
	if (ret != CC2_EEPROM_DATA_LEN) {
		ret = ret < 0 ? ret : -EIO;
		return ret;
	}

	*val = be16_to_cpup((__be16 *)&buf[1]);

	return cc2_read_command_status(data->client);
}

static int cc2_write_eeprom_reg(struct cc2_data *data, u8 cmd, u16 val)
{
	unsigned long timeout;
	int ret;

	cpu_to_be16s(&val);
	ret = i2c_smbus_write_word_data(data->client, cmd, val);
	if (ret < 0)
		return ret;

	if (data->irq_ready > 0) {
		timeout = msecs_to_jiffies(2 * CC2_RESP_EEPROM_W_MS);
		ret = wait_for_completion_timeout(&data->complete, timeout);
		if (!ret)
			return -ETIMEDOUT;
	} else {
		msleep(CC2_RESP_EEPROM_W_MS);
	}

	return cc2_read_command_status(data->client);
}

/* Command mode is only accessible in the first 10 ms after power-up, but the
 * device does not provide any kind of reset. In order to access the command
 * mode during normal operation, a power cycle must be triggered.
 */
static int cc2_start_command_window(struct cc2_data *data)
{
	int ret;

	/* ignore alarms triggered by voltage toggling during the power cycle */
	mutex_lock(&data->alarm_lock);
	data->ignore_irqs = true;
	mutex_unlock(&data->alarm_lock);

	/* clear any pending completion */
	try_wait_for_completion(&data->complete);

	if (regulator_is_enabled(data->regulator)) {
		ret = regulator_disable(data->regulator);
		if (ret < 0)
			return ret;

		msleep(CC2_POWER_CYCLE_MS); /* ensure a clean power cycle */
	}

	ret = regulator_enable(data->regulator);
	if (ret < 0)
		return ret;

	/*
	 * TODO: the startup-delay-us property of the regulator might be
	 * added to the delay (if provided).
	 * Currently there is no interface to read its value apart from
	 * a direct access to regulator->rdev->constraints->enable_time,
	 * which is discouraged like any direct access to the regulator_dev
	 * structure. This would be relevant in cases where the startup delay
	 * is in the range of milliseconds.
	 */
	usleep_range(CC2_STARTUP_TIME_US, CC2_STARTUP_TIME_US + 125);

	mutex_lock(&data->alarm_lock);
	data->ignore_irqs = false;
	mutex_unlock(&data->alarm_lock);

	return 0;
}

/*
 * An EEPROM update requires the following steps:
 * 1. Power cycle to accept the START_CM command.
 * 2. STARTM_CM command to enter command mode.
 * 3. Command(s) to update register(s).
 * 4. START_NOM command to exit command mode.
 * After that, the device switches to normal mode and a measurement is
 * triggered automatically. In order to obtain a recent measurement on demand,
 * the automatic measurement is dropped.
 */
static int cc2_update_eeprom(struct cc2_data *data, u8 *cmd, u16 *val, int len)
{
	int i, ret;

	if (!cmd || !val || len > CC2_EEPROM_SIZE)
		return -EINVAL;

	ret = cc2_start_command_window(data);
	if (ret < 0)
		return ret;

	mutex_lock(&data->i2c_lock);
	ret = cc2_command_mode_start(data);
	if (ret < 0)
		goto unlock;

	for (i = 0; i < len; i++) {
		ret = cc2_write_eeprom_reg(data, *cmd, *val);
		if (ret < 0) {
			dev_dbg(&data->client->dev,
				"Failed to write register %#02x\n", *cmd);
			goto cmd_finish;
		}
		cmd++;
		val++;
	}

cmd_finish:
	ret = cc2_command_mode_finish(data);
unlock:
	mutex_unlock(&data->i2c_lock);

	return ret;
}

/*
 * EEPROM access is a highly time-consuming task that involves a power cycle.
 * The alarm-related registers are read once and their values are stored and
 * updated without further EEPROM accesses.
 * Probe deferring is used to minimize the risk of missing the command window
 * if the task is waiting to be executed for too long.
 */
static int cc2_read_eeprom_alarm_registers(struct cc2_data *data)
{
	u16 val;
	int ret;

	mutex_lock(&data->i2c_lock);
	ret = cc2_command_mode_start(data);
	if (ret < 0) {
		ret = (ret == -ETIMEDOUT) ? -EPROBE_DEFER : ret;
		goto unlock;
	}

	ret = cc2_read_eeprom_reg(data, CC2_R_ALARM_LOW_ON, &val);
	if (ret < 0)
		goto unlock;

	data->alarm.min = cc2_rh_convert(val);

	ret = cc2_read_eeprom_reg(data, CC2_R_ALARM_LOW_OFF, &val);
	if (ret < 0)
		goto unlock;

	data->alarm.min_hyst = cc2_rh_convert(val) - data->alarm.min;

	ret = cc2_read_eeprom_reg(data, CC2_R_ALARM_HIGH_ON, &val);
	if (ret < 0)
		goto unlock;

	data->alarm.max = cc2_rh_convert(val);

	ret = cc2_read_eeprom_reg(data, CC2_R_ALARM_HIGH_OFF, &val);
	if (ret < 0)
		goto unlock;

	data->alarm.max_hyst = data->alarm.max - cc2_rh_convert(val);

	ret = cc2_command_mode_finish(data);

unlock:
	mutex_unlock(&data->i2c_lock);

	return ret;
}

static int cc2_retrive_alarm_config(struct cc2_data *data)
{
	int ret;

	ret = cc2_start_command_window(data);
	if (ret)
		return ret;

	return cc2_read_eeprom_alarm_registers(data);
}

/*
 * A measurement request must be sent in sleep mode to wake up the device and
 * start measuring. This request only contains the slave address and the write
 * bit.
 */
static int cc2_measurement_request(struct i2c_client *client)
{
	int ret;

	ret = i2c_master_send(client, NULL, 0);
	if (ret < 0)
		return ret;

	return 0;
};

static int cc2_data_fetch(struct i2c_client *client,
			  enum hwmon_sensor_types type, long *val)
{
	u8 data[CC2_MEAS_DATA_LEN];
	u8 status;
	int ret;

	ret = i2c_master_recv(client, data, CC2_MEAS_DATA_LEN);
	if (ret != CC2_MEAS_DATA_LEN) {
		ret = ret < 0 ? ret : -EIO;
		return ret;
	}
	status = FIELD_GET(CC2_STATUS_FIELD, data[0]);
	if (status == CC2_STATUS_STALE_DATA)
		return -EBUSY;

	if (status != CC2_STATUS_VALID_DATA)
		return -EIO;

	switch (type) {
	case hwmon_humidity:
		*val = cc2_rh_convert(be16_to_cpup((__be16 *)&data[0]));
		break;
	case hwmon_temp:
		*val = cc2_temp_convert(be16_to_cpup((__be16 *)&data[2]));
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

/*
 * non sleep-mode devies (cc2dxx variants) make measurements periodically and
 * indicate valid data with the ready signal, whose level stays high until data
 * is fetched even if new measurements are made.
 */
static int cc2dxx_meas(struct cc2_data *data, enum hwmon_sensor_types type,
		       long *val)
{
	unsigned long timeout;
	int ret;

	if (data->irq_ready > 0) {
		if (try_wait_for_completion(&data->complete))
			goto data_fetch;

		timeout = msecs_to_jiffies(CC2_UPDATE_RATE_MS);
		ret = wait_for_completion_timeout(&data->complete, timeout);
		if (!ret)
			return -ETIMEDOUT;
	} else {
		/* check if data is ready and retry for stale data */
		ret = cc2_data_fetch(data->client, type, val);
		if (ret != -EBUSY)
			return ret;

		msleep(CC2_UPDATE_RATE_MS);
	}

data_fetch:
	return cc2_data_fetch(data->client, type, val);
}

/*
 * sleep-mode devies (cc2dxxs variants) require a measurement request command to
 * wake up the device and trigger a measurement. Then a data fetch command is
 * sent to retrieve the measured values.
 */
static int cc2dxxs_meas(struct cc2_data *data, enum hwmon_sensor_types type,
			long *val)
{
	unsigned long timeout;
	int ret;

	if (data->irq_ready > 0) {
		/*
		 * the device carries out a first measurement after entering
		 * measurement mode automatically. To avoid outdated values,
		 * a new request is made
		 */
		if (try_wait_for_completion(&data->complete)) {
			ret = cc2_clear_data_ready(data->client);
			if (ret < 0)
				return ret;
		}

		ret = cc2_measurement_request(data->client);
		if (ret < 0)
			return ret;

		timeout = msecs_to_jiffies(2 * CC2_RESP_SLEEP_MS);
		ret = wait_for_completion_timeout(&data->complete, timeout);
		if (!ret)
			return -ETIMEDOUT;
	} else {
		ret = cc2_measurement_request(data->client);
		if (ret < 0)
			return ret;

		msleep(CC2_RESP_SLEEP_MS);
	}

	return cc2_data_fetch(data->client, type, val);
}

static umode_t cc2_is_visible(const void *data, enum hwmon_sensor_types type,
			      u32 attr, int channel)
{
	const struct cc2_data *cc2 = data;

	switch (type) {
	case hwmon_humidity:
		switch (attr) {
		case hwmon_humidity_input:
			return 0444;
		case hwmon_humidity_min_alarm:
			return cc2->alarm.rh_low_visible ? 0444 : 0;
		case hwmon_humidity_max_alarm:
			return cc2->alarm.rh_high_visible ? 0444 : 0;
		case hwmon_humidity_min:
		case hwmon_humidity_min_hyst:
			return cc2->alarm.rh_low_visible ? 0644 : 0;
		case hwmon_humidity_max:
		case hwmon_humidity_max_hyst:
			return cc2->alarm.rh_high_visible ? 0644 : 0;
		default:
			return 0;
		}
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_input:
			return 0444;
		default:
			return 0;
		}
	default:
		break;
	}

	return 0;
}

static irqreturn_t cc2_ready_interrupt(int irq, void *data)
{
	struct cc2_data *cc2 = data;

	mutex_lock(&cc2->alarm_lock);
	if (!cc2->ignore_irqs)
		complete(&cc2->complete);
	mutex_unlock(&cc2->alarm_lock);

	return IRQ_HANDLED;
}

static irqreturn_t cc2_low_interrupt(int irq, void *data)
{
	struct cc2_data *cc2 = data;

	mutex_lock(&cc2->alarm_lock);
	if (!cc2->ignore_irqs) {
		hwmon_notify_event(cc2->hwmon, hwmon_humidity,
				   hwmon_humidity_min_alarm, CC2_CHAN_HUMIDITY);
		cc2->alarm.rh_low = true;
	}
	mutex_unlock(&cc2->alarm_lock);

	return IRQ_HANDLED;
}

static irqreturn_t cc2_high_interrupt(int irq, void *data)
{
	struct cc2_data *cc2 = data;

	mutex_lock(&cc2->alarm_lock);
	if (!cc2->ignore_irqs) {
		hwmon_notify_event(cc2->hwmon, hwmon_humidity,
				   hwmon_humidity_max_alarm, CC2_CHAN_HUMIDITY);
		cc2->alarm.rh_high = true;
	}
	mutex_unlock(&cc2->alarm_lock);

	return IRQ_HANDLED;
}

static int cc2_humidity_min_alarm_status(struct cc2_data *data, long *val)
{
	long meas;
	int ret;

	mutex_lock(&data->i2c_lock);
	ret = data->config->measurement(data, hwmon_humidity, &meas);
	mutex_unlock(&data->i2c_lock);
	if (ret < 0)
		return ret;

	mutex_lock(&data->alarm_lock);
	if (data->alarm.rh_low) {
		*val = (meas < data->alarm.min + data->alarm.min_hyst) ? 1 : 0;
		data->alarm.rh_low = *val;
	} else {
		*val = 0;
	}
	mutex_unlock(&data->alarm_lock);

	return 0;
}

static int cc2_humidity_max_alarm_status(struct cc2_data *data, long *val)
{
	long meas;
	int ret;

	mutex_lock(&data->i2c_lock);
	ret = data->config->measurement(data, hwmon_humidity, &meas);
	mutex_unlock(&data->i2c_lock);
	if (ret < 0)
		return ret;

	mutex_lock(&data->alarm_lock);
	if (data->alarm.rh_high) {
		*val = (meas > data->alarm.max - data->alarm.max_hyst) ? 1 : 0;
		data->alarm.rh_high = *val;
	} else {
		*val = 0;
	}
	mutex_unlock(&data->alarm_lock);

	return 0;
}

static int cc2_read(struct device *dev, enum hwmon_sensor_types type, u32 attr,
		    int channel, long *val)
{
	struct cc2_data *data = dev_get_drvdata(dev);
	int ret;

	switch (type) {
	case hwmon_temp:
		mutex_lock(&data->i2c_lock);
		ret = data->config->measurement(data, type, val);
		mutex_unlock(&data->i2c_lock);
		break;
	case hwmon_humidity:
		switch (attr) {
		case hwmon_humidity_input:
			mutex_lock(&data->i2c_lock);
			ret = data->config->measurement(data, type, val);
			mutex_unlock(&data->i2c_lock);
			break;
		case hwmon_humidity_min:
			*val = data->alarm.min;
			break;
		case hwmon_humidity_min_hyst:
			*val = data->alarm.min_hyst;
			break;
		case hwmon_humidity_max:
			*val = data->alarm.max;
			break;
		case hwmon_humidity_max_hyst:
			*val = data->alarm.max_hyst;
			break;
		case hwmon_humidity_min_alarm:
			ret = cc2_humidity_min_alarm_status(data, val);
			if (ret < 0)
				return -EIO;
			break;
		case hwmon_humidity_max_alarm:
			ret = cc2_humidity_max_alarm_status(data, val);
			if (ret < 0)
				return -EIO;
			break;
		default:
			return -EOPNOTSUPP;
		}
		break;
	default:
		return -EOPNOTSUPP;
	}

	return ret;
}

static int cc2_write(struct device *dev, enum hwmon_sensor_types type, u32 attr,
		     int channel, long val)
{
	struct cc2_data *data = dev_get_drvdata(dev);
	u8 cmd[CC2_EEPROM_SIZE];
	u16 arg[CC2_EEPROM_SIZE];
	int ret;

	if (type != hwmon_humidity)
		return -EOPNOTSUPP;

	switch (attr) {
	case hwmon_humidity_min:
		if (val < CC2_RH_MIN || val > CC2_RH_MAX)
			return -EINVAL;

		cmd[0] = CC2_W_ALARM_LOW_ON;
		arg[0] = cc2_rh_to_reg(val);
		/* alarm low off must be updated to keep the hysteresis */
		cmd[1] = CC2_W_ALARM_LOW_OFF;
		arg[1] = cc2_rh_to_reg(val + data->alarm.min_hyst);
		ret = cc2_update_eeprom(data, cmd, arg, 2);
		if (ret < 0)
			return ret;

		data->alarm.min = cc2_rh_in_device_resolution(val);
		break;
	case hwmon_humidity_min_hyst:
		if (val < CC2_RH_MIN || val + data->alarm.min > CC2_RH_MAX)
			return -EINVAL;

		cmd[0] = CC2_W_ALARM_LOW_OFF;
		arg[0] = cc2_rh_to_reg(data->alarm.min + val);
		ret = cc2_update_eeprom(data, cmd, arg, 1);
		if (ret < 0)
			return ret;

		data->alarm.min_hyst = cc2_rh_in_device_resolution(val);
		break;
	case hwmon_humidity_max:
		if (val < CC2_RH_MIN || val > CC2_RH_MAX)
			return -EINVAL;

		cmd[0] = CC2_W_ALARM_HIGH_ON;
		arg[0] = cc2_rh_to_reg(val);
		/* alarm high off must be updated to keep the hysteresis */
		cmd[1] = CC2_W_ALARM_HIGH_OFF;
		arg[1] = cc2_rh_to_reg(val - data->alarm.max_hyst);
		ret = cc2_update_eeprom(data, cmd, arg, 2);
		if (ret < 0)
			return ret;

		data->alarm.max = cc2_rh_in_device_resolution(val);
		break;
	case hwmon_humidity_max_hyst:
		if (val < CC2_RH_MIN || data->alarm.max - val < CC2_RH_MIN)
			return -EINVAL;

		cmd[0] = CC2_W_ALARM_HIGH_OFF;
		arg[0] = cc2_rh_to_reg(data->alarm.max - val);
		ret = cc2_update_eeprom(data, cmd, arg, 1);
		if (ret < 0)
			return ret;

		data->alarm.max_hyst = cc2_rh_in_device_resolution(val);
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int cc2_disable(struct cc2_data *data)
{
	int ret = 0;

	if (data->regulator && regulator_is_enabled(data->regulator))
		ret = regulator_disable(data->regulator);

	return ret;
}

static int cc2_request_ready_irq(struct cc2_data *data, struct device *dev)
{
	int ret = 0;

	data->irq_ready = fwnode_irq_get_byname(dev_fwnode(dev), "ready");
	if (data->irq_ready > 0) {
		init_completion(&data->complete);
		ret = devm_request_threaded_irq(dev, data->irq_ready, NULL,
						cc2_ready_interrupt,
						IRQF_ONESHOT |
						IRQF_TRIGGER_RISING,
						dev_name(dev), data);
	}

	return ret;
}

static int cc2_request_alarm_irqs(struct cc2_data *data, struct device *dev)
{
	int ret;

	data->irq_low = fwnode_irq_get_byname(dev_fwnode(dev), "low");
	if (data->irq_low > 0) {
		ret = devm_request_threaded_irq(dev, data->irq_low, NULL,
						cc2_low_interrupt,
						IRQF_ONESHOT |
						IRQF_TRIGGER_RISING,
						dev_name(dev), data);
		if (!ret)
			data->alarm.rh_low_visible = true;
	}

	data->irq_high = fwnode_irq_get_byname(dev_fwnode(dev), "high");
	if (data->irq_high > 0) {
		ret = devm_request_threaded_irq(dev, data->irq_high, NULL,
						cc2_high_interrupt,
						IRQF_ONESHOT |
						IRQF_TRIGGER_RISING,
						dev_name(dev), data);
		if (!ret)
			data->alarm.rh_high_visible = true;
	}

	return ret;
}

static const struct hwmon_channel_info *cc2_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT),
	HWMON_CHANNEL_INFO(humidity, HWMON_H_INPUT | HWMON_H_MIN | HWMON_H_MAX |
			   HWMON_H_MIN_HYST | HWMON_H_MAX_HYST |
			   HWMON_H_MIN_ALARM | HWMON_H_MAX_ALARM),
	NULL
};

static const struct hwmon_ops cc2_hwmon_ops = {
	.is_visible = cc2_is_visible,
	.read = cc2_read,
	.write = cc2_write,
};

static const struct hwmon_chip_info cc2_chip_info = {
	.ops = &cc2_hwmon_ops,
	.info = cc2_info,
};

static const struct cc2_config cc2_config[] = {
	[cc2dxx] = {
		.measurement = cc2dxx_meas,
	},
	[cc2dxxs] = {
		.measurement = cc2dxxs_meas,
	},
};

static const struct i2c_device_id cc2_id[] = {
	{ "cc2dxx", cc2dxx },
	{ "cc2dxxs", cc2dxxs },
	{ }
};
MODULE_DEVICE_TABLE(i2c, cc2_id);

static const struct of_device_id cc2_of_match[] = {
	{
		.compatible = "amphenol,cc2dxx",
		.data = (void *)cc2dxx,
	},
	{
		.compatible = "amphenol,cc2dxxs",
		.data = (void *)cc2dxxs,
	},
	{ },
};
MODULE_DEVICE_TABLE(of, cc2_of_match);

static int cc2_probe(struct i2c_client *client)
{
	struct cc2_data *data;
	struct device *dev = &client->dev;
	enum cc2_ids chip;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	i2c_set_clientdata(client, data);

	mutex_init(&data->i2c_lock);
	mutex_init(&data->alarm_lock);

	data->client = client;

	if (client->dev.of_node)
		chip = (uintptr_t)of_device_get_match_data(&client->dev);
	else
		chip = i2c_match_id(cc2_id, client)->driver_data;

	data->config = &cc2_config[chip];

	ret = cc2_request_ready_irq(data, dev);
	if (ret)
		return ret;

	data->regulator = devm_regulator_get_optional(dev, "vdd");
	if (!IS_ERR(data->regulator)) {
		ret = cc2_retrive_alarm_config(data);
		if (ret)
			goto cleanup;
	} else {
		/* No access to EEPROM without regulator: no alarm control */
		goto dev_register;
	}

	ret = cc2_request_alarm_irqs(data, dev);
	if (ret)
		goto cleanup;

dev_register:
	data->hwmon = devm_hwmon_device_register_with_info(dev, client->name,
							   data, &cc2_chip_info,
							   NULL);
	if (IS_ERR(data->hwmon)) {
		ret = PTR_ERR(data->hwmon);
		goto cleanup;
	}

	return 0;

cleanup:
	if (cc2_disable(data))
		dev_dbg(dev, "Failed to disable device");

	return dev_err_probe(dev, ret,
			     "Unable to register hwmon device\n");
}

static void cc2_remove(struct i2c_client *client)
{
	struct cc2_data *data = i2c_get_clientdata(client);
	int ret = cc2_disable(data);

	if (ret)
		dev_dbg(&client->dev, "Failed to disable device");
}

static struct i2c_driver cc2_driver = {
	.driver = {
		.name	= CC2_DEV_NAME,
		.of_match_table = cc2_of_match,
	},
	.probe		= cc2_probe,
	.remove		= cc2_remove,
};
module_i2c_driver(cc2_driver);

MODULE_AUTHOR("Javier Carrasco <javier.carrasco.cruz@gamil.com>");
MODULE_DESCRIPTION("Amphenol ChipCap 2 humidity and temperature sensor driver");
MODULE_LICENSE("GPL");
