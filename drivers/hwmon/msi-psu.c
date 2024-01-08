// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Driver for MSI power supplies with USB-HID interfaces
 * Heavily derived from the corsair-psu and corsair-cpro drivers,
 * but different enough to be incompatible
 *
 * Copyright (C) 2023 Jack Doan <me@jackdoan.com>
 */

#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/errno.h>
#include <linux/hid.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/jiffies.h>
#include <linux/minmax.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/types.h>

/*
 * MSI protocol for PSUs
 *
 * message size = 64 bytes (request and response, little endian)
 * request:
 *	[command][register][param0][param1][paramX]...
 * reply:
 *	[echo of command][echo of register][data0][data1][dataX]...
 *
 *	- the driver uses raw events to be accessible from userspace (though this is not really
 *	  supported, it is just there for convenience, may be removed in the future)
 *	- a successful reply always starts with the address and command in the same order the
 *	  request used it
 *	- length of the reply data is specific to the command used.
 *	- The replies to most reads are pmbus linear11 encoded
 *	- the PSU expects a "handshake" init command before all other commands will work
 *	- send the handshake by sending 0x51 to the address 0xfa (packet will be [0xfa 0x51])
 *	- the driver supports debugfs for values not fitting into the hwmon class
 */

#define DRIVER_NAME "msi-psu"

#define REPLY_SIZE 40 /* max length of a reply to a single command */
#define SAMPLE_INTERVAL_MS 50
#define CMD_BUFFER_SIZE 64
#define CMD_TIMEOUT_MS 250
#define SECONDS_PER_HOUR (60 * 60)
#define SECONDS_PER_DAY (SECONDS_PER_HOUR * 24)

#define PSU_NAK 0xFE
#define PSU_REG_VEND_STR 0x10
#define PSU_REG_PROD_STR 0x11
#define PSU_REG_REVISION 0x12
#define PSU_REG_SERIAL 0x13 /* accepted, but returns all zeros */

#define PSU_REG_IN_VOLTS 0x20
#define PSU_REG_VOUT_12V_EACH_RAIL 0x22
#define PSU_REG_IOUT_12V_EACH_RAIL 0x23
#define PSU_REG_VOUT_12V 0x24
#define PSU_REG_IOUT_12V 0x25
#define PSU_REG_VOUT_5V 0x26
#define PSU_REG_IOUT_5V 0x27
#define PSU_REG_VOUT_3V 0x28
#define PSU_REG_IOUT_3V 0x29
#define PSU_REG_TOTAL_WATTS 0x2A
#define PSU_REG_EFFICIENCY 0x2B
#define PSU_REG_TEMP0 0x30
#define PSU_REG_FAN_RPM 0x40
#define PSU_REG_FAN_MODE 0x41
#define PSU_REG_FAN_DUTY_CYCLE 0x42

#define PSU_REG_MULTIRAIL 0xC0
#define PSU_REG_UNKNOWN_C4 0xC4
#define PSU_REG_UNKNOWN_C6 0xC6

#define PSU_REG_UPTIME 0xD0
#define PSU_REG_TOTAL_UPTIME 0xD1

#define PSU_REG_READ_EVERYTHING 0xE0
#define PSU_REG_SAVE_SETTINGS 0xF1

#define PSU_INIT 0xFA
#define PSU_READ 0x51
#define PSU_WRITE 0x50

#define PSU_MULTI_RAIL_ENABLED 2
#define PSU_MULTI_RAIL_DISABLED 1

#define COMBINED_12V 5

#define FAN_MODE_MANUAL 3
#define FAN_MODE_AUTO 1
#define FAN_SPEED_MAX 100
#define FAN_SPEED_MIN 0

struct volt_amp_pair {
	u16 volts;
	u16 amps;
} __packed;

struct msipsu_all {
	struct volt_amp_pair v12[6];
	struct volt_amp_pair v5;
	struct volt_amp_pair v3;
	u16 watts;
	u16 eff;
	u16 temp;
	u16 fan_speed;
} __packed;

struct msipsu_data {
	struct hid_device *hdev;
	struct device *hwmon_dev;
	struct dentry *debugfs;
	struct completion wait_completion;
	struct mutex lock; /* for locking access to cmd_buffer */
	u8 *cmd_buffer;
	char vendor[REPLY_SIZE];
	char product[REPLY_SIZE];
	struct debugfs_blob_wrapper vendor_blob;
	struct debugfs_blob_wrapper product_blob;
	struct msipsu_all data;
	ktime_t last_read_all;
};

/* some values are SMBus LINEAR11 data which need a conversion */
static int msipsu_lin11_to_int(u16 v16, int scale, bool is_signed)
{
	s32 exponent;
	s32 mantissa;
	int val;

	exponent = ((s16)v16) >> 11;
	if (is_signed)
		mantissa = ((s16)((v16 & 0x7ff) << 5)) >> 5;
	else
		mantissa = (u16)(v16 & 0x7ff); /* deliberately not sign-extending here */
	val = mantissa * scale;

	if (exponent >= 0)
		val <<= exponent;
	else
		val >>= -exponent;

	return val;
}

static int msipsu_percent_to_pwm(u16 val)
{
	return DIV_ROUND_CLOSEST(val * 255, 100);
}

static int msipsu_pwm_to_percent(long val)
{
	if (val < 0 || val > 255)
		return -EINVAL;

	return DIV_ROUND_CLOSEST(val * 100, 255);
}

static int msipsu_usb_cmd(struct msipsu_data *priv, const u8 *in, size_t in_len, void *data)
{
	unsigned long time;
	int ret;

	if (in_len > CMD_BUFFER_SIZE)
		return -ENOBUFS;

	memset(priv->cmd_buffer, 0, CMD_BUFFER_SIZE);
	memcpy(priv->cmd_buffer, in, in_len);

	reinit_completion(&priv->wait_completion);

	ret = hid_hw_output_report(priv->hdev, priv->cmd_buffer, CMD_BUFFER_SIZE);
	if (ret < 0)
		return ret;

	time = wait_for_completion_timeout(&priv->wait_completion,
					   msecs_to_jiffies(CMD_TIMEOUT_MS));
	if (!time)
		return -ETIMEDOUT;

	/*
	 * at the start of the reply is an echo of the send command/address in the same order it
	 * was sent. In the event a command/address is not supported, the PSU will reply with a NAK.
	 * For an invalid read, it will look like this:
	 *   Out -> [PSU_READ(0x51) A_FAKE_REGISTER(0x69)]
	 *   In  <- [PSU_READ(0x51) PSU_NAK(0xFE)]
	 * Writes differ slightly, this is the sequence for a write to a non-existent register:
	 *   Out -> [PSU_WRITE(0x50) A_FAKE_REGISTER(0x69)]
	 *   In  <- [PSU_WRITE(0x50) PSU_NAK(0xFE)]
	 * And this is a rejected write (of the correct length) to a real register:
	 *   Out -> [PSU_WRITE(0x50) PSU_REG_FAN_MODE(0x41) DATA(0x55 0x55 0x55)]
	 *   In  <- [PSU_WRITE(0x50) PSU_REG_FAN_MODE(0x41) PSU_NAK(0xFE)]
	 */
	if (in[0] != priv->cmd_buffer[0] || in[1] != priv->cmd_buffer[1])
		return -EOPNOTSUPP;
	else if (in[2] != PSU_NAK && priv->cmd_buffer[2] == PSU_NAK)
		return -EINVAL;

	if (data)
		memcpy(data, priv->cmd_buffer + 2, REPLY_SIZE);

	return 0;
}

static int msipsu_usb_cmd_locked(struct msipsu_data *priv, const u8 *in, size_t in_len, void *data)
{
	int ret;

	mutex_lock(&priv->lock);
	ret = msipsu_usb_cmd(priv, in, in_len, data);
	mutex_unlock(&priv->lock);
	return ret;
}

static int msipsu_fw_init(struct msipsu_data *priv)
{
	/*
	 * Vendor SW always begins with a message of [0xfa, 0x51]
	 * This init message is replied to with the model name of the PSU.
	 */
	const u8 init[] = {PSU_INIT, 0x51};
	const u8 read_vendor[] = {PSU_READ, PSU_REG_VEND_STR};
	int ret;

	ret = msipsu_usb_cmd(priv, init, sizeof(init), priv->product);
	if (ret < 0)
		return ret;
	ret = msipsu_usb_cmd(priv, read_vendor, sizeof(read_vendor), priv->vendor);
	if (ret < 0)
		return ret;
	priv->vendor_blob.data = priv->vendor;
	priv->product_blob.data = priv->product;
	priv->vendor_blob.size = strnlen(priv->vendor, sizeof(priv->vendor));
	priv->product_blob.size = strnlen(priv->product, sizeof(priv->product));
	return ret;
}

static int msipsu_save_settings(struct msipsu_data *priv)
{
	const u8 cmd[] = {PSU_WRITE, PSU_REG_SAVE_SETTINGS, 0x0};

	return msipsu_usb_cmd_locked(priv, cmd, sizeof(cmd), NULL);
}

static int msipsu_write_fan_settings(struct msipsu_data *priv, u8 mode, long pwm_duty)
{
	int ret;
	int percent_duty = 0;

	if (mode == FAN_MODE_AUTO) {
		/* vendor SW masks off the duty cycle for auto-mode, we should do the same */
		percent_duty = FAN_SPEED_MIN;
	} else if (pwm_duty != 0) {
		/*
		 * it is an error to write duty to non-zero while the device is not in
		 * manual-fan-mode, so switch mode automatically
		 */
		mode = FAN_MODE_MANUAL;
		percent_duty = msipsu_pwm_to_percent(pwm_duty);
		if (percent_duty < 0) /* return -EINVAL if we got a value we couldn't convert */
			return percent_duty;
	}
	const u8 cmd[] = {PSU_WRITE, PSU_REG_FAN_MODE, mode, 0, percent_duty};

	ret = msipsu_usb_cmd_locked(priv, cmd, sizeof(cmd), NULL);
	if (ret < 0)
		return ret;
	return msipsu_save_settings(priv);
}

static int msipsu_get_all_values(struct msipsu_data *priv)
{
	const u8 read_cmd[] = {PSU_READ, PSU_REG_READ_EVERYTHING};
	ktime_t t = ktime_get();
	s64 delta = ktime_ms_delta(t, priv->last_read_all);

	if (delta < SAMPLE_INTERVAL_MS)
		return 0;

	priv->last_read_all = t;
	return msipsu_usb_cmd_locked(priv, read_cmd, sizeof(read_cmd), &priv->data);
}

static int msipsu_get_value(struct msipsu_data *priv, u8 cmd, long *val)
{
	u8 data[REPLY_SIZE];
	const u8 read_cmd[] = {PSU_READ, cmd};
	int ret = msipsu_usb_cmd_locked(priv, read_cmd, sizeof(read_cmd), data);

	if (ret < 0)
		return ret;

	u16 tmp16 = (data[1] << 8) + data[0];

	switch (cmd) {
	case PSU_REG_IN_VOLTS:
	case PSU_REG_VOUT_12V:
	case PSU_REG_IOUT_12V:
	case PSU_REG_VOUT_5V:
	case PSU_REG_IOUT_5V:
	case PSU_REG_VOUT_3V:
	case PSU_REG_IOUT_3V:
	case PSU_REG_TEMP0:
		*val = msipsu_lin11_to_int(tmp16, 1000, true);
		break;
	case PSU_REG_EFFICIENCY:
		*val = msipsu_lin11_to_int(tmp16, 100, true);
		break;
	case PSU_REG_FAN_RPM:
		*val = msipsu_lin11_to_int(tmp16, 1, false);
		break;
	case PSU_REG_TOTAL_WATTS:
		*val = msipsu_lin11_to_int(tmp16, 1000000, true);
		break;
	case PSU_REG_FAN_MODE:
	case PSU_REG_FAN_DUTY_CYCLE:
	case PSU_REG_TOTAL_UPTIME:
	case PSU_REG_UPTIME:
	case PSU_REG_REVISION:
		*val = ((long)data[3] << 24) + (data[2] << 16) + tmp16;
		break;
	case PSU_REG_MULTIRAIL:
		switch (data[0]) {
		case PSU_MULTI_RAIL_DISABLED:
			*val = 0;
			break;
		case PSU_MULTI_RAIL_ENABLED:
			*val = 1;
			break;
		default:
			*val = 0xff;
			break;
		}
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}

	return ret;
}

static umode_t msipsu_hwmon_ops_is_visible(const void *data, enum hwmon_sensor_types type,
					   u32 attr, int channel)
{
	if (type == hwmon_temp && (attr == hwmon_temp_input || attr == hwmon_temp_label))
		return 0444;
	else if (type == hwmon_fan && (attr == hwmon_fan_input || attr == hwmon_fan_label))
		return 0444;
	else if (type == hwmon_pwm && (attr == hwmon_pwm_enable || attr == hwmon_pwm_input))
		return 0644;
	else if (type == hwmon_power && (attr == hwmon_power_input || attr == hwmon_power_label))
		return 0444;
	else if (type == hwmon_in && (attr == hwmon_in_input || attr == hwmon_in_label))
		return 0444;
	else if (type == hwmon_curr && (attr == hwmon_curr_input || attr == hwmon_curr_label))
		return 0444;

	return 0;
}

static int msipsu_hwmon_ops_read(struct device *dev, enum hwmon_sensor_types type, u32 attr,
				 int channel, long *val)
{
	struct msipsu_data *priv = dev_get_drvdata(dev);
	int ret;

	if (type != hwmon_pwm) {
		/* PWM controls don't use the "all" packet, so don't read it */
		ret = msipsu_get_all_values(priv);
		if (ret < 0)
			return ret;
	}

	if (type == hwmon_temp && attr == hwmon_temp_input) {
		*val = msipsu_lin11_to_int(priv->data.temp, 1000, true);
	} else if (type == hwmon_fan && attr == hwmon_fan_input) {
		*val = msipsu_lin11_to_int(priv->data.fan_speed, 1, false);
	} else if (type == hwmon_pwm) {
		switch (attr) {
		case hwmon_pwm_enable:
			ret = msipsu_get_value(priv, PSU_REG_FAN_MODE, val);
			*val &= 0xff;
			break;
		case hwmon_pwm_input:
			ret = msipsu_get_value(priv, PSU_REG_FAN_DUTY_CYCLE, val);
			*val &= 0xff; /* there's a "calculated" DC in the next byte up */
			*val = msipsu_percent_to_pwm(*val);
			break;
		default:
			ret = -EOPNOTSUPP;
			break;
		}
	} else if (type == hwmon_power && attr == hwmon_power_input) {
		*val = msipsu_lin11_to_int(priv->data.watts, 1000000, true);
	} else if (type == hwmon_in && attr == hwmon_in_input) {
		switch (channel) {
		case 0:
			ret = msipsu_get_value(priv, PSU_REG_IN_VOLTS, val);
			break;
		case 1:
			*val = msipsu_lin11_to_int(priv->data.v12[COMBINED_12V].volts, 1000, true);
			break;
		case 2:
			*val = msipsu_lin11_to_int(priv->data.v5.volts, 1000, true);
			break;
		case 3:
			*val = msipsu_lin11_to_int(priv->data.v3.volts, 1000, true);
			break;
		default:
			return -EOPNOTSUPP;
		}
	} else if (type == hwmon_curr && attr == hwmon_curr_input) {
		switch (channel) {
		case 0:
			*val = msipsu_lin11_to_int(priv->data.v12[COMBINED_12V].amps, 1000, true);
			break;
		case 1:
			*val = msipsu_lin11_to_int(priv->data.v5.amps, 1000, true);
			break;
		case 2:
			*val = msipsu_lin11_to_int(priv->data.v3.amps, 1000, true);
			break;
		default:
			return -EOPNOTSUPP;
		}
	} else {
		return -EOPNOTSUPP;
	}

	if (ret < 0)
		return ret;

	return 0;
}

static int msipsu_hwmon_ops_read_string(struct device *dev, enum hwmon_sensor_types type, u32 attr,
					int channel, const char **str)
{
	static const char *const label_volts[] = {
		"v_in",
		"v_out +12v",
		"v_out +5v",
		"v_out +3.3v"
	};

	static const char *const label_amps[] = {
		"curr +12v",
		"curr +5v",
		"curr +3.3v"
	};

	if (type == hwmon_temp && attr == hwmon_temp_label) {
		*str = "psu temp";
		return 0;
	} else if (type == hwmon_fan && attr == hwmon_fan_label) {
		*str = "psu fan";
		return 0;
	} else if (type == hwmon_power && attr == hwmon_power_label) {
		*str = "power total";
		return 0;
	} else if (type == hwmon_in && attr == hwmon_in_label && channel < 4) {
		*str = label_volts[channel];
		return 0;
	} else if (type == hwmon_curr && attr == hwmon_curr_label && channel < 3) {
		*str = label_amps[channel];
		return 0;
	}

	return -EOPNOTSUPP;
}

static int msipsu_hwmon_ops_write(struct device *dev, enum hwmon_sensor_types type, u32 attr,
				  int channel, long val)
{
	struct msipsu_data *priv = dev_get_drvdata(dev);

	if (type != hwmon_pwm)
		return -EOPNOTSUPP;

	switch (attr) {
	case hwmon_pwm_enable:
		switch (val) {
		case FAN_MODE_AUTO:
			return msipsu_write_fan_settings(priv, val, FAN_SPEED_MIN);
		case FAN_MODE_MANUAL:
			return msipsu_write_fan_settings(priv, val, FAN_SPEED_MAX);
		default:
			return -EINVAL;
		}
	case hwmon_pwm_input:
		return msipsu_write_fan_settings(priv, FAN_MODE_MANUAL, val);
	default:
		return -EOPNOTSUPP;
	}
}

static const struct hwmon_ops msipsu_hwmon_ops = {
	.is_visible = msipsu_hwmon_ops_is_visible,
	.read = msipsu_hwmon_ops_read,
	.write = msipsu_hwmon_ops_write,
	.read_string = msipsu_hwmon_ops_read_string,
};

static const struct hwmon_channel_info *msipsu_info[] = {
	HWMON_CHANNEL_INFO(chip,
			   HWMON_C_REGISTER_TZ),
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT | HWMON_F_LABEL),
	HWMON_CHANNEL_INFO(pwm,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE),
	HWMON_CHANNEL_INFO(power,
			   HWMON_P_INPUT | HWMON_P_LABEL),
	HWMON_CHANNEL_INFO(in,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL),
	HWMON_CHANNEL_INFO(curr,
			   HWMON_C_INPUT | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_LABEL),
	NULL
};

static const struct hwmon_chip_info msipsu_chip_info = {
	.ops	= &msipsu_hwmon_ops,
	.info	= msipsu_info,
};

#ifdef CONFIG_DEBUG_FS

static void print_uptime(struct seq_file *seqf, u8 cmd)
{
	struct msipsu_data *priv = seqf->private;
	long val = 0;
	int ret = msipsu_get_value(priv, cmd, &val);

	if (ret < 0) {
		seq_puts(seqf, "N/A\n");
		return;
	}

	if (val > SECONDS_PER_DAY) {
		seq_printf(seqf, "%ld day(s), %02ld:%02ld:%02ld\n", val / SECONDS_PER_DAY,
			   val % SECONDS_PER_DAY / SECONDS_PER_HOUR, val % SECONDS_PER_HOUR / 60,
			   val % 60);
		return;
	}

	seq_printf(seqf, "%02ld:%02ld:%02ld\n", val % SECONDS_PER_DAY / SECONDS_PER_HOUR,
		   val % SECONDS_PER_HOUR / 60, val % 60);
}

static int uptime_show(struct seq_file *seqf, void *unused)
{
	print_uptime(seqf, PSU_REG_UPTIME);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(uptime);

static int uptime_total_show(struct seq_file *seqf, void *unused)
{
	print_uptime(seqf, PSU_REG_TOTAL_UPTIME);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(uptime_total);

static int revision_show(struct seq_file *seqf, void *unused)
{
	struct msipsu_data *priv = seqf->private;
	long val = 0;
	int ret = msipsu_get_value(priv, PSU_REG_REVISION, &val);

	if (ret < 0) {
		seq_puts(seqf, "N/A\n");
		return 0;
	}

	seq_printf(seqf, "%c.%c\n", (char)(val & 0xff), (char)((val >> 8) & 0xff));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(revision);

static int efficiency_show(struct seq_file *seqf, void *unused)
{
	struct msipsu_data *priv = seqf->private;
	int ret = msipsu_get_all_values(priv);

	if (ret < 0) {
		seq_puts(seqf, "N/A\n");
		return 0;
	}

	seq_printf(seqf, "%d\n", msipsu_lin11_to_int(priv->data.eff, 100, true));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(efficiency);

static int multi_rail_read(void *data, u64 *val)
{
	return msipsu_get_value((struct msipsu_data *)data, PSU_REG_MULTIRAIL, (long *)val);
}

static int msipsu_write_rail_setting(struct msipsu_data *priv, bool multi_enabled)
{
	long currently_enabled;
	int ret = msipsu_get_value(priv, PSU_REG_MULTIRAIL, &currently_enabled);

	if (ret < 0)
		return ret;

	/* hardware returns an error if we try to set the mode to the current mode. Avoid this. */
	if (currently_enabled == multi_enabled)
		return 0;

	u8 to_write = multi_enabled ? PSU_MULTI_RAIL_ENABLED : PSU_MULTI_RAIL_DISABLED;
	const u8 cmd[] = {PSU_WRITE, PSU_REG_MULTIRAIL, to_write};

	ret = msipsu_usb_cmd_locked(priv, cmd, sizeof(cmd), NULL);
	if (ret < 0)
		return ret;
	return msipsu_save_settings(priv);
}

static int multi_rail_write(void *data, u64 val)
{
	struct msipsu_data *priv = data;

	switch (val) {
	case 0:
		return msipsu_write_rail_setting(priv, false);
	case 1:
		return msipsu_write_rail_setting(priv, true);
	default:
		return -EINVAL;
	}
}

DEFINE_SIMPLE_ATTRIBUTE(multi_rail_fops, multi_rail_read, multi_rail_write, "%llu\n");

static void msipsu_debugfs_init(struct msipsu_data *priv)
{
	char name[32];

	scnprintf(name, sizeof(name), "%s-%s", DRIVER_NAME, dev_name(&priv->hdev->dev));

	priv->debugfs = debugfs_create_dir(name, NULL);
	debugfs_create_file("uptime", 0444, priv->debugfs, priv, &uptime_fops);
	debugfs_create_file("uptime_total", 0444, priv->debugfs, priv, &uptime_total_fops);
	debugfs_create_blob("vendor", 0444, priv->debugfs, &priv->vendor_blob);
	debugfs_create_blob("product", 0444, priv->debugfs, &priv->product_blob);
	debugfs_create_file("revision", 0444, priv->debugfs, priv, &revision_fops);
	debugfs_create_file("efficiency", 0444, priv->debugfs, priv, &efficiency_fops);
	debugfs_create_file("multi_rail_enabled", 0644, priv->debugfs, priv, &multi_rail_fops);
}

#else

static void msipsu_debugfs_init(struct msipsu_data *priv)
{
}

#endif

static int msipsu_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct msipsu_data *priv;
	int ret;

	priv = devm_kzalloc(&hdev->dev, sizeof(struct msipsu_data), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->cmd_buffer = devm_kmalloc(&hdev->dev, CMD_BUFFER_SIZE, GFP_KERNEL);
	if (!priv->cmd_buffer)
		return -ENOMEM;

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret)
		return ret;

	ret = hid_hw_open(hdev);
	if (ret)
		goto fail_and_stop;

	priv->hdev = hdev;
	hid_set_drvdata(hdev, priv);
	mutex_init(&priv->lock);
	init_completion(&priv->wait_completion);

	hid_device_io_start(hdev);

	ret = msipsu_fw_init(priv);
	if (ret < 0) {
		dev_err(&hdev->dev, "unable to initialize device (%d)\n", ret);
		goto fail_and_close;
	}

	priv->hwmon_dev = hwmon_device_register_with_info(&hdev->dev, "msipsu", priv,
							  &msipsu_chip_info, NULL);

	if (IS_ERR(priv->hwmon_dev)) {
		ret = PTR_ERR(priv->hwmon_dev);
		goto fail_and_close;
	}

	msipsu_debugfs_init(priv);

	return 0;

fail_and_close:
	hid_hw_close(hdev);
fail_and_stop:
	hid_hw_stop(hdev);
	return ret;
}

static void msipsu_remove(struct hid_device *hdev)
{
	struct msipsu_data *priv = hid_get_drvdata(hdev);

	debugfs_remove_recursive(priv->debugfs);
	hwmon_device_unregister(priv->hwmon_dev);
	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static int msipsu_raw_event(struct hid_device *hdev, struct hid_report *report, u8 *data, int size)
{
	struct msipsu_data *priv = hid_get_drvdata(hdev);

	if (completion_done(&priv->wait_completion))
		return 0;

	memcpy(priv->cmd_buffer, data, min(CMD_BUFFER_SIZE, size));
	complete(&priv->wait_completion);

	return 0;
}

#ifdef CONFIG_PM
static int msipsu_resume(struct hid_device *hdev)
{
	struct msipsu_data *priv = hid_get_drvdata(hdev);

	/* some PSUs turn off the microcontroller during standby, so a reinit is required */
	return msipsu_fw_init(priv);
}
#endif

static const struct hid_device_id msipsu_idtable[] = {
	{ HID_USB_DEVICE(0xdb0, 0x56d4) }, /* MEG Ai1300P */
	{ HID_USB_DEVICE(0xdb0, 0xe749) }, /* MEG Ai1000P */
	{ },
};
MODULE_DEVICE_TABLE(hid, msipsu_idtable);

static struct hid_driver msipsu_driver = {
	.name		= DRIVER_NAME,
	.id_table	= msipsu_idtable,
	.probe		= msipsu_probe,
	.remove		= msipsu_remove,
	.raw_event	= msipsu_raw_event,
#ifdef CONFIG_PM
	.resume		= msipsu_resume,
	.reset_resume	= msipsu_resume,
#endif
};

static int __init msipsu_init(void)
{
	return hid_register_driver(&msipsu_driver);
}

static void __exit msipsu_exit(void)
{
	hid_unregister_driver(&msipsu_driver);
}

/*
 * With module_init() the driver would load before the HID bus when
 * built-in, so use late_initcall() instead.
 */
late_initcall(msipsu_init);
module_exit(msipsu_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jack Doan <me@jackdoan.com>");
MODULE_DESCRIPTION("Driver for MSI power supplies with HID sensor interface");
