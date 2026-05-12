/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2017, Microchip Technology Inc.
 * Author: Tudor Ambarus
 */

#ifndef __ATMEL_I2C_H__
#define __ATMEL_I2C_H__

#include <linux/device.h>
#include <crypto/internal/hash.h>
#include <linux/hw_random.h>
#include <linux/types.h>
#include <crypto/sha2.h>

#define ATMEL_I2C_PRIORITY		300

#define ATMEL_I2C_WAKE_TOKEN_MAX_SIZE	8

/* Definitions of Data and Command sizes */
#define ATMEL_I2C_ADDR_SIZE		1
#define ATMEL_I2C_OPCODE_SIZE		1
#define ATMEL_I2C_COUNT_SIZE		1
#define ATMEL_I2C_PARAM1_SIZE		1
#define ATMEL_I2C_PARAM2_SIZE		2
#define ATMEL_I2C_CRC_SIZE		2

#define ATMEL_I2C_RSP_OVERHEAD_SIZE	(ATMEL_I2C_COUNT_SIZE + \
					ATMEL_I2C_CRC_SIZE)
#define ATMEL_I2C_COUNT_OVERHEAD_SIZE	(ATMEL_I2C_OPCODE_SIZE + \
					ATMEL_I2C_COUNT_SIZE + \
					ATMEL_I2C_PARAM1_SIZE + \
					ATMEL_I2C_PARAM2_SIZE + \
					ATMEL_I2C_CRC_SIZE)

/* Definitions for the status Command */
#define ATMEL_I2C_STATUS_RSP_SIZE	4

/* size in bytes of the n prime */
#define ATMEL_ECC_NIST_P256_N_SIZE	32
#define ATMEL_ECC_PUBKEY_SIZE		(2 * ATMEL_ECC_NIST_P256_N_SIZE)
#define ATMEL_I2C_GENKEY_RSP_SIZE	(ATMEL_ECC_PUBKEY_SIZE + \
					 ATMEL_I2C_RSP_OVERHEAD_SIZE)
#define ATMEL_I2C_MAX_RSP_SIZE		ATMEL_I2C_GENKEY_RSP_SIZE

/* Definitions for Indexes common to all commands */
#define ATMEL_I2C_RSP_DATA_IDX		1 /* buffer index of data in response */
#define ATMEL_I2C_ECDH_SLOT_DEFAULT	2

/**
 * atmel_i2c_cmd - structure used for communicating with the device.
 * @word_addr: indicates the function of the packet sent to the device. This
 *             byte should have a value of COMMAND for normal operation.
 * @count    : number of bytes to be transferred to (or from) the device.
 * @opcode   : the command code.
 * @param1   : the first parameter; always present.
 * @param2   : the second parameter; always present.
 * @data     : optional remaining input data. Includes a 2-byte CRC.
 * @rxsize   : size of the data received from i2c client.
 * @msecs    : command execution time in milliseconds
 */
struct atmel_i2c_cmd {
	u8 word_addr;
	u8 count;
	u8 opcode;
	u8 param1;
	__le16 param2;
	u8 data[ATMEL_I2C_MAX_RSP_SIZE];
	u8 msecs;
	u16 rxsize;
} __packed;

/* Definitions for eeprom organization */
enum atmel_i2c_eeprom_zones {
	ATMEL_EEPROM_CONFIG_ZONE = 0,
	ATMEL_EEPROM_OTP_ZONE = 1,
	ATMEL_EEPROM_DATA_ZONE = 2,
};

struct atmel_i2c_max_exec_timings {
	unsigned int max_exec_time_genkey;
	unsigned int max_exec_time_ecdh;
	unsigned int max_exec_time_random;
	unsigned int max_exec_time_read;
	unsigned int max_exec_time_sha;
	unsigned int max_exec_time_write;
};

struct atmel_i2c_of_match_data {
	const unsigned short needs_legacy_hwrng;
	const unsigned short needs_sha_padding;
	struct atmel_i2c_max_exec_timings timings;
	size_t eeprom_zone_size[3]; /* all atmel devices have three zones */
};

/* Used for binding tfm objects to i2c clients. */
enum atmel_i2c_capability {
	ATMEL_CAP_ECDH = 0,
	ATMEL_CAP_SHA,
};

enum atmel_i2c_sha_engine_cmd {
	atmel_sha_init = 0,
	atmel_sha_compute,
	atmel_sha_ecc_end,
};

size_t atmel_i2c_sha_rsp_size[] = {
	[atmel_sha_init] = ATMEL_I2C_STATUS_RSP_SIZE,
	[atmel_sha_compute] = SHA256_DIGEST_SIZE + ATMEL_I2C_RSP_OVERHEAD_SIZE,
	[atmel_sha_ecc_end] = SHA256_DIGEST_SIZE + ATMEL_I2C_RSP_OVERHEAD_SIZE,
};

struct atmel_i2c_sha_ctx {
	struct i2c_client *client;
};

struct atmel_i2c_sha_reqctx {
	u8 buffer[SHA256_BLOCK_SIZE];
	size_t bufcnt;
	size_t total; /* size of full input, needed for padding */
	struct atmel_i2c_client_priv *ctx;
};

struct atmel_i2c_client_mgmt {
	struct list_head i2c_client_list;
	spinlock_t i2c_list_lock;
} ____cacheline_aligned;
extern struct atmel_i2c_client_mgmt atmel_i2c_mgmt;

/**
 * atmel_i2c_client_priv - i2c_client private data
 * @client              : pointer to i2c client device
 * @i2c_client_list_node: part of i2c_client_list
 * @lock                : lock for sending i2c commands
 * @wake_token          : wake token array of zeros
 * @wake_token_sz       : size in bytes of the wake_token
 * @tfm_count           : number of active crypto transformations on i2c client
 * @hwrng               : hold the hardware generated rng
 * @caps                : feature capability of the particular driver
 * @data                : preinitialized driver data
 *
 * Reads and writes from/to the i2c client are sequential. The first byte
 * transmitted to the device is treated as the byte size. Any attempt to send
 * more than this number of bytes will cause the device to not ACK those bytes.
 * After the host writes a single command byte to the input buffer, reads are
 * prohibited until after the device completes command execution. Use a mutex
 * when sending i2c commands.
 */
struct atmel_i2c_client_priv {
	struct i2c_client *client;
	struct list_head i2c_client_list_node;
	struct mutex lock;
	u8 wake_token[ATMEL_I2C_WAKE_TOKEN_MAX_SIZE];
	size_t wake_token_sz;
	atomic_t tfm_count ____cacheline_aligned;
	struct hwrng hwrng;
	u32 caps;
	const struct atmel_i2c_of_match_data *data;
};

/**
 * atmel_i2c_work_data - data structure representing the work
 * @ctx : transformation context.
 * @cbk : pointer to a callback function to be invoked upon completion of this
 *        request. This has the form:
 *        callback(struct atmel_i2c_work_data *work_data, void *areq, u8 status)
 *        where:
 *        @work_data: data structure representing the work
 *        @areq     : optional pointer to an argument passed with the original
 *                    request.
 *        @status   : status returned from the i2c client device or i2c error.
 * @areq: optional pointer to a user argument for use at callback time.
 * @work: describes the task to be executed.
 * @cmd : structure used for communicating with the device.
 */
struct atmel_i2c_work_data {
	void *ctx;
	struct i2c_client *client;
	void (*cbk)(struct atmel_i2c_work_data *work_data, void *areq,
		    int status);
	void *areq;
	struct work_struct work;
	struct atmel_i2c_cmd cmd;
};

int atmel_i2c_probe(struct i2c_client *client);

void atmel_i2c_enqueue(struct atmel_i2c_work_data *work_data,
		       void (*cbk)(struct atmel_i2c_work_data *work_data,
				   void *areq, int status),
		       void *areq);
void atmel_i2c_flush_queue(void);

int atmel_i2c_send_receive(struct i2c_client *client, struct atmel_i2c_cmd *cmd);

void atmel_i2c_init_random_cmd(struct atmel_i2c_cmd *cmd,
			       const struct atmel_i2c_max_exec_timings *timings);
void atmel_i2c_init_genkey_cmd(struct atmel_i2c_cmd *cmd, u16 keyid,
			       const struct atmel_i2c_max_exec_timings *timings);
int atmel_i2c_init_ecdh_cmd(struct atmel_i2c_cmd *cmd,
			    struct scatterlist *pubkey,
			    const struct atmel_i2c_max_exec_timings *timings);
int atmel_i2c_init_sha_cmd(struct atmel_i2c_cmd *cmd, u8 *challenge, size_t len,
			   enum atmel_i2c_sha_engine_cmd sha_engine_cmd,
			   const struct atmel_i2c_max_exec_timings *timings);
int atmel_i2c_register_rng(struct atmel_i2c_client_priv *i2c_priv,
			   struct device *dev);

int atmel_i2c_sha_init(struct ahash_request *req);
int atmel_i2c_sha_update(struct ahash_request *req);
int atmel_i2c_sha_final(struct ahash_request *req);
int atmel_i2c_sha_finup(struct ahash_request *req);
int atmel_i2c_sha_digest(struct ahash_request *req);
int atmel_i2c_sha_export(struct ahash_request *req, void *out);
int atmel_i2c_sha_import(struct ahash_request *req, const void *in);

int atmel_i2c_device_sanity_check(struct i2c_client *client);

ssize_t atmel_i2c_eeprom_display(struct device *dev,
				 struct device_attribute *attr,
				 char *buf,
				 enum atmel_i2c_eeprom_zones zone);

struct i2c_client *atmel_i2c_client_alloc(enum atmel_i2c_capability cap);
void atmel_i2c_unregister_client(struct atmel_i2c_client_priv *i2c_priv);

#endif /* __ATMEL_I2C_H__ */
