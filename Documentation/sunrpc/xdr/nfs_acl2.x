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
 * XDR data types inherited from the NFS version 2 protocol
 */

enum ftype {
    NFNON = 0,
    NFREG = 1,
    NFDIR = 2,
    NFBLK = 3,
    NFCHR = 4,
    NFLNK = 5
};

const FHSIZE = 32;
typedef opaque fhandle[FHSIZE];

struct timeval {
    unsigned int seconds;
    unsigned int useconds;
};

struct fattr {
    ftype        type;
    unsigned int mode;
    unsigned int nlink;
    unsigned int uid;
    unsigned int gid;
    unsigned int size;
    unsigned int blocksize;
    unsigned int rdev;
    unsigned int blocks;
    unsigned int fsid;
    unsigned int fileid;
    timeval      atime;
    timeval      mtime;
    timeval      ctime;
};

/*
 * ACL error codes; the numeric values match codes with the same
 * name used in NFS version 2.
 */
enum aclstat2 {
    ACL2_OK = 0,
    ACL2ERR_PERM = 1,
    ACL2ERR_NOENT = 2,
    ACL2ERR_IO = 5,
    ACL2ERR_ACCES = 13,
    ACL2ERR_INVAL = 22,
    ACL2ERR_NOSPC = 28,
    ACL2ERR_ROFS = 30,
    ACL2ERR_NOTSUPP = 45,
    ACL2ERR_DQUOT = 69,
    ACL2ERR_STALE = 70
};

/*
 * NFS_ACL version 2 procedure arguments and results
 */

struct GETACL2args {
    fhandle fh;
    unsigned int mask;
};

struct GETACL2resok {
    fattr attr;
    secattr acl;
};

union GETACL2res switch (aclstat2 status) {
case ACL2_OK:
    GETACL2resok resok;
default:
    void;
};

struct SETACL2args {
    fhandle fh;
    secattr acl;
};

struct SETACL2resok {
    fattr attr;
};

union SETACL2res switch (aclstat2 status) {
case ACL2_OK:
    SETACL2resok resok;
default:
    void;
};

struct GETATTR2args {
    fhandle fh;
};

struct GETATTR2resok {
    fattr attr;
};

union GETATTR2res switch (aclstat2 status) {
case ACL2_OK:
    GETATTR2resok resok;
default:
    void;
};

struct ACCESS2args {
    fhandle fh;
    unsigned int access;
};

const ACCESS2_READ = 0x1;           /* read data or readdir a directory */
const ACCESS2_LOOKUP = 0x2;         /* lookup a name in a directory */
const ACCESS2_MODIFY = 0x4;         /* rewrite existing file data or */
                                    /* modify existing directory entries */
const ACCESS2_EXTEND = 0x8;         /* write new data or add directory entries */
const ACCESS2_DELETE = 0x10;        /* delete existing directory entry */
const ACCESS2_EXECUTE = 0x20;       /* execute file (no meaning for a directory) */

struct ACCESS2resok {
    fattr attr;
    unsigned int access;
};

union ACCESS2res switch (aclstat2 status) {
case ACL2_OK:
    ACCESS2resok resok;
default:
    void;
};

/*
 * This is the definition for the GETXATTRDIR procedure which applies
 * to NFS Version 2 files.
 */
struct GETXATTRDIR2args {
    fhandle fh;
    bool create;
};

struct GETXATTRDIR2resok {
    fhandle fh;
    fattr attr;
};

union GETXATTRDIR2res switch (aclstat2 status) {
case ACL2_OK:
    GETXATTRDIR2resok resok;
default:
    void;
};

program NFS_ACL_PROGRAM {
    version NFS_ACL_V2 {
        void
            ACLPROC2_NULL(void) = 0;
        GETACL2res
            ACLPROC2_GETACL(GETACL2args) = 1;
        SETACL2res
            ACLPROC2_SETACL(SETACL2args) = 2;
        GETATTR2res
            ACLPROC2_GETATTR(GETATTR2args) = 3;
        ACCESS2res
            ACLPROC2_ACCESS(ACCESS2args) = 4;
        GETXATTRDIR2res
            ACLPROC2_GETXATTRDIR(GETXATTRDIR2args) = 5;
    } = 2;
} = 100227;
