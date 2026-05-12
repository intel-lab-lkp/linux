// SPDX-License-Identifier: GPL-2.0
/*
 * Microchip / Atmel ECC (I2C) driver.
 *
 * Copyright (c) 2017, Microchip Technology Inc.
 * Author: Tudor Ambarus
 */

#include <linux/bitrev.h>
#include <linux/crc16.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include "atmel-i2c.h"

#define ATMEL_I2C_COMMAND		0x03 /* packet function */
#define ATMEL_I2C_SLEEP_TOKEN		0x01

/* Definitions for the device lock state */
#define ATMEL_I2C_DEVICE_LOCK_ADDR	0x15
#define ATMEL_I2C_LOCK_VALUE_IDX	(ATMEL_I2C_RSP_DATA_IDX + 2)
#define ATMEL_I2C_LOCK_CONFIG_IDX	(ATMEL_I2C_RSP_DATA_IDX + 3)

/* Definitions for the READ Command */
#define ATMEL_I2C_READ_COUNT		ATMEL_I2C_COUNT_OVERHEAD_SIZE
#define ATMEL_I2C_READ_RSP_SIZE		(4 + ATMEL_I2C_RSP_OVERHEAD_SIZE)

/* Definitions for the RANDOM Command */
#define ATMEL_I2C_RANDOM_COUNT		ATMEL_I2C_COUNT_OVERHEAD_SIZE
#define ATMEL_I2C_RNG_BLOCK_SIZE	32
#define ATMEL_I2C_RANDOM_RSP_SIZE	(ATMEL_I2C_RNG_BLOCK_SIZE + \
					ATMEL_I2C_RSP_OVERHEAD_SIZE)
#define ATMEL_I2C_RANDOM_COUNT		ATMEL_I2C_COUNT_OVERHEAD_SIZE

/* Definitions for the GenKey Command */
#define ATMEL_I2C_GENKEY_COUNT		ATMEL_I2C_COUNT_OVERHEAD_SIZE
#define ATMEL_I2C_GENKEY_MODE_PRIVATE	0x04

/* Definitions for the ECDH Command */
#define ATMEL_I2C_ECDH_COUNT		71
#define ATMEL_I2C_ECDH_RSP_SIZE		(32 + ATMEL_I2C_RSP_OVERHEAD_SIZE)
#define ATMEL_I2C_ECDH_PREFIX_MODE	0x00

/* Command opcode */
#define ATMEL_I2C_OPCODE_ECDH		0x43
#define ATMEL_I2C_OPCODE_GENKEY		0x40
#define ATMEL_I2C_OPCODE_READ		0x02
#define ATMEL_I2C_OPCODE_RANDOM		0x1b
#define ATMEL_I2C_OPCODE_WRITE		0x12

/*
 * Wake High delay to data communication (microseconds). SDA should be stable
 * high for this entire duration.
 */
#define ATMEL_I2C_TWHI_MIN		1500
#define ATMEL_I2C_TWHI_MAX		1550

/* Wake Low duration */
#define ATMEL_I2C_TWLO_USEC		60

/* Status/Error codes */
enum atmel_i2c_error_codes {
	ATMEL_STATUS_OK_NOERR = 0x00,            /* success */
	ATMEL_STATUS_CHECKMAC_OR_VERIFY_MISCOMPARE = 0x01,
	ATMEL_STATUS_PARSE_ERROR = 0x03,
	ATMEL_STATUS_ECC_FAULT = 0x05,
	ATMEL_STATUS_EXECUTION_FAULT = 0x0F,
	ATMEL_STATUS_OK_WAKE_SUCCESSFULL = 0x11, /* success */
	ATMEL_STATUS_WATCHDOG_EXPIRE = 0xEE,
	ATMEL_STATUS_CRC_ERROR = 0xFF,
};

struct atmel_i2c_client_mgmt atmel_i2c_mgmt = {
	.i2c_list_lock = __SPIN_LOCK_UNLOCKED(atmel_i2c_mgmt.i2c_list_lock),
	.i2c_client_list = LIST_HEAD_INIT(atmel_i2c_mgmt.i2c_client_list),
};
EXPORT_SYMBOL_GPL(atmel_i2c_mgmt);

static const struct {
	u8 value;
	const char *error_text;
} error_list[] = {
	{ ATMEL_STATUS_CHECKMAC_OR_VERIFY_MISCOMPARE, "CheckMac or Verify miscompare" },
	{ ATMEL_STATUS_PARSE_ERROR, "Parse Error" },
	{ ATMEL_STATUS_ECC_FAULT, "ECC Fault" },
	{ ATMEL_STATUS_EXECUTION_FAULT, "Execution Error" },
	{ ATMEL_STATUS_WATCHDOG_EXPIRE, "Watchdog about to expire" },
	{ ATMEL_STATUS_CRC_ERROR, "CRC or other communication error" },
};

/**
 * atmel_i2c_checksum() - Generate 16-bit CRC as required by ATMEL ECC.
 * CRC16 verification of the count, opcode, param1, param2 and data bytes.
 * The checksum is saved in little-endian format in the least significant
 * two bytes of the command. CRC polynomial is 0x8005 and the initial register
 * value should be zero.
 *
 * @cmd : structure used for communicating with the device.
 */
static void atmel_i2c_checksum(struct atmel_i2c_cmd *cmd)
{
	u8 *data = &cmd->count;
	size_t len = cmd->count - ATMEL_I2C_CRC_SIZE;
	__le16 *__crc16 = (__le16 *)(data + len);

	*__crc16 = cpu_to_le16(bitrev16(crc16(0, data, len)));
}

struct i2c_client *atmel_i2c_client_alloc(enum atmel_i2c_capability cap)
{
	struct atmel_i2c_client_priv *i2c_priv, *min_i2c_priv = NULL;
	struct i2c_client *client = ERR_PTR(-ENODEV);
	int min_tfm_cnt = INT_MAX;
	int tfm_cnt;

	spin_lock(&atmel_i2c_mgmt.i2c_list_lock);

	if (list_empty(&atmel_i2c_mgmt.i2c_client_list)) {
		spin_unlock(&atmel_i2c_mgmt.i2c_list_lock);
		return ERR_PTR(-ENODEV);
	}

	list_for_each_entry(i2c_priv, &atmel_i2c_mgmt.i2c_client_list,
			    i2c_client_list_node) {
		if (!(i2c_priv->caps & BIT(cap)))
			continue;

		tfm_cnt = atomic_read(&i2c_priv->tfm_count);
		if (tfm_cnt < min_tfm_cnt) {
			min_tfm_cnt = tfm_cnt;
			min_i2c_priv = i2c_priv;
		}
		if (!min_tfm_cnt)
			break;
	}

	if (min_i2c_priv) {
		atomic_inc(&min_i2c_priv->tfm_count);
		client = min_i2c_priv->client;
	}

	spin_unlock(&atmel_i2c_mgmt.i2c_list_lock);

	return client;
}
EXPORT_SYMBOL(atmel_i2c_client_alloc);

static int atmel_i2c_init_read_eeprom_cmd(struct atmel_i2c_cmd *cmd, u16 addr,
					  enum atmel_i2c_eeprom_zones zone,
					  const struct atmel_i2c_of_match_data *data)
{
	const struct atmel_i2c_max_exec_timings *timings = &data->timings;
	size_t zone_size = data->eeprom_zone_size[zone];

	if (addr > zone_size)
		return -EINVAL;

	cmd->word_addr = ATMEL_I2C_COMMAND;
	cmd->opcode = ATMEL_I2C_OPCODE_READ;
	cmd->param1 = zone;
	cmd->param2 = cpu_to_le16(addr);
	cmd->count = ATMEL_I2C_READ_COUNT;

	atmel_i2c_checksum(cmd);

	cmd->msecs = timings->max_exec_time_read;
	cmd->rxsize = ATMEL_I2C_READ_RSP_SIZE;

	return 0;
}

void atmel_i2c_init_random_cmd(struct atmel_i2c_cmd *cmd,
			       const struct atmel_i2c_max_exec_timings *timings)
{
	cmd->word_addr = ATMEL_I2C_COMMAND;
	cmd->opcode = ATMEL_I2C_OPCODE_RANDOM;
	cmd->param1 = 0;
	cmd->param2 = 0;
	cmd->count = ATMEL_I2C_RANDOM_COUNT;

	atmel_i2c_checksum(cmd);

	cmd->msecs = timings->max_exec_time_random;
	cmd->rxsize = ATMEL_I2C_RANDOM_RSP_SIZE;
}
EXPORT_SYMBOL(atmel_i2c_init_random_cmd);

void atmel_i2c_init_genkey_cmd(struct atmel_i2c_cmd *cmd, u16 keyid,
			       const struct atmel_i2c_max_exec_timings *timings)
{
	cmd->word_addr = ATMEL_I2C_COMMAND;
	cmd->count = ATMEL_I2C_GENKEY_COUNT;
	cmd->opcode = ATMEL_I2C_OPCODE_GENKEY;
	cmd->param1 = ATMEL_I2C_GENKEY_MODE_PRIVATE;
	/* a random private key will be generated and stored in slot keyID */
	cmd->param2 = cpu_to_le16(keyid);

	atmel_i2c_checksum(cmd);

	cmd->msecs = timings->max_exec_time_genkey;
	cmd->rxsize = ATMEL_I2C_GENKEY_RSP_SIZE;
}
EXPORT_SYMBOL(atmel_i2c_init_genkey_cmd);

int atmel_i2c_init_ecdh_cmd(struct atmel_i2c_cmd *cmd,
			    struct scatterlist *pubkey,
			    const struct atmel_i2c_max_exec_timings *timings)
{
	size_t copied;

	cmd->word_addr = ATMEL_I2C_COMMAND;
	cmd->count = ATMEL_I2C_ECDH_COUNT;
	cmd->opcode = ATMEL_I2C_OPCODE_ECDH;
	cmd->param1 = ATMEL_I2C_ECDH_PREFIX_MODE;
	/* private key slot */
	cmd->param2 = cpu_to_le16(ATMEL_I2C_ECDH_SLOT_DEFAULT);

	/*
	 * The device only supports NIST P256 ECC keys. The public key size will
	 * always be the same. Use a macro for the key size to avoid unnecessary
	 * computations.
	 */
	copied = sg_copy_to_buffer(pubkey,
				   sg_nents_for_len(pubkey,
						    ATMEL_ECC_PUBKEY_SIZE),
				   cmd->data, ATMEL_ECC_PUBKEY_SIZE);
	if (copied != ATMEL_ECC_PUBKEY_SIZE)
		return -EINVAL;

	atmel_i2c_checksum(cmd);

	cmd->msecs = timings->max_exec_time_ecdh;
	cmd->rxsize = ATMEL_I2C_ECDH_RSP_SIZE;

	return 0;
}
EXPORT_SYMBOL(atmel_i2c_init_ecdh_cmd);

static void atmel_i2c_rng_done(struct atmel_i2c_work_data *work_data,
			       void *areq, int status)
{
	struct atmel_i2c_client_priv *i2c_priv = work_data->ctx;
	struct hwrng *rng = areq;

	if (status)
		dev_warn_ratelimited(&i2c_priv->client->dev,
				     "i2c transaction failed (%d)\n",
				     status);

	rng->priv = (unsigned long)work_data;
	atomic_dec(&i2c_priv->tfm_count);
}

static int atmel_i2c_rng_read_nonblocking(struct hwrng *rng, void *buf,
					  size_t max)
{
	struct atmel_i2c_client_priv *i2c_priv = container_of(rng,
							      struct atmel_i2c_client_priv,
							      hwrng);
	const struct atmel_i2c_of_match_data *data = i2c_priv->data;
	struct atmel_i2c_work_data *work_data;

	/* keep maximum 1 asynchronous read in flight at any time */
	if (!atomic_add_unless(&i2c_priv->tfm_count, 1, 1))
		return 0;

	if (rng->priv) {
		work_data = (struct atmel_i2c_work_data *)rng->priv;
		max = min(ATMEL_I2C_RANDOM_RSP_SIZE - ATMEL_I2C_RSP_OVERHEAD_SIZE, max);
		memcpy(buf, &work_data->cmd.data[ATMEL_I2C_RSP_DATA_IDX], max);
		rng->priv = 0;
	} else {
		work_data = kmalloc_obj(*work_data, GFP_ATOMIC);
		if (!work_data) {
			atomic_dec(&i2c_priv->tfm_count);
			return -ENOMEM;
		}
		work_data->ctx = i2c_priv;
		work_data->client = i2c_priv->client;

		max = 0;
	}

	atmel_i2c_init_random_cmd(&work_data->cmd, &data->timings);
	atmel_i2c_enqueue(work_data, atmel_i2c_rng_done, rng);

	return max;
}

static int atmel_i2c_rng_read(struct hwrng *rng, void *buf, size_t max,
			      bool wait)
{
	struct atmel_i2c_client_priv *i2c_priv = container_of(rng,
							      struct atmel_i2c_client_priv,
							      hwrng);
	const struct atmel_i2c_of_match_data *data = i2c_priv->data;
	struct atmel_i2c_cmd cmd;
	int ret;

	if (!wait)
		return atmel_i2c_rng_read_nonblocking(rng, buf, max);

	atmel_i2c_init_random_cmd(&cmd, &data->timings);

	ret = atmel_i2c_send_receive(i2c_priv->client, &cmd);
	if (ret)
		return ret;

	max = min(ATMEL_I2C_RANDOM_RSP_SIZE - ATMEL_I2C_RSP_OVERHEAD_SIZE, max);
	memcpy(buf, &cmd.data[ATMEL_I2C_RSP_DATA_IDX], max);

	return max;
}

int atmel_i2c_register_rng(struct atmel_i2c_client_priv *i2c_priv,
			   struct device *dev)
{
	const struct atmel_i2c_of_match_data *data = i2c_priv->data;

	memset(&i2c_priv->hwrng, 0, sizeof(i2c_priv->hwrng));

	i2c_priv->hwrng.name = dev_name(dev);
	i2c_priv->hwrng.read = atmel_i2c_rng_read;

	if (data->needs_legacy_hwrng)
		i2c_priv->hwrng.quality = data->needs_legacy_hwrng;

	return devm_hwrng_register(dev, &i2c_priv->hwrng);
}
EXPORT_SYMBOL(atmel_i2c_register_rng);

static int atmel_i2c_eeprom_read(struct i2c_client *client, u16 addr,
				 enum atmel_i2c_eeprom_zones zone, u8 *buf)
{
	struct atmel_i2c_client_priv *i2c_priv = i2c_get_clientdata(client);
	const struct atmel_i2c_of_match_data *data = i2c_priv->data;
	struct atmel_i2c_cmd *cmd;
	int ret = -1;

	cmd = kmalloc_obj(*cmd);
	if (!cmd)
		return -ENOMEM;

	ret = atmel_i2c_init_read_eeprom_cmd(cmd, addr, zone, data);
	if (ret < 0) {
		dev_err(&client->dev, "failed, invalid eeprom address %04X\n",
			addr);
		goto err;
	}

	ret = atmel_i2c_send_receive(client, cmd);
	if (ret)
		goto err;

	if (cmd->data[0] == 0xff) {
		dev_err(&client->dev, "failed, device not ready\n");
		ret = -EINVAL;
		goto err;
	}

	memcpy(buf, cmd->data + ATMEL_I2C_RSP_DATA_IDX, ATMEL_I2C_STATUS_RSP_SIZE);

err:
	kfree(cmd);
	return ret;
}

ssize_t atmel_i2c_eeprom_display(struct device *dev,
				 struct device_attribute *attr,
				 char *buf,
				 enum atmel_i2c_eeprom_zones zone)
{
	struct i2c_client *client = to_i2c_client(dev);
	const struct atmel_i2c_client_priv *i2c_priv = i2c_get_clientdata(client);
	const struct atmel_i2c_of_match_data *data = i2c_priv->data;
	const size_t *eeprom = data->eeprom_zone_size;
	u16 block_addr;
	u8 *eeprom_buf;
	ssize_t len = 0;
	int i, ret = 0;

	eeprom_buf = kcalloc(eeprom[zone], sizeof(*eeprom_buf), GFP_KERNEL);
	if (!eeprom_buf)
		return -ENOMEM;

	for (block_addr = 0; block_addr < eeprom[zone] / 4; block_addr++) {
		ret = atmel_i2c_eeprom_read(client, block_addr, zone,
					    eeprom_buf + block_addr * 4);
		if (ret < 0) {
			dev_err(dev, "failed to read %s zone\n",
				zone == ATMEL_EEPROM_CONFIG_ZONE ? "CONFIG"
				: (zone == ATMEL_EEPROM_OTP_ZONE ? "OTP" : "DATA"));
			goto err;
		}
	}

	for (i = 0; i < eeprom[zone]; i++)
		len += sysfs_emit_at(buf, len, "%02X", eeprom_buf[i]);
	len += sysfs_emit_at(buf, len, "\n");
	ret = len;
err:
	kfree(eeprom_buf);
	return ret;
}
EXPORT_SYMBOL(atmel_i2c_eeprom_display);

/*
 * After wake and after execution of a command, there will be error, status, or
 * result bytes in the device's output register that can be retrieved by the
 * system. When the length of that group is four bytes, the codes returned are
 * detailed in error_list.
 */
static int atmel_i2c_status(struct device *dev, u8 *status)
{
	size_t err_list_len = ARRAY_SIZE(error_list);
	int i;
	u8 err_id = status[1];

	if (*status != ATMEL_I2C_STATUS_RSP_SIZE)
		return 0;

	if (err_id == ATMEL_STATUS_OK_WAKE_SUCCESSFULL || err_id == ATMEL_STATUS_OK_NOERR)
		return 0;

	for (i = 0; i < err_list_len; i++)
		if (error_list[i].value == err_id)
			break;

	/* if err_id is not in the error_list then ignore it */
	if (i != err_list_len) {
		dev_err(dev, "%02x: %s:\n", err_id, error_list[i].error_text);
		return err_id;
	}

	return 0;
}

static int atmel_i2c_wakeup(struct i2c_client *client)
{
	struct atmel_i2c_client_priv *i2c_priv = i2c_get_clientdata(client);
	u8 status[ATMEL_I2C_STATUS_RSP_SIZE];
	int ret;

	/*
	 * The device ignores any levels or transitions on the SCL pin when the
	 * device is idle, asleep or during waking up. Don't check for error
	 * when waking up the device.
	 */
	i2c_transfer_buffer_flags(client, i2c_priv->wake_token,
				i2c_priv->wake_token_sz, I2C_M_IGNORE_NAK);

	/*
	 * Wait to wake the device. Typical execution times for ecdh and genkey
	 * are around tens of milliseconds. Delta is chosen to 50 microseconds.
	 */
	usleep_range(ATMEL_I2C_TWHI_MIN, ATMEL_I2C_TWHI_MAX);

	ret = i2c_master_recv(client, status, ATMEL_I2C_STATUS_RSP_SIZE);
	if (ret < 0)
		return ret;

	return atmel_i2c_status(&client->dev, status);
}

static int atmel_i2c_sleep(struct i2c_client *client)
{
	u8 sleep = ATMEL_I2C_SLEEP_TOKEN;

	return i2c_master_send(client, &sleep, 1);
}

/*
 * atmel_i2c_send_receive() - send a command to the device and receive its
 *                            response.
 * @client: i2c client device
 * @cmd   : structure used to communicate with the device
 *
 * After the device receives a Wake token, a watchdog counter starts within the
 * device. After the watchdog timer expires, the device enters sleep mode
 * regardless of whether some I/O transmission or command execution is in
 * progress. If a command is attempted when insufficient time remains prior to
 * watchdog timer execution, the device will return the watchdog timeout error
 * code without attempting to execute the command. There is no way to reset the
 * counter other than to put the device into sleep or idle mode and then
 * wake it up again.
 */
int atmel_i2c_send_receive(struct i2c_client *client, struct atmel_i2c_cmd *cmd)
{
	struct atmel_i2c_client_priv *i2c_priv = i2c_get_clientdata(client);
	int ret;

	mutex_lock(&i2c_priv->lock);

	ret = atmel_i2c_wakeup(client);
	if (ret)
		goto err;

	/* send the command */
	ret = i2c_master_send(client, (u8 *)cmd, cmd->count + ATMEL_I2C_ADDR_SIZE);
	if (ret < 0)
		goto err;

	/* delay the appropriate amount of time for command to execute */
	msleep(cmd->msecs);

	/* receive the response */
	ret = i2c_master_recv(client, cmd->data, cmd->rxsize);
	if (ret < 0)
		goto err;

	/* put the device into low-power mode */
	ret = atmel_i2c_sleep(client);
	if (ret < 0)
		goto err;

	mutex_unlock(&i2c_priv->lock);
	return atmel_i2c_status(&client->dev, cmd->data);
err:
	mutex_unlock(&i2c_priv->lock);
	return ret;
}
EXPORT_SYMBOL(atmel_i2c_send_receive);

static void atmel_i2c_work_handler(struct work_struct *work)
{
	struct atmel_i2c_work_data *work_data =
			container_of(work, struct atmel_i2c_work_data, work);
	struct atmel_i2c_cmd *cmd = &work_data->cmd;
	struct i2c_client *client = work_data->client;
	int status;

	status = atmel_i2c_send_receive(client, cmd);
	work_data->cbk(work_data, work_data->areq, status);
}

static struct workqueue_struct *atmel_wq;

void atmel_i2c_enqueue(struct atmel_i2c_work_data *work_data,
		       void (*cbk)(struct atmel_i2c_work_data *work_data,
				   void *areq, int status),
		       void *areq)
{
	work_data->cbk = (void *)cbk;
	work_data->areq = areq;

	INIT_WORK(&work_data->work, atmel_i2c_work_handler);
	queue_work(atmel_wq, &work_data->work);
}
EXPORT_SYMBOL(atmel_i2c_enqueue);

void atmel_i2c_flush_queue(void)
{
	flush_workqueue(atmel_wq);
}
EXPORT_SYMBOL(atmel_i2c_flush_queue);

static inline size_t atmel_i2c_wake_token_sz(u32 bus_clk_rate)
{
	u32 no_of_bits = DIV_ROUND_UP(ATMEL_I2C_TWLO_USEC * bus_clk_rate, USEC_PER_SEC);

	/* return the size of the wake_token in bytes */
	return DIV_ROUND_UP(no_of_bits, 8);
}

int atmel_i2c_device_sanity_check(struct i2c_client *client)
{
	struct atmel_i2c_client_priv *i2c_priv = i2c_get_clientdata(client);
	const struct atmel_i2c_of_match_data *data = i2c_priv->data;
	struct atmel_i2c_cmd *cmd;
	int ret;

	cmd = kmalloc_obj(*cmd);
	if (!cmd)
		return -ENOMEM;

	atmel_i2c_init_read_eeprom_cmd(cmd, ATMEL_I2C_DEVICE_LOCK_ADDR,
				       ATMEL_EEPROM_CONFIG_ZONE, data);

	ret = atmel_i2c_send_receive(client, cmd);
	if (ret)
		goto free_cmd;

	/*
	 * It is vital that the Configuration, Data and OTP zones be locked
	 * prior to release into the field of the system containing the device.
	 * Failure to lock these zones may permit modification of any secret
	 * keys and may lead to other security problems.
	 */
	if (cmd->data[ATMEL_I2C_LOCK_CONFIG_IDX] || cmd->data[ATMEL_I2C_LOCK_VALUE_IDX]) {
		dev_err(&client->dev, "Config, Data and OTP zones are unlocked!\n");
		ret = -ENOTSUPP;
	}

	/* fall through */
free_cmd:
	kfree(cmd);
	return ret;
}
EXPORT_SYMBOL(atmel_i2c_device_sanity_check);

void atmel_i2c_unregister_client(struct atmel_i2c_client_priv *i2c_priv)
{
	spin_lock(&atmel_i2c_mgmt.i2c_list_lock);
	if (!list_empty(&i2c_priv->i2c_client_list_node))
		list_del_init(&i2c_priv->i2c_client_list_node);
	spin_unlock(&atmel_i2c_mgmt.i2c_list_lock);
}
EXPORT_SYMBOL(atmel_i2c_unregister_client);

int atmel_i2c_probe(struct i2c_client *client)
{
	struct atmel_i2c_client_priv *i2c_priv;
	struct device *dev = &client->dev;
	int ret;
	u32 bus_clk_rate;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(dev, "I2C_FUNC_I2C not supported\n");
		return -ENODEV;
	}

	bus_clk_rate = i2c_acpi_find_bus_speed(&client->adapter->dev);
	if (!bus_clk_rate) {
		ret = device_property_read_u32(&client->adapter->dev,
					       "clock-frequency", &bus_clk_rate);
		if (ret) {
			dev_err(dev, "failed to read clock-frequency property\n");
			return ret;
		}
	}

	if (bus_clk_rate > I2C_MAX_FAST_MODE_PLUS_FREQ) {
		dev_err(dev, "%u exceeds maximum supported clock frequency (1MHz)\n",
			bus_clk_rate);
		return -EINVAL;
	}

	i2c_priv = devm_kmalloc(dev, sizeof(*i2c_priv), GFP_KERNEL);
	if (!i2c_priv)
		return -ENOMEM;

	i2c_priv->client = client;
	mutex_init(&i2c_priv->lock);

	/*
	 * WAKE_TOKEN_MAX_SIZE was calculated for the maximum bus_clk_rate -
	 * 1MHz. The previous bus_clk_rate check ensures us that wake_token_sz
	 * will always be smaller than or equal to WAKE_TOKEN_MAX_SIZE.
	 */
	i2c_priv->wake_token_sz = atmel_i2c_wake_token_sz(bus_clk_rate);

	memset(i2c_priv->wake_token, 0, sizeof(i2c_priv->wake_token));

	atomic_set(&i2c_priv->tfm_count, 0);

	i2c_set_clientdata(client, i2c_priv);

	return 0;
}
EXPORT_SYMBOL(atmel_i2c_probe);

static int __init atmel_i2c_init(void)
{
	atmel_wq = alloc_workqueue("atmel_wq", WQ_MEM_RECLAIM, 0);
	return atmel_wq ? 0 : -ENOMEM;
}

static void __exit atmel_i2c_exit(void)
{
	flush_workqueue(atmel_wq);
	destroy_workqueue(atmel_wq);
}

module_init(atmel_i2c_init);
module_exit(atmel_i2c_exit);

MODULE_AUTHOR("Tudor Ambarus");
MODULE_DESCRIPTION("Microchip / Atmel ECC (I2C) driver");
MODULE_LICENSE("GPL v2");
