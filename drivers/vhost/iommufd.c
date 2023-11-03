#include <linux/vdpa.h>
#include <linux/iommufd.h>

#include "vhost.h"

MODULE_IMPORT_NS(IOMMUFD);

int vdpa_iommufd_bind(struct vdpa_device *vdpa, struct iommufd_ctx *ictx,
		      u32 *ioas_id, u32 *device_id)
{
	int ret;

	vhost_vdpa_lockdep_assert_held(vdpa);

	/*
        * If the driver doesn't provide this op then it means the device does
        * not do DMA at all. So nothing to do.
        */
	if (!vdpa->config->bind_iommufd)
		return 0;
	ret = vdpa->config->bind_iommufd(vdpa, ictx, device_id);
	if (ret)
		return ret;

	return 0;
}

void vdpa_iommufd_unbind(struct vdpa_device *vdpa)
{
	vhost_vdpa_lockdep_assert_held(vdpa);

	if (vdpa->config->unbind_iommufd)
		vdpa->config->unbind_iommufd(vdpa);
}

int vdpa_iommufd_physical_bind(struct vdpa_device *vdpa,
			       struct iommufd_ctx *ictx, u32 *out_device_id)
{
	struct device *dma_dev = vdpa_get_dma_dev(vdpa);
	struct iommufd_device *idev;

	idev = iommufd_device_bind(ictx, dma_dev, out_device_id);
	if (IS_ERR(idev))
		return PTR_ERR(idev);
	vdpa->iommufd_device = idev;
	return 0;
}
EXPORT_SYMBOL_GPL(vdpa_iommufd_physical_bind);

void vdpa_iommufd_physical_unbind(struct vdpa_device *vdpa)
{
	vhost_vdpa_lockdep_assert_held(vdpa);

	if (vdpa->iommufd_attached) {
		iommufd_device_detach(vdpa->iommufd_device);
		vdpa->iommufd_attached = false;
	}
	iommufd_device_unbind(vdpa->iommufd_device);
	vdpa->iommufd_device = NULL;
}
EXPORT_SYMBOL_GPL(vdpa_iommufd_physical_unbind);

int vdpa_iommufd_physical_attach_ioas(struct vdpa_device *vdpa,
				      u32 *iommufd_ioasid)
{
	int rc;

	vhost_vdpa_lockdep_assert_held(vdpa);

	if (WARN_ON(!vdpa->iommufd_device))
		return -EINVAL;

	if (vdpa->iommufd_attached)
		rc = iommufd_device_replace(vdpa->iommufd_device,
					    iommufd_ioasid);
	else
		rc = iommufd_device_attach(vdpa->iommufd_device,
					   iommufd_ioasid);
	if (rc)
		return rc;
	vdpa->iommufd_attached = true;

	return 0;
}

EXPORT_SYMBOL_GPL(vdpa_iommufd_physical_attach_ioas);
int vdpa_iommufd_physical_detach_ioas(struct vdpa_device *vdpa)
{
	vhost_vdpa_lockdep_assert_held(vdpa);

	if (WARN_ON(!vdpa->iommufd_device) || !vdpa->iommufd_attached)
		return -1;

	iommufd_device_detach(vdpa->iommufd_device);
	vdpa->iommufd_attached = false;
	return 0;
}
EXPORT_SYMBOL_GPL(vdpa_iommufd_physical_detach_ioas);

static void vdpa_emulated_unmap(void *data, unsigned long iova,
				unsigned long length)
{
	struct vdpa_device *vdpa = data;
	/* todo: need to unmap the iova-lenth in all ASID*/

	//	vdpa->config->dma_unmap(vdpa, 0, iova, length);
}

static const struct iommufd_access_ops vdpa_user_ops = {
	.needs_pin_pages = 1,
	.unmap = vdpa_emulated_unmap,
};

int vdpa_iommufd_emulated_bind(struct vdpa_device *vdpa,
			       struct iommufd_ctx *ictx, u32 *out_device_id)
{
	vhost_vdpa_lockdep_assert_held(vdpa);

	struct iommufd_access *user;

	user = iommufd_access_create(ictx, &vdpa_user_ops, vdpa, out_device_id);
	if (IS_ERR(user))
		return PTR_ERR(user);
	vdpa->iommufd_access = user;
	return 0;
}
EXPORT_SYMBOL_GPL(vdpa_iommufd_emulated_bind);

void vdpa_iommufd_emulated_unbind(struct vdpa_device *vdpa)
{
	vhost_vdpa_lockdep_assert_held(vdpa);

	if (vdpa->iommufd_access) {
		iommufd_access_destroy(vdpa->iommufd_access);
		vdpa->iommufd_attached = false;
		vdpa->iommufd_access = NULL;
	}
}
EXPORT_SYMBOL_GPL(vdpa_iommufd_emulated_unbind);

int vdpa_iommufd_emulated_attach_ioas(struct vdpa_device *vdpa,
				      u32 *iommufd_ioasid)
{
	int rc;

	struct iommufd_access *user;

	vhost_vdpa_lockdep_assert_held(vdpa);

	if (vdpa->iommufd_attached) {
		rc = iommufd_access_replace(vdpa->iommufd_access,
					    *iommufd_ioasid);
	} else {
		rc = iommufd_access_attach(vdpa->iommufd_access,
					   *iommufd_ioasid);
	}
	user = vdpa->iommufd_access;

	if (rc)
		return rc;
	vdpa->iommufd_attached = true;
	return 0;
}
EXPORT_SYMBOL_GPL(vdpa_iommufd_emulated_attach_ioas);

int vdpa_iommufd_emulated_detach_ioas(struct vdpa_device *vdpa)
{
	vhost_vdpa_lockdep_assert_held(vdpa);

	if (WARN_ON(!vdpa->iommufd_access) || !vdpa->iommufd_attached)
		return -1;

	iommufd_access_detach(vdpa->iommufd_access);
	vdpa->iommufd_attached = false;

	return 0;
}
EXPORT_SYMBOL_GPL(vdpa_iommufd_emulated_detach_ioas);
