// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/file.h>
#include <linux/io_uring.h>
#include <linux/hugetlb.h>
#include <linux/mm.h>
#include <linux/mm_inline.h>
#include <linux/shm.h>
#include <linux/mman.h>
#include <linux/audit.h>
#include "../mm/internal.h"
#include <uapi/linux/io_uring.h>

#include "io_uring.h"
#include "mmap.h"
#include "rsrc.h"

struct io_mmap_data {
	struct file *file;
	unsigned long flags;
	struct io_uring_mmap_desc __user *uaddr;
};
struct io_mmap_async {
	int nr_maps;
	struct io_uring_mmap_desc maps[] __counted_by(nr_maps);
};

#define MMAP_MAX_BATCH 1024

int io_mmap_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	struct io_mmap_data *mmap = io_kiocb_to_cmd(req, struct io_mmap_data);
	struct io_mmap_async *maps;
	int nr_maps;

	mmap->uaddr = u64_to_user_ptr(READ_ONCE(sqe->addr));
	mmap->flags = READ_ONCE(sqe->mmap_flags);
	nr_maps = READ_ONCE(sqe->len);

	if (mmap->flags & MAP_ANONYMOUS && req->cqe.fd != -1)
		return -EINVAL;
	if (nr_maps < 0 || nr_maps > MMAP_MAX_BATCH)
		return -EINVAL;
	if (!access_ok(mmap->uaddr, nr_maps*sizeof(struct io_uring_mmap_desc)))
		return -EFAULT;

	maps = kzalloc(struct_size_t(struct io_mmap_async, maps, nr_maps),
		       GFP_KERNEL);
	if (!maps)
		return -ENOMEM;
	maps->nr_maps = nr_maps;

	req->flags |= REQ_F_ASYNC_DATA;
	req->async_data = maps;
	return 0;
}

static int io_prep_mmap_hugetlb(struct file **filp, unsigned long *len,
				int flags)
{
	if (*filp) {
		*len = ALIGN(*len, huge_page_size(hstate_file(*filp)));
	} else {
		struct hstate *hs;
		unsigned long nlen = *len;

		hs = hstate_sizelog((flags >> MAP_HUGE_SHIFT) & MAP_HUGE_MASK);
		if (!hs)
			return -EINVAL;
		nlen = ALIGN(nlen, huge_page_size(hs));
		*filp = hugetlb_file_setup(HUGETLB_ANON_FILE, nlen,
					   VM_NORESERVE,
					   HUGETLB_ANONHUGE_INODE,
				   (flags >> MAP_HUGE_SHIFT) & MAP_HUGE_MASK);

		if (IS_ERR(*filp))
			return PTR_ERR(*filp);
		*len = nlen;
	}
	return 0;
}

int io_mmap(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_mmap_data *mmap = io_kiocb_to_cmd(req, struct io_mmap_data);
	struct io_mmap_async *data = (struct io_mmap_async *) req->async_data;
	int i, mapped, ret;

	if (unlikely(mmap->flags & MAP_HUGETLB && req->file &&
		     !is_file_hugepages(req->file))) {
		ret = -EINVAL;
		goto out;
	}

	for (i = 0; i < data->nr_maps; i++) {
		struct io_uring_mmap_desc *desc = &data->maps[i];

		if (copy_from_user(desc, &mmap->uaddr[i], sizeof(*desc))) {
			ret = -EFAULT;
			goto out;
		}
	}

	mapped = 0;
	while (mapped < data->nr_maps) {
		struct io_uring_mmap_desc *desc = &data->maps[mapped++];
		unsigned long flags = (mmap->flags | desc->flags);
		unsigned long len = desc->len;
		struct file *file = req->file;

		/* These cannot be mixed and matched.  need to be passed
		 * on the SQE.
		 */
		if (unlikely(desc->flags & (MAP_ANONYMOUS|MAP_HUGETLB))) {
			desc->addr = ERR_PTR(-EINVAL);
			break;
		}
		if (!(flags & MAP_ANONYMOUS))
			audit_mmap_fd(req->cqe.fd, flags);

		if (unlikely(flags & MAP_HUGETLB)) {
			ret = io_prep_mmap_hugetlb(&file, &len, flags);
			if (ret) {
				desc->addr = ERR_PTR(-ret);
				break;
			}
		}

		desc->addr = (void *) vm_mmap_pgoff(file,
					   (unsigned long) desc->addr,
					   len, desc->prot, flags, desc->pgoff);
		if (IS_ERR_OR_NULL(desc->addr))
			break;
	}

	if (copy_to_user(mmap->uaddr, data->maps,
			 sizeof(struct io_uring_mmap_desc)*mapped))
		ret = -EFAULT;

	ret = mapped;
out:
	if (ret < 0)
		req_set_fail(req);
	io_req_set_res(req, ret, 0);
	return IOU_COMPLETE;
}
