/* SPDX-License-Identifier: LGPL-2.1 */
/*
 *
 *   Copyright (c) International Business Machines  Corp., 2002,2008
 *   Author(s): Steve French (sfrench@us.ibm.com)
 *
 */
#ifndef _CIFSPROTO_H
#define _CIFSPROTO_H
#include <linux/nls.h>
#include <linux/ctype.h>
#include <net/genetlink.h>
#include "trace.h"
#ifdef CONFIG_CIFS_DFS_UPCALL
#include "dfs_cache.h"
#endif

struct statfs;
struct smb_rqst;
struct smb3_fs_context;


/*
 * asn1.c
 */
int decode_negTokenInit(unsigned char *security_blob, int length,
		    struct TCP_Server_Info *server);
int cifs_gssapi_this_mech(void *context, size_t hdrlen,
			  unsigned char tag, const void *value, size_t vlen);
int cifs_neg_token_init_mech_type(void *context, size_t hdrlen,
				  unsigned char tag,
				  const void *value, size_t vlen);

/*
 * cifsacl.c
 */
int sid_to_id(struct cifs_sb_info *cifs_sb, struct smb_sid *psid,
		struct cifs_fattr *fattr, uint sidtype);
int init_cifs_idmap(void);
void exit_cifs_idmap(void);
unsigned int setup_authusers_ACE(struct smb_ace *pntace);
unsigned int setup_special_mode_ACE(struct smb_ace *pntace,
				    bool posix,
				    __u64 nmode);
unsigned int setup_special_user_owner_ACE(struct smb_ace *pntace);
struct smb_ntsd *get_cifs_acl_by_fid(struct cifs_sb_info *cifs_sb,
				      const struct cifs_fid *cifsfid, u32 *pacllen,
				      u32 info);
struct smb_ntsd *get_cifs_acl(struct cifs_sb_info *cifs_sb,
				      struct inode *inode, const char *path,
			       u32 *pacllen, u32 info);
int set_cifs_acl(struct smb_ntsd *pnntsd, __u32 acllen,
			struct inode *inode, const char *path, int aclflag);
int cifs_acl_to_fattr(struct cifs_sb_info *cifs_sb, struct cifs_fattr *fattr,
		  struct inode *inode, bool mode_from_special_sid,
		  const char *path, const struct cifs_fid *pfid);
int id_mode_to_cifs_acl(struct inode *inode, const char *path, __u64 *pnmode,
			kuid_t uid, kgid_t gid);
struct posix_acl *cifs_get_acl(struct mnt_idmap *idmap,
			       struct dentry *dentry, int type);
int cifs_set_acl(struct mnt_idmap *idmap, struct dentry *dentry,
		 struct posix_acl *acl, int type);

/*
 * cifs_debug.c
 */
void cifs_dump_mem(char *label, void *data, int length);
void cifs_dump_detail(void *buf, struct TCP_Server_Info *server);
void cifs_dump_mids(struct TCP_Server_Info *server);
void cifs_proc_init(void);
void cifs_proc_clean(void);
void cifs_proc_init(void);
void cifs_proc_clean(void);

/*
 * cifsencrypt.c
 */
int __cifs_calc_signature(struct smb_rqst *rqst,
			  struct TCP_Server_Info *server, char *signature,
			  struct shash_desc *shash);
int cifs_sign_rqst(struct smb_rqst *rqst, struct TCP_Server_Info *server,
		   __u32 *pexpected_response_sequence_number);
int cifs_sign_smbv(struct kvec *iov, int n_vec, struct TCP_Server_Info *server,
		   __u32 *pexpected_response_sequence);
int cifs_sign_smb(struct smb_hdr *cifs_pdu, struct TCP_Server_Info *server,
		  __u32 *pexpected_response_sequence_number);
int cifs_verify_signature(struct smb_rqst *rqst,
			  struct TCP_Server_Info *server,
			  __u32 expected_sequence_number);
int setup_ntlmv2_rsp(struct cifs_ses *ses, const struct nls_table *nls_cp);
int calc_seckey(struct cifs_ses *ses);
void cifs_crypto_secmech_release(struct TCP_Server_Info *server);

/*
 * cifsfs.c
 */
void cifs_sb_active(struct super_block *sb);
void cifs_sb_deactive(struct super_block *sb);
struct dentry *cifs_smb3_do_mount(struct file_system_type *fs_type,
	      int flags, struct smb3_fs_context *old_ctx);
const char *cifs_get_link(struct dentry *dentry, struct inode *inode,
			    struct delayed_call *done);
ssize_t cifs_file_copychunk_range(unsigned int xid,
				struct file *src_file, loff_t off,
				struct file *dst_file, loff_t destoff,
				size_t len, unsigned int flags);

/*
 * cifsroot.c
 */
int __init cifs_root_data(char **dev, char **opts);

/*
 * cifs_spnego.c
 */
struct key *cifs_get_spnego_key(struct cifs_ses *sesInfo,
		    struct TCP_Server_Info *server);
int init_cifs_spnego(void);
void exit_cifs_spnego(void);

/*
 * cifs_unicode.c
 */
int cifs_remap(struct cifs_sb_info *cifs_sb);
int cifs_from_utf16(char *to, const __le16 *from, int tolen, int fromlen,
		const struct nls_table *codepage, int map_type);
int cifs_strtoUTF16(__le16 *to, const char *from, int len,
	      const struct nls_table *codepage);
int cifs_utf16_bytes(const __le16 *from, int maxbytes,
		const struct nls_table *codepage);
char *cifs_strndup_from_utf16(const char *src, const int maxlen,
			const bool is_unicode, const struct nls_table *codepage);
int cifsConvertToUTF16(__le16 *target, const char *source, int srclen,
		 const struct nls_table *cp, int map_chars);
__le16 *cifs_strndup_to_utf16(const char *src, const int maxlen, int *utf16_len,
		      const struct nls_table *cp, int remap);

/*
 * connect.c
 */
void smb2_query_server_interfaces(struct work_struct *work);
void cifs_signal_cifsd_for_reconnect(struct TCP_Server_Info *server,
				bool all_channels);
void cifs_mark_tcp_ses_conns_for_reconnect(struct TCP_Server_Info *server,
				      bool mark_smb_session);
int cifs_reconnect(struct TCP_Server_Info *server, bool mark_smb_session);
int cifs_read_from_socket(struct TCP_Server_Info *server, char *buf,
		      unsigned int to_read);
ssize_t cifs_discard_from_socket(struct TCP_Server_Info *server, size_t to_read);
int cifs_read_iter_from_socket(struct TCP_Server_Info *server, struct iov_iter *iter,
			   unsigned int to_read);
void dequeue_mid(struct mid_q_entry *mid, bool malformed);
int cifs_enable_signing(struct TCP_Server_Info *server, bool mnt_sign_required);
int cifs_handle_standard(struct TCP_Server_Info *server, struct mid_q_entry *mid);
int cifs_ipaddr_cmp(struct sockaddr *srcaddr, struct sockaddr *rhs);
bool cifs_match_ipaddr(struct sockaddr *srcaddr, struct sockaddr *rhs);
struct TCP_Server_Info *cifs_find_tcp_session(struct smb3_fs_context *ctx);
void cifs_put_tcp_session(struct TCP_Server_Info *server, int from_reconnect);
struct TCP_Server_Info *cifs_get_tcp_session(struct smb3_fs_context *ctx,
		     struct TCP_Server_Info *primary_server);
void __cifs_put_smb_ses(struct cifs_ses *ses);
struct cifs_ses *cifs_get_smb_ses(struct TCP_Server_Info *server, struct smb3_fs_context *ctx);
void cifs_put_tcon(struct cifs_tcon *tcon, enum smb3_tcon_ref_trace trace);
void cifs_put_tlink(struct tcon_link *tlink);
int cifs_match_super(struct super_block *sb, void *data);
void reset_cifs_unix_caps(unsigned int xid, struct cifs_tcon *tcon,
			  struct cifs_sb_info *cifs_sb, struct smb3_fs_context *ctx);
int cifs_setup_cifs_sb(struct cifs_sb_info *cifs_sb);
void cifs_mount_put_conns(struct cifs_mount_ctx *mnt_ctx);
int cifs_mount_get_session(struct cifs_mount_ctx *mnt_ctx);
int cifs_mount_get_tcon(struct cifs_mount_ctx *mnt_ctx);
int cifs_is_path_remote(struct cifs_mount_ctx *mnt_ctx);
int cifs_mount(struct cifs_sb_info *cifs_sb, struct smb3_fs_context *ctx);
int cifs_mount(struct cifs_sb_info *cifs_sb, struct smb3_fs_context *ctx);
int CIFSTCon(const unsigned int xid, struct cifs_ses *ses,
	 const char *tree, struct cifs_tcon *tcon,
	 const struct nls_table *nls_codepage);
void cifs_umount(struct cifs_sb_info *cifs_sb);
int cifs_negotiate_protocol(const unsigned int xid, struct cifs_ses *ses,
			struct TCP_Server_Info *server);
int cifs_setup_session(const unsigned int xid, struct cifs_ses *ses,
		   struct TCP_Server_Info *server,
		   struct nls_table *nls_info);
struct cifs_tcon *cifs_sb_master_tcon(struct cifs_sb_info *cifs_sb);
struct tcon_link *cifs_sb_tlink(struct cifs_sb_info *cifs_sb);
int cifs_tree_connect(const unsigned int xid, struct cifs_tcon *tcon);

/*
 * dfs.c
 */
int dfs_parse_target_referral(const char *full_path, const struct dfs_info3_param *ref,
			      struct smb3_fs_context *ctx);
int dfs_mount_share(struct cifs_mount_ctx *mnt_ctx);
int cifs_tree_connect(const unsigned int xid, struct cifs_tcon *tcon);

/*
 * dfs_cache.c
 */
char *dfs_cache_canonical_path(const char *path, const struct nls_table *cp, int remap);
int dfs_cache_init(void);
void dfs_cache_destroy(void);
int dfs_cache_find(const unsigned int xid, struct cifs_ses *ses, const struct nls_table *cp,
		   int remap, const char *path, struct dfs_info3_param *ref,
		   struct dfs_cache_tgt_list *tgt_list);
int dfs_cache_noreq_find(const char *path, struct dfs_info3_param *ref,
			 struct dfs_cache_tgt_list *tgt_list);
void dfs_cache_noreq_update_tgthint(const char *path, const struct dfs_cache_tgt_iterator *it);
int dfs_cache_get_tgt_referral(const char *path, const struct dfs_cache_tgt_iterator *it,
			       struct dfs_info3_param *ref);
int dfs_cache_get_tgt_share(char *path, const struct dfs_cache_tgt_iterator *it, char **share,
			    char **prefix);
int dfs_cache_remount_fs(struct cifs_sb_info *cifs_sb);
void dfs_cache_refresh(struct work_struct *work);

/*
 * dir.c
 */
char *cifs_build_path_to_root(struct smb3_fs_context *ctx, struct cifs_sb_info *cifs_sb,
			struct cifs_tcon *tcon, int add_treename);
const char *build_path_from_dentry(struct dentry *direntry, void *page);
char *__build_path_from_dentry_optional_prefix(struct dentry *direntry, void *page,
					       const char *tree, int tree_len,
					       bool prefix);
char *build_path_from_dentry_optional_prefix(struct dentry *direntry, void *page,
					     bool prefix);
int cifs_atomic_open(struct inode *inode, struct dentry *direntry,
		 struct file *file, unsigned oflags, umode_t mode);
int cifs_create(struct mnt_idmap *idmap, struct inode *inode,
		struct dentry *direntry, umode_t mode, bool excl);
int cifs_mknod(struct mnt_idmap *idmap, struct inode *inode,
	       struct dentry *direntry, umode_t mode, dev_t device_number);
struct dentry *cifs_lookup(struct inode *parent_dir_inode, struct dentry *direntry,
	    unsigned int flags);

/*
 * dns_resolve.c
 */
int dns_resolve_name(const char *dom, const char *name,
		     size_t namelen, struct sockaddr *ip_addr);

/*
 * file.c
 */
void cifs_mark_open_files_invalid(struct cifs_tcon *tcon);
int cifs_posix_open(const char *full_path, struct inode **pinode,
			struct super_block *sb, int mode, unsigned int f_flags,
			__u32 *poplock, __u16 *pnetfid, unsigned int xid);
void cifs_down_write(struct rw_semaphore *sem);
void serverclose_work(struct work_struct *work);
struct cifsFileInfo *cifs_new_fileinfo(struct cifs_fid *fid, struct file *file,
				       struct tcon_link *tlink, __u32 oplock,
				       const char *symlink_target);
struct cifsFileInfo *cifsFileInfo_get(struct cifsFileInfo *cifs_file);
void serverclose_work(struct work_struct *work);
void cifsFileInfo_put(struct cifsFileInfo *cifs_file);
void _cifsFileInfo_put(struct cifsFileInfo *cifs_file,
		       bool wait_oplock_handler, bool offload);
int cifs_open(struct inode *inode, struct file *file);
void smb2_deferred_work_close(struct work_struct *work);
int cifs_close(struct inode *inode, struct file *file);
void cifs_reopen_persistent_handles(struct cifs_tcon *tcon);
int cifs_closedir(struct inode *inode, struct file *file);
void cifs_del_lock_waiters(struct cifsLockInfo *lock);
bool cifs_find_lock_conflict(struct cifsFileInfo *cfile, __u64 offset, __u64 length,
			__u8 type, __u16 flags,
			struct cifsLockInfo **conf_lock, int rw_check);
int cifs_push_mandatory_locks(struct cifsFileInfo *cfile);
void cifs_move_llist(struct list_head *source, struct list_head *dest);
int cifs_get_hardlink_path(struct cifs_tcon *tcon, struct inode *inode,
				struct file *file);
void cifs_free_llist(struct list_head *llist);
int cifs_unlock_range(struct cifsFileInfo *cfile, struct file_lock *flock,
		  unsigned int xid);
int cifs_flock(struct file *file, int cmd, struct file_lock *fl);
int cifs_lock(struct file *file, int cmd, struct file_lock *flock);
void cifs_write_subrequest_terminated(struct cifs_io_subrequest *wdata, ssize_t result);
struct cifsFileInfo *find_readable_file(struct cifsInodeInfo *cifs_inode,
					bool fsuid_only);
int cifs_get_writable_file(struct cifsInodeInfo *cifs_inode, int flags,
		       struct cifsFileInfo **ret_file);
struct cifsFileInfo *find_writable_file(struct cifsInodeInfo *cifs_inode, int flags);
int cifs_get_writable_path(struct cifs_tcon *tcon, const char *name,
		       int flags,
		       struct cifsFileInfo **ret_file);
int cifs_get_readable_path(struct cifs_tcon *tcon, const char *name,
		       struct cifsFileInfo **ret_file);
int cifs_strict_fsync(struct file *file, loff_t start, loff_t end,
		      int datasync);
int cifs_fsync(struct file *file, loff_t start, loff_t end, int datasync);
int cifs_flush(struct file *file, fl_owner_t id);
ssize_t cifs_strict_writev(struct kiocb *iocb, struct iov_iter *from);
ssize_t cifs_loose_read_iter(struct kiocb *iocb, struct iov_iter *iter);
ssize_t cifs_file_write_iter(struct kiocb *iocb, struct iov_iter *from);
ssize_t cifs_strict_readv(struct kiocb *iocb, struct iov_iter *to);
int cifs_file_strict_mmap_prepare(struct vm_area_desc *desc);
int cifs_file_mmap_prepare(struct vm_area_desc *desc);
bool is_size_safe_to_change(struct cifsInodeInfo *cifsInode, __u64 end_of_file,
			    bool from_readdir);
void cifs_oplock_break(struct work_struct *work);

/*
 * fs_context.c
 */
int smb3_fs_context_dup(struct smb3_fs_context *new_ctx, struct smb3_fs_context *ctx);
int smb3_parse_opt(const char *options, const char *key, char **val);
char *cifs_sanitize_prepath(char *prepath, gfp_t gfp);
char *smb3_fs_context_fullpath(const struct smb3_fs_context *ctx, char dirsep);
int smb3_parse_devname(const char *devname, struct smb3_fs_context *ctx);
int smb3_sync_session_ctx_passwords(struct cifs_sb_info *cifs_sb, struct cifs_ses *ses);
int smb3_init_fs_context(struct fs_context *fc);
void smb3_cleanup_fs_context_contents(struct smb3_fs_context *ctx);
void smb3_cleanup_fs_context(struct smb3_fs_context *ctx);
void smb3_update_mnt_flags(struct cifs_sb_info *cifs_sb);

/*
 * inode.c
 */
int cifs_fattr_to_inode(struct inode *inode, struct cifs_fattr *fattr,
		    bool from_readdir);
void cifs_fill_uniqueid(struct super_block *sb, struct cifs_fattr *fattr);
void cifs_unix_basic_to_fattr(struct cifs_fattr *fattr, FILE_UNIX_BASIC_INFO *info,
			 struct cifs_sb_info *cifs_sb);
int cifs_get_inode_info_unix(struct inode **pinode,
			     const unsigned char *full_path,
			     struct super_block *sb, unsigned int xid);
int cifs_get_inode_info_unix(struct inode **pinode,
			     const unsigned char *full_path,
			     struct super_block *sb, unsigned int xid);
umode_t wire_mode_to_posix(u32 wire, bool is_dir);
int cifs_get_inode_info(struct inode **inode,
			const char *full_path,
			struct cifs_open_info_data *data,
			struct super_block *sb, int xid,
			const struct cifs_fid *fid);
int smb311_posix_get_inode_info(struct inode **inode,
				const char *full_path,
				struct cifs_open_info_data *data,
				struct super_block *sb,
				const unsigned int xid);
struct inode *cifs_iget(struct super_block *sb, struct cifs_fattr *fattr);
struct inode *cifs_root_iget(struct super_block *sb);
int cifs_set_file_info(struct inode *inode, struct iattr *attrs, unsigned int xid,
		   const char *full_path, __u32 dosattr);
int cifs_rename_pending_delete(const char *full_path, struct dentry *dentry,
			   const unsigned int xid);
int cifs_unlink(struct inode *dir, struct dentry *dentry);
struct dentry *cifs_mkdir(struct mnt_idmap *idmap, struct inode *inode,
			  struct dentry *direntry, umode_t mode);
int cifs_rmdir(struct inode *inode, struct dentry *direntry);
int cifs_rename2(struct mnt_idmap *idmap, struct inode *source_dir,
	     struct dentry *source_dentry, struct inode *target_dir,
	     struct dentry *target_dentry, unsigned int flags);
int cifs_revalidate_mapping(struct inode *inode);
int cifs_zap_mapping(struct inode *inode);
int cifs_revalidate_file_attr(struct file *filp);
int cifs_revalidate_dentry_attr(struct dentry *dentry);
int cifs_revalidate_file(struct file *filp);
int cifs_revalidate_dentry(struct dentry *dentry);
int cifs_getattr(struct mnt_idmap *idmap, const struct path *path,
		 struct kstat *stat, u32 request_mask, unsigned int flags);
int cifs_fiemap(struct inode *inode, struct fiemap_extent_info *fei, u64 start,
		u64 len);
void cifs_setsize(struct inode *inode, loff_t offset);
int cifs_setattr(struct mnt_idmap *idmap, struct dentry *direntry,
	     struct iattr *attrs);

/*
 * ioctl.c
 */
long cifs_ioctl(struct file *filep, unsigned int command, unsigned long arg);

/*
 * link.c
 */
bool couldbe_mf_symlink(const struct cifs_fattr *fattr);
int check_mf_symlink(unsigned int xid, struct cifs_tcon *tcon,
		 struct cifs_sb_info *cifs_sb, struct cifs_fattr *fattr,
		 const unsigned char *path);
int cifs_query_mf_symlink(unsigned int xid, struct cifs_tcon *tcon,
		      struct cifs_sb_info *cifs_sb, const unsigned char *path,
		      char *pbuf, unsigned int *pbytes_read);
int cifs_create_mf_symlink(unsigned int xid, struct cifs_tcon *tcon,
		       struct cifs_sb_info *cifs_sb, const unsigned char *path,
		       char *pbuf, unsigned int *pbytes_written);
int smb3_query_mf_symlink(unsigned int xid, struct cifs_tcon *tcon,
		      struct cifs_sb_info *cifs_sb, const unsigned char *path,
		      char *pbuf, unsigned int *pbytes_read);
int smb3_create_mf_symlink(unsigned int xid, struct cifs_tcon *tcon,
		       struct cifs_sb_info *cifs_sb, const unsigned char *path,
		       char *pbuf, unsigned int *pbytes_written);
int cifs_hardlink(struct dentry *old_file, struct inode *inode,
	      struct dentry *direntry);
int cifs_symlink(struct mnt_idmap *idmap, struct inode *inode,
	     struct dentry *direntry, const char *symname);

/*
 * misc.c
 */
unsigned int _get_xid(void);
void _free_xid(unsigned int xid);
struct cifs_ses *sesInfoAlloc(void);
void sesInfoFree(struct cifs_ses *buf_to_free);
struct cifs_tcon *tcon_info_alloc(bool dir_leases_enabled, enum smb3_tcon_ref_trace trace);
void tconInfoFree(struct cifs_tcon *tcon, enum smb3_tcon_ref_trace trace);
struct smb_hdr *cifs_buf_get(void);
void cifs_buf_release(void *buf_to_free);
struct smb_hdr *cifs_small_buf_get(void);
void cifs_small_buf_release(void *buf_to_free);
void free_rsp_buf(int resp_buftype, void *rsp);
void header_assemble(struct smb_hdr *buffer, char smb_command /* command */ ,
		const struct cifs_tcon *treeCon, int word_count
		/* length of fixed section word count in two byte units  */);
int checkSMB(char *buf, unsigned int total_read, struct TCP_Server_Info *server);
bool is_valid_oplock_break(char *buffer, struct TCP_Server_Info *srv);
void dump_smb(void *buf, int smb_buf_length);
void cifs_autodisable_serverino(struct cifs_sb_info *cifs_sb);
void cifs_set_oplock_level(struct cifsInodeInfo *cinode, __u32 oplock);
int cifs_get_writer(struct cifsInodeInfo *cinode);
void cifs_put_writer(struct cifsInodeInfo *cinode);
void cifs_queue_oplock_break(struct cifsFileInfo *cfile);
void cifs_done_oplock_break(struct cifsInodeInfo *cinode);
bool backup_cred(struct cifs_sb_info *cifs_sb);
void cifs_del_pending_open(struct cifs_pending_open *open);
void cifs_add_pending_open_locked(struct cifs_fid *fid, struct tcon_link *tlink,
			     struct cifs_pending_open *open);
void cifs_add_pending_open(struct cifs_fid *fid, struct tcon_link *tlink,
		      struct cifs_pending_open *open);
bool cifs_is_deferred_close(struct cifsFileInfo *cfile, struct cifs_deferred_close **pdclose);
void cifs_add_deferred_close(struct cifsFileInfo *cfile, struct cifs_deferred_close *dclose);
void cifs_del_deferred_close(struct cifsFileInfo *cfile);
void cifs_close_deferred_file(struct cifsInodeInfo *cifs_inode);
void cifs_close_all_deferred_files(struct cifs_tcon *tcon);
void cifs_close_deferred_file_under_dentry(struct cifs_tcon *tcon, const char *path);
void cifs_mark_open_handles_for_deleted_file(struct inode *inode,
					     const char *path);
int parse_dfs_referrals(struct get_dfs_referral_rsp *rsp, u32 rsp_size,
		    unsigned int *num_of_nodes,
		    struct dfs_info3_param **target_nodes,
		    const struct nls_table *nls_codepage, int remap,
		    const char *searchName, bool is_unicode);
int cifs_alloc_hash(const char *name, struct shash_desc **sdesc);
void cifs_free_hash(struct shash_desc **sdesc);
void extract_unc_hostname(const char *unc, const char **h, size_t *len);
int copy_path_name(char *dst, const char *src);
struct super_block *cifs_get_dfs_tcon_super(struct cifs_tcon *tcon);
void cifs_put_tcp_super(struct super_block *sb);
int match_target_ip(struct TCP_Server_Info *server,
		    const char *host, size_t hostlen,
		    bool *result);
int cifs_update_super_prepath(struct cifs_sb_info *cifs_sb, char *prefix);
int cifs_inval_name_dfs_link_error(const unsigned int xid,
				   struct cifs_tcon *tcon,
				   struct cifs_sb_info *cifs_sb,
				   const char *full_path,
				   bool *islink);
int cifs_wait_for_server_reconnect(struct TCP_Server_Info *server, bool retry);

/*
 * namespace.c
 */
void cifs_release_automount_timer(void);
char *cifs_build_devname(char *nodename, const char *prepath);
struct vfsmount *cifs_d_automount(struct path *path);

/*
 * netlink.c
 */
int cifs_genl_init(void);
void cifs_genl_exit(void);

/*
 * netmisc.c
 */
int cifs_convert_address(struct sockaddr *dst, const char *src, int len);
void cifs_set_port(struct sockaddr *addr, const unsigned short int port);
int map_smb_to_linux_error(char *buf, bool logErr);
int map_and_check_smb_error(struct mid_q_entry *mid, bool logErr);
unsigned int smbCalcSize(void *buf);
struct timespec64 cifs_NTtimeToUnix(__le64 ntutc);
u64 cifs_UnixTimeToNT(struct timespec64 t);
struct timespec64 cnvrtDosUnixTm(__le16 le_date, __le16 le_time, int offset);

/*
 * readdir.c
 */
void cifs_dir_info_to_fattr(struct cifs_fattr *fattr, FILE_DIRECTORY_INFO *info,
		       struct cifs_sb_info *cifs_sb);
int cifs_readdir(struct file *file, struct dir_context *ctx);

/*
 * reparse.c
 */
int create_reparse_symlink(const unsigned int xid, struct inode *inode,
				struct dentry *dentry, struct cifs_tcon *tcon,
				const char *full_path, const char *symname);
int mknod_reparse(unsigned int xid, struct inode *inode,
		       struct dentry *dentry, struct cifs_tcon *tcon,
		       const char *full_path, umode_t mode, dev_t dev);
int smb2_parse_native_symlink(char **target, const char *buf, unsigned int len,
			      bool relative,
			      const char *full_path,
			      struct cifs_sb_info *cifs_sb);
int parse_reparse_point(struct reparse_data_buffer *buf,
			u32 plen, struct cifs_sb_info *cifs_sb,
			const char *full_path,
			struct cifs_open_info_data *data);
struct reparse_data_buffer *smb2_get_reparse_point_buffer(const struct kvec *rsp_iov,
							  u32 *plen);
bool cifs_reparse_point_to_fattr(struct cifs_sb_info *cifs_sb,
				 struct cifs_fattr *fattr,
				 struct cifs_open_info_data *data);

/*
 * sess.c
 */
bool is_ses_using_iface(struct cifs_ses *ses, struct cifs_server_iface *iface);
int cifs_ses_get_chan_index(struct cifs_ses *ses,
			struct TCP_Server_Info *server);
void cifs_chan_set_in_reconnect(struct cifs_ses *ses,
			     struct TCP_Server_Info *server);
void cifs_chan_clear_in_reconnect(struct cifs_ses *ses,
			     struct TCP_Server_Info *server);
void cifs_chan_set_need_reconnect(struct cifs_ses *ses,
			     struct TCP_Server_Info *server);
void cifs_chan_clear_need_reconnect(struct cifs_ses *ses,
			       struct TCP_Server_Info *server);
bool cifs_chan_needs_reconnect(struct cifs_ses *ses,
			  struct TCP_Server_Info *server);
bool cifs_chan_is_iface_active(struct cifs_ses *ses,
			  struct TCP_Server_Info *server);
int cifs_try_adding_channels(struct cifs_ses *ses);
void cifs_disable_secondary_channels(struct cifs_ses *ses);
void cifs_chan_update_iface(struct cifs_ses *ses, struct TCP_Server_Info *server);
int decode_ntlmssp_challenge(char *bcc_ptr, int blob_len,
				    struct cifs_ses *ses);
int build_ntlmssp_negotiate_blob(unsigned char **pbuffer,
				 u16 *buflen,
				 struct cifs_ses *ses,
				 struct TCP_Server_Info *server,
				 const struct nls_table *nls_cp);
int build_ntlmssp_smb3_negotiate_blob(unsigned char **pbuffer,
				 u16 *buflen,
				 struct cifs_ses *ses,
				 struct TCP_Server_Info *server,
				 const struct nls_table *nls_cp);
int build_ntlmssp_auth_blob(unsigned char **pbuffer,
					u16 *buflen,
				   struct cifs_ses *ses,
				   struct TCP_Server_Info *server,
				   const struct nls_table *nls_cp);
enum securityEnum cifs_select_sectype(struct TCP_Server_Info *server, enum securityEnum requested);
int CIFS_SessSetup(const unsigned int xid, struct cifs_ses *ses,
		   struct TCP_Server_Info *server,
		   const struct nls_table *nls_cp);

/*
 * smbencrypt.c
 */
int E_md4hash(const unsigned char *passwd, unsigned char *p16,
	const struct nls_table *codepage);

/*
 * transport.c
 */
void cifs_wake_up_task(struct mid_q_entry *mid);
void __release_mid(struct kref *refcount);
void delete_mid(struct mid_q_entry *mid);
int smb_send_kvec(struct TCP_Server_Info *server, struct msghdr *smb_msg,
	      size_t *sent);
unsigned long smb_rqst_len(struct TCP_Server_Info *server, struct smb_rqst *rqst);
int __smb_send_rqst(struct TCP_Server_Info *server, int num_rqst,
		    struct smb_rqst *rqst);
int wait_for_free_request(struct TCP_Server_Info *server, const int flags,
			  unsigned int *instance);
int cifs_wait_mtu_credits(struct TCP_Server_Info *server, size_t size,
		      size_t *num, struct cifs_credits *credits);
int wait_for_response(struct TCP_Server_Info *server, struct mid_q_entry *midQ);
int cifs_call_async(struct TCP_Server_Info *server, struct smb_rqst *rqst,
		mid_receive_t *receive, mid_callback_t *callback,
		mid_handle_t *handle, void *cbdata, const int flags,
		const struct cifs_credits *exist_credits);
int cifs_sync_mid_result(struct mid_q_entry *mid, struct TCP_Server_Info *server);
struct TCP_Server_Info *cifs_pick_channel(struct cifs_ses *ses);
int compound_send_recv(const unsigned int xid, struct cifs_ses *ses,
		   struct TCP_Server_Info *server,
		   const int flags, const int num_rqst, struct smb_rqst *rqst,
		   int *resp_buf_type, struct kvec *resp_iov);
int cifs_send_recv(const unsigned int xid, struct cifs_ses *ses,
	       struct TCP_Server_Info *server,
	       struct smb_rqst *rqst, int *resp_buf_type, const int flags,
	       struct kvec *resp_iov);
int cifs_discard_remaining_data(struct TCP_Server_Info *server);
int cifs_readv_receive(struct TCP_Server_Info *server, struct mid_q_entry *mid);

/*
 * unc.c
 */
char *extract_hostname(const char *unc);
char *extract_sharename(const char *unc);

/*
 * winucase.c
 */
wchar_t cifs_toupper(wchar_t in);
wchar_t cifs_toupper(wchar_t in);

/*
 * xattr.c
 */
ssize_t cifs_listxattr(struct dentry *direntry, char *data, size_t buf_size);

#define get_xid()							\
({									\
	unsigned int __xid = _get_xid();				\
	cifs_dbg(FYI, "VFS: in %s as Xid: %u with uid: %d\n",		\
		 __func__, __xid,					\
		 from_kuid(&init_user_ns, current_fsuid()));		\
	trace_smb3_enter(__xid, __func__);				\
	__xid;								\
})

#define free_xid(curr_xid)						\
do {									\
	_free_xid(curr_xid);						\
	cifs_dbg(FYI, "VFS: leaving %s (xid = %u) rc = %d\n",		\
		 __func__, curr_xid, (int)rc);				\
	if (rc)								\
		trace_smb3_exit_err(curr_xid, __func__, (int)rc);	\
	else								\
		trace_smb3_exit_done(curr_xid, __func__);		\
} while (0)
static inline void *alloc_dentry_path(void)
{
	return __getname();
}

static inline void free_dentry_path(void *page)
{
	if (page)
		__putname(page);
}

static inline int
send_cancel(struct TCP_Server_Info *server, struct smb_rqst *rqst,
	    struct mid_q_entry *mid)
{
	return server->ops->send_cancel ?
				server->ops->send_cancel(server, rqst, mid) : 0;
}

struct cifs_unix_set_info_args {
	__u64	ctime;
	__u64	atime;
	__u64	mtime;
	__u64	mode;
	kuid_t	uid;
	kgid_t	gid;
	dev_t	device;
};

#ifdef CONFIG_CIFS_ALLOW_INSECURE_LEGACY
#endif /* CONFIG_CIFS_ALLOW_INSECURE_LEGACY */

#ifdef CONFIG_CIFS_DFS_UPCALL
static inline int get_dfs_path(const unsigned int xid, struct cifs_ses *ses,
			       const char *old_path,
			       const struct nls_table *nls_codepage,
			       struct dfs_info3_param *referral, int remap)
{
	return dfs_cache_find(xid, ses, nls_codepage, remap, old_path,
			      referral, NULL);
}

#else
static inline int cifs_inval_name_dfs_link_error(const unsigned int xid,
				   struct cifs_tcon *tcon,
				   struct cifs_sb_info *cifs_sb,
				   const char *full_path,
				   bool *islink)
{
	*islink = false;
	return 0;
}
#endif

static inline int cifs_create_options(struct cifs_sb_info *cifs_sb, int options)
{
	if (cifs_sb && (backup_cred(cifs_sb)))
		return options | CREATE_OPEN_BACKUP_INTENT;
	else
		return options;
}

static inline void cifs_put_smb_ses(struct cifs_ses *ses)
{
	__cifs_put_smb_ses(ses);
}

/* Get an active reference of @ses and its children.
 *
 * NOTE: make sure to call this function when incrementing reference count of
 * @ses to ensure that any DFS root session attached to it (@ses->dfs_root_ses)
 * will also get its reference count incremented.
 *
 * cifs_put_smb_ses() will put all references, so call it when you're done.
 */
static inline void cifs_smb_ses_inc_refcount(struct cifs_ses *ses)
{
	lockdep_assert_held(&cifs_tcp_ses_lock);
	ses->ses_count++;
}

static inline bool dfs_src_pathname_equal(const char *s1, const char *s2)
{
	if (strlen(s1) != strlen(s2))
		return false;
	for (; *s1; s1++, s2++) {
		if (*s1 == '/' || *s1 == '\\') {
			if (*s2 != '/' && *s2 != '\\')
				return false;
		} else if (tolower(*s1) != tolower(*s2))
			return false;
	}
	return true;
}

static inline void release_mid(struct mid_q_entry *mid)
{
	kref_put(&mid->refcount, __release_mid);
}

static inline void cifs_free_open_info(struct cifs_open_info_data *data)
{
	kfree(data->symlink_target);
	free_rsp_buf(data->reparse.io.buftype, data->reparse.io.iov.iov_base);
	memset(data, 0, sizeof(*data));
}

#endif			/* _CIFSPROTO_H */
