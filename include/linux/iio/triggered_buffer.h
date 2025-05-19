/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_IIO_TRIGGERED_BUFFER_H_
#define _LINUX_IIO_TRIGGERED_BUFFER_H_

#include <linux/iio/buffer.h>
#include <linux/interrupt.h>

struct iio_dev;
struct iio_dev_attr;
struct iio_buffer_setup_ops;

int iio_triggered_buffer_setup_ext_new(struct iio_dev *indio_dev,
	irqreturn_t (*thread)(int irq, void *p),
	bool timestamp_enabled,
	enum iio_buffer_direction direction,
	const struct iio_buffer_setup_ops *setup_ops,
	const struct iio_dev_attr **buffer_attrs);

int iio_triggered_buffer_setup_ext(struct iio_dev *indio_dev,
	irqreturn_t (*h)(int irq, void *p),
	irqreturn_t (*thread)(int irq, void *p),
	enum iio_buffer_direction direction,
	const struct iio_buffer_setup_ops *setup_ops,
	const struct iio_dev_attr **buffer_attrs);
void iio_triggered_buffer_cleanup(struct iio_dev *indio_dev);

#define iio_triggered_buffer_setup_new(indio_dev, h, timestamp_enabled, setup_ops)	\
	iio_triggered_buffer_setup_ext_new((indio_dev), (h), (timestamp_enabled),	\
					IIO_BUFFER_DIRECTION_IN, (setup_ops),		\
					NULL)

#define iio_triggered_buffer_setup(indio_dev, h, thread, setup_ops)		\
	iio_triggered_buffer_setup_ext((indio_dev), (h), (thread),		\
					IIO_BUFFER_DIRECTION_IN, (setup_ops),	\
					NULL)

int devm_iio_triggered_buffer_setup_ext_new(struct device *dev,
					struct iio_dev *indio_dev,
					irqreturn_t (*thread)(int irq, void *p),
					bool timestamp_enabled,
					enum iio_buffer_direction direction,
					const struct iio_buffer_setup_ops *ops,
					const struct iio_dev_attr **buffer_attrs);

int devm_iio_triggered_buffer_setup_ext(struct device *dev,
					struct iio_dev *indio_dev,
					irqreturn_t (*h)(int irq, void *p),
					irqreturn_t (*thread)(int irq, void *p),
					enum iio_buffer_direction direction,
					const struct iio_buffer_setup_ops *ops,
					const struct iio_dev_attr **buffer_attrs);

#define devm_iio_triggered_buffer_setup_new(dev, indio_dev, thread, timestamp_enabled, setup_ops)	\
	devm_iio_triggered_buffer_setup_ext_new((dev), (indio_dev), (thread), (timestamp_enabled),	\
					    IIO_BUFFER_DIRECTION_IN,					\
					    (setup_ops), NULL)

#define devm_iio_triggered_buffer_setup(dev, indio_dev, h, thread, setup_ops)	\
	devm_iio_triggered_buffer_setup_ext((dev), (indio_dev), (h), (thread),	\
					    IIO_BUFFER_DIRECTION_IN,		\
					    (setup_ops), NULL)

#endif
