/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Honeywell TruStability HSC Series pressure/temperature sensor
 *
 * Copyright (c) 2023 Petre Rodan <petre.rodan@subdimension.ro>
 *
 */

#ifndef _HSC030PA_H
#define _HSC030PA_H

#define  HSC_REG_MEASUREMENT_RD_SIZE  4         // get all conversions in one go since transfers are not address-based
#define            HSC_RANGE_STR_LEN  6

struct hsc_chip_data;

struct hsc_data {
	void *client;                           // either i2c or spi kernel interface struct for current dev
	const struct hsc_chip_data *chip;
	struct mutex lock;                      // lock protecting chip reads
	int (*xfer)(struct hsc_data *data);    // function that implements the chip reads
	bool is_valid;                          // false if last transfer has failed
	unsigned long last_update;              // time of last successful conversion
	u8 buffer[HSC_REG_MEASUREMENT_RD_SIZE]; // raw conversion data
	char range_str[HSC_RANGE_STR_LEN];	// range as defined by the chip nomenclature - ie "030PA" or "NA"
	s32 pmin;                               // min pressure limit
	s32 pmax;                               // max pressure limit
	u32 outmin;                             // minimum raw pressure in counts (based on transfer function)
	u32 outmax;                             // maximum raw pressure in counts (based on transfer function)
	u32 function;                           // transfer function
	s64 p_scale;                            // pressure scale
	s32 p_scale_nano;                       // pressure scale, decimal places
	s64 p_offset;                           // pressure offset
	s32 p_offset_nano;                      // pressure offset, decimal places
};

struct hsc_chip_data {
	bool (*valid)(struct hsc_data *data);  // function that checks the two status bits
	const struct iio_chan_spec *channels;   // channel specifications
	u8 num_channels;                        // pressure and temperature channels
};

enum hsc_func_id {
	HSC_FUNCTION_A,
	HSC_FUNCTION_B,
	HSC_FUNCTION_C,
	HSC_FUNCTION_F
};

enum hsc_variant {
	HSC,
	SSC
};

int hsc_probe(struct iio_dev *indio_dev, struct device *dev,
	      const char *name, int type);
void hsc_remove(struct iio_dev *indio_dev);

#endif
