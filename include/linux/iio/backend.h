/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _IIO_BACKEND_H_
#define _IIO_BACKEND_H_

#include <linux/types.h>

struct iio_backend;
struct device;

enum iio_backend_data_type {
	IIO_BACKEND_TWOS_COMPLEMENT,
	IIO_BACKEND_OFFSET_BINARY,
	IIO_BACKEND_DATA_TYPE_MAX
};

/**
 * struct iio_backend_data_fmt - Backend data format
 * @type:		Data type.
 * @sign_extend:	Bool to tell if the data is sign extended.
 * @enable:		Enable/Disable the data format module. If disabled,
 *			not formatting will happen.
 */
struct iio_backend_data_fmt {
	enum iio_backend_data_type type;
	bool sign_extend;
	bool enable;
};

/**
 * struct iio_backend_ops - operations structure for an iio_backend
 * @enable:		Enable backend.
 * @disable:		Disable backend.
 * @chan_enable:	Enable one channel.
 * @chan_disable:	Disable one channel.
 * @data_format_set:	Configure the data format for a specific channel.
 **/
struct iio_backend_ops {
	int (*enable)(struct iio_backend *back);
	void (*disable)(struct iio_backend *back);
	int (*chan_enable)(struct iio_backend *back, unsigned int chan);
	int (*chan_disable)(struct iio_backend *back, unsigned int chan);
	int (*data_format_set)(struct iio_backend *conv, unsigned int chan,
			       const struct iio_backend_data_fmt *data);
};

int iio_backend_chan_disable(struct iio_backend *back, unsigned int chan);
int iio_backend_chan_enable(struct iio_backend *back, unsigned int chan);
void iio_backend_disable(struct iio_backend *back);
int iio_backend_enable(struct iio_backend *back);
int iio_backend_data_format_set(struct iio_backend *back, unsigned int chan,
				const struct iio_backend_data_fmt *data);

void *iio_backend_get_priv(const struct iio_backend *conv);
struct iio_backend *devm_iio_backend_get(struct device *dev, const char *name);
int devm_iio_backend_register(struct device *dev,
			      const struct iio_backend_ops *ops, void *priv);

#endif
