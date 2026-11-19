// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 ARM Limited.
 */

#include <linux/errno.h>
#include <media/videobuf2-dma-sg.h>

#include "mali-vpu-buffer.h"
#include "mali-vpu-session.h"

static int mali_vpu_buffer_alloc_planes(struct mali_vpu_buffer *vpu_buf,
					struct sg_table **sgt)
{
	u32 i;
	int ret;

	for (i = 0; i < vpu_buf->nplanes; ++i) {
		struct mali_vpu_buffer_plane *plane = &vpu_buf->planes[i];

		plane->pages = mali_vpu_mmu_alloc_pages_sg(vpu_buf->dev, sgt[i], 0);
		if (IS_ERR(plane->pages)) {
			ret = PTR_ERR(plane->pages);
			goto free_pages;
		}
	}

	return 0;

free_pages:
	while (i--)
		mali_vpu_mmu_free_pages(vpu_buf->planes[i].pages);

	return ret;
}

int mali_vpu_vb2_buffer_init(struct vb2_buffer *vb2_buf)
{
	struct vb2_v4l2_buffer *v4l2_buf = to_vb2_v4l2_buffer(vb2_buf);
	struct mali_vpu_buffer *vpu_buf = to_mali_vpu_buffer(v4l2_buf);
	struct mali_vpu_session *sess = vb2_get_drv_priv(vb2_buf->vb2_queue);
	struct sg_table *sgt[MALI_VPU_BUFFER_MAX_PLANES] = { 0 };
	u32 i;
	int ret;

	if (vb2_buf->num_planes > MALI_VPU_BUFFER_MAX_PLANES) {
		dev_err(sess->vpu_dev->dev,
			"buffer has too many planes: %u\n",
			vb2_buf->num_planes);
		return -EINVAL;
	}

	vpu_buf->dev = sess->vpu_dev->dev;
	vpu_buf->mmu = &sess->mmu;
	vpu_buf->nplanes = vb2_buf->num_planes;
	vpu_buf->fw_buffer_id = 0;
	vpu_buf->attr = MALI_VPU_BUF_ATTR_NONE;

	for (i = 0; i < vb2_buf->num_planes; ++i) {
		sgt[i] = vb2_dma_sg_plane_desc(vb2_buf, i);
		if (!sgt[i]) {
			dev_err(sess->vpu_dev->dev,
				"cannot fetch SG descriptor. vb=%p, plane=%u",
				vb2_buf, i);
			return -ENOMEM;
		}
	}

	ret = mali_vpu_buffer_alloc_planes(vpu_buf, sgt);

	return ret;
}

bool mali_vpu_is_buffer_mapped(struct mali_vpu_buffer *vpu_buf)
{
	return (vpu_buf->planes[0].pages &&  vpu_buf->planes[0].pages->va);
}

static size_t mali_vpu_buffer_get_bytesused(struct vb2_buffer *b)
{
	size_t size;
	u32 i;

	for (i = 0, size = 0; i < b->num_planes; i++)
		size += b->planes[i].bytesused;

	return size;
}

static int mali_vpu_buffer_clear_bytesused(struct mali_vpu_buffer *vpu_buf)
{
	struct vb2_buffer *vb2_buf = &vpu_buf->m2m_buf.vb.vb2_buf;
	u32 i;

	for (i = 0; i < vb2_buf->num_planes; i++) {
		vb2_set_plane_payload(vb2_buf, i, 0);
		vb2_buf->planes[i].data_offset = 0;
	}

	return 0;
}

static int mali_vpu_buffer_update_bytesused_to_v4l2(struct mali_vpu_buffer *vpu_buf)
{
	struct vb2_buffer *vb2_buf = &vpu_buf->m2m_buf.vb.vb2_buf;
	u32 i;

	if (vpu_buf->nplanes != vb2_buf->num_planes) {
		dev_dbg(vpu_buf->dev,
			"mali VPU buf and vb2 buf have different num_planes. vpu_buf=%u vb2_buf=%u.\n",
			vpu_buf->nplanes, vb2_buf->num_planes);
		return -EINVAL;
	}

	for (i = 0; i < vpu_buf->nplanes; i++) {
		vb2_buf->planes[i].bytesused =
			vpu_buf->planes[i].payload_size + vpu_buf->planes[i].offset;
		vb2_buf->planes[i].data_offset = vpu_buf->planes[i].offset;
	}

	return 0;
}

static void mali_vpu_buffer_update_flags_from_v4l2(struct vb2_buffer *vb2_buf)
{
	struct vb2_v4l2_buffer *v4l2_buf = to_vb2_v4l2_buffer(vb2_buf);
	struct mali_vpu_buffer *vpu_buf = to_mali_vpu_buffer(v4l2_buf);

	u32 flags = v4l2_buf->flags;

	vpu_buf->flags = 0;

	if (V4L2_TYPE_IS_OUTPUT(vb2_buf->type) && mali_vpu_buffer_get_bytesused(vb2_buf) == 0)
		flags |= V4L2_BUF_FLAG_LAST;

	if (flags & V4L2_BUF_FLAG_LAST)
		vpu_buf->flags |= MALI_VPU_BUFFER_EOS;
}

static void mali_vpu_buffer_update_flags_to_v4l2(struct mali_vpu_buffer *vpu_buf)
{
	struct vb2_v4l2_buffer *v4l2_buf = &vpu_buf->m2m_buf.vb;

	v4l2_buf->flags &= ~(V4L2_BUF_FLAG_ERROR |
			     V4L2_BUF_FLAG_KEYFRAME |
			     V4L2_BUF_FLAG_LAST);

	if (vpu_buf->flags & MALI_VPU_BUFFER_EOS ||
	    (V4L2_TYPE_IS_CAPTURE(vpu_buf->type) &&
	     mali_vpu_buffer_get_bytesused(&vpu_buf->m2m_buf.vb.vb2_buf) == 0))
		v4l2_buf->flags |= V4L2_BUF_FLAG_LAST;

	if (vpu_buf->flags & MALI_VPU_BUFFER_CORRUPT)
		v4l2_buf->flags |= V4L2_BUF_FLAG_ERROR;

	if (V4L2_TYPE_IS_CAPTURE(vpu_buf->type) &&
	    vpu_buf->flags & MALI_VPU_BUFFER_REJECTED)
		mali_vpu_buffer_clear_bytesused(vpu_buf);
}

int mali_vpu_buffer_update_info_from_v4l2(struct vb2_buffer *vb2_buf)
{
	struct vb2_v4l2_buffer *v4l2_buf = to_vb2_v4l2_buffer(vb2_buf);
	struct mali_vpu_buffer *vpu_buf = to_mali_vpu_buffer(v4l2_buf);
	u32 offset;
	u32 i;
	int ret;

	vpu_buf->user_data = vb2_buf->timestamp;
	vpu_buf->type = vb2_buf->type;

	for (i = 0; i < vb2_buf->num_planes; i++) {
		offset = vb2_buf->planes[i].data_offset;

		/* set plane payload size and offset */
		ret = mali_vpu_buffer_set_plane_payload(vpu_buf, i, vb2_buf->planes[i].bytesused,
							offset);
		if (ret)
			return ret;
	}

	/* update flags */
	mali_vpu_buffer_update_flags_from_v4l2(vb2_buf);

	return 0;
}

enum vb2_buffer_state mali_vpu_buffer_update_info_to_v4l2(struct mali_vpu_buffer *vpu_buf)
{
	struct vb2_v4l2_buffer *v4l2_buf = &vpu_buf->m2m_buf.vb;
	struct vb2_buffer *vb2_buf = &v4l2_buf->vb2_buf;
	int ret;

	vb2_buf->timestamp = vpu_buf->user_data;

	if (V4L2_TYPE_IS_CAPTURE(vpu_buf->type)) {
		ret = mali_vpu_buffer_update_bytesused_to_v4l2(vpu_buf);
		if (ret)
			goto error;
	}

	mali_vpu_buffer_update_flags_to_v4l2(vpu_buf);
	if ((v4l2_buf->flags & V4L2_BUF_FLAG_ERROR) != 0)
		goto error;

	return VB2_BUF_STATE_DONE;

error:
	return VB2_BUF_STATE_ERROR;
}

int mali_vpu_vb2_buffer_prepare(struct vb2_buffer *vb2_buf)
{
	struct mali_vpu_session *sess = vb2_get_drv_priv(vb2_buf->vb2_queue);
	struct mali_vpu_port *port;
	struct v4l2_pix_format_mplane *pix_mp;
	u32 i;

	port = V4L2_TYPE_IS_OUTPUT(vb2_buf->vb2_queue->type) ? &sess->port_src :
							       &sess->port_dst;
	pix_mp = &port->curr_fmt.fmt.pix_mp;

	for (i = 0; i < pix_mp->num_planes; i++) {
		if (vb2_plane_size(vb2_buf, i) < pix_mp->plane_fmt[i].sizeimage)
			return -EINVAL;
	}

	return 0;
}

int mali_vpu_vb2_buffer_out_validate(struct vb2_buffer *vb2_buf)
{
	struct vb2_v4l2_buffer *v4l2_buf = to_vb2_v4l2_buffer(vb2_buf);

	v4l2_buf->field = V4L2_FIELD_NONE;

	return 0;
}

mali_vpu_mmu_va mali_vpu_buffer_plane_va(struct mali_vpu_buffer *vpu_buf, u32 plane)
{
	struct mali_vpu_buffer_plane *vpu_buf_plane;

	if (plane >= vpu_buf->nplanes)
		return 0;

	vpu_buf_plane = &vpu_buf->planes[plane];
	if (!vpu_buf_plane->pages)
		return 0;

	return vpu_buf_plane->pages->va + vpu_buf_plane->pages->offset;
}

static int mali_vpu_buffer_plane_map(struct mali_vpu_buffer *vpu_buf, mali_vpu_mmu_va begin,
				     mali_vpu_mmu_va end, u32 plane)
{
	while (begin < end) {
		struct mali_vpu_buffer_plane *p = &vpu_buf->planes[plane];
		int ret;

		ret = mali_vpu_mmu_map_pages(vpu_buf->mmu, begin, p->pages,
					     MALI_VPU_ATTR_SHARED_RW, MALI_VPU_ACCESS_READ_WRITE);
		if (!ret) {
			dev_dbg(vpu_buf->dev,
				"memory map buffer. buf=%p, plane=%u, va=0x%x, size=%zu.",
				vpu_buf, plane, p->pages->va,
				mali_vpu_mmu_size_pages(p->pages));
			return 0;
		}

		if (ret != -EAGAIN)
			return ret;

		begin += SZ_1M;
	}

	return -ENOMEM;
}

int mali_vpu_buffer_map(struct mali_vpu_buffer *vpu_buf, mali_vpu_mmu_va begin, mali_vpu_mmu_va end)
{
	u32 i;
	int ret;

	for (i = 0; i < vpu_buf->nplanes; i++) {
		struct mali_vpu_buffer_plane *plane = &vpu_buf->planes[i];

		if (plane->pages) {
			ret = mali_vpu_buffer_plane_map(vpu_buf, begin, end, i);
			if (ret) {
				mali_vpu_buffer_unmap(vpu_buf);
				return ret;
			}
		}
	}
	return 0;
}

void mali_vpu_buffer_unmap(struct mali_vpu_buffer *vpu_buf)
{
	u32 i;

	for (i = 0; i < vpu_buf->nplanes; i++) {
		struct mali_vpu_buffer_plane *plane = &vpu_buf->planes[i];

		if (plane->pages && plane->pages->va != 0)
			mali_vpu_mmu_unmap_pages(plane->pages);
	}
}

size_t mali_vpu_buffer_plane_size(struct mali_vpu_buffer *vpu_buf, u32 plane)
{
	struct vb2_buffer *vb2_buf = &vpu_buf->m2m_buf.vb.vb2_buf;

	if (plane >= vb2_buf->num_planes)
		return 0;

	return vb2_plane_size(vb2_buf, plane);
}

int mali_vpu_buffer_sync(struct mali_vpu_buffer *vpu_buf, enum dma_data_direction dir)
{
	u32 i;
	int ret;

	for (i = 0; i < vpu_buf->nplanes; i++) {
		struct mali_vpu_buffer_plane *plane = &vpu_buf->planes[i];

		if (plane->pages) {
			ret = mali_vpu_mmu_sync_pages(plane->pages, dir);
			if (ret)
				return ret;
		}
	}

	return 0;
}

int mali_vpu_buffer_set_plane_payload(struct mali_vpu_buffer *vpu_buf, u32 plane,
				      u32 bytesused, u32 offset)
{
	struct mali_vpu_buffer_plane *p;
	size_t capacity;

	if (plane >= vpu_buf->nplanes)
		return -EINVAL;

	capacity = mali_vpu_buffer_plane_size(vpu_buf, plane);

	if (offset > capacity || bytesused > capacity || offset > bytesused) {
		dev_err(vpu_buf->dev,
			"invalid plane payload. Plane: %u, capacity: %zu, bytesused=%u, offset=%u.\n",
			plane, capacity, bytesused, offset);
		return -EINVAL;
	}

	p = &vpu_buf->planes[plane];

	p->payload_size = bytesused - offset;
	p->offset = offset;

	return 0;
}

int mali_vpu_buffer_set_frame_layout(struct mali_vpu_buffer *vpu_buf)
{
	struct vb2_buffer *vb2_buf = &vpu_buf->m2m_buf.vb.vb2_buf;
	struct mali_vpu_session *sess = vb2_get_drv_priv(vb2_buf->vb2_queue);
	struct mali_vpu_port *port;
	struct v4l2_pix_format_mplane *pix_mp;
	int ret;
	size_t size;
	u32 i;

	port = V4L2_TYPE_IS_OUTPUT(vb2_buf->vb2_queue->type) ? &sess->port_src :
							       &sess->port_dst;

	pix_mp = &port->curr_fmt.fmt.pix_mp;

	vpu_buf->width = pix_mp->width;
	vpu_buf->height = pix_mp->height;
	vpu_buf->format = mali_vpu_find_format(sess, pix_mp->pixelformat, vpu_buf->type);
	if (!vpu_buf->format)
		return -EINVAL;

	if (vpu_buf->nplanes != pix_mp->num_planes)
		return -EINVAL;

	for (i = 0; i < vpu_buf->nplanes; i++) {
		struct mali_vpu_buffer_plane *plane = &vpu_buf->planes[i];

		size = pix_mp->plane_fmt[i].sizeimage;
		plane->stride = pix_mp->plane_fmt[i].bytesperline;
		plane->offset = 0;

		if (!V4L2_TYPE_IS_OUTPUT(vpu_buf->type)) {
			ret = mali_vpu_buffer_set_plane_payload(vpu_buf, i, size, 0);
			if (ret)
				return ret;
		}

		if (plane->payload_size != size) {
			dev_err(vpu_buf->dev,
				"payload size mismatch. Plane: %u, payload_size: %u, actual: %zu.\n",
				i, plane->payload_size, size);
			return -EINVAL;
		}
	}

	return 0;
}

void mali_vpu_vb2_buffer_cleanup(struct vb2_buffer *vb2_buf)
{
	struct vb2_v4l2_buffer *v4l2_buf = to_vb2_v4l2_buffer(vb2_buf);
	struct mali_vpu_buffer *vpu_buf = to_mali_vpu_buffer(v4l2_buf);
	u32 i;

	for (i = 0; i < vpu_buf->nplanes; i++) {
		if (!vpu_buf->planes[i].pages)
			continue;

		mali_vpu_mmu_free_pages(vpu_buf->planes[i].pages);
		vpu_buf->planes[i].pages = NULL;
	}
}
