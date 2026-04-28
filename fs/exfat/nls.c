// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2012-2013 Samsung Electronics Co., Ltd.
 */

#include <linux/string.h>
#include <linux/slab.h>
#include <linux/buffer_head.h>
#include <linux/unaligned.h>

#include "exfat_raw.h"
#include "exfat_fs.h"

static int exfat_convert_char_to_ucs2(struct nls_table *nls,
		const unsigned char *ch, int ch_len, unsigned short *ucs2,
		int *lossy)
{
	int len;

	*ucs2 = 0x0;

	if (ch[0] < 0x80) {
		*ucs2 = ch[0];
		return 1;
	}

	len = nls->char2uni(ch, ch_len, ucs2);
	if (len < 0) {
		/* conversion failed */
		if (lossy != NULL)
			*lossy |= NLS_NAME_LOSSY;
		*ucs2 = '_';
		return 1;
	}
	return len;
}

static int exfat_convert_ucs2_to_char(struct nls_table *nls,
		unsigned short ucs2, unsigned char *ch, int *lossy)
{
	int len;

	ch[0] = 0x0;

	if (ucs2 < 0x0080) {
		ch[0] = ucs2;
		return 1;
	}

	len = nls->uni2char(ucs2, ch, MAX_CHARSET_SIZE);
	if (len < 0) {
		/* conversion failed */
		if (lossy != NULL)
			*lossy |= NLS_NAME_LOSSY;
		ch[0] = '_';
		return 1;
	}
	return len;
}

unsigned short exfat_toupper(struct super_block *sb, unsigned short a)
{
	struct exfat_sb_info *sbi = EXFAT_SB(sb);

	return sbi->vol_utbl[a] ? sbi->vol_utbl[a] : a;
}

static const unsigned short *exfat_wstrchr(const unsigned short *str,
		const unsigned short wchar)
{
	while (*str) {
		if (*(str++) == wchar)
			return str;
	}
	return NULL;
}

int exfat_uniname_ncmp(struct super_block *sb, unsigned short *a,
		unsigned short *b, unsigned int len)
{
	int i;

	for (i = 0; i < len; i++, a++, b++)
		if (exfat_toupper(sb, *a) != exfat_toupper(sb, *b))
			return 1;
	return 0;
}

static int exfat_utf16_to_utf8(struct super_block *sb,
		struct exfat_uni_name *p_uniname, unsigned char *p_cstring,
		int buflen)
{
	int len;
	const unsigned short *uniname = p_uniname->name;

	/* always len >= 0 */
	len = utf16s_to_utf8s(uniname, MAX_NAME_LENGTH, UTF16_HOST_ENDIAN,
		p_cstring, buflen);
	p_cstring[len] = '\0';
	return len;
}

static int exfat_utf8_to_utf16(struct super_block *sb,
		const unsigned char *p_cstring, const int len,
		struct exfat_uni_name *p_uniname, int *p_lossy)
{
	int i, unilen, lossy = NLS_NAME_NO_LOSSY;
	__le16 upname[MAX_NAME_LENGTH + 1];
	unsigned short *uniname = p_uniname->name;

	WARN_ON(!len);

	unilen = utf8s_to_utf16s(p_cstring, len, UTF16_HOST_ENDIAN,
			(wchar_t *)uniname, MAX_NAME_LENGTH + 2);
	if (unilen < 0) {
		exfat_err(sb, "failed to %s (err : %d) nls len : %d",
			  __func__, unilen, len);
		return unilen;
	}

	if (unilen > MAX_NAME_LENGTH) {
		exfat_debug(sb, "failed to %s (estr:ENAMETOOLONG) nls len : %d, unilen : %d > %d",
			  __func__, len, unilen, MAX_NAME_LENGTH);
		return -ENAMETOOLONG;
	}

	for (i = 0; i < unilen; i++) {
		if (*uniname < 0x0020 ||
		    exfat_wstrchr(exfat_bad_uni_chars, *uniname))
			lossy |= NLS_NAME_LOSSY;

		upname[i] = cpu_to_le16(exfat_toupper(sb, *uniname));
		uniname++;
	}

	*uniname = '\0';
	p_uniname->name_len = unilen;
	p_uniname->name_hash = exfat_calc_chksum16(upname, unilen << 1, 0,
			CS_DEFAULT);

	if (p_lossy)
		*p_lossy = lossy;
	return unilen;
}

#define SURROGATE_MASK	0xfffff800
#define SURROGATE_PAIR	0x0000d800
#define SURROGATE_LOW	0x00000400

static int __exfat_utf16_to_nls(struct super_block *sb,
		struct exfat_uni_name *p_uniname, unsigned char *p_cstring,
		int buflen)
{
	int i, j, len, out_len = 0;
	unsigned char buf[MAX_CHARSET_SIZE];
	const unsigned short *uniname = p_uniname->name;
	struct nls_table *nls = EXFAT_SB(sb)->nls_io;

	i = 0;
	while (i < MAX_NAME_LENGTH && out_len < (buflen - 1)) {
		if (*uniname == '\0')
			break;
		if ((*uniname & SURROGATE_MASK) != SURROGATE_PAIR) {
			len = exfat_convert_ucs2_to_char(nls, *uniname, buf,
				NULL);
		} else {
			/* Process UTF-16 surrogate pair as one character */
			if (!(*uniname & SURROGATE_LOW) &&
			    i+1 < MAX_NAME_LENGTH &&
			    (*(uniname+1) & SURROGATE_MASK) == SURROGATE_PAIR &&
			    (*(uniname+1) & SURROGATE_LOW)) {
				uniname++;
				i++;
			}

			/*
			 * UTF-16 surrogate pair encodes code points above
			 * U+FFFF. Code points above U+FFFF are not supported
			 * by kernel NLS framework therefore use replacement
			 * character
			 */
			len = 1;
			buf[0] = '_';
		}

		if (out_len + len >= buflen)
			len = buflen - 1 - out_len;
		out_len += len;

		if (len > 1) {
			for (j = 0; j < len; j++)
				*p_cstring++ = buf[j];
		} else { /* len == 1 */
			*p_cstring++ = *buf;
		}

		uniname++;
		i++;
	}

	*p_cstring = '\0';
	return out_len;
}

static int exfat_nls_to_ucs2(struct super_block *sb,
		const unsigned char *p_cstring, const int len,
		struct exfat_uni_name *p_uniname, int *p_lossy)
{
	int i = 0, unilen = 0, lossy = NLS_NAME_NO_LOSSY;
	__le16 upname[MAX_NAME_LENGTH + 1];
	unsigned short *uniname = p_uniname->name;
	struct nls_table *nls = EXFAT_SB(sb)->nls_io;

	WARN_ON(!len);

	while (unilen < MAX_NAME_LENGTH && i < len) {
		i += exfat_convert_char_to_ucs2(nls, p_cstring + i, len - i,
				uniname, &lossy);

		if (*uniname < 0x0020 ||
		    exfat_wstrchr(exfat_bad_uni_chars, *uniname))
			lossy |= NLS_NAME_LOSSY;

		upname[unilen] = cpu_to_le16(exfat_toupper(sb, *uniname));
		uniname++;
		unilen++;
	}

	*uniname = '\0';
	p_uniname->name_len = unilen;
	p_uniname->name_hash = exfat_calc_chksum16(upname, unilen << 1, 0,
			CS_DEFAULT);

	if (p_lossy)
		*p_lossy = lossy;
	return unilen;
}

int exfat_utf16_to_nls(struct super_block *sb, struct exfat_uni_name *uniname,
		unsigned char *p_cstring, int buflen)
{
	if (EXFAT_SB(sb)->options.utf8)
		return exfat_utf16_to_utf8(sb, uniname, p_cstring,
				buflen);
	return __exfat_utf16_to_nls(sb, uniname, p_cstring, buflen);
}

int exfat_nls_to_utf16(struct super_block *sb, const unsigned char *p_cstring,
		const int len, struct exfat_uni_name *uniname, int *p_lossy)
{
	if (EXFAT_SB(sb)->options.utf8)
		return exfat_utf8_to_utf16(sb, p_cstring, len,
				uniname, p_lossy);
	return exfat_nls_to_ucs2(sb, p_cstring, len, uniname, p_lossy);
}

static int exfat_load_upcase_table(struct super_block *sb,
		sector_t sector, unsigned long long num_sectors,
		unsigned int utbl_checksum)
{
	struct exfat_sb_info *sbi = EXFAT_SB(sb);
	unsigned int sect_size = sb->s_blocksize;
	unsigned int i, index = 0;
	u32 chksum = 0;
	unsigned char skip = false;
	unsigned short *upcase_table;

	upcase_table = kvcalloc(EXFAT_UTBL_COUNT, sizeof(unsigned short), GFP_KERNEL);
	if (!upcase_table)
		return -ENOMEM;

	sbi->vol_utbl = upcase_table;
	num_sectors += sector;

	while (sector < num_sectors) {
		struct buffer_head *bh;

		bh = sb_bread(sb, sector);
		if (!bh) {
			exfat_err(sb, "failed to read sector(0x%llx)",
				  (unsigned long long)sector);
			return -EIO;
		}
		sector++;
		for (i = 0; i < sect_size && index <= 0xFFFF; i += 2) {
			unsigned short uni = get_unaligned_le16(bh->b_data + i);

			if (skip) {
				index += uni;
				skip = false;
			} else if (uni == index) {
				index++;
			} else if (uni == 0xFFFF) {
				skip = true;
			} else { /* uni != index , uni != 0xFFFF */
				upcase_table[index] = uni;
				index++;
			}
		}
		chksum = exfat_calc_chksum32(bh->b_data, i, chksum, CS_DEFAULT);
		brelse(bh);
	}

	if (index >= 0xFFFF && utbl_checksum == chksum)
		return 0;

	exfat_err(sb, "failed to load upcase table (idx : 0x%08x, chksum : 0x%08x, utbl_chksum : 0x%08x)",
		  index, chksum, utbl_checksum);
	return -EINVAL;
}

static int exfat_load_default_upcase_table(struct super_block *sb)
{
	int i;
	struct exfat_sb_info *sbi = EXFAT_SB(sb);
	unsigned char skip = false;
	unsigned short uni = 0, *upcase_table;
	unsigned int index = 0;

	upcase_table = kvcalloc(EXFAT_UTBL_COUNT, sizeof(unsigned short), GFP_KERNEL);
	if (!upcase_table)
		return -ENOMEM;

	sbi->vol_utbl = upcase_table;

	for (i = 0; index <= 0xFFFF && i < EXFAT_NUM_UPCASE; i++) {
		uni = exfat_uni_def_upcase[i];
		if (skip) {
			index += uni;
			skip = false;
		} else if (uni == index) {
			index++;
		} else if (uni == 0xFFFF) {
			skip = true;
		} else {
			upcase_table[index] = uni;
			index++;
		}
	}

	if (index >= 0xFFFF)
		return 0;

	/* FATAL error: default upcase table has error */
	return -EIO;
}

int exfat_create_upcase_table(struct super_block *sb)
{
	int i, ret;
	unsigned int tbl_clu, type;
	sector_t sector;
	unsigned long long tbl_size, num_sectors;
	unsigned char blksize_bits = sb->s_blocksize_bits;
	struct exfat_chain clu;
	struct exfat_dentry *ep;
	struct exfat_sb_info *sbi = EXFAT_SB(sb);
	struct buffer_head *bh;

	clu.dir = sbi->root_dir;
	clu.flags = ALLOC_FAT_CHAIN;

	while (clu.dir != EXFAT_EOF_CLUSTER) {
		for (i = 0; i < sbi->dentries_per_clu; i++) {
			ep = exfat_get_dentry(sb, &clu, i, &bh);
			if (!ep)
				return -EIO;

			type = exfat_get_entry_type(ep);
			if (type == TYPE_UNUSED) {
				brelse(bh);
				break;
			}

			if (type != TYPE_UPCASE) {
				brelse(bh);
				continue;
			}

			tbl_clu  = le32_to_cpu(ep->dentry.upcase.start_clu);
			tbl_size = le64_to_cpu(ep->dentry.upcase.size);
			if (tbl_size) {
				sector = exfat_cluster_to_sector(sbi, tbl_clu);
				num_sectors = ((tbl_size - 1) >> blksize_bits) + 1;
				ret = exfat_load_upcase_table(sb, sector, num_sectors,
					le32_to_cpu(ep->dentry.upcase.checksum));
			} else {
				exfat_fs_error(sb,
					       "bad upcase table size (0 bytes). Please run fsck");
				ret = -EINVAL;
			}
			brelse(bh);

			if (ret && ret != -EIO) {
				/* free memory from exfat_load_upcase_table call */
				exfat_free_upcase_table(sbi);
				goto load_default;
			}

			/* load successfully */
			return ret;
		}

		if (exfat_get_next_cluster(sb, &clu.dir))
			return -EIO;
	}

	exfat_fs_error(sb, "no upcase table entry. Please run fsck");

load_default:
	/* load default upcase table */
	return exfat_load_default_upcase_table(sb);
}

void exfat_free_upcase_table(struct exfat_sb_info *sbi)
{
	kvfree(sbi->vol_utbl);
	sbi->vol_utbl = NULL;
}
