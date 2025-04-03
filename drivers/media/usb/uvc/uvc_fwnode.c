// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *      uvc_fwnode.c  --  USB Video Class driver
 *
 *      Copyright 2025 Google LLC
 */

#include <linux/kernel.h>
#include <linux/usb/uvc.h>
#include <media/v4l2-fwnode.h>
#include "uvcvideo.h"

static int uvc_fwnode_get_cur(struct uvc_device *dev, struct uvc_entity *entity,
			      u8 cs, void *data, u16 size)
{
	if (size < 1)
		return -EINVAL;

	switch (entity->fwnode.props.orientation) {
	case V4L2_FWNODE_ORIENTATION_FRONT:
		*(u8 *)data = V4L2_CAMERA_ORIENTATION_FRONT;
		break;
	case V4L2_FWNODE_ORIENTATION_BACK:
		*(u8 *)data = V4L2_CAMERA_ORIENTATION_BACK;
		break;
	default:
		*(u8 *)data = V4L2_CAMERA_ORIENTATION_EXTERNAL;
	}

	return 0;
}

static int uvc_fwnode_get_info(struct uvc_device *dev,
			       struct uvc_entity *entity, u8 cs, u8 *caps)
{
	*caps = UVC_CONTROL_CAP_GET;
	return 0;
}

int uvc_fwnode_parse(struct uvc_device *dev)
{
	static const u8 uvc_fwnode_guid[] = UVC_GUID_FWNODE;
	struct v4l2_fwnode_device_properties props;
	struct uvc_entity *unit;
	int ret;

	ret = v4l2_fwnode_device_parse(&dev->udev->dev, &props);
	if (ret)
		return dev_err_probe(&dev->intf->dev, ret,
				     "Can't parse fwnode\n");

	if (props.orientation == V4L2_FWNODE_PROPERTY_UNSET)
		return 0;

	unit = uvc_alloc_entity(UVC_FWNODE_UNIT, UVC_FWNODE_UNIT_ID, 0, 1);
	if (!unit)
		return -ENOMEM;

	memcpy(unit->guid, uvc_fwnode_guid, sizeof(unit->guid));
	unit->fwnode.props = props;
	unit->fwnode.bControlSize = 1;
	unit->fwnode.bmControls = (u8 *)unit + sizeof(*unit);
	unit->fwnode.bmControls[0] = 1;
	unit->get_cur = uvc_fwnode_get_cur;
	unit->get_info = uvc_fwnode_get_info;
	strscpy(unit->name, "FWNODE", sizeof(unit->name));

	list_add_tail(&unit->list, &dev->entities);

	dev->fwnode_unit = unit;

	return 0;
}
