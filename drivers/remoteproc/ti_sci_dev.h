/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Texas Instruments TI-SCI Device Controller Helper Functions
 *
 * Copyright (C) 2026 Texas Instruments Incorporated - https://www.ti.com/
 */

#ifndef REMOTEPROC_TI_SCI_DEV_H
#define REMOTEPROC_TI_SCI_DEV_H

#include <linux/soc/ti/ti_sci_protocol.h>
#include <linux/pm_runtime.h>

/**
 * struct ti_sci_dev - structure representing a device control client
 * @sci: cached TI-SCI protocol handle
 * @ops: cached TI-SCI device ops
 * @dev: cached client device pointer
 * @dev_id: TI-SCI device id for the consumer remoteproc device
 */
struct ti_sci_dev {
	const struct ti_sci_handle *sci;
	const struct ti_sci_dev_ops *ops;
	struct device *dev;
	u32 dev_id;
};

static inline
struct ti_sci_dev *ti_sci_dev_of_get_tsd(struct device *dev,
					 const struct ti_sci_handle *sci)
{
	struct ti_sci_dev *tsd;
	int ret;

	tsd = devm_kzalloc(dev, sizeof(*tsd), GFP_KERNEL);
	if (!tsd)
		return ERR_PTR(-ENOMEM);

	ret = of_property_read_u32(dev_of_node(dev), "ti,sci-dev-id",
				   &tsd->dev_id);
	if (ret < 0)
		return ERR_PTR(ret);

	tsd->dev = dev;
	tsd->sci = sci;
	tsd->ops = &sci->ops.dev_ops;

	return tsd;
}

static inline int ti_sci_dev_is_on(struct ti_sci_dev *tsd, bool *r_state,
				   bool *c_state)
{
	int ret;

	ret = tsd->ops->is_on(tsd->sci, tsd->dev_id, r_state, c_state);
	if (ret)
		dev_err(tsd->dev, "ti-sci device is_on failed: %d\n", ret);
	return ret;
}

static inline int ti_sci_dev_get_device(struct ti_sci_dev *tsd)
{
	int ret;

	ret = tsd->ops->get_device(tsd->sci, tsd->dev_id);
	if (ret)
		dev_err(tsd->dev, "ti-sci device get failed: %d\n", ret);
	return ret;
}

static inline int ti_sci_dev_put_device(struct ti_sci_dev *tsd)
{
	int ret;

	ret = tsd->ops->put_device(tsd->sci, tsd->dev_id);
	if (ret)
		dev_err(tsd->dev, "ti-sci device put failed: %d\n", ret);
	return ret;
}

#endif /* REMOTEPROC_TI_SCI_DEV_H */
