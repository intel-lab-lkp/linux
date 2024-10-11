/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */

#ifndef BMI270_H_
#define BMI270_H_

#include <linux/regmap.h>
#include <linux/iio/iio.h>

struct device;
struct bmi270_data {
	struct device *dev;
	struct regmap *regmap;
	const struct bmi270_chip_info *chip_info;

	/*
	 * Ensure natural alignment for timestamp if present.
	 * Max length needed: 2 * 3 channels + 4 bytes padding + 8 byte ts.
	 * If fewer channels are enabled, less space may be needed, as
	 * long as the timestamp is still aligned to 8 bytes.
	 */
	__le16 buf[12] __aligned(8);
};

enum bmi270_device_type {
	BMI260,
	BMI270,
};

struct bmi270_chip_info {
	const char *name;
	int chip_id;
	const char *fw_name;
};

extern const struct regmap_config bmi270_regmap_config;
extern const struct bmi270_chip_info bmi270_chip_info[];

int bmi270_core_probe(struct device *dev, struct regmap *regmap,
		      const struct bmi270_chip_info *chip_info);

#endif  /* BMI270_H_ */
