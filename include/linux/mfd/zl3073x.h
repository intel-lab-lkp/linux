/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __LINUX_MFD_ZL3073X_H
#define __LINUX_MFD_ZL3073X_H

#include <linux/device.h>
#include <linux/regmap.h>

struct zl3073x_dev {
	struct device		*dev;
	struct regmap		*regmap;
	struct mutex		lock;
};

/**
 * zl3073x_lock - Lock the device
 * @zldev: device structure pointer
 *
 * Caller has to take this lock when it needs to access device registers.
 */
static inline void zl3073x_lock(struct zl3073x_dev *zldev)
{
	mutex_lock(&zldev->lock);
}

/**
 * zl3073x_unlock - Unlock the device
 * @zldev: device structure pointer
 *
 * Caller unlocks the device when it does not need to access device
 * registers anymore.
 */
static inline void zl3073x_unlock(struct zl3073x_dev *zldev)
{
	mutex_unlock(&zldev->lock);
}

DEFINE_GUARD(zl3073x, struct zl3073x_dev *, zl3073x_lock(_T),
	     zl3073x_unlock(_T));

int zl3073x_read_reg(struct zl3073x_dev *zldev, unsigned int reg,
		     unsigned int len, void *value);

int zl3073x_write_reg(struct zl3073x_dev *zldev, unsigned int reg,
		      unsigned int len, const void *value);

/**
 * __ZL3073X_REG_DEF - Define a device register helpers
 * @_name: register name
 * @_addr: register address
 * @_len: size of register value in bytes
 * @_type: type of register value
 *
 * The macro defines helper functions for particular device register
 * to access it.
 *
 * Example:
 * __ZL3073X_REG_DEF(sample_reg, 0x1234, 4, u32)
 *
 * generates static inline functions:
 * int zl3073x_read_sample_reg(struct zl3073x_dev *dev, u32 *value);
 * int zl3073x_write_sample_reg(struct zl3073x_dev *dev, u32 value);
 *
 * Note that these functions have to be called with the device lock
 * taken.
 */
#define __ZL3073X_REG_DEF(_name, _addr, _len, _type)			\
typedef _type zl3073x_##_name##_t;					\
static inline								\
int zl3073x_read_##_name(struct zl3073x_dev *zldev, _type * value)	\
{									\
	return zl3073x_read_reg(zldev, _addr, _len, value);		\
}									\
static inline								\
int zl3073x_write_##_name(struct zl3073x_dev *zldev, _type value)	\
{									\
	return zl3073x_write_reg(zldev, _addr, _len, &value);		\
}

/**
 * __ZL3073X_REG_IDX_DEF - Define an indexed device register helpers
 * @_name: register name
 * @_addr: register address
 * @_len: size of register value in bytes
 * @_type: type of register value
 * @_num: number of register instances
 * @_stride: address stride between instances
 *
 * The macro defines helper functions for particular indexed device
 * register to access it.
 *
 * Example:
 * __ZL3073X_REG_IDX_DEF(sample_reg, 0x1234, 2, u16, 4, 0x10)
 *
 * generates static inline functions:
 * int zl3073x_read_sample_reg(struct zl3073x_dev *dev, unsigned int idx,
 *			       u32 *value);
 * int zl3073x_write_sample_reg(struct zl3073x_dev *dev, unsigned int idx,
 *				u32 value);
 *
 * Note that these functions have to be called with the device lock
 * taken.
 */
#define __ZL3073X_REG_IDX_DEF(_name, _addr, _len, _type, _num, _stride)	\
typedef _type zl3073x_##_name##_t;					\
static inline								\
int zl3073x_read_##_name(struct zl3073x_dev *zldev, unsigned int idx,	\
			 _type * value)					\
{									\
	WARN_ON(idx >= (_num));						\
	return zl3073x_read_reg(zldev, (_addr) + idx * (_stride), _len,	\
				value);					\
}									\
static inline								\
int zl3073x_write_##_name(struct zl3073x_dev *zldev, unsigned int idx,	\
			  _type value)					\
{									\
	WARN_ON(idx >= (_num));						\
	return zl3073x_write_reg(zldev, (_addr) + idx * (_stride),	\
				 _len, &value);				\
}

/*
 * Add register definition shortcuts for 8, 16, 32 and 48 bits
 */
#define ZL3073X_REG8_DEF(_name, _addr)	__ZL3073X_REG_DEF(_name, _addr, 1, u8)
#define ZL3073X_REG16_DEF(_name, _addr)	__ZL3073X_REG_DEF(_name, _addr, 2, u16)
#define ZL3073X_REG32_DEF(_name, _addr)	__ZL3073X_REG_DEF(_name, _addr, 4, u32)
#define ZL3073X_REG48_DEF(_name, _addr)	__ZL3073X_REG_DEF(_name, _addr, 6, u64)

/*
 * Add indexed register definition shortcuts for 8, 16, 32 and 48 bits
 */
#define ZL3073X_REG8_IDX_DEF(_name, _addr, _num, _stride)		\
	__ZL3073X_REG_IDX_DEF(_name, _addr, 1, u8, _num, _stride)

#define ZL3073X_REG16_IDX_DEF(_name, _addr, _num, _stride)		\
	__ZL3073X_REG_IDX_DEF(_name, _addr, 2, u16, _num, _stride)

#define ZL3073X_REG32_IDX_DEF(_name, _addr, _num, _stride)		\
	__ZL3073X_REG_IDX_DEF(_name, _addr, 4, u32, _num, _stride)

#define ZL3073X_REG48_IDX_DEF(_name, _addr, _num, _stride)		\
	__ZL3073X_REG_IDX_DEF(_name, _addr, 6, u64, _num, _stride)

/**
 * zl3073x_wait_clear_bits - wait for specific bits to be cleared
 * _zldev: pointer to device structure
 * _reg: register name
 * _bits: bits that should be cleared
 * _index: optional index for indexed register
 *
 * The macro waits up to @READ_TIMEOUT_US microseconds for @_bits in @_reg
 * to be cleared.
 *
 * Returns:
 * -ETIMEDOUT: if timeout occurred
 *         <0: for other errors occurred during communication
 *          0: success
 */
#define READ_SLEEP_US	10
#define READ_TIMEOUT_US	2000000
#define zl3073x_wait_clear_bits(_zldev, _reg, _bits, _index...)		\
({									\
	zl3073x_##_reg##_t __val;					\
	int __rc;							\
	if (read_poll_timeout(zl3073x_read_##_reg, __rc,		\
			      __rc || !((_bits) & __val),		\
			      READ_SLEEP_US, READ_TIMEOUT_US, false,	\
			      _zldev, ##_index, &__val))		\
		__rc = -ETIMEDOUT;					\
	__rc;								\
})

/*
 * Mailbox operations
 */
int zl3073x_mb_dpll_read(struct zl3073x_dev *zldev, u8 index);
int zl3073x_mb_dpll_write(struct zl3073x_dev *zldev, u8 index);
int zl3073x_mb_output_read(struct zl3073x_dev *zldev, u8 index);
int zl3073x_mb_output_write(struct zl3073x_dev *zldev, u8 index);
int zl3073x_mb_ref_read(struct zl3073x_dev *zldev, u8 index);
int zl3073x_mb_ref_write(struct zl3073x_dev *zldev, u8 index);
int zl3073x_mb_synth_read(struct zl3073x_dev *zldev, u8 index);
int zl3073x_mb_synth_write(struct zl3073x_dev *zldev, u8 index);

#endif /* __LINUX_MFD_ZL3073X_H */
