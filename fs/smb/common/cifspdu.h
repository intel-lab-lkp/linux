/* SPDX-License-Identifier: LGPL-2.1 */
/*
 *
 *   Copyright (c) International Business Machines  Corp., 2002,2009
 *   Author(s): Steve French (sfrench@us.ibm.com)
 *
 */

#ifndef _COMMON_CIFSPDU_H
#define _COMMON_CIFSPDU_H

#define BAD_PROT 0xFFFF

/* SMB command codes:
 * Note some commands have minimal (wct=0,bcc=0), or uninteresting, responses
 * (ie which include no useful data other than the SMB error code itself).
 * This can allow us to avoid response buffer allocations and copy in some cases
 */
#define SMB_COM_CREATE_DIRECTORY      0x00 /* trivial response */
#define SMB_COM_DELETE_DIRECTORY      0x01 /* trivial response */
#define SMB_COM_CLOSE                 0x04 /* triv req/rsp, timestamp ignored */
#define SMB_COM_FLUSH                 0x05 /* triv req/rsp */
#define SMB_COM_DELETE                0x06 /* trivial response */
#define SMB_COM_RENAME                0x07 /* trivial response */
#define SMB_COM_QUERY_INFORMATION     0x08 /* aka getattr */
#define SMB_COM_SETATTR               0x09 /* trivial response */
#define SMB_COM_LOCKING_ANDX          0x24 /* trivial response */
#define SMB_COM_COPY                  0x29 /* trivial rsp, fail filename ignrd*/
#define SMB_COM_ECHO                  0x2B /* echo request */
#define SMB_COM_OPEN_ANDX             0x2D /* Legacy open for old servers */
#define SMB_COM_READ_ANDX             0x2E
#define SMB_COM_WRITE_ANDX            0x2F
#define SMB_COM_TRANSACTION2          0x32
#define SMB_COM_TRANSACTION2_SECONDARY 0x33
#define SMB_COM_FIND_CLOSE2           0x34 /* trivial response */
#define SMB_COM_TREE_DISCONNECT       0x71 /* trivial response */
#define SMB_COM_NEGOTIATE             0x72
#define SMB_COM_SESSION_SETUP_ANDX    0x73
#define SMB_COM_LOGOFF_ANDX           0x74 /* trivial response */
#define SMB_COM_TREE_CONNECT_ANDX     0x75
#define SMB_COM_NT_TRANSACT           0xA0
#define SMB_COM_NT_TRANSACT_SECONDARY 0xA1
#define SMB_COM_NT_CREATE_ANDX        0xA2
#define SMB_COM_NT_CANCEL             0xA4 /* no response */
#define SMB_COM_NT_RENAME             0xA5 /* trivial response */

#define MAX_CIFS_SMALL_BUFFER_SIZE 448 /* big enough for most */

/*
 * SMB flag definitions
 */
#define SMBFLG_EXTD_LOCK 0x01	/* server supports lock-read write-unlock smb */
#define SMBFLG_RCV_POSTED 0x02	/* obsolete */
#define SMBFLG_RSVD 0x04
#define SMBFLG_CASELESS 0x08	/* all pathnames treated as caseless (off
				implies case sensitive file handling request) */
#define SMBFLG_CANONICAL_PATH_FORMAT 0x10	/* obsolete */
#define SMBFLG_OLD_OPLOCK 0x20	/* obsolete */
#define SMBFLG_OLD_OPLOCK_NOTIFY 0x40	/* obsolete */
#define SMBFLG_RESPONSE 0x80	/* this PDU is a response from server */

/*
 * SMB flag2 definitions
 */
#define SMBFLG2_KNOWS_LONG_NAMES cpu_to_le16(1)	/* can send long (non-8.3)
						   path names in response */
#define SMBFLG2_KNOWS_EAS cpu_to_le16(2)
#define SMBFLG2_SECURITY_SIGNATURE cpu_to_le16(4)
#define SMBFLG2_COMPRESSED (8)
#define SMBFLG2_SECURITY_SIGNATURE_REQUIRED (0x10)
#define SMBFLG2_IS_LONG_NAME cpu_to_le16(0x40)
#define SMBFLG2_REPARSE_PATH (0x400)
#define SMBFLG2_EXT_SEC cpu_to_le16(0x800)
#define SMBFLG2_DFS cpu_to_le16(0x1000)
#define SMBFLG2_PAGING_IO cpu_to_le16(0x2000)
#define SMBFLG2_ERR_STATUS cpu_to_le16(0x4000)
#define SMBFLG2_UNICODE cpu_to_le16(0x8000)

/*
 * These are the file access permission bits defined in CIFS for the
 * NTCreateAndX as well as the level 0x107
 * TRANS2_QUERY_PATH_INFORMATION API.  The level 0x107, SMB_QUERY_FILE_ALL_INFO
 * responds with the AccessFlags.
 * The AccessFlags specifies the access permissions a caller has to the
 * file and can have any suitable combination of the following values:
 */

#define FILE_READ_DATA        0x00000001  /* Data can be read from the file   */
					  /* or directory child entries can   */
					  /* be listed together with the      */
					  /* associated child attributes      */
					  /* (so the FILE_READ_ATTRIBUTES on  */
					  /* the child entry is not needed)   */
#define FILE_WRITE_DATA       0x00000002  /* Data can be written to the file  */
					  /* or new file can be created in    */
					  /* the directory                    */
#define FILE_APPEND_DATA      0x00000004  /* Data can be appended to the file */
					  /* (for non-local files over SMB it */
					  /* is same as FILE_WRITE_DATA)      */
					  /* or new subdirectory can be       */
					  /* created in the directory         */
#define FILE_READ_EA          0x00000008  /* Extended attributes associated   */
					  /* with the file can be read        */
#define FILE_WRITE_EA         0x00000010  /* Extended attributes associated   */
					  /* with the file can be written     */
#define FILE_EXECUTE          0x00000020  /*Data can be read into memory from */
					  /* the file using system paging I/O */
					  /* for executing the file / script  */
					  /* or right to traverse directory   */
					  /* (but by default all users have   */
					  /* directory bypass traverse        */
					  /* privilege and do not need this   */
					  /* permission on directories at all)*/
#define FILE_DELETE_CHILD     0x00000040  /* Child entry can be deleted from  */
					  /* the directory (so the DELETE on  */
					  /* the child entry is not needed)   */
#define FILE_READ_ATTRIBUTES  0x00000080  /* Attributes associated with the   */
					  /* file or directory can be read    */
#define FILE_WRITE_ATTRIBUTES 0x00000100  /* Attributes associated with the   */
					  /* file or directory can be written */
#define DELETE                0x00010000  /* The file or dir can be deleted   */
#define READ_CONTROL          0x00020000  /* The discretionary access control */
					  /* list and ownership associated    */
					  /* with the file or dir can be read */
#define WRITE_DAC             0x00040000  /* The discretionary access control */
					  /* list associated with the file or */
					  /* directory can be written         */
#define WRITE_OWNER           0x00080000  /* Ownership information associated */
					  /* with the file/dir can be written */
#define SYNCHRONIZE           0x00100000  /* The file handle can waited on to */
					  /* synchronize with the completion  */
					  /* of an input/output request       */
#define SYSTEM_SECURITY       0x01000000  /* The system access control list   */
					  /* associated with the file or      */
					  /* directory can be read or written */
					  /* (cannot be in DACL, can in SACL) */
#define MAXIMUM_ALLOWED       0x02000000  /* Maximal subset of GENERIC_ALL    */
					  /* permissions which can be granted */
					  /* (cannot be in DACL nor SACL)     */
#define GENERIC_ALL           0x10000000  /* Same as: GENERIC_EXECUTE |       */
					  /*          GENERIC_WRITE |         */
					  /*          GENERIC_READ |          */
					  /*          FILE_DELETE_CHILD |     */
					  /*          DELETE |                */
					  /*          WRITE_DAC |             */
					  /*          WRITE_OWNER             */
					  /* So GENERIC_ALL contains all bits */
					  /* mentioned above except these two */
					  /* SYSTEM_SECURITY  MAXIMUM_ALLOWED */
#define GENERIC_EXECUTE       0x20000000  /* Same as: FILE_EXECUTE |          */
					  /*          FILE_READ_ATTRIBUTES |  */
					  /*          READ_CONTROL |          */
					  /*          SYNCHRONIZE             */
#define GENERIC_WRITE         0x40000000  /* Same as: FILE_WRITE_DATA |       */
					  /*          FILE_APPEND_DATA |      */
					  /*          FILE_WRITE_EA |         */
					  /*          FILE_WRITE_ATTRIBUTES | */
					  /*          READ_CONTROL |          */
					  /*          SYNCHRONIZE             */
#define GENERIC_READ          0x80000000  /* Same as: FILE_READ_DATA |        */
					  /*          FILE_READ_EA |          */
					  /*          FILE_READ_ATTRIBUTES |  */
					  /*          READ_CONTROL |          */
					  /*          SYNCHRONIZE             */

#define FILE_READ_RIGHTS (FILE_READ_DATA | FILE_READ_EA | FILE_READ_ATTRIBUTES)
#define FILE_WRITE_RIGHTS (FILE_WRITE_DATA | FILE_APPEND_DATA \
				| FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES)
#define FILE_EXEC_RIGHTS (FILE_EXECUTE)

#define CLIENT_SET_FILE_READ_RIGHTS (FILE_READ_DATA | FILE_READ_EA | FILE_WRITE_EA \
				| FILE_READ_ATTRIBUTES \
				| FILE_WRITE_ATTRIBUTES \
				| DELETE | READ_CONTROL | WRITE_DAC \
				| WRITE_OWNER | SYNCHRONIZE)
#define SERVER_SET_FILE_READ_RIGHTS (FILE_READ_DATA | FILE_READ_EA \
				| FILE_READ_ATTRIBUTES \
				| DELETE | READ_CONTROL | WRITE_DAC \
				| WRITE_OWNER | SYNCHRONIZE)
#define CLIENT_SET_FILE_WRITE_RIGHTS (FILE_WRITE_DATA | FILE_APPEND_DATA \
				| FILE_READ_EA | FILE_WRITE_EA \
				| FILE_READ_ATTRIBUTES \
				| FILE_WRITE_ATTRIBUTES \
				| DELETE | READ_CONTROL | WRITE_DAC \
				| WRITE_OWNER | SYNCHRONIZE)
#define SERVER_SET_FILE_WRITE_RIGHTS (FILE_WRITE_DATA | FILE_APPEND_DATA \
				| FILE_WRITE_EA \
				| FILE_DELETE_CHILD \
				| FILE_WRITE_ATTRIBUTES \
				| DELETE | READ_CONTROL | WRITE_DAC \
				| WRITE_OWNER | SYNCHRONIZE)
#define SET_FILE_EXEC_RIGHTS (FILE_READ_EA | FILE_WRITE_EA | FILE_EXECUTE \
				| FILE_READ_ATTRIBUTES \
				| FILE_WRITE_ATTRIBUTES \
				| DELETE | READ_CONTROL | WRITE_DAC \
				| WRITE_OWNER | SYNCHRONIZE)
#define SET_MINIMUM_RIGHTS (FILE_READ_EA | FILE_READ_ATTRIBUTES \
				| READ_CONTROL | SYNCHRONIZE)

/*
 * File Attribute flags - see MS-SMB 2.2.1.4.1
 */
#define ATTR_READONLY  0x0001
#define ATTR_HIDDEN    0x0002
#define ATTR_SYSTEM    0x0004
#define ATTR_VOLUME    0x0008
#define ATTR_DIRECTORY 0x0010
#define ATTR_ARCHIVE   0x0020
#define ATTR_DEVICE    0x0040
#define ATTR_NORMAL    0x0080
#define ATTR_TEMPORARY 0x0100
#define ATTR_SPARSE    0x0200
#define ATTR_REPARSE   0x0400
#define ATTR_COMPRESSED 0x0800
#define ATTR_OFFLINE    0x1000	/* ie file not immediately available -
					on offline storage */
#define ATTR_NOT_CONTENT_INDEXED 0x2000
#define ATTR_ENCRYPTED  0x4000
#define ATTR_POSIX_SEMANTICS 0x01000000
#define ATTR_BACKUP_SEMANTICS 0x02000000
#define ATTR_DELETE_ON_CLOSE 0x04000000
#define ATTR_SEQUENTIAL_SCAN 0x08000000
#define ATTR_RANDOM_ACCESS   0x10000000
#define ATTR_NO_BUFFERING    0x20000000
#define ATTR_WRITE_THROUGH   0x80000000

struct smb_hdr {
	__be32 smb_buf_length;	/* BB length is only two (rarely three) bytes,
		with one or two byte "type" preceding it that will be
		zero - we could mask the type byte off */
	__u8 Protocol[4];
	__u8 Command;
	union {
		struct {
			__u8 ErrorClass;
			__u8 Reserved;
			__le16 Error;
		} __attribute__((packed)) DosError;
		__le32 CifsError;
	} __attribute__((packed)) Status;
	__u8 Flags;
	__le16 Flags2;		/* note: le */
	__le16 PidHigh;
	union {
		struct {
			__le32 SequenceNumber;  /* le */
			__u32 Reserved; /* zero */
		} __attribute__((packed)) Sequence;
		__u8 SecuritySignature[8];	/* le */
	} __attribute__((packed)) Signature;
	__u8 pad[2];
	__u16 Tid;
	__le16 Pid;
	__u16 Uid;
	__le16 Mid;
	__u8 WordCount;
} __attribute__((packed));

/*
 *  SMB frame definitions  (following must be packed structs)
 *  See the SNIA CIFS Specification for details.
 *
 *  The Naming convention is the lower case version of the
 *  smb command code name for the struct and this is typedef to the
 *  uppercase version of the same name with the prefix SMB_ removed
 *  for brevity.  Although typedefs are not commonly used for
 *  structure definitions in the Linux kernel, their use in the
 *  CIFS standards document, which this code is based on, may
 *  make this one of the cases where typedefs for structures make
 *  sense to improve readability for readers of the standards doc.
 *  Typedefs can always be removed later if they are too distracting
 *  and they are only used for the CIFSs PDUs themselves, not
 *  internal cifs vfs structures
 *
 */

typedef struct negotiate_req {
	struct smb_hdr hdr;	/* wct = 0 */
	__le16 ByteCount;
	unsigned char DialectsArray[];
} __attribute__((packed)) NEGOTIATE_REQ;

#define MIN_TZ_ADJ (15 * 60) /* minimum grid for timezones in seconds */

#define READ_RAW_ENABLE 1
#define WRITE_RAW_ENABLE 2
#define RAW_ENABLE (READ_RAW_ENABLE | WRITE_RAW_ENABLE)
#define SMB1_CLIENT_GUID_SIZE (16)

typedef struct client_negotiate_rsp {
	struct smb_hdr hdr;	/* wct = 17 */
	__le16 DialectIndex; /* 0xFFFF = no dialect acceptable */
	__u8 SecurityMode;
	__le16 MaxMpxCount;
	__le16 MaxNumberVcs;
	__le32 MaxBufferSize;
	__le32 MaxRawSize;
	__le32 SessionKey;
	__le32 Capabilities;	/* see below */
	__le32 SystemTimeLow;
	__le32 SystemTimeHigh;
	__le16 ServerTimeZone;
	__u8 EncryptionKeyLength;
	__u16 ByteCount;
	union {
		/* cap extended security off */
		DECLARE_FLEX_ARRAY(unsigned char, EncryptionKey);
		/* followed by Domain name - if extended security is off */
		/* followed by 16 bytes of server GUID */
		/* then security blob if cap_extended_security negotiated */
		struct {
			unsigned char GUID[SMB1_CLIENT_GUID_SIZE];
			unsigned char SecurityBlob[];
		} __attribute__((packed)) extended_response;
	} __attribute__((packed)) u;
} __attribute__((packed)) CLIENT_NEGOTIATE_RSP;

typedef struct server_negotiate_rsp {
	struct smb_hdr hdr;     /* wct = 17 */
	__le16 DialectIndex; /* 0xFFFF = no dialect acceptable */
	__le16 ByteCount;
} __attribute__((packed)) SERVER_NEGOTIATE_RSP;

/* See MS-CIFS 2.2.8.2.4 */
typedef struct {
	__le64 TotalAllocationUnits;
	__le64 FreeAllocationUnits;
	__le32 SectorsPerAllocationUnit;
	__le32 BytesPerSector;
} __attribute__((packed)) FILE_SYSTEM_INFO;	/* size info, level 0x103 */

typedef struct {
	/* For undefined recommended transfer size return -1 in that field */
	__le32 OptimalTransferSize;  /* bsize on some os, iosize on other os */
	__le32 BlockSize;
	/* The next three fields are in terms of the block size.
	 * (above). If block size is unknown, 4096 would be a
	 * reasonable block size for a server to report.
	 * Note that returning the blocks/blocksavail removes need
	 * to make a second call (to QFSInfo level 0x103 to get this info.
	 * UserBlockAvail is typically less than or equal to BlocksAvail,
	 * if no distinction is made return the same value in each
	 */
	__le64 TotalBlocks;
	__le64 BlocksAvail;       /* bfree */
	__le64 UserBlocksAvail;   /* bavail */
	/* For undefined Node fields or FSID return -1 */
	__le64 TotalFileNodes;
	__le64 FreeFileNodes;
	__le64 FileSysIdentifier;   /* fsid */
	/* NB Namelen comes from FILE_SYSTEM_ATTRIBUTE_INFO call */
	/* NB flags can come from FILE_SYSTEM_DEVICE_INFO call   */
} __attribute__((packed)) FILE_SYSTEM_POSIX_INFO;

/* See MS-CIFS 2.2.8.2.5 */
typedef struct {
	__le32 DeviceType;
	__le32 DeviceCharacteristics;
} __attribute__((packed)) FILE_SYSTEM_DEVICE_INFO; /* device info level 0x104 */

/* List of FileSystemAttributes - see 2.5.1 of MS-FSCC */
#define FILE_SUPPORTS_SPARSE_VDL	0x10000000 /* faster nonsparse extend */
#define FILE_SUPPORTS_BLOCK_REFCOUNTING	0x08000000 /* allow ioctl dup extents */
#define FILE_SUPPORT_INTEGRITY_STREAMS	0x04000000
#define FILE_SUPPORTS_USN_JOURNAL	0x02000000
#define FILE_SUPPORTS_OPEN_BY_FILE_ID	0x01000000
#define FILE_SUPPORTS_EXTENDED_ATTRIBUTES 0x00800000
#define FILE_SUPPORTS_HARD_LINKS	0x00400000
#define FILE_SUPPORTS_TRANSACTIONS	0x00200000
#define FILE_SEQUENTIAL_WRITE_ONCE	0x00100000
#define FILE_READ_ONLY_VOLUME		0x00080000
#define FILE_NAMED_STREAMS		0x00040000
#define FILE_SUPPORTS_ENCRYPTION	0x00020000
#define FILE_SUPPORTS_OBJECT_IDS	0x00010000
#define FILE_VOLUME_IS_COMPRESSED	0x00008000
#define FILE_SUPPORTS_POSIX_UNLINK_RENAME 0x00000400
#define FILE_RETURNS_CLEANUP_RESULT_INFO  0x00000200
#define FILE_SUPPORTS_REMOTE_STORAGE	0x00000100
#define FILE_SUPPORTS_REPARSE_POINTS	0x00000080
#define FILE_SUPPORTS_SPARSE_FILES	0x00000040
#define FILE_VOLUME_QUOTAS		0x00000020
#define FILE_FILE_COMPRESSION		0x00000010
#define FILE_PERSISTENT_ACLS		0x00000008
#define FILE_UNICODE_ON_DISK		0x00000004
#define FILE_CASE_PRESERVED_NAMES	0x00000002
#define FILE_CASE_SENSITIVE_SEARCH	0x00000001

/* See FS-FSCC 2.5.1 */
typedef struct {
	__le32 Attributes;
	__le32 MaxPathNameComponentLength;
	__le32 FileSystemNameLen;
	__le16 FileSystemName[]; /* do not have to save this - get subset? */
} __attribute__((packed)) FILE_SYSTEM_ATTRIBUTE_INFO;

/* See MS-CIFS 2.2.8.1.4 */
typedef struct {
	__le32 NextEntryOffset;
	__u32 FileIndex;
	__le64 CreationTime;
	__le64 LastAccessTime;
	__le64 LastWriteTime;
	__le64 ChangeTime;
	__le64 EndOfFile;
	__le64 AllocationSize;
	__le32 ExtFileAttributes;
	__le32 FileNameLength;
	char FileName[];
} __attribute__((packed)) FILE_DIRECTORY_INFO;   /* level 0x101 FF resp data */

/* See MS-CIFS 2.2.8.1.5 */
typedef struct {
	__le32 NextEntryOffset;
	__u32 FileIndex;
	__le64 CreationTime;
	__le64 LastAccessTime;
	__le64 LastWriteTime;
	__le64 ChangeTime;
	__le64 EndOfFile;
	__le64 AllocationSize;
	__le32 ExtFileAttributes;
	__le32 FileNameLength;
	__le32 EaSize; /* length of the xattrs */
	char FileName[];
} __attribute__((packed)) FILE_FULL_DIRECTORY_INFO; /* level 0x102 rsp data */

/* See MS-SMB 2.2.8.1.2 */
typedef struct {
	__le32 NextEntryOffset;
	__u32 FileIndex;
	__le64 CreationTime;
	__le64 LastAccessTime;
	__le64 LastWriteTime;
	__le64 ChangeTime;
	__le64 EndOfFile;
	__le64 AllocationSize;
	__le32 ExtFileAttributes;
	__le32 FileNameLength;
	__le32 EaSize; /* EA size */
	__le32 Reserved;
	__le64 UniqueId; /* inode num - le since Samba puts ino in low 32 bit*/
	char FileName[];
} __attribute__((packed)) SEARCH_ID_FULL_DIR_INFO; /* level 0x105 FF rsp data */

/* See MS-CIFS 2.2.8.1.7 */
typedef struct {
	__le32 NextEntryOffset;
	__u32 FileIndex;
	__le64 CreationTime;
	__le64 LastAccessTime;
	__le64 LastWriteTime;
	__le64 ChangeTime;
	__le64 EndOfFile;
	__le64 AllocationSize;
	__le32 ExtFileAttributes;
	__le32 FileNameLength;
	__le32 EaSize; /* length of the xattrs */
	__u8   ShortNameLength;
	__u8   Reserved;
	__u8   ShortName[24];
	char FileName[];
} __attribute__((packed)) FILE_BOTH_DIRECTORY_INFO; /* level 0x104 FFrsp data */

#endif /* _COMMON_CIFSPDU_H */
