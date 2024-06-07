/* SPDX-License-Identifier: GPL-2.0 */
/*
 * NFS protocol definitions
 *
 * This file contains constants mostly for Version 2 of the protocol,
 * but also has a couple of NFSv3 bits in (notably the error codes).
 */
#ifndef _LINUX_NFS_H
#define _LINUX_NFS_H

#include <linux/cred.h>
#include <linux/sunrpc/auth.h>
#include <linux/sunrpc/msg_prot.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/crc32.h>
#include <uapi/linux/nfs.h>

/*
 * This is the kernel NFS client file handle representation
 */
#define NFS_MAXFHSIZE		128
struct nfs_fh {
	unsigned short		size;
	unsigned char		data[NFS_MAXFHSIZE];
};

/*
 * Returns a zero iff the size and data fields match.
 * Checks only "size" bytes in the data field.
 */
static inline int nfs_compare_fh(const struct nfs_fh *a, const struct nfs_fh *b)
{
	return a->size != b->size || memcmp(a->data, b->data, a->size) != 0;
}

static inline void nfs_copy_fh(struct nfs_fh *target, const struct nfs_fh *source)
{
	target->size = source->size;
	memcpy(target->data, source->data, source->size);
}

enum nfs3_stable_how {
	NFS_UNSTABLE = 0,
	NFS_DATA_SYNC = 1,
	NFS_FILE_SYNC = 2,

	/* used by direct.c to mark verf as invalid */
	NFS_INVALID_STABLE_HOW = -1
};

/*
 * We need to translate between nfs status return values and
 * the local errno values which may not be the same.
 */
static const struct {
	int stat;
	int errno;
} nfs_common_errtbl[] = {
	{ NFS_OK,		0		},
	{ NFSERR_PERM,		-EPERM		},
	{ NFSERR_NOENT,		-ENOENT		},
	{ NFSERR_IO,		-EIO		},
	{ NFSERR_NXIO,		-ENXIO		},
/*	{ NFSERR_EAGAIN,	-EAGAIN		}, */
	{ NFSERR_ACCES,		-EACCES		},
	{ NFSERR_EXIST,		-EEXIST		},
	{ NFSERR_XDEV,		-EXDEV		},
	{ NFSERR_NODEV,		-ENODEV		},
	{ NFSERR_NOTDIR,	-ENOTDIR	},
	{ NFSERR_ISDIR,		-EISDIR		},
	{ NFSERR_INVAL,		-EINVAL		},
	{ NFSERR_FBIG,		-EFBIG		},
	{ NFSERR_NOSPC,		-ENOSPC		},
	{ NFSERR_ROFS,		-EROFS		},
	{ NFSERR_MLINK,		-EMLINK		},
	{ NFSERR_NAMETOOLONG,	-ENAMETOOLONG	},
	{ NFSERR_NOTEMPTY,	-ENOTEMPTY	},
	{ NFSERR_DQUOT,		-EDQUOT		},
	{ NFSERR_STALE,		-ESTALE		},
	{ NFSERR_REMOTE,	-EREMOTE	},
#ifdef EWFLUSH
	{ NFSERR_WFLUSH,	-EWFLUSH	},
#endif
	{ NFSERR_BADHANDLE,	-EBADHANDLE	},
	{ NFSERR_NOT_SYNC,	-ENOTSYNC	},
	{ NFSERR_BAD_COOKIE,	-EBADCOOKIE	},
	{ NFSERR_NOTSUPP,	-ENOTSUPP	},
	{ NFSERR_TOOSMALL,	-ETOOSMALL	},
	{ NFSERR_SERVERFAULT,	-EREMOTEIO	},
	{ NFSERR_BADTYPE,	-EBADTYPE	},
	{ NFSERR_JUKEBOX,	-EJUKEBOX	},
	{ -1,			-EIO		}
};

/**
 * nfs_stat_to_errno - convert an NFS status code to a local errno
 * @status: NFS status code to convert
 *
 * Returns a local errno value, or -EIO if the NFS status code is
 * not recognized.  This function is used jointly by NFSv2 and NFSv3.
 */
static inline int nfs_stat_to_errno(enum nfs_stat status)
{
	int i;

	for (i = 0; nfs_common_errtbl[i].stat != -1; i++) {
		if (nfs_common_errtbl[i].stat == (int)status)
			return nfs_common_errtbl[i].errno;
	}
	return nfs_common_errtbl[i].errno;
}

#ifdef CONFIG_CRC32
/**
 * nfs_fhandle_hash - calculate the crc32 hash for the filehandle
 * @fh - pointer to filehandle
 *
 * returns a crc32 hash for the filehandle that is compatible with
 * the one displayed by "wireshark".
 */
static inline u32 nfs_fhandle_hash(const struct nfs_fh *fh)
{
	return ~crc32_le(0xFFFFFFFF, &fh->data[0], fh->size);
}
#else /* CONFIG_CRC32 */
static inline u32 nfs_fhandle_hash(const struct nfs_fh *fh)
{
	return 0;
}
#endif /* CONFIG_CRC32 */
#endif /* _LINUX_NFS_H */
