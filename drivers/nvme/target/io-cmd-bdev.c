// SPDX-License-Identifier: GPL-2.0
/*
 * NVMe I/O command implementation.
 * Copyright (c) 2015-2016 HGST, a Western Digital Company.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/blkdev.h>
#include <linux/blk-integrity.h>
#include <linux/memremap.h>
#include <linux/module.h>
#include "nvmet.h"

void nvmet_bdev_set_limits(struct block_device *bdev, struct nvme_id_ns *id)
{
	/* Logical blocks per physical block, 0's based. */
	const __le16 lpp0b = to0based(bdev_physical_block_size(bdev) /
				      bdev_logical_block_size(bdev));

	/*
	 * For NVMe 1.2 and later, bit 1 indicates that the fields NAWUN,
	 * NAWUPF, and NACWU are defined for this namespace and should be
	 * used by the host for this namespace instead of the AWUN, AWUPF,
	 * and ACWU fields in the Identify Controller data structure. If
	 * any of these fields are zero that means that the corresponding
	 * field from the identify controller data structure should be used.
	 */
	id->nsfeat |= 1 << 1;
	id->nawun = lpp0b;
	id->nawupf = lpp0b;
	id->nacwu = lpp0b;

	/*
	 * Bit 4 indicates that the fields NPWG, NPWA, NPDG, NPDA, and
	 * NOWS are defined for this namespace and should be used by
	 * the host for I/O optimization.
	 */
	id->nsfeat |= 1 << 4;
	/* NPWG = Namespace Preferred Write Granularity. 0's based */
	id->npwg = lpp0b;
	/* NPWA = Namespace Preferred Write Alignment. 0's based */
	id->npwa = id->npwg;
	/* NPDG = Namespace Preferred Deallocate Granularity. 0's based */
	id->npdg = to0based(bdev_discard_granularity(bdev) /
			    bdev_logical_block_size(bdev));
	/* NPDG = Namespace Preferred Deallocate Alignment */
	id->npda = id->npdg;
	/* NOWS = Namespace Optimal Write Size */
	id->nows = to0based(bdev_io_opt(bdev) / bdev_logical_block_size(bdev));
}

void nvmet_bdev_ns_disable(struct nvmet_ns *ns)
{
	if (ns->bdev) {
		blkdev_put(ns->bdev, NULL);
		ns->bdev = NULL;
	}
}

static void nvmet_bdev_ns_enable_integrity(struct nvmet_ns *ns)
{
	struct blk_integrity *bi = bdev_get_integrity(ns->bdev);

	if (bi) {
		ns->metadata_size = bi->tuple_size;
		if (bi->profile == &t10_pi_type1_crc)
			ns->pi_type = NVME_NS_DPS_PI_TYPE1;
		else if (bi->profile == &t10_pi_type3_crc)
			ns->pi_type = NVME_NS_DPS_PI_TYPE3;
		else
			/* Unsupported metadata type */
			ns->metadata_size = 0;
	}
}

int nvmet_bdev_ns_enable(struct nvmet_ns *ns)
{
	int ret;

	/*
	 * When buffered_io namespace attribute is enabled that means user want
	 * this block device to be used as a file, so block device can take
	 * an advantage of cache.
	 */
	if (ns->buffered_io)
		return -ENOTBLK;

	ns->bdev = blkdev_get_by_path(ns->device_path,
			BLK_OPEN_READ | BLK_OPEN_WRITE, NULL, NULL);
	if (IS_ERR(ns->bdev)) {
		ret = PTR_ERR(ns->bdev);
		if (ret != -ENOTBLK) {
			pr_err("failed to open block device %s: (%ld)\n",
					ns->device_path, PTR_ERR(ns->bdev));
		}
		ns->bdev = NULL;
		return ret;
	}
	ns->size = bdev_nr_bytes(ns->bdev);
	ns->blksize_shift = blksize_bits(bdev_logical_block_size(ns->bdev));

	ns->pi_type = 0;
	ns->metadata_size = 0;
	if (IS_ENABLED(CONFIG_BLK_DEV_INTEGRITY_T10))
		nvmet_bdev_ns_enable_integrity(ns);

	if (bdev_is_zoned(ns->bdev)) {
		if (!nvmet_bdev_zns_enable(ns)) {
			nvmet_bdev_ns_disable(ns);
			return -EINVAL;
		}
		ns->csi = NVME_CSI_ZNS;
	}

	return 0;
}

void nvmet_bdev_ns_revalidate(struct nvmet_ns *ns)
{
	ns->size = bdev_nr_bytes(ns->bdev);
}

u16 blk_to_nvme_status(struct nvmet_req *req, blk_status_t blk_sts)
{
	u16 status = NVME_SC_SUCCESS;

	if (likely(blk_sts == BLK_STS_OK))
		return status;
	/*
	 * Right now there exists M : 1 mapping between block layer error
	 * to the NVMe status code (see nvme_error_status()). For consistency,
	 * when we reverse map we use most appropriate NVMe Status code from
	 * the group of the NVMe staus codes used in the nvme_error_status().
	 */
	switch (blk_sts) {
	case BLK_STS_NOSPC:
		status = NVME_SC_CAP_EXCEEDED | NVME_SC_DNR;
		req->error_loc = offsetof(struct nvme_rw_command, length);
		break;
	case BLK_STS_TARGET:
		status = NVME_SC_LBA_RANGE | NVME_SC_DNR;
		req->error_loc = offsetof(struct nvme_rw_command, slba);
		break;
	case BLK_STS_NOTSUPP:
		req->error_loc = offsetof(struct nvme_common_command, opcode);
		switch (req->cmd->common.opcode) {
		case nvme_cmd_dsm:
		case nvme_cmd_write_zeroes:
			status = NVME_SC_ONCS_NOT_SUPPORTED | NVME_SC_DNR;
			break;
		default:
			status = NVME_SC_INVALID_OPCODE | NVME_SC_DNR;
		}
		break;
	case BLK_STS_MEDIUM:
		status = NVME_SC_ACCESS_DENIED;
		req->error_loc = offsetof(struct nvme_rw_command, nsid);
		break;
	case BLK_STS_IOERR:
	default:
		status = NVME_SC_INTERNAL | NVME_SC_DNR;
		req->error_loc = offsetof(struct nvme_common_command, opcode);
	}

	switch (req->cmd->common.opcode) {
	case nvme_cmd_read:
	case nvme_cmd_write:
		req->error_slba = le64_to_cpu(req->cmd->rw.slba);
		break;
	case nvme_cmd_write_zeroes:
		req->error_slba =
			le64_to_cpu(req->cmd->write_zeroes.slba);
		break;
	default:
		req->error_slba = 0;
	}
	return status;
}

static void nvmet_bio_done(struct bio *bio)
{
	struct nvmet_req *req = bio->bi_private;

	nvmet_req_complete(req, blk_to_nvme_status(req, bio->bi_status));
	nvmet_req_bio_put(req, bio);
}

static void nvmet_pqt_bio_done(struct bio *bio)
{
	struct nvmet_pqt_bio_req *req_done = bio->bi_private;

	nvmet_req_complete(req_done->req, blk_to_nvme_status(req_done->req,
							bio->bi_status));
	nvmet_req_bio_put(req_done->req, bio);
	req_done->io_completed = 1;
}

#ifdef CONFIG_BLK_DEV_INTEGRITY
static int nvmet_bdev_alloc_bip(struct nvmet_req *req, struct bio *bio,
				struct sg_mapping_iter *miter)
{
	struct blk_integrity *bi;
	struct bio_integrity_payload *bip;
	int rc;
	size_t resid, len;

	bi = bdev_get_integrity(req->ns->bdev);
	if (unlikely(!bi)) {
		pr_err("Unable to locate bio_integrity\n");
		return -ENODEV;
	}

	bip = bio_integrity_alloc(bio, GFP_NOIO,
					bio_max_segs(req->metadata_sg_cnt));
	if (IS_ERR(bip)) {
		pr_err("Unable to allocate bio_integrity_payload\n");
		return PTR_ERR(bip);
	}

	/* virtual start sector must be in integrity interval units */
	bip_set_seed(bip, bio->bi_iter.bi_sector >>
		     (bi->interval_exp - SECTOR_SHIFT));

	resid = bio_integrity_bytes(bi, bio_sectors(bio));
	while (resid > 0 && sg_miter_next(miter)) {
		len = min_t(size_t, miter->length, resid);
		rc = bio_integrity_add_page(bio, miter->page, len,
					    offset_in_page(miter->addr));
		if (unlikely(rc != len)) {
			pr_err("bio_integrity_add_page() failed; %d\n", rc);
			sg_miter_stop(miter);
			return -ENOMEM;
		}

		resid -= len;
		if (len < miter->length)
			miter->consumed -= miter->length - len;
	}
	sg_miter_stop(miter);

	return 0;
}
#else
static int nvmet_bdev_alloc_bip(struct nvmet_req *req, struct bio *bio,
				struct sg_mapping_iter *miter)
{
	return -EINVAL;
}
#endif /* CONFIG_BLK_DEV_INTEGRITY */

#ifdef CONFIG_NVME_MULTIPATH
extern struct block_device *nvme_mpath_get_bdev(struct block_device *bdev);
extern const struct block_device_operations nvme_ns_head_ops;
#endif

static inline int nvmet_chain_par_bio(struct nvmet_req *req, struct bio **bio,
					struct sg_mapping_iter *prot_miter, struct block_device *bdev,
					sector_t sector, struct bio_list *blist)
{
	struct bio *parent, *child;
	unsigned int vec_cnt;
	int rc;

	parent = *bio;
	vec_cnt = queue_max_segments(bdev->bd_disk->queue);
	if (req->metadata_len) {
		rc = nvmet_bdev_alloc_bip(req, parent,
						prot_miter);
		if (unlikely(rc))
			return rc;
	}
	child = bio_alloc(bdev, vec_cnt, parent->bi_opf, GFP_KERNEL);
	child->bi_iter.bi_sector = sector;
	*bio = child;
	bio_chain(*bio, parent);
	parent->bi_opf |= REQ_POLLED;
	parent->bi_opf |= REQ_NOWAIT;
	parent->bi_opf |= REQ_NOMERGE;
	bio_list_add(blist, parent);
	return 0;
}

static void nvmet_bdev_execute_rw(struct nvmet_req *req)
{
	unsigned int sg_cnt = req->sg_cnt;
	struct bio *bio;
	struct scatterlist *sg;
	struct blk_plug plug;
	sector_t sector;
	blk_opf_t opf;
	int i, rc;
	struct sg_mapping_iter prot_miter;
	unsigned int iter_flags, max_sectors;
	unsigned short vec_cnt, max_segments;
	unsigned int total_len = nvmet_rw_data_len(req) + req->metadata_len;
	bool pqt_enabled = nvmet_pqt_enabled();
	unsigned int sg_len;
	struct nvmet_pqt_bio_req *req_done = NULL;
	struct block_device *bdev = req->ns->bdev;

	if (!nvmet_check_transfer_len(req, total_len))
		return;

	if (!req->sg_cnt) {
		nvmet_req_complete(req, 0);
		return;
	}

	if (req->cmd->rw.opcode == nvme_cmd_write) {
		opf = REQ_OP_WRITE | REQ_SYNC | REQ_IDLE;
		if (req->cmd->rw.control & cpu_to_le16(NVME_RW_FUA))
			opf |= REQ_FUA;
		iter_flags = SG_MITER_TO_SG;
	} else {
		opf = REQ_OP_READ;
		iter_flags = SG_MITER_FROM_SG;
	}

#ifdef CONFIG_NVME_MULTIPATH
	if (pqt_enabled && bdev->bd_disk->fops == &nvme_ns_head_ops) {
		bdev = nvme_mpath_get_bdev(bdev);
		if (!bdev) {
			nvmet_req_complete(req, 0);
			return;
		}
		opf |= REQ_DRV;
	}
#endif
	if (pqt_enabled) {
		req_done = kmalloc(sizeof(struct nvmet_pqt_bio_req), GFP_KERNEL);
		if (!req_done) {
			nvmet_req_complete(req, 0);
			return;
		}
	}

	if (is_pci_p2pdma_page(sg_page(req->sg)))
		opf |= REQ_NOMERGE;

	sector = nvmet_lba_to_sect(req->ns, req->cmd->rw.slba);

	if (nvmet_use_inline_bvec(req)) {
		bio = &req->b.inline_bio;
		bio_init(bio, req->ns->bdev, req->inline_bvec,
			 ARRAY_SIZE(req->inline_bvec), opf);
	} else {
		vec_cnt = bio_max_segs(sg_cnt);
		if (pqt_enabled)
			vec_cnt = queue_max_segments(bdev->bd_disk->queue);
		bio = bio_alloc(bdev, vec_cnt, opf,
				GFP_KERNEL);
	}
	bio->bi_iter.bi_sector = sector;
	if (!pqt_enabled) {
		bio->bi_private = req;
		bio->bi_end_io = nvmet_bio_done;
	} else {
		req_done->req = req;
		bio->bi_private = req_done;
		bio->bi_end_io = nvmet_pqt_bio_done;
	}

	if (!pqt_enabled)
		blk_start_plug(&plug);
	if (req->metadata_len)
		sg_miter_start(&prot_miter, req->metadata_sg,
			       req->metadata_sg_cnt, iter_flags);

	if (!pqt_enabled) {
		for_each_sg(req->sg, sg, req->sg_cnt, i) {
			while (bio_add_page(bio, sg_page(sg), sg->length, sg->offset)
					!= sg->length) {
				struct bio *prev = bio;

				if (req->metadata_len) {
					rc = nvmet_bdev_alloc_bip(req, bio,
								  &prot_miter);
					if (unlikely(rc)) {
						bio_io_error(bio);
						return;
					}
				}

				bio = bio_alloc(req->ns->bdev, bio_max_segs(sg_cnt),
						opf, GFP_KERNEL);
				bio->bi_iter.bi_sector = sector;

				bio_chain(bio, prev);
				submit_bio(prev);
			}

			sector += sg->length >> 9;
			sg_cnt--;
		}
	} else {
		bio_list_init(&req_done->blist);
		if (!test_bit(QUEUE_FLAG_POLL, &bdev->bd_disk->queue->queue_flags))
			goto err_bio;
		max_sectors = bdev->bd_disk->queue->limits.max_sectors;
		max_sectors <<= 9;
		max_segments = queue_max_segments(bdev->bd_disk->queue);
		sg_len = 0;
		unsigned int offset, len, vec_len, i;
		bool sg_start_pg = true, need_chain_bio = false;
		struct page *sglist_page, *max_sector_align;
		sector_t temp_sector;

		/*
		 * for bio's polling mode we will split bio to
		 * avoid low level's bio splitting when submit.
		 */
		for_each_sg(req->sg, sg, req->sg_cnt, i) {
			temp_sector = sector;
			offset = (sg->offset % PAGE_SIZE);
			if (offset + sg->length > PAGE_SIZE) { // need to split
				len = sg->length;
				i = 0;
				sglist_page = virt_to_page(page_to_virt(sg_page(sg)) + offset);
				if (offset != 0)
					sg_start_pg = false;
				while (len > PAGE_SIZE) {
					max_sector_align = virt_to_page(page_to_virt(sglist_page) +
											(PAGE_SIZE*i));
					vec_len = sg_start_pg?PAGE_SIZE:(PAGE_SIZE - offset);
					if (bio->bi_vcnt == max_segments - 1 ||
							sg_len + vec_len > max_sectors)
						need_chain_bio = true;
					else {
						__bio_add_page(bio, max_sector_align,
								vec_len, sg_start_pg?0:offset);
						temp_sector += vec_len >> 9;
						sg_len += vec_len;
					}
					if (need_chain_bio) {
						rc = nvmet_chain_par_bio(req, &bio, &prot_miter,
								bdev, temp_sector, &req_done->blist);
						if (unlikely(rc))
							goto err_bio;
						__bio_add_page(bio, max_sector_align, vec_len,
								sg_start_pg?0:(PAGE_SIZE - offset));
						temp_sector += vec_len >> 9;
						sg_len = vec_len;
						need_chain_bio = false;
					}
					if (!sg_start_pg) {
						len -= (PAGE_SIZE - offset);
						sg_start_pg = true;
					} else {
						len -= PAGE_SIZE;
					}
					i++;
				}
				if (len > 0) {
					max_sector_align = virt_to_page(page_to_virt(sglist_page) +
											(i * PAGE_SIZE));
					if (bio->bi_vcnt == max_segments - 1 ||
							sg_len + len > max_sectors) {
						rc = nvmet_chain_par_bio(req, &bio, &prot_miter,
								bdev, temp_sector, &req_done->blist);
						if (unlikely(rc))
							goto err_bio;
						sg_len = len;
					} else {
						sg_len += len;
					}
					__bio_add_page(bio, max_sector_align, len, 0);
					temp_sector += len >> 9;
				}
			} else {
				if (bio->bi_vcnt == max_segments - 1 ||
						sg_len + sg->length > max_sectors) {
					rc = nvmet_chain_par_bio(req, &bio, &prot_miter,
							bdev, temp_sector, &req_done->blist);
					if (unlikely(rc))
						goto err_bio;
					sg_len = sg->length;
				} else {
					sg_len += sg->length;
				}
				__bio_add_page(bio, sg_page(sg), sg->length, sg->offset);
			}
			sector += sg->length >> 9;
			sg_cnt--;
		}
	}

	if (req->metadata_len) {
		rc = nvmet_bdev_alloc_bip(req, bio, &prot_miter);
		if (unlikely(rc)) {
			goto err_bio;
		}
	}

	if (pqt_enabled) {
		bio->bi_opf |= REQ_POLLED;
		bio->bi_opf |= REQ_NOWAIT;
		bio->bi_opf |= REQ_NOMERGE;
		bio_list_add(&req_done->blist, bio);
		req_done->io_completed = 0;
		rc = nvmet_pqt_ring_enqueue(req_done);
		if (rc < 0)
			goto err_bio;
		nvmet_wakeup_pq_thread();
	} else {
	    submit_bio(bio);
    }
	if (!pqt_enabled)
		blk_finish_plug(&plug);
	return;
err_bio:
	bio_io_error(bio);
	if (pqt_enabled)
		kfree(req_done);
	return;
}

static void nvmet_bdev_execute_flush(struct nvmet_req *req)
{
	struct bio *bio = &req->b.inline_bio;

	if (!bdev_write_cache(req->ns->bdev)) {
		nvmet_req_complete(req, NVME_SC_SUCCESS);
		return;
	}

	if (!nvmet_check_transfer_len(req, 0))
		return;

	bio_init(bio, req->ns->bdev, req->inline_bvec,
		 ARRAY_SIZE(req->inline_bvec), REQ_OP_WRITE | REQ_PREFLUSH);
	bio->bi_private = req;
	bio->bi_end_io = nvmet_bio_done;

	submit_bio(bio);
}

u16 nvmet_bdev_flush(struct nvmet_req *req)
{
	if (!bdev_write_cache(req->ns->bdev))
		return 0;

	if (blkdev_issue_flush(req->ns->bdev))
		return NVME_SC_INTERNAL | NVME_SC_DNR;
	return 0;
}

static u16 nvmet_bdev_discard_range(struct nvmet_req *req,
		struct nvme_dsm_range *range, struct bio **bio)
{
	struct nvmet_ns *ns = req->ns;
	int ret;

	ret = __blkdev_issue_discard(ns->bdev,
			nvmet_lba_to_sect(ns, range->slba),
			le32_to_cpu(range->nlb) << (ns->blksize_shift - 9),
			GFP_KERNEL, bio);
	if (ret && ret != -EOPNOTSUPP) {
		req->error_slba = le64_to_cpu(range->slba);
		return errno_to_nvme_status(req, ret);
	}
	return NVME_SC_SUCCESS;
}

static void nvmet_bdev_execute_discard(struct nvmet_req *req)
{
	struct nvme_dsm_range range;
	struct bio *bio = NULL;
	int i;
	u16 status;

	for (i = 0; i <= le32_to_cpu(req->cmd->dsm.nr); i++) {
		status = nvmet_copy_from_sgl(req, i * sizeof(range), &range,
				sizeof(range));
		if (status)
			break;

		status = nvmet_bdev_discard_range(req, &range, &bio);
		if (status)
			break;
	}

	if (bio) {
		bio->bi_private = req;
		bio->bi_end_io = nvmet_bio_done;
		if (status)
			bio_io_error(bio);
		else
			submit_bio(bio);
	} else {
		nvmet_req_complete(req, status);
	}
}

static void nvmet_bdev_execute_dsm(struct nvmet_req *req)
{
	if (!nvmet_check_data_len_lte(req, nvmet_dsm_len(req)))
		return;

	switch (le32_to_cpu(req->cmd->dsm.attributes)) {
	case NVME_DSMGMT_AD:
		nvmet_bdev_execute_discard(req);
		return;
	case NVME_DSMGMT_IDR:
	case NVME_DSMGMT_IDW:
	default:
		/* Not supported yet */
		nvmet_req_complete(req, 0);
		return;
	}
}

static void nvmet_bdev_execute_write_zeroes(struct nvmet_req *req)
{
	struct nvme_write_zeroes_cmd *write_zeroes = &req->cmd->write_zeroes;
	struct bio *bio = NULL;
	sector_t sector;
	sector_t nr_sector;
	int ret;

	if (!nvmet_check_transfer_len(req, 0))
		return;

	sector = nvmet_lba_to_sect(req->ns, write_zeroes->slba);
	nr_sector = (((sector_t)le16_to_cpu(write_zeroes->length) + 1) <<
		(req->ns->blksize_shift - 9));

	ret = __blkdev_issue_zeroout(req->ns->bdev, sector, nr_sector,
			GFP_KERNEL, &bio, 0);
	if (bio) {
		bio->bi_private = req;
		bio->bi_end_io = nvmet_bio_done;
		submit_bio(bio);
	} else {
		nvmet_req_complete(req, errno_to_nvme_status(req, ret));
	}
}

u16 nvmet_bdev_parse_io_cmd(struct nvmet_req *req)
{
	switch (req->cmd->common.opcode) {
	case nvme_cmd_read:
	case nvme_cmd_write:
		req->execute = nvmet_bdev_execute_rw;
		if (req->sq->ctrl->pi_support && nvmet_ns_has_pi(req->ns))
			req->metadata_len = nvmet_rw_metadata_len(req);
		return 0;
	case nvme_cmd_flush:
		req->execute = nvmet_bdev_execute_flush;
		return 0;
	case nvme_cmd_dsm:
		req->execute = nvmet_bdev_execute_dsm;
		return 0;
	case nvme_cmd_write_zeroes:
		req->execute = nvmet_bdev_execute_write_zeroes;
		return 0;
	default:
		return nvmet_report_invalid_opcode(req);
	}
}
