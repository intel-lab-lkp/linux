/*
 * JFFS2 -- Journalling Flash File System, Version 2.
 *
 * Copyright © 2001-2007 Red Hat, Inc.
 *
 * Created by David Woodhouse <dwmw2@infradead.org>
 *
 * For licensing information, see the file 'LICENCE' in this directory.
 *
 */

#include <linux/kernel.h>
#include <linux/vmalloc.h>
#include <linux/mtd/mtd.h>
#include "nodelist.h"

#ifdef CONFIG_JFFS2_FS_WRITE_VERIFY
int jffs2_verify_write(struct jffs2_sb_info *c, const unsigned char *buf,
			      uint32_t ofs, size_t len)
{
	int ret = 0;
	size_t retlen, i;
	char *eccstr;
	void *verify_buf;

	verify_buf = __vmalloc(len, GFP_NOFS);
	if (!verify_buf) {
		pr_warn("%s(): verify buffer allocation failed, skipping verification\n",
			__func__);
		return 0;
	}

	ret = mtd_read(c->mtd, ofs, len, &retlen, verify_buf);
	if (ret && ret != -EUCLEAN && ret != -EBADMSG) {
		pr_warn("%s(): Read back of page at %08x failed: %d\n",
			__func__, ofs, ret);
		goto out_free;
	} else if (retlen != len) {
		pr_warn("%s(): Read back of page at %08x gave short read: %zu not %zu\n",
			__func__, ofs, retlen, len);
		ret = -EIO;
		goto out_free;
	}

	for (i = 0; i < len; i++) {
		uint8_t c1 = ((uint8_t *)buf)[i];
		uint8_t c2 = ((uint8_t *)verify_buf)[i];
		int dump_len;

		if (c1 == c2)
			continue;

		if (ret == -EUCLEAN)
			eccstr = "corrected";
		else if (ret == -EBADMSG)
			eccstr = "correction failed";
		else
			eccstr = "OK or unused";

		dump_len = min_t(int, 128, len - i);
		pr_warn("Write verify error (ECC %s) at %08x (+%zu/%zu). Wrote:\n",
			eccstr, ofs, i, len);
		print_hex_dump(KERN_WARNING, "", DUMP_PREFIX_OFFSET, 16, 1,
			       buf + i, dump_len, 0);

		pr_warn("Read back:\n");
		print_hex_dump(KERN_WARNING, "", DUMP_PREFIX_OFFSET, 16, 1,
			       verify_buf + i, dump_len, 0);

		ret = -EIO;
		goto out_free;
	}

	vfree(verify_buf);
	return 0;

out_free:
	vfree(verify_buf);
	return ret;
}

int jffs2_verify_writev(struct jffs2_sb_info *c,
			const struct kvec *vecs,
			unsigned long count, loff_t to)
{
	loff_t ofs = to;
	unsigned long i;
	int ret;

	for (i = 0; i < count; i++) {
		if (!vecs[i].iov_len)
			continue;
		ret = jffs2_verify_write(c, vecs[i].iov_base, ofs,
					 vecs[i].iov_len);
		if (ret)
			return ret;
		ofs += vecs[i].iov_len;
	}
	return 0;
}
#endif /* CONFIG_JFFS2_FS_WRITE_VERIFY */

int jffs2_flash_direct_writev(struct jffs2_sb_info *c, const struct kvec *vecs,
			      unsigned long count, loff_t to, size_t *retlen)
{
	int ret;
	if (!jffs2_is_writebuffered(c)) {
		if (jffs2_sum_active()) {
			int res;
			res = jffs2_sum_add_kvec(c, vecs, count, (uint32_t) to);
			if (res) {
				return res;
			}
		}
	}

	ret = mtd_writev(c->mtd, vecs, count, to, retlen);

	if (ret) {
		pr_warn("%s(): Write failed with %d\n", __func__, ret);
	} else {
		size_t totlen = 0;
		unsigned long i;

		for (i = 0; i < count; i++)
			totlen += vecs[i].iov_len;
		if (*retlen != totlen) {
			pr_warn("%s(): Write was short: %zu instead of %zu\n",
				__func__, *retlen, totlen);
			ret = -EIO;
		} else
			ret = jffs2_verify_writev(c, vecs, count, to);
	}

	return ret;
}

int jffs2_flash_direct_write(struct jffs2_sb_info *c, loff_t ofs, size_t len,
			size_t *retlen, const u_char *buf)
{
	int ret;
	if (jffs2_sum_active()) {
		struct kvec vecs[1];
		int res;

		vecs[0].iov_base = (unsigned char *) buf;
		vecs[0].iov_len = len;

		res = jffs2_sum_add_kvec(c, vecs, 1, (uint32_t) ofs);
		if (res) {
			return res;
		}
	}

	ret = mtd_write(c->mtd, ofs, len, retlen, buf);

	if (ret) {
		pr_warn("%s(): Write failed with %d\n", __func__, ret);
	} else if (*retlen != len) {
		pr_warn("%s(): Write was short: %zu instead of %zu\n",
			__func__, *retlen, len);
		ret = -EIO;
	} else
		ret = jffs2_verify_write(c, buf, ofs, len);

	return ret;
}
