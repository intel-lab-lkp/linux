==================================
Administrative interfaces for nfsd
==================================

Note that normally these interfaces are used only by the utilities in
nfs-utils.

nfsd is controlled mainly by pseudofiles under the "nfsd" filesystem,
which is normally mounted at /proc/fs/nfsd/.

The server is always started by the first write of a nonzero value to
nfsd/threads.

Before doing that, NFSD can be told which sockets to listen on by
writing to nfsd/portlist; that write may be:

	-  an ascii-encoded file descriptor, which should refer to a
	   bound (and listening, for tcp) socket, or
	-  "transportname port", where transportname is currently either
	   "udp", "tcp", or "rdma".

If nfsd is started without doing any of these, then it will create one
udp and one tcp listener at port 2049 (see nfsd_init_socks).

On startup, nfsd and lockd grace periods start. nfsd is shut down by a write of
0 to nfsd/threads.  All locks and state are thrown away at that point.

Between startup and shutdown, the number of threads may be adjusted up
or down by additional writes to nfsd/threads or by writes to
nfsd/pool_threads.

NFSv4 client visibility
=======================

The privileged ``client-get`` dump in the ``nfsd`` Generic Netlink family
emits one message for each NFSv4 client.  Each message identifies the client
by its server-generated client ID and transport address, then reports its
minor version, client and callback states, signed lease time remaining, and
whether an NFSv4.1 or later client sent RECLAIM_COMPLETE.

Clients can change between messages.  If that can make the dump skip or repeat
a record, the kernel sets ``NLM_F_DUMP_INTR`` and userspace should retry.

The existing ``/proc/fs/nfsd/clients/`` files remain available for inspection.
The ``states`` file contains individual stateids, and writing ``expire`` to
``ctl`` forcibly removes the client and all state it owns.

For more detail about files under nfsd/ and what they control, see
fs/nfsd/nfsctl.c; most of them have detailed comments.

Implementation notes
====================

Note that the rpc server requires the caller to serialize addition and
removal of listening sockets, and startup and shutdown of the server.
For nfsd this is done using nfsd_mutex.
