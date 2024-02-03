// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * NET3:	Garbage Collector For AF_UNIX sockets
 *
 * Garbage Collector:
 *	Copyright (C) Barak A. Pearlmutter.
 *
 * Chopped about by Alan Cox 22/3/96 to make it fit the AF_UNIX socket problem.
 * If it doesn't work blame me, it worked when Barak sent it.
 *
 * Assumptions:
 *
 *  - object w/ a bit
 *  - free list
 *
 * Current optimizations:
 *
 *  - explicit stack instead of recursion
 *  - tail recurse on first born instead of immediate push/pop
 *  - we gather the stuff that should not be killed into tree
 *    and stack is just a path from root to the current pointer.
 *
 *  Future optimizations:
 *
 *  - don't just push entire root set; process in place
 *
 *  Fixes:
 *	Alan Cox	07 Sept	1997	Vmalloc internal stack as needed.
 *					Cope with changing max_files.
 *	Al Viro		11 Oct 1998
 *		Graph may have cycles. That is, we can send the descriptor
 *		of foo to bar and vice versa. Current code chokes on that.
 *		Fix: move SCM_RIGHTS ones into the separate list and then
 *		skb_free() them all instead of doing explicit fput's.
 *		Another problem: since fput() may block somebody may
 *		create a new unix_socket when we are in the middle of sweep
 *		phase. Fix: revert the logic wrt MARKED. Mark everything
 *		upon the beginning and unmark non-junk ones.
 *
 *		[12 Oct 1998] AAARGH! New code purges all SCM_RIGHTS
 *		sent to connect()'ed but still not accept()'ed sockets.
 *		Fixed. Old code had slightly different problem here:
 *		extra fput() in situation when we passed the descriptor via
 *		such socket and closed it (descriptor). That would happen on
 *		each unix_gc() until the accept(). Since the struct file in
 *		question would go to the free list and might be reused...
 *		That might be the reason of random oopses on filp_close()
 *		in unrelated processes.
 *
 *	AV		28 Feb 1999
 *		Kill the explicit allocation of stack. Now we keep the tree
 *		with root in dummy + pointer (gc_current) to one of the nodes.
 *		Stack is represented as path from gc_current to dummy. Unmark
 *		now means "add to tree". Push == "make it a son of gc_current".
 *		Pop == "move gc_current to parent". We keep only pointers to
 *		parents (->gc_tree).
 *	AV		1 Mar 1999
 *		Damn. Added missing check for ->dead in listen queues scanning.
 *
 *	Miklos Szeredi 25 Jun 2007
 *		Reimplement with a cycle collecting algorithm. This should
 *		solve several problems with the previous code, like being racy
 *		wrt receive and holding up unrelated socket operations.
 */

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/socket.h>
#include <linux/un.h>
#include <linux/net.h>
#include <linux/fs.h>
#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <linux/file.h>
#include <linux/proc_fs.h>
#include <linux/mutex.h>
#include <linux/wait.h>

#include <net/sock.h>
#include <net/af_unix.h>
#include <net/scm.h>
#include <net/tcp_states.h>

struct unix_sock *unix_get_socket(struct file *filp)
{
	struct inode *inode = file_inode(filp);

	/* Socket ? */
	if (S_ISSOCK(inode->i_mode) && !(filp->f_mode & FMODE_PATH)) {
		struct socket *sock = SOCKET_I(inode);
		const struct proto_ops *ops;
		struct sock *sk = sock->sk;

		ops = READ_ONCE(sock->ops);

		/* PF_UNIX ? */
		if (sk && ops && ops->family == PF_UNIX)
			return unix_sk(sk);
	}

	return NULL;
}

void unix_init_vertex(struct unix_sock *u)
{
	struct unix_vertex *vertex = &u->vertex;

	vertex->out_degree = 0;
	vertex->self_degree = 0;
	INIT_LIST_HEAD(&vertex->edges);
	INIT_LIST_HEAD(&vertex->entry);
	INIT_LIST_HEAD(&vertex->scc_entry);
}

static bool unix_graph_updated;
static bool unix_graph_maybe_cyclic;

static void unix_graph_update(struct unix_edge *edge)
{
	if (unix_graph_updated)
		return;

	if (!edge->successor->out_degree)
		return;

	unix_graph_updated = true;
	unix_graph_maybe_cyclic = true;
}

DEFINE_SPINLOCK(unix_gc_lock);
static LIST_HEAD(unix_unvisited_vertices);
unsigned int unix_tot_inflight;

enum unix_vertex_index {
	UNIX_VERTEX_INDEX_MARK1,
	UNIX_VERTEX_INDEX_MARK2,
	UNIX_VERTEX_INDEX_START,
};

static unsigned long unix_vertex_unvisited_index = UNIX_VERTEX_INDEX_MARK1;

void unix_add_edges(struct scm_fp_list *fpl, struct unix_sock *receiver)
{
	struct unix_vertex *successor;
	int i = 0, j = 0;

	spin_lock(&unix_gc_lock);

	if (receiver->listener)
		successor = &unix_sk(receiver->listener)->vertex;
	else
		successor = &receiver->vertex;

	while (i < fpl->count_unix) {
		struct unix_sock *inflight = unix_get_socket(fpl->fp[j++]);
		struct unix_edge *edge;

		if (!inflight)
			continue;

		edge = fpl->edges + i++;
		edge->predecessor = &inflight->vertex;
		edge->successor = successor;

		if (edge->predecessor == edge->successor)
			edge->predecessor->self_degree++;

		if (!edge->predecessor->out_degree++) {
			edge->predecessor->index = unix_vertex_unvisited_index;

			list_add_tail(&edge->predecessor->entry, &unix_unvisited_vertices);
		}

		INIT_LIST_HEAD(&edge->entry);
		list_add_tail(&edge->entry, &edge->predecessor->edges);

		if (receiver->listener) {
			INIT_LIST_HEAD(&edge->embryo_entry);
			list_add_tail(&edge->embryo_entry, &receiver->vertex.edges);
		}

		unix_graph_update(edge);
	}

	WRITE_ONCE(unix_tot_inflight, unix_tot_inflight + fpl->count_unix);
	WRITE_ONCE(fpl->user->unix_inflight, fpl->user->unix_inflight + fpl->count);

	spin_unlock(&unix_gc_lock);

	fpl->inflight = true;
}

void unix_del_edges(struct scm_fp_list *fpl)
{
	int i = 0;

	spin_lock(&unix_gc_lock);

	while (i < fpl->count_unix) {
		struct unix_edge *edge = fpl->edges + i++;

		unix_graph_update(edge);

		list_del(&edge->entry);

		if (!--edge->predecessor->out_degree)
			list_del_init(&edge->predecessor->entry);

		if (edge->predecessor == edge->successor)
			edge->predecessor->self_degree--;
	}

	WRITE_ONCE(unix_tot_inflight, unix_tot_inflight - fpl->count_unix);
	WRITE_ONCE(fpl->user->unix_inflight, fpl->user->unix_inflight - fpl->count);

	spin_unlock(&unix_gc_lock);

	fpl->inflight = false;
}

void unix_update_edges(struct unix_sock *receiver)
{
	struct unix_edge *edge;

	spin_lock(&unix_gc_lock);

	list_for_each_entry(edge, &receiver->vertex.edges, embryo_entry) {
		unix_graph_update(edge);

		if (edge->predecessor == edge->successor)
			edge->predecessor->self_degree--;

		edge->successor = &receiver->vertex;
	}

	list_del_init(&receiver->vertex.edges);

	receiver->listener = NULL;

	spin_unlock(&unix_gc_lock);
}

int unix_alloc_edges(struct scm_fp_list *fpl)
{
	if (!fpl->count_unix)
		return 0;

	fpl->edges = kvmalloc_array(fpl->count_unix, sizeof(*fpl->edges),
				    GFP_KERNEL_ACCOUNT);
	if (!fpl->edges)
		return -ENOMEM;

	return 0;
}

void unix_free_edges(struct scm_fp_list *fpl)
{
	if (fpl->inflight)
		unix_del_edges(fpl);

	kvfree(fpl->edges);
}

static bool unix_vertex_dead(struct unix_vertex *vertex)
{
	struct unix_edge *edge;
	struct unix_sock *u;
	long total_ref;

	list_for_each_entry(edge, &vertex->edges, entry) {
		if (!edge->successor->out_degree)
			return false;

		if (edge->successor->scc_index != vertex->scc_index)
			return false;
	}

	u = container_of(vertex, typeof(*u), vertex);
	total_ref = file_count(u->sk.sk_socket->file);

	if (total_ref != vertex->out_degree)
		return false;

	return true;
}

static struct sk_buff_head hitlist;

static void unix_collect_skb(struct list_head *scc)
{
	struct unix_vertex *vertex;

	list_for_each_entry_reverse(vertex, scc, scc_entry) {
		struct unix_sock *u = container_of(vertex, typeof(*u), vertex);
		struct sk_buff_head *queue = &u->sk.sk_receive_queue;

		spin_lock(&queue->lock);

		if (u->sk.sk_state == TCP_LISTEN) {
			struct sk_buff *skb;

			skb_queue_walk(queue, skb) {
				struct sk_buff_head *embryo_queue = &skb->sk->sk_receive_queue;

				spin_lock(&embryo_queue->lock);
				skb_queue_splice_init(embryo_queue, &hitlist);
				spin_unlock(&embryo_queue->lock);
			}
		} else {
			skb_queue_splice_init(queue, &hitlist);
		}

		spin_unlock(&queue->lock);
	}
}

static LIST_HEAD(unix_visited_vertices);
static unsigned long unix_vertex_grouped_index = UNIX_VERTEX_INDEX_MARK2;
static unsigned long unix_vertex_last_index = UNIX_VERTEX_INDEX_START;

static void __unix_walk_scc(struct unix_vertex *vertex)
{
	LIST_HEAD(vertex_stack);
	struct unix_edge *edge;
	LIST_HEAD(edge_stack);

next_vertex:
	vertex->index = unix_vertex_last_index;
	vertex->scc_index = unix_vertex_last_index;
	unix_vertex_last_index++;

	list_move(&vertex->scc_entry, &vertex_stack);

	list_for_each_entry(edge, &vertex->edges, entry) {
		if (!edge->successor->out_degree)
			continue;

		if (edge->successor->index == unix_vertex_unvisited_index) {
			list_add(&edge->stack_entry, &edge_stack);

			vertex = edge->successor;
			goto next_vertex;
		}

		if (edge->successor->index != unix_vertex_grouped_index)
			vertex->scc_index = min(vertex->scc_index, edge->successor->scc_index);
next_edge:
	}

	if (vertex->index == vertex->scc_index) {
		bool dead = true;
		LIST_HEAD(scc);

		list_cut_position(&scc, &vertex_stack, &vertex->scc_entry);

		list_for_each_entry_reverse(vertex, &scc, scc_entry) {
			list_move_tail(&vertex->entry, &unix_visited_vertices);

			vertex->index = unix_vertex_grouped_index;

			if (dead)
				dead = unix_vertex_dead(vertex);
		}

		if (dead) {
			unix_collect_skb(&scc);
		} else if (!list_is_singular(&scc)) {
			unix_graph_maybe_cyclic = true;
		} else {
			vertex = list_first_entry(&scc, typeof(*vertex), scc_entry);
			if (vertex->self_degree)
				unix_graph_maybe_cyclic = true;
		}

		list_del(&scc);
	}

	if (!list_empty(&edge_stack)) {
		edge = list_first_entry(&edge_stack, typeof(*edge), stack_entry);
		list_del_init(&edge->stack_entry);

		vertex = edge->predecessor;
		vertex->scc_index = min(vertex->scc_index, edge->successor->scc_index);
		goto next_edge;
	}
}

static void unix_walk_scc(void)
{
	unix_vertex_last_index = UNIX_VERTEX_INDEX_START;
	unix_graph_maybe_cyclic = false;

	while (!list_empty(&unix_unvisited_vertices)) {
		struct unix_vertex *vertex;

		vertex = list_first_entry(&unix_unvisited_vertices, typeof(*vertex), entry);
		__unix_walk_scc(vertex);
	}

	list_replace_init(&unix_visited_vertices, &unix_unvisited_vertices);
	swap(unix_vertex_unvisited_index, unix_vertex_grouped_index);
	unix_graph_updated = false;
}

static void unix_walk_scc_fast(void)
{
	while (!list_empty(&unix_unvisited_vertices)) {
		struct unix_vertex *vertex;
		bool dead = true;
		LIST_HEAD(scc);

		vertex = list_first_entry(&unix_unvisited_vertices, typeof(*vertex), entry);
		list_add(&scc, &vertex->scc_entry);

		list_for_each_entry_reverse(vertex, &scc, scc_entry) {
			list_move_tail(&vertex->entry, &unix_visited_vertices);

			if (dead)
				dead = unix_vertex_dead(vertex);
		}

		if (dead)
			unix_collect_skb(&scc);

		list_del(&scc);
	}

	list_replace_init(&unix_visited_vertices, &unix_unvisited_vertices);
}

static bool gc_in_progress;

static void __unix_gc(struct work_struct *work)
{
	spin_lock(&unix_gc_lock);

	if (!unix_graph_maybe_cyclic) {
		spin_unlock(&unix_gc_lock);
		goto skip_gc;
	}

	__skb_queue_head_init(&hitlist);

	if (unix_graph_updated)
		unix_walk_scc();
	else
		unix_walk_scc_fast();

	spin_unlock(&unix_gc_lock);

	__skb_queue_purge(&hitlist);
skip_gc:
	WRITE_ONCE(gc_in_progress, false);
}

static DECLARE_WORK(unix_gc_work, __unix_gc);

void unix_gc(void)
{
	WRITE_ONCE(gc_in_progress, true);
	queue_work(system_unbound_wq, &unix_gc_work);
}

#define UNIX_INFLIGHT_TRIGGER_GC 16000
#define UNIX_INFLIGHT_SANE_USER (SCM_MAX_FD * 8)

void wait_for_unix_gc(struct scm_fp_list *fpl)
{
	/* If number of inflight sockets is insane,
	 * force a garbage collect right now.
	 *
	 * Paired with the WRITE_ONCE() in unix_inflight(),
	 * unix_notinflight(), and __unix_gc().
	 */
	if (READ_ONCE(unix_tot_inflight) > UNIX_INFLIGHT_TRIGGER_GC &&
	    !READ_ONCE(gc_in_progress))
		unix_gc();

	/* Penalise users who want to send AF_UNIX sockets
	 * but whose sockets have not been received yet.
	 */
	if (!fpl || !fpl->count_unix ||
	    READ_ONCE(fpl->user->unix_inflight) < UNIX_INFLIGHT_SANE_USER)
		return;

	if (READ_ONCE(gc_in_progress))
		flush_work(&unix_gc_work);
}
