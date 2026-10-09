/* SPDX-License-Identifier: GPL-2.0-only */

#define LTC2497_ENABLE			0xA0
#define LTC2497_CONFIG_DEFAULT		LTC2497_ENABLE

/*
 * Worst-case conversion times (datasheet t_CONV max), one per rejection/speed
 * combination.  Both FA/FB rejection and SPD speed shift the conversion time,
 * so both must be correct to avoid reprogramming a conversion mid-flight (-EIO).
 *
 * The simultaneous 50/60Hz 1x value is also used by LTC2496/LTC2497 (no speed
 * or rejection control) and is the power-up default.
 */
#define LTC2497_CONV_TIME_50_60_1X_MS	150ULL	/* t_CONV_1 simult. max 149.9 */
#define LTC2499_CONV_TIME_50_60_2X_MS	76ULL	/* t_CONV_2 simult. max  75.1 */
#define LTC2499_CONV_TIME_50_1X_MS	164ULL	/* t_CONV_1 50Hz    max 163.5 */
#define LTC2499_CONV_TIME_50_2X_MS	82ULL	/* t_CONV_2 50Hz    max  81.9 */
#define LTC2499_CONV_TIME_60_1X_MS	137ULL	/* t_CONV_1 60Hz    max 136.3 */
#define LTC2499_CONV_TIME_60_2X_MS	69ULL	/* t_CONV_2 60Hz    max  68.2 */

/*
 * Sentinel passed as `address` to result_and_measure() to request a
 * temperature conversion instead of a voltage channel.  Valid channel
 * addresses fit in 5 bits (0x00–0x1F), so 0xFF is unambiguous.
 */
#define LTC2497_TEMP_ADDR		0xFF

/* Second config-byte bits (LTC2499 / LTC2493 only) */
#define LTC2499_EN2			BIT(7)	/* enable second config byte */
#define LTC2499_IM			BIT(6)	/* 1 = measure internal temp sensor */
#define LTC2499_FA			BIT(5)	/* line-rejection select bit A */
#define LTC2499_FB			BIT(4)	/* line-rejection select bit B */
#define LTC2499_SPD			BIT(3)	/* 1 = 2x output rate (offset cal off) */

/*
 * Line-frequency rejection modes selected by FA/FB in the second config byte.
 * The datasheet's fourth combination (FA = FB = 1) is reserved, so it is not
 * given an enumerator and is unreachable through this driver.  Applies to both
 * voltage and temperature conversions (unlike SPD, which the part ignores while
 * measuring temperature).
 */
enum ltc2497_rejection {
	LTC2499_REJECT_50_60,	/* FA=0 FB=0: simultaneous 50/60Hz (default) */
	LTC2499_REJECT_50,	/* FA=0 FB=1: 50Hz only */
	LTC2499_REJECT_60,	/* FA=1 FB=0: 60Hz only */
};

struct ltc2497_chip_info {
	const char *name;
	u32 resolution;
	bool has_temp;
	bool has_speed_mode;	/* SPD bit in the 2nd config byte (LTC2499/LTC2493) */
	bool has_rejection_mode;	/* FA/FB bits in the 2nd config byte (LTC2499) */
	bool four_channel;	/* 4 inputs (LTC2493) instead of 16 */
};

struct ltc2497core_driverdata {
	struct regulator *ref;
	ktime_t	time_prev;
	/* lock to protect against multiple access to the device */
	struct mutex lock;
	const struct ltc2497_chip_info	*chip_info;
	u8 addr_prev;
	bool speed_2x;		/* SPD: false = 1x (default), true = 2x */
	enum ltc2497_rejection rejection;	/* FA/FB line-rejection select */
	/*
	 * Conversion time (ms) of the conversion currently in flight.  It is
	 * fixed by the mode active when that conversion was started, which
	 * differs from the newly selected mode for the first read after a
	 * sampling_frequency or filter_mode change.
	 */
	unsigned int conv_time_prev;
	int (*result_and_measure)(struct ltc2497core_driverdata *ddata,
				  u8 address, int *val);
};

int ltc2497core_probe(struct device *dev, struct iio_dev *indio_dev);
void ltc2497core_remove(struct iio_dev *indio_dev);

MODULE_IMPORT_NS("LTC2497");
