// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0+

/*
 * Scatterlist builder helpers for virtio-media.
 *
 * Copyright (c) 2024-2026 Google LLC.
 */

#include <linux/moduleparam.h>
#include <linux/scatterlist.h>
#include <linux/videodev2.h>
#include <media/videobuf2-memops.h>

#include "uapi/linux/virtio_media.h"
#include "scatterlist_builder.h"
#include "session.h"

/*
 * If set to %true, then the driver will always copy the data passed to the
 * host into the shadow buffer (instead of trying to map the source memory into
 * the SG table directly when possible).
 */
static bool always_use_shadow_buffer;
module_param(always_use_shadow_buffer, bool, 0660);

/* Convert a V4L2 IOCTL into the IOCTL code we can give to the host */
#define VIRTIO_MEDIA_IOCTL_CODE(IOCTL) (((IOCTL) >> _IOC_NRSHIFT) & _IOC_NRMASK)

/**
 * scatterlist_builder_add_descriptor() - Add a descriptor to the chain.
 * @builder: builder to use.
 * @desc_index: index of the descriptor to add.
 *
 * Returns %-ENOSPC if @builder->sgs is already full.
 */
int scatterlist_builder_add_descriptor(struct scatterlist_builder *builder,
				       size_t desc_index)
{
	if (builder->cur_sg >= builder->num_sgs)
		return -ENOSPC;
	builder->sgs[builder->cur_sg++] = &builder->descs[desc_index];

	return 0;
}

/**
 * scatterlist_builder_add_data() - Append arbitrary data to the descriptor
 *                                  chain.
 * @builder: builder to use.
 * @data: pointer to the data to add to the descriptor chain.
 * @len: length of the data to add.
 *
 * @data will either be directly referenced, or copied into the shadow buffer
 * to be referenced from there.
 */
int scatterlist_builder_add_data(struct scatterlist_builder *builder,
				 void *data, size_t len)
{
	const size_t cur_desc = builder->cur_desc;

	if (len == 0)
		return 0;

	if (builder->cur_desc >= builder->num_descs)
		return -ENOSPC;

	if (!always_use_shadow_buffer && virt_addr_valid(data) &&
	    virt_addr_valid(data + len)) {
		/*
		 * If "data" is in the 1:1 physical memory mapping then we can
		 * use a single SG entry and avoid copying.
		 */
		struct page *page = virt_to_page(data);
		size_t offset = (((size_t)data) & ~PAGE_MASK);
		struct scatterlist *next_desc =
			&builder->descs[builder->cur_desc];

		memset(next_desc, 0, sizeof(*next_desc));
		sg_set_page(next_desc, page, len, offset);
		builder->cur_desc++;
	} else if (!always_use_shadow_buffer && is_vmalloc_addr(data)) {
		bool prev_pfn_valid = false;
		unsigned long prev_pfn = 0;

		/*
		 * If "data" has been vmalloc'ed, we need at most one entry per
		 * memory page but can avoid copying.
		 */
		while (len > 0) {
			struct page *page = vmalloc_to_page(data);
			unsigned long cur_pfn = page_to_pfn(page);
			/* All pages but the first will start at offset 0. */
			unsigned long offset =
				(((unsigned long)data) & ~PAGE_MASK);
			size_t len_in_page = min(PAGE_SIZE - offset, len);
			struct scatterlist *next_desc =
				&builder->descs[builder->cur_desc];

			if (builder->cur_desc >= builder->num_descs)
				return -ENOSPC;

			/* Optimize contiguous pages */
			if (prev_pfn_valid && (cur_pfn == prev_pfn + 1)) {
				(next_desc - 1)->length += len_in_page;
			} else {
				memset(next_desc, 0, sizeof(*next_desc));
				sg_set_page(next_desc, page, len_in_page,
					    offset);
				builder->cur_desc++;
			}
			data += len_in_page;
			len -= len_in_page;
			prev_pfn = cur_pfn;
			prev_pfn_valid = true;
		}
	} else {
		/*
		 * As a last resort, copy into the shadow buffer and reference
		 * it with a single SG entry. Calling
		 * scatterlist_builder_retrieve_data() will be necessary to copy
		 * the data written by the device back into @data.
		 */
		void *shadow_buffer =
			builder->shadow_buffer + builder->shadow_buffer_pos;
		struct page *page = virt_to_page(shadow_buffer);
		unsigned long offset =
			(((unsigned long)shadow_buffer) & ~PAGE_MASK);
		struct scatterlist *next_desc =
			&builder->descs[builder->cur_desc];

		if (len >
		    builder->shadow_buffer_size - builder->shadow_buffer_pos)
			return -ENOSPC;

		memcpy(shadow_buffer, data, len);
		memset(next_desc, 0, sizeof(*next_desc));
		sg_set_page(next_desc, page, len, offset);
		builder->cur_desc++;
		builder->shadow_buffer_pos += len;
	}

	sg_mark_end(&builder->descs[builder->cur_desc - 1]);
	return scatterlist_builder_add_descriptor(builder, cur_desc);
}

/**
 * scatterlist_builder_retrieve_data() - Retrieve a response written by the
 *                                       device on the shadow buffer.
 * @builder: builder to use.
 * @sg_index: index of the descriptor to read from.
 * @data: destination for the shadowed data.
 *
 * If the shadow buffer is pointed to by the descriptor at index @sg_index of
 * the chain, then ``sg->length`` bytes are copied back from it into @data.
 * Otherwise nothing is done since the device has written into @data directly.
 *
 * @data must have originally been added by scatterlist_builder_add_data() as
 * the same size as passed to scatterlist_builder_add_data() will be copied
 * back.
 */
int scatterlist_builder_retrieve_data(struct scatterlist_builder *builder,
				      size_t sg_index, void *data)
{
	void *shadow_buf = builder->shadow_buffer;
	struct scatterlist *sg;
	void *kaddr;

	/* We can only retrieve from the range of sgs currently set. */
	if (sg_index >= builder->cur_sg)
		return -ERANGE;

	sg = builder->sgs[sg_index];
	kaddr = pfn_to_kaddr(page_to_pfn(sg_page(sg))) + sg->offset;

	if (kaddr >= shadow_buf &&
	    kaddr < shadow_buf + builder->shadow_buffer_size) {
		if (kaddr + sg->length >
		    shadow_buf + builder->shadow_buffer_size)
			return -EINVAL;

		memcpy(data, kaddr, sg->length);
	}

	return 0;
}

/**
 * scatterlist_builder_add_ioctl_cmd() - Add an ioctl command to the descriptor
 *                                       chain.
 * @builder: builder to use.
 * @session: session on behalf of which the ioctl command is added.
 * @ioctl_code: code of the ioctl to add (i.e. ``VIDIOC_*``).
 */
int scatterlist_builder_add_ioctl_cmd(struct scatterlist_builder *builder,
				      struct virtio_media_session *session,
				      u32 ioctl_code)
{
	struct virtio_media_cmd_ioctl *cmd_ioctl = &session->cmd.ioctl;

	cmd_ioctl->hdr.cmd = VIRTIO_MEDIA_CMD_IOCTL;
	cmd_ioctl->hdr.__reserved = 0;
	cmd_ioctl->session_id = session->id;
	cmd_ioctl->code = VIRTIO_MEDIA_IOCTL_CODE(ioctl_code);

	return scatterlist_builder_add_data(builder, cmd_ioctl,
					    sizeof(*cmd_ioctl));
}

/**
 * scatterlist_builder_add_ioctl_resp() - Add storage to receive an ioctl
 *                                        response to the descriptor chain.
 * @builder: builder to use.
 * @session: session on behalf of which the ioctl response is added.
 */
int scatterlist_builder_add_ioctl_resp(struct scatterlist_builder *builder,
				       struct virtio_media_session *session)
{
	struct virtio_media_resp_ioctl *resp_ioctl = &session->resp.ioctl;

	return scatterlist_builder_add_data(builder, resp_ioctl,
					    sizeof(*resp_ioctl));
}

/**
 * __scatterlist_builder_add_userptr() - Add user pages to @builder.
 * @builder: builder to use.
 * @userptr: pointer to userspace memory that we want to add.
 * @length: length of the data to add.
 * @sg_list: output parameter. Upon success, points to the area of the shadow
 *           buffer containing the array of SG entries to be added to the
 *           descriptor chain.
 * @nents: output parameter. Upon success, contains the number of entries
 *         pointed to by @sg_list.
 *
 * Data referenced by userspace pointers can be potentially large and very
 * scattered, which could overwhelm the descriptor chain if added as-is. For
 * these, we instead build an array of &struct virtio_media_sg_entry in the
 * shadow buffer and reference it using a single descriptor.
 *
 * This function is a helper to perform that. Callers should then add the
 * descriptor to the chain properly.
 *
 * Returns %-EFAULT if @userptr is not a valid user address, which is a case the
 * driver should consider as "normal" operation. All other failures signal a
 * problem with the driver.
 */
static int
__scatterlist_builder_add_userptr(struct scatterlist_builder *builder,
				  unsigned long userptr, unsigned long length,
				  struct virtio_media_sg_entry **sg_list,
				  int *nents)
{
	struct sg_table sg_table = {};
	struct frame_vector *framevec;
	struct scatterlist *sg_iter;
	struct page **pages;
	const unsigned int offset = userptr & ~PAGE_MASK;
	unsigned int pages_count;
	size_t entries_size;
	int i;
	int ret;

	if (builder->framevec) {
		pr_warn("previous framevec found!");
		vb2_destroy_framevec(builder->framevec);
		builder->framevec = NULL;
	}

	framevec = vb2_create_framevec(userptr, length, true);
	if (IS_ERR(framevec)) {
		if (PTR_ERR(framevec) != -EFAULT) {
			pr_warn("error %ld creating frame vector for userptr 0x%lx, length 0x%lx\n",
				PTR_ERR(framevec), userptr, length);
		} else {
			/* -EINVAL is expected in case of invalid userptr. */
			framevec = ERR_PTR(-EINVAL);
		}
		return PTR_ERR(framevec);
	}

	pages = frame_vector_pages(framevec);
	if (IS_ERR(pages)) {
		pr_warn("error getting vector pages\n");
		ret = PTR_ERR(pages);
		goto free_framevec;
	}
	pages_count = frame_vector_count(framevec);
	ret = sg_alloc_table_from_pages(&sg_table, pages, pages_count, offset,
					length, 0);
	if (ret) {
		pr_warn("error creating sg table\n");
		goto free_framevec;
	}

	/* Allocate our actual SG in the shadow buffer. */
	*nents = sg_nents(sg_table.sgl);
	entries_size = sizeof(**sg_list) * *nents;
	if (builder->shadow_buffer_pos + entries_size >
	    builder->shadow_buffer_size) {
		ret = -ENOMEM;
		goto free_sg;
	}

	*sg_list = builder->shadow_buffer + builder->shadow_buffer_pos;
	builder->shadow_buffer_pos += entries_size;

	for_each_sgtable_sg(&sg_table, sg_iter, i) {
		struct virtio_media_sg_entry *sg_entry = &(*sg_list)[i];

		sg_entry->start = sg_phys(sg_iter);
		sg_entry->len = sg_iter->length;
		sg_entry->__reserved = 0;
	}

	builder->framevec = framevec;

free_sg:
	sg_free_table(&sg_table);
free_framevec:
	// Only destroy the framevec if we didn't save it
	if (!builder->framevec && framevec)
		vb2_destroy_framevec(framevec);
	return ret;
}

/**
 * scatterlist_builder_add_userptr() - Add a user-memory buffer using an array
 *                                     of &struct virtio_media_sg_entry.
 * @builder: builder to use.
 * @userptr: pointer to userspace memory that we want to add.
 * @length: length of the data to add.
 *
 * Upon success, an array of &struct virtio_media_sg_entry referencing
 * @userptr has been built into the shadow buffer, and that array added to the
 * descriptor chain.
 */
static int scatterlist_builder_add_userptr(struct scatterlist_builder *builder,
					   unsigned long userptr,
					   unsigned long length)
{
	int ret;
	int nents;
	struct virtio_media_sg_entry *sg_list;

	ret = __scatterlist_builder_add_userptr(builder, userptr, length,
						&sg_list, &nents);
	if (ret)
		return ret;

	ret = scatterlist_builder_add_data(builder, sg_list,
					   sizeof(*sg_list) * nents);
	if (ret)
		return ret;

	return 0;
}

/**
 * scatterlist_builder_add_ext_ctrls() - Add a &struct v4l2_ext_controls and its
 *                                       controls to @builder.
 * @builder: builder to use.
 * @ctrls: &struct v4l2_ext_controls to add.
 *
 * Add @ctrls and its array of &struct v4l2_ext_control to the descriptor
 * chain.
 */
int scatterlist_builder_add_ext_ctrls(struct scatterlist_builder *builder,
				      struct v4l2_ext_controls *ctrls)
{
	int ret;

	/* v4l2_ext_controls */
	ret = scatterlist_builder_add_data(builder, ctrls, sizeof(*ctrls));
	if (ret)
		return ret;

	if (ctrls->count > 0) {
		/* array of v4l2_controls */
		ret = scatterlist_builder_add_data(builder, ctrls->controls,
						   sizeof(ctrls->controls[0]) *
							   ctrls->count);
		if (ret)
			return ret;
	}

	return 0;
}

/**
 * scatterlist_builder_add_ext_ctrls_userptrs() - Add the userspace payloads of
 *                                                a &struct v4l2_ext_controls
 *                                                to the descriptor chain.
 * @builder: builder to use.
 * @ctrls: &struct v4l2_ext_controls from which we want to add the
 *         userspace payload.
 *
 * Add the userspace payloads of @ctrls to the descriptor chain. This is split
 * out of scatterlist_builder_add_ext_ctrls() because we only want to add
 * these to the device-readable part of the descriptor chain.
 */
int
scatterlist_builder_add_ext_ctrls_userptrs(struct scatterlist_builder *builder,
					   struct v4l2_ext_controls *ctrls)
{
	int i;
	int ret;

	/* Pointers to user memory in individual controls */
	for (i = 0; i < ctrls->count; i++) {
		struct v4l2_ext_control *ctrl = &ctrls->controls[i];

		if (ctrl->size > 0) {
			unsigned long uptr = (unsigned long)ctrl->ptr;

			ret = scatterlist_builder_add_userptr(builder, uptr,
							      ctrl->size);
			if (ret)
				return ret;
		}
	}

	return 0;
}

/**
 * scatterlist_builder_retrieve_ext_ctrls() - Retrieve controls written by the
 *                                            device on the shadow buffer,
 *                                            if needed.
 * @builder: builder to use.
 * @sg_index: index of the first SG entry of the controls in the builder's
 *            descriptor chain.
 * @ctrls: &struct v4l2_ext_controls to copy shadow buffer data into.
 *
 * If the shadow buffer is pointed to by @sg_index, copy its content back into
 * @ctrls.
 */
int scatterlist_builder_retrieve_ext_ctrls(struct scatterlist_builder *builder,
					   size_t sg_index,
					   struct v4l2_ext_controls *ctrls)
{
	struct v4l2_ext_control *controls_backup = ctrls->controls;
	int ret;

	ret = scatterlist_builder_retrieve_data(builder, sg_index++, ctrls);

	if (ret)
		goto cleanup;

	ctrls->controls = controls_backup;

	if (ctrls->count > 0 && ctrls->controls) {
		ret = scatterlist_builder_retrieve_data(builder, sg_index++,
							ctrls->controls);
		if (ret)
			goto cleanup;
	}

cleanup:
	// Clean up the framevec created in __scatterlist_builder_add_userptr()
	if (builder->framevec) {
		vb2_destroy_framevec(builder->framevec);
		builder->framevec = NULL;
	}

	return ret;
}
