// SPDX-License-Identifier: GPL-2.0
#include <linux/ceph/ceph_debug.h>

#include <linux/device.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/ctype.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/math64.h>
#include <linux/ktime.h>
#include <linux/uaccess.h>
#include <linux/kstrtox.h>

#include <linux/ceph/libceph.h>
#include <linux/ceph/mon_client.h>
#include <linux/ceph/auth.h>
#include <linux/ceph/debugfs.h>

#include "super.h"

#ifdef CONFIG_DEBUG_FS

#include "mds_client.h"
#include "metric.h"

#define CEPH_DEBUGFS_MODE_READONLY 0400
#define CEPH_DEBUGFS_MODE_WRITEONLY 0200
#define CEPH_DEBUGFS_MODE_READWRITE 0600

static int mdsmap_show(struct seq_file *s, void *p)
{
	int i;
	struct ceph_fs_client *fsc = s->private;
	struct ceph_mdsmap *mdsmap;

	if (!fsc->mdsc || !fsc->mdsc->mdsmap)
		return 0;
	mdsmap = fsc->mdsc->mdsmap;
	seq_printf(s, "epoch %d\n", mdsmap->m_epoch);
	seq_printf(s, "root %d\n", mdsmap->m_root);
	seq_printf(s, "max_mds %d\n", mdsmap->m_max_mds);
	seq_printf(s, "session_timeout %d\n", mdsmap->m_session_timeout);
	seq_printf(s, "session_autoclose %d\n", mdsmap->m_session_autoclose);
	for (i = 0; i < mdsmap->possible_max_rank; i++) {
		struct ceph_entity_addr *addr = &mdsmap->m_info[i].addr;
		int state = mdsmap->m_info[i].state;
		seq_printf(s, "\tmds%d\t%s\t(%s)\n", i,
			       ceph_pr_addr(addr),
			       ceph_mds_state_name(state));
	}
	return 0;
}

/*
 * mdsc debugfs
 */
static int mdsc_show(struct seq_file *s, void *p)
{
	struct ceph_fs_client *fsc = s->private;
	struct ceph_mds_client *mdsc = fsc->mdsc;
	struct ceph_mds_request *req;
	struct rb_node *rp;
	char *path;

	mutex_lock(&mdsc->mutex);
	for (rp = rb_first(&mdsc->request_tree); rp; rp = rb_next(rp)) {
		req = rb_entry(rp, struct ceph_mds_request, r_node);

		if (req->r_request && req->r_session)
			seq_printf(s, "%lld\tmds%d\t", req->r_tid,
				   req->r_session->s_mds);
		else if (!req->r_request)
			seq_printf(s, "%lld\t(no request)\t", req->r_tid);
		else
			seq_printf(s, "%lld\t(no session)\t", req->r_tid);

		seq_printf(s, "%s", ceph_mds_op_name(req->r_op));

		if (test_bit(CEPH_MDS_R_GOT_UNSAFE, &req->r_req_flags))
			seq_puts(s, "\t(unsafe)");
		else
			seq_puts(s, "\t");

		if (req->r_inode) {
			seq_printf(s, " #%llx", ceph_ino(req->r_inode));
		} else if (req->r_dentry) {
			struct ceph_path_info path_info;
			path = ceph_mdsc_build_path(mdsc, req->r_dentry, &path_info, 0);
			if (IS_ERR(path))
				path = NULL;
			spin_lock(&req->r_dentry->d_lock);
			seq_printf(s, " #%llx/%pd (%s)",
				   ceph_ino(d_inode(req->r_dentry->d_parent)),
				   req->r_dentry,
				   path ? path : "");
			spin_unlock(&req->r_dentry->d_lock);
			ceph_mdsc_free_path_info(&path_info);
		} else if (req->r_path1) {
			seq_printf(s, " #%llx/%s", req->r_ino1.ino,
				   req->r_path1);
		} else {
			seq_printf(s, " #%llx", req->r_ino1.ino);
		}

		if (req->r_old_dentry) {
			struct ceph_path_info path_info;
			path = ceph_mdsc_build_path(mdsc, req->r_old_dentry, &path_info, 0);
			if (IS_ERR(path))
				path = NULL;
			spin_lock(&req->r_old_dentry->d_lock);
			seq_printf(s, " #%llx/%pd (%s)",
				   req->r_old_dentry_dir ?
				   ceph_ino(req->r_old_dentry_dir) : 0,
				   req->r_old_dentry,
				   path ? path : "");
			spin_unlock(&req->r_old_dentry->d_lock);
			ceph_mdsc_free_path_info(&path_info);
		} else if (req->r_path2 && req->r_op != CEPH_MDS_OP_SYMLINK) {
			if (req->r_ino2.ino)
				seq_printf(s, " #%llx/%s", req->r_ino2.ino,
					   req->r_path2);
			else
				seq_printf(s, " %s", req->r_path2);
		}

		seq_puts(s, "\n");
	}
	mutex_unlock(&mdsc->mutex);

	return 0;
}

#define CEPH_LAT_METRIC_SHOW(name, total, avg, min, max, sq) {		\
	s64 _total, _avg, _min, _max, _sq, _st;				\
	_avg = ktime_to_us(avg);					\
	_min = ktime_to_us(min == KTIME_MAX ? 0 : min);			\
	_max = ktime_to_us(max);					\
	_total = total - 1;						\
	_sq = _total > 0 ? DIV64_U64_ROUND_CLOSEST(sq, _total) : 0;	\
	_st = int_sqrt64(_sq);						\
	_st = ktime_to_us(_st);						\
	seq_printf(s, "%-14s%-12lld%-16lld%-16lld%-16lld%lld\n",	\
		   name, total, _avg, _min, _max, _st);			\
}

#define CEPH_SZ_METRIC_SHOW(name, total, avg, min, max, sum) {		\
	u64 _min = min == U64_MAX ? 0 : min;				\
	seq_printf(s, "%-14s%-12lld%-16llu%-16llu%-16llu%llu\n",	\
		   name, total, avg, _min, max, sum);			\
}

static int metrics_file_show(struct seq_file *s, void *p)
{
	struct ceph_fs_client *fsc = s->private;
	struct ceph_client_metric *m = &fsc->mdsc->metric;

	seq_printf(s, "item                               total\n");
	seq_printf(s, "------------------------------------------\n");
	seq_printf(s, "%-35s%lld\n", "total inodes",
		   percpu_counter_sum(&m->total_inodes));
	seq_printf(s, "%-35s%lld\n", "opened files",
		   atomic64_read(&m->opened_files));
	seq_printf(s, "%-35s%lld\n", "pinned i_caps",
		   atomic64_read(&m->total_caps));
	seq_printf(s, "%-35s%lld\n", "opened inodes",
		   percpu_counter_sum(&m->opened_inodes));
	return 0;
}

static const char * const metric_str[] = {
	"read",
	"write",
	"metadata",
	"copyfrom"
};
static int metrics_latency_show(struct seq_file *s, void *p)
{
	struct ceph_fs_client *fsc = s->private;
	struct ceph_client_metric *cm = &fsc->mdsc->metric;
	struct ceph_metric *m;
	s64 total, avg, min, max, sq;
	int i;

	seq_printf(s, "item          total       avg_lat(us)     min_lat(us)     max_lat(us)     stdev(us)\n");
	seq_printf(s, "-----------------------------------------------------------------------------------\n");

	for (i = 0; i < METRIC_MAX; i++) {
		m = &cm->metric[i];
		spin_lock(&m->lock);
		total = m->total;
		avg = m->latency_avg;
		min = m->latency_min;
		max = m->latency_max;
		sq = m->latency_sq_sum;
		spin_unlock(&m->lock);
		CEPH_LAT_METRIC_SHOW(metric_str[i], total, avg, min, max, sq);
	}

	return 0;
}

static int metrics_size_show(struct seq_file *s, void *p)
{
	struct ceph_fs_client *fsc = s->private;
	struct ceph_client_metric *cm = &fsc->mdsc->metric;
	struct ceph_metric *m;
	s64 total;
	u64 sum, avg, min, max;
	int i;

	seq_printf(s, "item          total       avg_sz(bytes)   min_sz(bytes)   max_sz(bytes)  total_sz(bytes)\n");
	seq_printf(s, "----------------------------------------------------------------------------------------\n");

	for (i = 0; i < METRIC_MAX; i++) {
		/* skip 'metadata' as it doesn't use the size metric */
		if (i == METRIC_METADATA)
			continue;
		m = &cm->metric[i];
		spin_lock(&m->lock);
		total = m->total;
		sum = m->size_sum;
		avg = total > 0 ? DIV64_U64_ROUND_CLOSEST(sum, total) : 0;
		min = m->size_min;
		max = m->size_max;
		spin_unlock(&m->lock);
		CEPH_SZ_METRIC_SHOW(metric_str[i], total, avg, min, max, sum);
	}

	return 0;
}

static int metrics_caps_show(struct seq_file *s, void *p)
{
	struct ceph_fs_client *fsc = s->private;
	struct ceph_client_metric *m = &fsc->mdsc->metric;
	int nr_caps = 0;

	seq_printf(s, "item          total           miss            hit\n");
	seq_printf(s, "-------------------------------------------------\n");

	seq_printf(s, "%-14s%-16lld%-16lld%lld\n", "d_lease",
		   atomic64_read(&m->total_dentries),
		   percpu_counter_sum(&m->d_lease_mis),
		   percpu_counter_sum(&m->d_lease_hit));

	nr_caps = atomic64_read(&m->total_caps);
	seq_printf(s, "%-14s%-16d%-16lld%lld\n", "caps", nr_caps,
		   percpu_counter_sum(&m->i_caps_mis),
		   percpu_counter_sum(&m->i_caps_hit));

	return 0;
}

static int caps_show_cb(struct inode *inode, int mds, void *p)
{
	struct ceph_inode_info *ci = ceph_inode(inode);
	struct seq_file *s = p;
	struct ceph_cap *cap;

	spin_lock(&ci->i_ceph_lock);
	cap = __get_cap_for_mds(ci, mds);
	if (cap)
		seq_printf(s, "0x%-17llx%-3d%-17s%-17s\n", ceph_ino(inode),
			   cap->session->s_mds,
			   ceph_cap_string(cap->issued),
			   ceph_cap_string(cap->implemented));
	spin_unlock(&ci->i_ceph_lock);
	return 0;
}

static int caps_show(struct seq_file *s, void *p)
{
	struct ceph_fs_client *fsc = s->private;
	struct ceph_mds_client *mdsc = fsc->mdsc;
	int total, avail, used, reserved, min, i;
	struct cap_wait	*cw;

	ceph_reservation_status(fsc, &total, &avail, &used, &reserved, &min);
	seq_printf(s, "total\t\t%d\n"
		   "avail\t\t%d\n"
		   "used\t\t%d\n"
		   "reserved\t%d\n"
		   "min\t\t%d\n\n",
		   total, avail, used, reserved, min);
	seq_printf(s, "ino              mds  issued           implemented\n");
	seq_printf(s, "--------------------------------------------------\n");

	mutex_lock(&mdsc->mutex);
	for (i = 0; i < mdsc->max_sessions; i++) {
		struct ceph_mds_session *session;

		session = __ceph_lookup_mds_session(mdsc, i);
		if (!session)
			continue;
		mutex_unlock(&mdsc->mutex);
		mutex_lock(&session->s_mutex);
		ceph_iterate_session_caps(session, caps_show_cb, s);
		mutex_unlock(&session->s_mutex);
		ceph_put_mds_session(session);
		mutex_lock(&mdsc->mutex);
	}
	mutex_unlock(&mdsc->mutex);

	seq_printf(s, "\n\nWaiters:\n--------\n");
	seq_printf(s, "tgid         ino                need             want\n");
	seq_printf(s, "-----------------------------------------------------\n");

	spin_lock(&mdsc->caps_list_lock);
	list_for_each_entry(cw, &mdsc->cap_wait_list, list) {
		seq_printf(s, "%-13d0x%-17llx%-17s%-17s\n", cw->tgid, cw->ino,
				ceph_cap_string(cw->need),
				ceph_cap_string(cw->want));
	}
	spin_unlock(&mdsc->caps_list_lock);

	return 0;
}

static int mds_sessions_show(struct seq_file *s, void *ptr)
{
	struct ceph_fs_client *fsc = s->private;
	struct ceph_mds_client *mdsc = fsc->mdsc;
	struct ceph_auth_client *ac = fsc->client->monc.auth;
	struct ceph_options *opt = fsc->client->options;
	int mds;

	mutex_lock(&mdsc->mutex);

	/* The 'num' portion of an 'entity name' */
	seq_printf(s, "global_id %llu\n", ac->global_id);

	/* The -o name mount argument */
	seq_printf(s, "name \"%s\"\n", opt->name ? opt->name : "");

	/* The list of MDS session rank+state */
	for (mds = 0; mds < mdsc->max_sessions; mds++) {
		struct ceph_mds_session *session =
			__ceph_lookup_mds_session(mdsc, mds);
		if (!session) {
			continue;
		}
		mutex_unlock(&mdsc->mutex);
		seq_printf(s, "mds.%d %s\n",
				session->s_mds,
				ceph_session_state_name(session->s_state));

		ceph_put_mds_session(session);
		mutex_lock(&mdsc->mutex);
	}
	mutex_unlock(&mdsc->mutex);

	return 0;
}

static int status_show(struct seq_file *s, void *p)
{
	struct ceph_fs_client *fsc = s->private;
	struct ceph_entity_inst *inst = &fsc->client->msgr.inst;
	struct ceph_entity_addr *client_addr = ceph_client_addr(fsc->client);

	seq_printf(s, "instance: %s.%lld %s/%u\n", ENTITY_NAME(inst->name),
		   ceph_pr_addr(client_addr), le32_to_cpu(client_addr->nonce));
	seq_printf(s, "blocklisted: %s\n", str_true_false(fsc->blocklisted));

	return 0;
}

static int reset_status_show(struct seq_file *s, void *p)
{
	struct ceph_fs_client *fsc = s->private;
	struct ceph_mds_client *mdsc = fsc->mdsc;
	struct ceph_client_reset_state *st;
	u64 trigger = 0, success = 0, failure = 0;
	unsigned long last_start = 0, last_finish = 0;
	int last_errno = 0;
	bool in_progress = false;
	bool inject_error = false;
	int pending_reconnects = 0;
	int blocked_requests = 0;
	char reason[CEPH_CLIENT_RESET_REASON_LEN];

	if (!mdsc)
		return 0;

	st = &mdsc->reset_state;

	spin_lock(&st->lock);
	trigger = st->trigger_count;
	success = st->success_count;
	failure = st->failure_count;
	last_start = st->last_start;
	last_finish = st->last_finish;
	last_errno = st->last_errno;
	in_progress = st->in_progress;
	inject_error = st->inject_error;
	strscpy(reason, st->last_reason, sizeof(reason));
	spin_unlock(&st->lock);

	pending_reconnects = atomic_read(&st->pending_reconnects);
	blocked_requests = atomic_read(&st->blocked_requests);

	seq_printf(s, "in_progress: %s\n", in_progress ? "yes" : "no");
	seq_printf(s, "trigger_count: %llu\n", trigger);
	seq_printf(s, "success_count: %llu\n", success);
	seq_printf(s, "failure_count: %llu\n", failure);
	if (last_start)
		seq_printf(s, "last_start_ms_ago: %u\n",
			   jiffies_to_msecs(jiffies - last_start));
	else
		seq_puts(s, "last_start_ms_ago: (never)\n");
	if (last_finish)
		seq_printf(s, "last_finish_ms_ago: %u\n",
			   jiffies_to_msecs(jiffies - last_finish));
	else
		seq_puts(s, "last_finish_ms_ago: (never)\n");
	seq_printf(s, "last_errno: %d\n", last_errno);
	seq_printf(s, "last_reason: %s\n",
		   reason[0] ? reason : "(none)");
	seq_printf(s, "inject_error_pending: %s\n",
		   inject_error ? "yes" : "no");
	seq_printf(s, "pending_reconnects: %d\n", pending_reconnects);
	seq_printf(s, "blocked_requests: %d\n", blocked_requests);

	return 0;
}

static ssize_t reset_trigger_write(struct file *file, const char __user *buf,
				   size_t len, loff_t *ppos)
{
	struct ceph_fs_client *fsc = file->private_data;
	struct ceph_mds_client *mdsc = fsc->mdsc;
	char reason[CEPH_CLIENT_RESET_REASON_LEN];
	size_t copy;
	int ret;

	if (!mdsc)
		return -ENODEV;

	copy = min_t(size_t, len, sizeof(reason) - 1);
	if (copy && copy_from_user(reason, buf, copy))
		return -EFAULT;
	reason[copy] = '\0';
	strim(reason);

	ret = ceph_mdsc_schedule_reset(mdsc, reason);
	if (ret)
		return ret;

	return len;
}

static ssize_t reset_inject_error_write(struct file *file,
					const char __user *buf,
					size_t len, loff_t *ppos)
{
	struct ceph_fs_client *fsc = file->private_data;
	struct ceph_mds_client *mdsc = fsc->mdsc;
	struct ceph_client_reset_state *st;
	bool enable;
	int ret;

	if (!mdsc)
		return -ENODEV;

	ret = kstrtobool_from_user(buf, len, &enable);
	if (ret)
		return ret;

	st = &mdsc->reset_state;
	spin_lock(&st->lock);
	st->inject_error = enable;
	spin_unlock(&st->lock);

	return len;
}

DEFINE_SHOW_ATTRIBUTE(mdsmap);
DEFINE_SHOW_ATTRIBUTE(mdsc);
DEFINE_SHOW_ATTRIBUTE(caps);
DEFINE_SHOW_ATTRIBUTE(mds_sessions);
DEFINE_SHOW_ATTRIBUTE(status);
DEFINE_SHOW_ATTRIBUTE(reset_status);
DEFINE_SHOW_ATTRIBUTE(metrics_file);
DEFINE_SHOW_ATTRIBUTE(metrics_latency);
DEFINE_SHOW_ATTRIBUTE(metrics_size);
DEFINE_SHOW_ATTRIBUTE(metrics_caps);

static const struct file_operations ceph_reset_trigger_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = reset_trigger_write,
	.llseek = noop_llseek,
};

static const struct file_operations ceph_reset_inject_error_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = reset_inject_error_write,
	.llseek = noop_llseek,
};


/*
 * debugfs
 */
static int congestion_kb_set(void *data, u64 val)
{
	struct ceph_fs_client *fsc = (struct ceph_fs_client *)data;

	fsc->mount_options->congestion_kb = (int)val;
	return 0;
}

static int congestion_kb_get(void *data, u64 *val)
{
	struct ceph_fs_client *fsc = (struct ceph_fs_client *)data;

	*val = (u64)fsc->mount_options->congestion_kb;
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(congestion_kb_fops, congestion_kb_get,
			congestion_kb_set, "%llu\n");


void ceph_fs_debugfs_cleanup(struct ceph_fs_client *fsc)
{
	doutc(fsc->client, "begin\n");
	debugfs_remove(fsc->debugfs_bdi);
	debugfs_remove(fsc->debugfs_congestion_kb);
	debugfs_remove(fsc->debugfs_mdsmap);
	debugfs_remove(fsc->debugfs_mds_sessions);
	debugfs_remove(fsc->debugfs_caps);
	debugfs_remove(fsc->debugfs_status);
	debugfs_remove(fsc->debugfs_mdsc);
	debugfs_remove(fsc->debugfs_reset_status);
	debugfs_remove(fsc->debugfs_reset_force);
	debugfs_remove(fsc->debugfs_reset_inject);
	debugfs_remove_recursive(fsc->debugfs_reset_dir);
	debugfs_remove_recursive(fsc->debugfs_metrics_dir);
	doutc(fsc->client, "done\n");
}

void ceph_fs_debugfs_init(struct ceph_fs_client *fsc)
{
	char name[NAME_MAX];

	doutc(fsc->client, "begin\n");
	fsc->debugfs_congestion_kb =
		debugfs_create_file("writeback_congestion_kb",
				    CEPH_DEBUGFS_MODE_READWRITE,
				    fsc->client->debugfs_dir,
				    fsc,
				    &congestion_kb_fops);

	snprintf(name, sizeof(name), "../../bdi/%s",
		 bdi_dev_name(fsc->sb->s_bdi));
	fsc->debugfs_bdi =
		debugfs_create_symlink("bdi",
				       fsc->client->debugfs_dir,
				       name);

	fsc->debugfs_mdsmap = debugfs_create_file("mdsmap",
					CEPH_DEBUGFS_MODE_READONLY,
					fsc->client->debugfs_dir,
					fsc,
					&mdsmap_fops);

	fsc->debugfs_mds_sessions = debugfs_create_file("mds_sessions",
					CEPH_DEBUGFS_MODE_READONLY,
					fsc->client->debugfs_dir,
					fsc,
					&mds_sessions_fops);

	fsc->debugfs_mdsc = debugfs_create_file("mdsc",
						CEPH_DEBUGFS_MODE_READONLY,
						fsc->client->debugfs_dir,
						fsc,
						&mdsc_fops);

	fsc->debugfs_caps = debugfs_create_file("caps",
						CEPH_DEBUGFS_MODE_READONLY,
						fsc->client->debugfs_dir,
						fsc,
						&caps_fops);

	fsc->debugfs_reset_dir = debugfs_create_dir("reset",
						    fsc->client->debugfs_dir);
	if (fsc->debugfs_reset_dir) {
		fsc->debugfs_reset_force =
			debugfs_create_file("trigger", CEPH_DEBUGFS_MODE_WRITEONLY,
					    fsc->debugfs_reset_dir, fsc,
					    &ceph_reset_trigger_fops);
		fsc->debugfs_reset_status =
			debugfs_create_file("status", CEPH_DEBUGFS_MODE_READONLY,
					    fsc->debugfs_reset_dir, fsc,
					    &reset_status_fops);
		fsc->debugfs_reset_inject =
			debugfs_create_file("inject_error", CEPH_DEBUGFS_MODE_WRITEONLY,
					    fsc->debugfs_reset_dir, fsc,
					    &ceph_reset_inject_error_fops);
	}

	fsc->debugfs_status = debugfs_create_file("status",
						  CEPH_DEBUGFS_MODE_READONLY,
						  fsc->client->debugfs_dir,
						  fsc,
						  &status_fops);

	fsc->debugfs_metrics_dir = debugfs_create_dir("metrics",
						      fsc->client->debugfs_dir);

	debugfs_create_file("file", CEPH_DEBUGFS_MODE_READONLY, fsc->debugfs_metrics_dir, fsc,
			    &metrics_file_fops);
	debugfs_create_file("latency", CEPH_DEBUGFS_MODE_READONLY, fsc->debugfs_metrics_dir, fsc,
			    &metrics_latency_fops);
	debugfs_create_file("size", CEPH_DEBUGFS_MODE_READONLY, fsc->debugfs_metrics_dir, fsc,
			    &metrics_size_fops);
	debugfs_create_file("caps", CEPH_DEBUGFS_MODE_READONLY, fsc->debugfs_metrics_dir, fsc,
			    &metrics_caps_fops);
	doutc(fsc->client, "done\n");
}


#else  /* CONFIG_DEBUG_FS */

void ceph_fs_debugfs_init(struct ceph_fs_client *fsc)
{
}

void ceph_fs_debugfs_cleanup(struct ceph_fs_client *fsc)
{
}

#endif  /* CONFIG_DEBUG_FS */
