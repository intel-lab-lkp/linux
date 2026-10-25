/*
 * Copyright (c) 2024 IETF Trust and the persons
 * identified as authors of the code.  All rights reserved.
 *
 * The authors of the code are:
 * Oracle
 *
 * Redistribution and use in source and binary forms, with
 * or without modification, are permitted provided that the
 * following conditions are met:
 *
 * - Redistributions of source code must retain the above
 *   copyright notice, this list of conditions and the
 *   following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above
 *   copyright notice, this list of conditions and the
 *   following disclaimer in the documentation and/or other
 *   materials provided with the distribution.
 *
 * - Neither the name of Internet Society, IETF or IETF
 *   Trust, nor the names of specific contributors, may be
 *   used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS
 *   AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
 *   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *   IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *   FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO
 *   EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 *   LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 *   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 *   NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 *   SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *   INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 *   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 *   OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 *   IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 *   ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

const NFS_ACL_MAX_ENTRIES = 1024;

typedef unsigned int uid;
typedef unsigned short o_mode;

/*
 * This is the format of an ACL which is passed over the network.
 */
struct aclent {
    int type;
    uid id;
    o_mode perm;
};

/*
 * The values for the type element of the aclent structure.
 */
const NA_USER_OBJ = 0x1;            /* object owner */
const NA_USER = 0x2;                /* additional users */
const NA_GROUP_OBJ = 0x4;           /* owning group of the object */
const NA_GROUP = 0x8;               /* additional groups */
const NA_CLASS_OBJ = 0x10;          /* file group class and mask entry */
const NA_OTHER_OBJ = 0x20;          /* other entry for the object */
const NA_ACL_DEFAULT = 0x1000;      /* default flag */

/*
 * The bit field values for the perm element of the aclent
 * structure.  The three values can be combined to form any
 * of the 8 combinations.
 */
const NA_READ = 0x4;                /* read permission */
const NA_WRITE = 0x2;               /* write permission */
const NA_EXEC = 0x1;                /* exec permission */

/*
 * This is the structure which contains the ACL entries for a
 * particular entity.  It contains the ACL entries which apply
 * to this object plus any default ACL entries which are
 * inherited by its children.
 *
 * The values for the mask field are defined below.
 */
struct secattr {
    unsigned int mask;
    int aclcnt;
    aclent aclent<NFS_ACL_MAX_ENTRIES>;
    int dfaclcnt;
    aclent dfaclent<NFS_ACL_MAX_ENTRIES>;
};

/*
 * The values for the mask element of the secattr struct as well
 * as for the mask element in the arguments in the GETACL2 and
 * GETACL3 procedures.
 */
const NA_ACL = 0x1;                 /* aclent contains a valid list */
const NA_ACLCNT = 0x2;              /* number of entries in the aclent list */
const NA_DFACL = 0x4;               /* dfaclent contains a valid list */
const NA_DFACLCNT = 0x8;            /* number of entries in the dfaclent list */

/*
 * Share the ports with the NFS service.
 */
const NFS_ACL_PORT = 2049;
const NFS_ACL_RDMA_PORT = 20049;

/*
 * XDR data types inherited from the NFS version 3 protocol
 */

typedef unsigned hyper uint64;
typedef unsigned long uint32;
typedef uint64 fileid3;
typedef uint32 uid3;
typedef uint32 gid3;
typedef uint64 size3;
typedef uint32 mode3;

enum ftype3 {
    NF3REG    = 1,
    NF3DIR    = 2,
    NF3BLK    = 3,
    NF3CHR    = 4,
    NF3LNK    = 5,
    NF3SOCK   = 6,
    NF3FIFO   = 7
};

struct specdata3 {
    uint32     specdata1;
    uint32     specdata2;
};

const NFS3_FHSIZE = 64;

struct nfs_fh3 {
    opaque       data<NFS3_FHSIZE>;
};

struct nfstime3 {
    uint32   seconds;
    uint32   nseconds;
};

struct fattr3 {
    ftype3     type;
    mode3      mode;
    uint32     nlink;
    uid3       uid;
    gid3       gid;
    size3      size;
    size3      used;
    specdata3  rdev;
    uint64     fsid;
    fileid3    fileid;
    nfstime3   atime;
    nfstime3   mtime;
    nfstime3   ctime;
};

union post_op_attr switch (bool attributes_follow) {
case TRUE:
    fattr3   attributes;
case FALSE:
    void;
};

/*
 * ACL error codes; the numeric values match codes with the same
 * name used in NFS version 3.
 */
enum aclstat3 {
    ACL3_OK = 0,
    ACL3ERR_PERM = 1,
    ACL3ERR_NOENT = 2,
    ACL3ERR_IO = 5,
    ACL3ERR_ACCES = 13,
    ACL3ERR_INVAL = 22,
    ACL3ERR_NOSPC = 28,
    ACL3ERR_ROFS = 30,
    ACL3ERR_DQUOT = 69,
    ACL3ERR_STALE = 70,
    ACL3ERR_BADHANDLE = 10001,
    ACL3ERR_NOTSUPP = 10004,
    ACL3ERR_SERVERFAULT = 10006,
    ACL3ERR_JUKEBOX = 10008
};

/*
 * NFS_ACL version 3 procedure arguments and results
 */

struct GETACL3args {
    nfs_fh3 fh;
    unsigned int mask;
};

struct GETACL3resok {
    post_op_attr attr;
    secattr acl;
};

struct GETACL3resfail {
    post_op_attr attr;
};

union GETACL3res switch (aclstat3 status) {
case ACL3_OK:
    GETACL3resok resok;
default:
    GETACL3resfail resfail;
};

struct SETACL3args {
    nfs_fh3 fh;
    secattr acl;
};

struct SETACL3resok {
    post_op_attr attr;
};

struct SETACL3resfail {
    post_op_attr attr;
};

union SETACL3res switch (aclstat3 status) {
case ACL3_OK:
    SETACL3resok resok;
default:
    SETACL3resfail resfail;
};

/*
 * This is the definition for the GETXATTRDIR procedure which
 * applies to NFS Version 3 files.
 */
struct GETXATTRDIR3args {
    nfs_fh3 fh;
    bool create;
};

struct GETXATTRDIR3resok {
    nfs_fh3 fh;
    post_op_attr attr;
};

union GETXATTRDIR3res switch (aclstat3 status) {
case ACL3_OK:
    GETXATTRDIR3resok resok;
default:
    void;
};

program NFS_ACL_PROGRAM {
    version NFS_ACL_V3 {
        void
            ACLPROC3_NULL(void) = 0;
        GETACL3res
            ACLPROC3_GETACL(GETACL3args) = 1;
        SETACL3res
            ACLPROC3_SETACL(SETACL3args) = 2;
        GETXATTRDIR3res
            ACLPROC3_GETXATTRDIR(GETXATTRDIR3args) = 3;
    } = 3;
} = 100227;
