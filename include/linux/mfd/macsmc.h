/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Apple SMC (System Management Controller) core definitions
 *
 * Copyright (C) The Asahi Linux Contributors
 */

#ifndef _LINUX_MFD_MACSMC_H
#define _LINUX_MFD_MACSMC_H

#include <linux/soc/apple/rtkit.h>

/**
 * typedef smc_key - Alias for u32 to be used for SMC keys
 *
 * SMC keys are 32bit integers containing packed ASCII characters in natural
 * integer order, i.e. 0xAABBCCDD, which represent the FourCC ABCD.
 * The SMC driver is designed with this assumption and ensures the right
 * endianness is used when these are stored to memory and sent to or received
 * from the actual SMC firmware (which can be done in either shared memory or
 * as 64bit mailbox message on Apple Silicon).
 * Internally, SMC stores these keys in a table sorted lexicographically and
 * allows resolving an index into this table to the corresponding SMC key.
 * Thus, storing keys as u32 is very convenient as it allows to e.g. use
 * normal comparison operators which directly map to the natural order used
 * by SMC firmware.
 *
 * This simple type alias is introduced to allow easy recognition of SMC key
 * variables and arguments.
 */
typedef u32 smc_key;

/**
 * SMC_KEY - Convert FourCC SMC keys in source code to smc_key
 *
 * This macro can be used to easily define FourCC SMC keys in source code
 * and convert these to u32 / smc_key, e.g. SMC_KEY(NTAP) will expand to
 * 0x4e544150.
 */
#define SMC_KEY(s) (smc_key)(_SMC_KEY(#s))
#define _SMC_KEY(s) (((s)[0] << 24) | ((s)[1] << 16) | ((s)[2] << 8) | (s)[3])

#define APPLE_SMC_READABLE BIT(7)
#define APPLE_SMC_WRITABLE BIT(6)
#define APPLE_SMC_FUNCTION BIT(4)

/**
 * struct apple_smc_key_info - information for a SMC key as returned by SMC
 * @size: size of the buffer associated with this key
 * @type_code: FourCC code indicating the type for this key.
 *             Known types:
 *              ch8*: ASCII string
 *              flag: boolean, 1 or 0
 *              flt: 32-bit single-precision IEEE 754 float
 *              hex: binary data
 *              ioft: 64bit unsigned fixed-point intger (48.16)
 *              si8, ui8, si16, ui16, si32, ui32, si64, ui64: signed/unsigned 8-/16-/32-/64-bit integer
 * @flags: bitfield encoding flags (APPLE_SMC_{READABLE,WRITABLE,FUNCTION})
 */
struct apple_smc_key_info {
	u8 size;
	u32 type_code;
	u8 flags;
};

/**
 * struct apple_smc
 * @dev: underlying device struct for the physical backend device
 * @key_count: number of available SMC keys
 * @first_key: first valid SMC key
 * @last_key: last valid SMC key
 * @event_handlers: notifier call chain for events received from SMC
 * @rtk: pointer to Apple RTKit instance
 * @init_done: completion for initialization
 * @initialized: flag indicating if SMC is initialized
 * @alive: flag indicating if SMC is alive
 * @sram: pointer to SRAM resource
 * @sram_base: SRAM base address
 * @shmem: RTKit shared memory structure for SRAM
 * @msg_id: current message id for commands, will be incremented for each command
 * @atomic_mode: flag set when atomic mode is entered
 * @atomic_pending: flag indicating pending atomic command
 * @cmd_done: completion for command execution in non-atomic mode
 * @cmd_ret: return value from SMC for last command
 * @mutex: mutex for non-atomic mode
 * @lock: spinlock for atomic mode
 */
struct apple_smc {
	struct device *dev;

	u32 key_count;
	smc_key first_key;
	smc_key last_key;

	struct blocking_notifier_head event_handlers;

	struct apple_rtkit *rtk;

	struct completion init_done;
	bool initialized;
	bool alive;

	struct resource *sram;
	void __iomem *sram_base;
	struct apple_rtkit_shmem shmem;

	unsigned int msg_id;

	bool atomic_mode;
	bool atomic_pending;
	struct completion cmd_done;
	u64 cmd_ret;

	struct mutex mutex;
	spinlock_t lock;
};

/**
 * apple_smc_read - read size bytes from given SMC key into buf
 * @smc: pointer to apple_smc struct
 * @key: smc_key to be read
 * @buf: buffer into which size bytes of data will be read from SMC
 * @size: number of bytes to be read into buf
 *
 * Return: Zero on success, negative errno on error
 */
int apple_smc_read(struct apple_smc *smc, smc_key key, void *buf, size_t size);

/**
 * apple_smc_write - write size bytes into given SMC key from buf
 * @smc: pointer to apple_smc struct
 * @key: smc_key data will be written to
 * @buf: buffer from which size bytes of data will be written to SMC
 * @size: number of bytes to be written
 *
 * Return: Zero on success, negative errno on error
 */
int apple_smc_write(struct apple_smc *smc, smc_key key, void *buf, size_t size);

/**
 * apple_smc_enter_atomic - enter atomic mode to be able to use apple_smc_write_atomic
 * @smc: pointer to apple_smc struct
 *
 * This function switches the SMC backend to atomic mode which allows the
 * use of apple_smc_write_atomic while disabling *all* other functions.
 * This is only used for shutdown/reboot which requires writing to a SMC
 * key from atomic context.
 *
 * Return: Zero on success, negative errno on error
 */
int apple_smc_enter_atomic(struct apple_smc *smc);

/**
 * apple_smc_write_atomic - write size bytes into given SMC key from buf without sleeping
 * @smc: pointer to apple_smc struct
 * @key: smc_key data will be written to
 * @buf: buffer from which size bytes of data will be written to SMC
 * @size: number of bytes to be written
 *
 * Note that this function will fail if apple_smc_enter_atomic hasn't been
 * called before.
 *
 * Return: Zero on success, negative errno on error
 */
int apple_smc_write_atomic(struct apple_smc *smc, smc_key key, void *buf, size_t size);

/**
 * apple_smc_rw - write and then read using the given SMC key
 * @smc: pointer to apple_smc struct
 * @key: smc_key data will be written to
 * @wbuf: buffer from which size bytes of data will be written to SMC
 * @wsize: number of bytes to be written
 * @rbuf: buffer to which size bytes of data will be read from SMC
 * @rsize: number of bytes to be read
 *
 * Return: Zero on success, negative errno on error
 */
int apple_smc_rw(struct apple_smc *smc, smc_key key, void *wbuf, size_t wsize,
		 void *rbuf, size_t rsize);

/**
 * apple_smc_get_key_by_index - given an index return the corresponding SMC key
 * @smc: pointer to apple_smc struct
 * @index: index to be resolved
 * @key: buffer for SMC key to be returned
 *
 * Return: Zero on success, negative errno on error
 */
int apple_smc_get_key_by_index(struct apple_smc *smc, int index, smc_key *key);

/**
 * apple_smc_get_key_info - get key information from SMC
 * @smc: pointer to apple_smc struct
 * @key: key to acquire information for
 * @info: pointer to struct apple_smc_key_info which will be filled
 *
 * Return: Zero on success, negative errno on error
 */
int apple_smc_get_key_info(struct apple_smc *smc, smc_key key, struct apple_smc_key_info *info);

/**
 * apple_smc_find_first_key_index - find index of first SMC key bigger or equal to key
 * @smc: pointer to apple_smc struct
 * @key: smc_key to be found
 *
 * SMC keys are represented using either FourCC (which is stored as
 * uint32_t / smc_key in this driver) or an index into the table of available
 * keys which is sorted lexicographically.
 * This function takes a FourCC key and uses binary search to find the
 * index of the first SMC key that is lexicographically equal or bigger than the
 * given input.
 * This is required for e.g. the GPIO driver: GPIO keys start with gP
 * and the driver has to find the first such key (by calling this function with
 * key = SMC_KEY(gP00)) to be able to enumerate and register all available GPIOs
 * at probe time.
 *
 * Return: Index of the first smc key that's bigger or equal to the given key.
 *
 * If the key is smaller than the first available key zero will be returned.
 * If the key is bigger than the last available key smc->key_count (i.e. an out
 * of bounds key) will be returned.
 */
int apple_smc_find_first_key_index(struct apple_smc *smc, smc_key key);

/**
 * apple_smc_key_exists - check if the given SMC key exists
 * @smc: pointer to apple_smc struct
 * @key: smc_key to be checked
 */
static inline bool apple_smc_key_exists(struct apple_smc *smc, smc_key key)
{
	return apple_smc_get_key_info(smc, key, NULL) >= 0;
}

#define APPLE_SMC_TYPE_OPS(type) \
	static inline int apple_smc_read_##type(struct apple_smc *smc, smc_key key, type *p) \
	{ \
		int ret = apple_smc_read(smc, key, p, sizeof(*p)); \
		return (ret < 0) ? ret : ((ret != sizeof(*p)) ? -EINVAL : 0); \
	} \
	static inline int apple_smc_write_##type(struct apple_smc *smc, smc_key key, type p) \
	{ \
		return apple_smc_write(smc, key, &p, sizeof(p)); \
	} \
	static inline int apple_smc_write_##type##_atomic(struct apple_smc *smc, smc_key key, type p) \
	{ \
		return apple_smc_write_atomic(smc, key, &p, sizeof(p)); \
	} \
	static inline int apple_smc_rw_##type(struct apple_smc *smc, smc_key key, \
					      type w, type *r) \
	{ \
		int ret = apple_smc_rw(smc, key, &w, sizeof(w), r, sizeof(*r)); \
		return (ret < 0) ? ret : ((ret != sizeof(*r)) ? -EINVAL : 0); \
	}

APPLE_SMC_TYPE_OPS(u64)
APPLE_SMC_TYPE_OPS(u32)
APPLE_SMC_TYPE_OPS(u16)
APPLE_SMC_TYPE_OPS(u8)
APPLE_SMC_TYPE_OPS(s64)
APPLE_SMC_TYPE_OPS(s32)
APPLE_SMC_TYPE_OPS(s16)
APPLE_SMC_TYPE_OPS(s8)

static inline int apple_smc_read_flag(struct apple_smc *smc, smc_key key, bool *flag)
{
	u8 val;
	int ret = apple_smc_read_u8(smc, key, &val);

	if (ret < 0)
		return ret;

	*flag = val ? true : false;
	return ret;
}

static inline int apple_smc_write_flag(struct apple_smc *smc, smc_key key, bool state)
{
	return apple_smc_write_u8(smc, key, state ? 1 : 0);
}

static inline int apple_smc_write_flag_atomic(struct apple_smc *smc, smc_key key, bool state)
{
	return apple_smc_write_u8_atomic(smc, key, state ? 1 : 0);
}

/**
 * apple_smc_read_f32_scaled - read a float value from SMC and scale to a regular integer
 * @smc: pointer to apple_smc struct
 * @key: key to be read
 * @p: pointer to integer that will be overwritten with the read value
 * @scale: target scale
 *
 * Read a float value from the given SMC key and scale it to the given order
 * of magnitude. If the value is smaller than the given scale zero will be
 * used. If the value overflows an integer at the given scale p will be set to
 * U64_MAX.
 *
 * This is useful for e.g. reading power consumption, which is reported by SMC
 * in Watt (W) as a floating point number, and scale it to uW like so:
 *
 *     apple_smc_read_f32_scaled(smc, SMC_KEY(PSTR), &power_uW, 1000000);
 *
 * At that scale, noise starts to dominate the power measurements anyway and
 * there's no reason to deal with floats.
 *
 * Return: Zero on success, negative errno on error
 */
int apple_smc_read_f32_scaled(struct apple_smc *smc, smc_key key, int *p, int scale);

/**
 * apple_smc_read_ioft_scaled - read a 48.16 fixed point from SMC and scale to a regular integer
 * @smc: pointer to apple_smc struct
 * @key: key to be read
 * @p: pointer to integer that will be overwritten with the read value
 * @scale: target scale
 *
 * Read a 48.16 fixed point value from the given SMC key and scale it to the
 * given order of magnitude. If the value is smaller than the given scale zero
 * will be used. If the value overflows an integer at the given scale p will be
 * set to U64_MAX.
 *
 * This is useful for e.g. reading temperature, which is reported by SMC in
 * degrees as a 48.16 fixed point number, and scale it to milli-degrees Celsius
 * like so:
 *
 *     apple_smc_read_ioft_scaled(smc, SMC_KEY(TR0Z), &temperature_mC, 1000);
 *
 * Reporting temperature with more precision runs into measurements errors and
 * isn't very useful such that using a simple integer instead of fixed point
 * numbers is reasonable.
 *
 * Return: Zero on success, negative errno on error
 */
int apple_smc_read_ioft_scaled(struct apple_smc *smc, smc_key key, u64 *p, int scale);

#endif
