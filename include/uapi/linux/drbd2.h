/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */
/* Do not edit directly, auto-generated from: */
/*	Documentation/netlink/specs/drbd2.yaml */
/* YNL-GEN uapi header */
/* To regenerate run: tools/net/ynl/ynl-regen.sh */

#ifndef _UAPI_LINUX_DRBD2_H
#define _UAPI_LINUX_DRBD2_H

#define DRBD2_FAMILY_NAME	"drbd2"
#define DRBD2_FAMILY_VERSION	1

#define DRBD2_RESOURCE_NAME_MAX		128
#define DRBD2_CONNECTION_NAME_MAX	64
#define DRBD2_SHARED_SECRET_MAX		64
#define DRBD2_ALG_NAME_MAX		64
#define DRBD2_TRANSPORT_NAME_MAX	64
#define DRBD2_DEVICE_PATH_MAX		128
#define DRBD2_CPU_MASK_SIZE		256
#define DRBD2_HELPER_NAME_MAX		32
#define DRBD2_HISTORY_UUIDS_SIZE	256

/**
 * enum drbd2_io_error_policy - What to do when the backing device reports an
 *   I/O error (on-io-error).
 * @DRBD2_IO_ERROR_POLICY_PASS_ON: Mark the failed block out of sync, change
 *   the disk state to inconsistent and retry the request on a peer.
 * @DRBD2_IO_ERROR_POLICY_CALL_LOCAL_IO_ERROR: Call the local-io-error handler.
 * @DRBD2_IO_ERROR_POLICY_DETACH: Detach the backing device and continue in
 *   diskless mode.
 */
enum drbd2_io_error_policy {
	DRBD2_IO_ERROR_POLICY_PASS_ON,
	DRBD2_IO_ERROR_POLICY_CALL_LOCAL_IO_ERROR,
	DRBD2_IO_ERROR_POLICY_DETACH,
};

/**
 * enum drbd2_fencing_policy - Fencing policy (fencing).
 * @DRBD2_FENCING_POLICY_DONT_CARE: Take no fencing action at all.
 * @DRBD2_FENCING_POLICY_RESOURCE: A node that becomes a disconnected primary
 *   calls the fence-peer handler to outdate the peer.
 * @DRBD2_FENCING_POLICY_STONITH: A node that becomes a disconnected primary
 *   freezes its I/O and calls the fence-peer handler, which may have to shoot
 *   the peer.
 */
enum drbd2_fencing_policy {
	DRBD2_FENCING_POLICY_DONT_CARE,
	DRBD2_FENCING_POLICY_RESOURCE,
	DRBD2_FENCING_POLICY_STONITH,
};

/**
 * enum drbd2_after_sb_policy - Split-brain recovery policy (after-sb-0pri,
 *   after-sb-1pri, after-sb-2pri, rr-conflict).
 * @DRBD2_AFTER_SB_POLICY_DISCONNECT: Do not resynchronize automatically,
 *   simply disconnect.
 * @DRBD2_AFTER_SB_POLICY_DISCARD_YOUNGER_PRIMARY: Resynchronize from the node
 *   that became primary first.
 * @DRBD2_AFTER_SB_POLICY_DISCARD_OLDER_PRIMARY: Resynchronize from the node
 *   that became primary last.
 * @DRBD2_AFTER_SB_POLICY_DISCARD_ZERO_CHANGES: Resynchronize from the node
 *   that wrote data since the split brain was detected, or disconnect if both
 *   nodes wrote.
 * @DRBD2_AFTER_SB_POLICY_DISCARD_LEAST_CHANGES: Resynchronize from the node
 *   with more modified blocks.
 * @DRBD2_AFTER_SB_POLICY_DISCARD_LOCAL: Discard the data on this node and
 *   resynchronize it from the peer.
 * @DRBD2_AFTER_SB_POLICY_DISCARD_REMOTE: Discard the data on the peer and
 *   resynchronize it from this node.
 * @DRBD2_AFTER_SB_POLICY_CONSENSUS: Discard the data on the secondary if the
 *   after-sb-0pri policy would discard it as well, otherwise disconnect.
 * @DRBD2_AFTER_SB_POLICY_DISCARD_SECONDARY: Discard the data on the secondary
 *   node.
 * @DRBD2_AFTER_SB_POLICY_CALL_PRI_LOST_AFTER_SB: Take the after-sb-0pri
 *   decision and call the pri-lost-after-sb handler on the node that has to
 *   give up its data.
 * @DRBD2_AFTER_SB_POLICY_VIOLENTLY_AS0P: Always take the after-sb-0pri
 *   decision, even if that changes the primary's view of the data; this can
 *   crash the primary.
 * @DRBD2_AFTER_SB_POLICY_RETRY_CONNECT: Disconnect now and immediately retry
 *   to connect (rr-conflict only).
 * @DRBD2_AFTER_SB_POLICY_AUTO_DISCARD: Reverse the resync direction so that
 *   the current primary is resynchronized from the current secondary
 *   (rr-conflict with protocol A only).
 */
enum drbd2_after_sb_policy {
	DRBD2_AFTER_SB_POLICY_DISCONNECT,
	DRBD2_AFTER_SB_POLICY_DISCARD_YOUNGER_PRIMARY,
	DRBD2_AFTER_SB_POLICY_DISCARD_OLDER_PRIMARY,
	DRBD2_AFTER_SB_POLICY_DISCARD_ZERO_CHANGES,
	DRBD2_AFTER_SB_POLICY_DISCARD_LEAST_CHANGES,
	DRBD2_AFTER_SB_POLICY_DISCARD_LOCAL,
	DRBD2_AFTER_SB_POLICY_DISCARD_REMOTE,
	DRBD2_AFTER_SB_POLICY_CONSENSUS,
	DRBD2_AFTER_SB_POLICY_DISCARD_SECONDARY,
	DRBD2_AFTER_SB_POLICY_CALL_PRI_LOST_AFTER_SB,
	DRBD2_AFTER_SB_POLICY_VIOLENTLY_AS0P,
	DRBD2_AFTER_SB_POLICY_RETRY_CONNECT,
	DRBD2_AFTER_SB_POLICY_AUTO_DISCARD,
};

/**
 * enum drbd2_on_no_data_policy - What to do when no data is accessible
 *   (on-no-data-accessible).
 * @DRBD2_ON_NO_DATA_POLICY_IO_ERROR: Complete the requests with an I/O error,
 *   so that system calls fail with EIO.
 * @DRBD2_ON_NO_DATA_POLICY_SUSPEND_IO: Suspend I/O until the data becomes
 *   accessible again or the administrator forces I/O to resume.
 */
enum drbd2_on_no_data_policy {
	DRBD2_ON_NO_DATA_POLICY_IO_ERROR,
	DRBD2_ON_NO_DATA_POLICY_SUSPEND_IO,
};

/**
 * enum drbd2_on_no_quorum_policy - What to do when quorum is lost
 *   (on-no-quorum).
 * @DRBD2_ON_NO_QUORUM_POLICY_IO_ERROR: Complete the requests with an I/O error
 *   while the resource has no quorum.
 * @DRBD2_ON_NO_QUORUM_POLICY_SUSPEND_IO: Freeze I/O while the resource has no
 *   quorum.
 */
enum drbd2_on_no_quorum_policy {
	DRBD2_ON_NO_QUORUM_POLICY_IO_ERROR,
	DRBD2_ON_NO_QUORUM_POLICY_SUSPEND_IO,
};

/**
 * enum drbd2_on_suspended_primary_outdated_policy - What to do with a
 *   suspended primary that became outdated.
 * @DRBD2_ON_SUSPENDED_PRIMARY_OUTDATED_POLICY_DISCONNECT: Reject connection
 *   attempts and stay isolated.
 * @DRBD2_ON_SUSPENDED_PRIMARY_OUTDATED_POLICY_FORCE_SECONDARY: Demote to
 *   secondary immediately and fail all pending and new I/O requests.
 */
enum drbd2_on_suspended_primary_outdated_policy {
	DRBD2_ON_SUSPENDED_PRIMARY_OUTDATED_POLICY_DISCONNECT,
	DRBD2_ON_SUSPENDED_PRIMARY_OUTDATED_POLICY_FORCE_SECONDARY,
};

/**
 * enum drbd2_on_congestion_policy - What to do when the replication link is
 *   congested (on-congestion).
 * @DRBD2_ON_CONGESTION_POLICY_BLOCK: Block application writes until the
 *   transport's send queue drains.
 * @DRBD2_ON_CONGESTION_POLICY_PULL_AHEAD: Switch into ahead/behind mode and
 *   record further changes in the bitmap instead of replicating them.
 * @DRBD2_ON_CONGESTION_POLICY_DISCONNECT: Disconnect from the peer instead of
 *   blocking application writes.
 */
enum drbd2_on_congestion_policy {
	DRBD2_ON_CONGESTION_POLICY_BLOCK,
	DRBD2_ON_CONGESTION_POLICY_PULL_AHEAD,
	DRBD2_ON_CONGESTION_POLICY_DISCONNECT,
};

/**
 * enum drbd2_read_balancing_policy - Read balancing policy (read-balancing).
 * @DRBD2_READ_BALANCING_POLICY_PREFER_LOCAL: Always read from the local disk.
 * @DRBD2_READ_BALANCING_POLICY_PREFER_REMOTE: Always read from a peer's disk.
 * @DRBD2_READ_BALANCING_POLICY_ROUND_ROBIN: Alternate between the local disk
 *   and a peer for consecutive reads.
 * @DRBD2_READ_BALANCING_POLICY_LEAST_PENDING: Read from the disk with the
 *   fewest pending requests.
 * @DRBD2_READ_BALANCING_POLICY_WHEN_CONGESTED_REMOTE: Read from a peer only
 *   while the local disk is congested; deprecated since DRBD 9.1.12.
 * @DRBD2_READ_BALANCING_POLICY_STRIPING_32K: Read alternating 32 KiB stripes
 *   from the local disk and from a peer.
 * @DRBD2_READ_BALANCING_POLICY_STRIPING_64K: Read alternating 64 KiB stripes
 *   from the local disk and from a peer.
 * @DRBD2_READ_BALANCING_POLICY_STRIPING_128K: Read alternating 128 KiB stripes
 *   from the local disk and from a peer.
 * @DRBD2_READ_BALANCING_POLICY_STRIPING_256K: Read alternating 256 KiB stripes
 *   from the local disk and from a peer.
 * @DRBD2_READ_BALANCING_POLICY_STRIPING_512K: Read alternating 512 KiB stripes
 *   from the local disk and from a peer.
 * @DRBD2_READ_BALANCING_POLICY_STRIPING_1M: Read alternating 1 MiB stripes
 *   from the local disk and from a peer.
 */
enum drbd2_read_balancing_policy {
	DRBD2_READ_BALANCING_POLICY_PREFER_LOCAL,
	DRBD2_READ_BALANCING_POLICY_PREFER_REMOTE,
	DRBD2_READ_BALANCING_POLICY_ROUND_ROBIN,
	DRBD2_READ_BALANCING_POLICY_LEAST_PENDING,
	DRBD2_READ_BALANCING_POLICY_WHEN_CONGESTED_REMOTE,
	DRBD2_READ_BALANCING_POLICY_STRIPING_32K,
	DRBD2_READ_BALANCING_POLICY_STRIPING_64K,
	DRBD2_READ_BALANCING_POLICY_STRIPING_128K,
	DRBD2_READ_BALANCING_POLICY_STRIPING_256K,
	DRBD2_READ_BALANCING_POLICY_STRIPING_512K,
	DRBD2_READ_BALANCING_POLICY_STRIPING_1M,
};

/**
 * enum drbd2_wire_protocol - Replication protocol (protocol).
 * @DRBD2_WIRE_PROTOCOL_A: Writes complete as soon as they reached the local
 *   disk and the local send buffer.
 * @DRBD2_WIRE_PROTOCOL_B: Writes complete as soon as they reached the local
 *   disk and all peers acknowledged their receipt.
 * @DRBD2_WIRE_PROTOCOL_C: Writes complete as soon as they reached the local
 *   disk and all remote disks.
 */
enum drbd2_wire_protocol {
	DRBD2_WIRE_PROTOCOL_A = 1,
	DRBD2_WIRE_PROTOCOL_B,
	DRBD2_WIRE_PROTOCOL_C,
};

/**
 * enum drbd2_role - Role of a resource on a node. Values mirror enum drbd_role
 *   in linux/drbd.h so both netlink dialects share one numbering.
 * @DRBD2_ROLE_UNKNOWN: The role is not known, for example because the peer is
 *   not connected.
 * @DRBD2_ROLE_PRIMARY: The resource is primary; its devices may be opened for
 *   writing.
 * @DRBD2_ROLE_SECONDARY: The resource is secondary; its devices may not be
 *   opened for writing.
 */
enum drbd2_role {
	DRBD2_ROLE_UNKNOWN,
	DRBD2_ROLE_PRIMARY,
	DRBD2_ROLE_SECONDARY,
};

/**
 * enum drbd2_connection_state - State of a connection to a peer. Values mirror
 *   enum drbd_conn_state in linux/drbd.h so both netlink dialects share one
 *   numbering.
 * @DRBD2_CONNECTION_STATE_STANDALONE: No network configuration is in effect
 *   and no connection is attempted.
 * @DRBD2_CONNECTION_STATE_DISCONNECTING: Temporary state on the way to
 *   standalone while the connection is torn down.
 * @DRBD2_CONNECTION_STATE_UNCONNECTED: Temporary state before the next
 *   connection attempt starts.
 * @DRBD2_CONNECTION_STATE_TIMEOUT: The connection was dropped because the peer
 *   did not answer in time.
 * @DRBD2_CONNECTION_STATE_BROKEN_PIPE: The connection was dropped because the
 *   transport reported a broken connection.
 * @DRBD2_CONNECTION_STATE_NETWORK_FAILURE: The connection was dropped because
 *   of a network failure.
 * @DRBD2_CONNECTION_STATE_PROTOCOL_ERROR: The connection was dropped because
 *   the peer violated the replication protocol.
 * @DRBD2_CONNECTION_STATE_TEAR_DOWN: Temporary state while the peer closes the
 *   connection.
 * @DRBD2_CONNECTION_STATE_CONNECTING: Listening for the peer and trying to
 *   establish a connection.
 * @DRBD2_CONNECTION_STATE_CONNECTED: A connection to the peer is established.
 */
enum drbd2_connection_state {
	DRBD2_CONNECTION_STATE_STANDALONE,
	DRBD2_CONNECTION_STATE_DISCONNECTING,
	DRBD2_CONNECTION_STATE_UNCONNECTED,
	DRBD2_CONNECTION_STATE_TIMEOUT,
	DRBD2_CONNECTION_STATE_BROKEN_PIPE,
	DRBD2_CONNECTION_STATE_NETWORK_FAILURE,
	DRBD2_CONNECTION_STATE_PROTOCOL_ERROR,
	DRBD2_CONNECTION_STATE_TEAR_DOWN,
	DRBD2_CONNECTION_STATE_CONNECTING,
	DRBD2_CONNECTION_STATE_CONNECTED,
};

/**
 * enum drbd2_repl_state - Replication state of a peer device. Values mirror
 *   enum drbd_repl_state in linux/drbd.h so both netlink dialects share one
 *   numbering.
 * @DRBD2_REPL_STATE_OFF: The peer device does not replicate because the
 *   connection is not established.
 * @DRBD2_REPL_STATE_ESTABLISHED: The peer device is connected and replicating
 *   normally.
 * @DRBD2_REPL_STATE_STARTING_SYNC_S: A full resynchronization was requested by
 *   the administrator and starts with this node as the source.
 * @DRBD2_REPL_STATE_STARTING_SYNC_T: A full resynchronization was requested by
 *   the administrator and starts with this node as the target.
 * @DRBD2_REPL_STATE_WF_BITMAP_S: Waiting for the bitmap exchange to finish,
 *   with this node as the prospective resync source.
 * @DRBD2_REPL_STATE_WF_BITMAP_T: Waiting for the bitmap exchange to finish,
 *   with this node as the prospective resync target.
 * @DRBD2_REPL_STATE_WF_SYNC_UUID: Waiting for the resync source to create and
 *   send the new resync UUID.
 * @DRBD2_REPL_STATE_SYNC_SOURCE: Resynchronization is running with this node
 *   as the source.
 * @DRBD2_REPL_STATE_SYNC_TARGET: Resynchronization is running with this node
 *   as the target.
 * @DRBD2_REPL_STATE_VERIFY_S: Online verification is running with this node as
 *   the initiator.
 * @DRBD2_REPL_STATE_VERIFY_T: Online verification is running with this node as
 *   the target.
 * @DRBD2_REPL_STATE_PAUSED_SYNC_S: Resynchronization with this node as the
 *   source is paused.
 * @DRBD2_REPL_STATE_PAUSED_SYNC_T: Resynchronization with this node as the
 *   target is paused.
 * @DRBD2_REPL_STATE_AHEAD: Replication is suspended because the link is
 *   congested; changes are only recorded in the bitmap.
 * @DRBD2_REPL_STATE_BEHIND: The peer is ahead of this node, which will be
 *   resynchronized once the congestion is over.
 */
enum drbd2_repl_state {
	DRBD2_REPL_STATE_OFF = 9,
	DRBD2_REPL_STATE_ESTABLISHED,
	DRBD2_REPL_STATE_STARTING_SYNC_S,
	DRBD2_REPL_STATE_STARTING_SYNC_T,
	DRBD2_REPL_STATE_WF_BITMAP_S,
	DRBD2_REPL_STATE_WF_BITMAP_T,
	DRBD2_REPL_STATE_WF_SYNC_UUID,
	DRBD2_REPL_STATE_SYNC_SOURCE,
	DRBD2_REPL_STATE_SYNC_TARGET,
	DRBD2_REPL_STATE_VERIFY_S,
	DRBD2_REPL_STATE_VERIFY_T,
	DRBD2_REPL_STATE_PAUSED_SYNC_S,
	DRBD2_REPL_STATE_PAUSED_SYNC_T,
	DRBD2_REPL_STATE_AHEAD,
	DRBD2_REPL_STATE_BEHIND,
};

/**
 * enum drbd2_disk_state - State of a backing disk. Values mirror enum
 *   drbd_disk_state in linux/drbd.h so both netlink dialects share one
 *   numbering.
 * @DRBD2_DISK_STATE_DISKLESS: No backing device is attached.
 * @DRBD2_DISK_STATE_ATTACHING: The backing device's metadata is being read.
 * @DRBD2_DISK_STATE_DETACHING: The backing device is being detached while
 *   pending I/O drains.
 * @DRBD2_DISK_STATE_FAILED: The backing device failed; the disk becomes
 *   diskless once the peers have been told.
 * @DRBD2_DISK_STATE_NEGOTIATING: Attaching while connected, with the data
 *   generation still being negotiated with the peers.
 * @DRBD2_DISK_STATE_INCONSISTENT: The data is inconsistent, for example
 *   because a resynchronization is in progress.
 * @DRBD2_DISK_STATE_OUTDATED: The data is consistent but known to be stale
 *   relative to a peer.
 * @DRBD2_DISK_STATE_UNKNOWN: The disk state is not known; used only for a
 *   peer's disk.
 * @DRBD2_DISK_STATE_CONSISTENT: The data is consistent but it is not yet known
 *   whether it is up to date.
 * @DRBD2_DISK_STATE_UP_TO_DATE: The data is consistent and up to date; only
 *   this state allows application I/O.
 */
enum drbd2_disk_state {
	DRBD2_DISK_STATE_DISKLESS,
	DRBD2_DISK_STATE_ATTACHING,
	DRBD2_DISK_STATE_DETACHING,
	DRBD2_DISK_STATE_FAILED,
	DRBD2_DISK_STATE_NEGOTIATING,
	DRBD2_DISK_STATE_INCONSISTENT,
	DRBD2_DISK_STATE_OUTDATED,
	DRBD2_DISK_STATE_UNKNOWN,
	DRBD2_DISK_STATE_CONSISTENT,
	DRBD2_DISK_STATE_UP_TO_DATE,
};

/**
 * enum drbd2_write_ordering - Write ordering method in use for the backing
 *   device.
 * @DRBD2_WRITE_ORDERING_NONE: No ordering between dependent writes is enforced
 *   on the backing device.
 * @DRBD2_WRITE_ORDERING_DRAIN_IO: Dependent writes are ordered by draining the
 *   backing device's request queue.
 * @DRBD2_WRITE_ORDERING_BDEV_FLUSH: Dependent writes are ordered by issuing
 *   flushes to the backing device.
 * @DRBD2_WRITE_ORDERING_BIO_BARRIER: Dependent writes are ordered with block
 *   device barriers, which the Linux block layer no longer provides.
 */
enum drbd2_write_ordering {
	DRBD2_WRITE_ORDERING_NONE,
	DRBD2_WRITE_ORDERING_DRAIN_IO,
	DRBD2_WRITE_ORDERING_BDEV_FLUSH,
	DRBD2_WRITE_ORDERING_BIO_BARRIER,
};

/**
 * enum drbd2_timeout_type - Which wait-for-connection timeout applies
 *   (timeout-type-get reply).
 * @DRBD2_TIMEOUT_TYPE_DEFAULT: The regular wait-for-connection timeout
 *   applies.
 * @DRBD2_TIMEOUT_TYPE_DEGRADED: The degraded-cluster timeout applies because
 *   the local data is not up to date.
 * @DRBD2_TIMEOUT_TYPE_PEER_OUTDATED: The timeout for an outdated peer applies.
 */
enum drbd2_timeout_type {
	DRBD2_TIMEOUT_TYPE_DEFAULT,
	DRBD2_TIMEOUT_TYPE_DEGRADED,
	DRBD2_TIMEOUT_TYPE_PEER_OUTDATED,
};

/**
 * enum drbd2_helper_phase - Whether a helper notification announces the call
 *   or reports its result.
 * @DRBD2_HELPER_PHASE_CALL: The helper script is about to be called.
 * @DRBD2_HELPER_PHASE_RESPONSE: The helper script has finished and its exit
 *   status is reported.
 */
enum drbd2_helper_phase {
	DRBD2_HELPER_PHASE_CALL,
	DRBD2_HELPER_PHASE_RESPONSE,
};

/**
 * enum drbd2_state_change_action - What a state-change message reports about
 *   the object it carries. Values are assigned independently of the legacy
 *   drbd_notification_type.
 * @DRBD2_STATE_CHANGE_ACTION_EXISTS: The object already existed when the
 *   notification stream was opened.
 * @DRBD2_STATE_CHANGE_ACTION_CREATE: The object was just created.
 * @DRBD2_STATE_CHANGE_ACTION_CHANGE: The state or statistics of the object
 *   changed.
 * @DRBD2_STATE_CHANGE_ACTION_DESTROY: The object was destroyed.
 * @DRBD2_STATE_CHANGE_ACTION_RENAME: The resource was renamed; the new name is
 *   carried in resource.new-name.
 */
enum drbd2_state_change_action {
	DRBD2_STATE_CHANGE_ACTION_EXISTS,
	DRBD2_STATE_CHANGE_ACTION_CREATE,
	DRBD2_STATE_CHANGE_ACTION_CHANGE,
	DRBD2_STATE_CHANGE_ACTION_DESTROY,
	DRBD2_STATE_CHANGE_ACTION_RENAME,
};

/**
 * enum drbd2_state_result - Outcome of a state change request. Error values
 *   equal the negated kernel-internal SS_* codes (value 3 is unused); success
 *   and the other non-error outcomes follow at 29 and above.
 * @DRBD2_STATE_RESULT_UNKNOWN_ERROR: The state change failed for an
 *   unspecified reason.
 * @DRBD2_STATE_RESULT_TWO_PRIMARIES: Multiple primaries are not allowed by the
 *   configuration.
 * @DRBD2_STATE_RESULT_NO_UP_TO_DATE_DISK: The request needs access to
 *   up-to-date data.
 * @DRBD2_STATE_RESULT_NO_LOCAL_DISK: Cannot resynchronize without a local
 *   disk.
 * @DRBD2_STATE_RESULT_NO_REMOTE_DISK: Cannot resynchronize without a remote
 *   disk.
 * @DRBD2_STATE_RESULT_CONNECTED_OUTDATES: Refusing to become outdated while
 *   connected.
 * @DRBD2_STATE_RESULT_PRIMARY_NOP: Refusing to become primary while the peer
 *   is not outdated.
 * @DRBD2_STATE_RESULT_RESYNC_RUNNING: Cannot start online verify or
 *   resynchronization because one is already active.
 * @DRBD2_STATE_RESULT_ALREADY_STANDALONE: Cannot disconnect a connection that
 *   is already standalone.
 * @DRBD2_STATE_RESULT_CW_FAILED_BY_PEER: The cluster-wide state change was
 *   refused by a peer node.
 * @DRBD2_STATE_RESULT_IS_DISKLESS: The device is diskless but the requested
 *   operation requires a disk.
 * @DRBD2_STATE_RESULT_DEVICE_IN_USE: The device is held open by someone.
 * @DRBD2_STATE_RESULT_NO_NET_CONFIG: There is no network configuration for
 *   this connection.
 * @DRBD2_STATE_RESULT_NO_VERIFY_ALG: Starting online verify requires a verify
 *   algorithm to be configured.
 * @DRBD2_STATE_RESULT_NEED_CONNECTION: The state change requires an
 *   established connection.
 * @DRBD2_STATE_RESULT_LOWER_THAN_OUTDATED: The disk state is lower than
 *   outdated.
 * @DRBD2_STATE_RESULT_NOT_SUPPORTED: The peer does not support the requested
 *   protocol feature.
 * @DRBD2_STATE_RESULT_IN_TRANSIENT_STATE: The object is in a transient state;
 *   retry after the next state change.
 * @DRBD2_STATE_RESULT_CONCURRENT_STATE_CHANGE: Concurrent state changes were
 *   detected and this one was aborted.
 * @DRBD2_STATE_RESULT_OTHER_VOLUME_PEER_PRIMARY: Another volume is primary on
 *   the peer, which the configuration does not allow.
 * @DRBD2_STATE_RESULT_INTERRUPTED: The state change was interrupted.
 * @DRBD2_STATE_RESULT_PRIMARY_READER: The peer may not become primary while
 *   the device is open for reading here.
 * @DRBD2_STATE_RESULT_TIMEOUT: The state change timed out.
 * @DRBD2_STATE_RESULT_WEAKLY_CONNECTED: Primary nodes must be strongly
 *   connected among each other.
 * @DRBD2_STATE_RESULT_NO_QUORUM: The resource does not have quorum.
 * @DRBD2_STATE_RESULT_ATTACH_NO_BITMAP: An intentionally diskless peer may not
 *   attach a disk.
 * @DRBD2_STATE_RESULT_HANDSHAKE_DISCONNECT: Disconnect was chosen during the
 *   connection handshake.
 * @DRBD2_STATE_RESULT_HANDSHAKE_RETRY: Retry was chosen during the connection
 *   handshake.
 * @DRBD2_STATE_RESULT_SUCCESS: The state change succeeded.
 * @DRBD2_STATE_RESULT_NOTHING_TO_DO: The requested state was already in
 *   effect, so nothing had to be done.
 * @DRBD2_STATE_RESULT_CW_SUCCESS: The cluster-wide state change succeeded.
 * @DRBD2_STATE_RESULT_CW_NO_NEED: No cluster-wide state change was necessary.
 */
enum drbd2_state_result {
	DRBD2_STATE_RESULT_UNKNOWN_ERROR,
	DRBD2_STATE_RESULT_TWO_PRIMARIES,
	DRBD2_STATE_RESULT_NO_UP_TO_DATE_DISK,
	DRBD2_STATE_RESULT_NO_LOCAL_DISK = 4,
	DRBD2_STATE_RESULT_NO_REMOTE_DISK,
	DRBD2_STATE_RESULT_CONNECTED_OUTDATES,
	DRBD2_STATE_RESULT_PRIMARY_NOP,
	DRBD2_STATE_RESULT_RESYNC_RUNNING,
	DRBD2_STATE_RESULT_ALREADY_STANDALONE,
	DRBD2_STATE_RESULT_CW_FAILED_BY_PEER,
	DRBD2_STATE_RESULT_IS_DISKLESS,
	DRBD2_STATE_RESULT_DEVICE_IN_USE,
	DRBD2_STATE_RESULT_NO_NET_CONFIG,
	DRBD2_STATE_RESULT_NO_VERIFY_ALG,
	DRBD2_STATE_RESULT_NEED_CONNECTION,
	DRBD2_STATE_RESULT_LOWER_THAN_OUTDATED,
	DRBD2_STATE_RESULT_NOT_SUPPORTED,
	DRBD2_STATE_RESULT_IN_TRANSIENT_STATE,
	DRBD2_STATE_RESULT_CONCURRENT_STATE_CHANGE,
	DRBD2_STATE_RESULT_OTHER_VOLUME_PEER_PRIMARY,
	DRBD2_STATE_RESULT_INTERRUPTED,
	DRBD2_STATE_RESULT_PRIMARY_READER,
	DRBD2_STATE_RESULT_TIMEOUT,
	DRBD2_STATE_RESULT_WEAKLY_CONNECTED,
	DRBD2_STATE_RESULT_NO_QUORUM,
	DRBD2_STATE_RESULT_ATTACH_NO_BITMAP,
	DRBD2_STATE_RESULT_HANDSHAKE_DISCONNECT,
	DRBD2_STATE_RESULT_HANDSHAKE_RETRY,
	DRBD2_STATE_RESULT_SUCCESS,
	DRBD2_STATE_RESULT_NOTHING_TO_DO,
	DRBD2_STATE_RESULT_CW_SUCCESS,
	DRBD2_STATE_RESULT_CW_NO_NEED,
};

enum {
	DRBD2_A_ADDRESS_FAMILY = 1,
	DRBD2_A_ADDRESS_PORT,
	DRBD2_A_ADDRESS_IPV4,
	DRBD2_A_ADDRESS_IPV6,

	__DRBD2_A_ADDRESS_MAX,
	DRBD2_A_ADDRESS_MAX = (__DRBD2_A_ADDRESS_MAX - 1)
};

enum {
	DRBD2_A_CONTEXT_RESOURCE_NAME = 1,
	DRBD2_A_CONTEXT_VOLUME,
	DRBD2_A_CONTEXT_MINOR,
	DRBD2_A_CONTEXT_PEER_NODE_ID,
	DRBD2_A_CONTEXT_CONNECTION_NAME,
	DRBD2_A_CONTEXT_MY_ADDRESS,
	DRBD2_A_CONTEXT_PEER_ADDRESS,

	__DRBD2_A_CONTEXT_MAX,
	DRBD2_A_CONTEXT_MAX = (__DRBD2_A_CONTEXT_MAX - 1)
};

enum {
	DRBD2_A_DISK_CONF_BACKING_DEV = 1,
	DRBD2_A_DISK_CONF_META_DEV,
	DRBD2_A_DISK_CONF_META_DEV_IDX,
	DRBD2_A_DISK_CONF_SIZE,
	DRBD2_A_DISK_CONF_ON_IO_ERROR,
	DRBD2_A_DISK_CONF_RESYNC_AFTER,
	DRBD2_A_DISK_CONF_AL_EXTENTS,
	DRBD2_A_DISK_CONF_DISK_BARRIER,
	DRBD2_A_DISK_CONF_DISK_FLUSHES,
	DRBD2_A_DISK_CONF_DISK_DRAIN,
	DRBD2_A_DISK_CONF_MD_FLUSHES,
	DRBD2_A_DISK_CONF_DISK_TIMEOUT,
	DRBD2_A_DISK_CONF_READ_BALANCING,
	DRBD2_A_DISK_CONF_UNPLUG_WATERMARK,
	DRBD2_A_DISK_CONF_RS_DISCARD_GRANULARITY,
	DRBD2_A_DISK_CONF_AL_UPDATES,
	DRBD2_A_DISK_CONF_DISCARD_ZEROES_IF_ALIGNED,
	DRBD2_A_DISK_CONF_DISABLE_WRITE_SAME,
	DRBD2_A_DISK_CONF_BITMAP,

	__DRBD2_A_DISK_CONF_MAX,
	DRBD2_A_DISK_CONF_MAX = (__DRBD2_A_DISK_CONF_MAX - 1)
};

enum {
	DRBD2_A_RESOURCE_OPTS_CPU_MASK = 1,
	DRBD2_A_RESOURCE_OPTS_ON_NO_DATA_ACCESSIBLE,
	DRBD2_A_RESOURCE_OPTS_AUTO_PROMOTE,
	DRBD2_A_RESOURCE_OPTS_NODE_ID,
	DRBD2_A_RESOURCE_OPTS_PEER_ACK_WINDOW,
	DRBD2_A_RESOURCE_OPTS_TWOPC_TIMEOUT,
	DRBD2_A_RESOURCE_OPTS_TWOPC_RETRY_TIMEOUT,
	DRBD2_A_RESOURCE_OPTS_PEER_ACK_DELAY,
	DRBD2_A_RESOURCE_OPTS_AUTO_PROMOTE_TIMEOUT,
	DRBD2_A_RESOURCE_OPTS_MAX_IO_DEPTH,
	DRBD2_A_RESOURCE_OPTS_QUORUM,
	DRBD2_A_RESOURCE_OPTS_ON_NO_QUORUM,
	DRBD2_A_RESOURCE_OPTS_QUORUM_MIN_REDUNDANCY,
	DRBD2_A_RESOURCE_OPTS_ON_SUSPENDED_PRIMARY_OUTDATED,
	DRBD2_A_RESOURCE_OPTS_DRBD8_COMPAT_MODE,
	DRBD2_A_RESOURCE_OPTS_EXPLICIT_DRBD8_COMPAT,

	__DRBD2_A_RESOURCE_OPTS_MAX,
	DRBD2_A_RESOURCE_OPTS_MAX = (__DRBD2_A_RESOURCE_OPTS_MAX - 1)
};

enum {
	DRBD2_A_NET_CONF_SHARED_SECRET = 1,
	DRBD2_A_NET_CONF_CRAM_HMAC_ALG,
	DRBD2_A_NET_CONF_INTEGRITY_ALG,
	DRBD2_A_NET_CONF_VERIFY_ALG,
	DRBD2_A_NET_CONF_CSUMS_ALG,
	DRBD2_A_NET_CONF_PROTOCOL,
	DRBD2_A_NET_CONF_CONNECT_INT,
	DRBD2_A_NET_CONF_TIMEOUT,
	DRBD2_A_NET_CONF_PING_INT,
	DRBD2_A_NET_CONF_PING_TIMEO,
	DRBD2_A_NET_CONF_SNDBUF_SIZE,
	DRBD2_A_NET_CONF_RCVBUF_SIZE,
	DRBD2_A_NET_CONF_KO_COUNT,
	DRBD2_A_NET_CONF_MAX_EPOCH_SIZE,
	DRBD2_A_NET_CONF_AFTER_SB_0PRI,
	DRBD2_A_NET_CONF_AFTER_SB_1PRI,
	DRBD2_A_NET_CONF_AFTER_SB_2PRI,
	DRBD2_A_NET_CONF_RR_CONFLICT,
	DRBD2_A_NET_CONF_ON_CONGESTION,
	DRBD2_A_NET_CONF_CONG_FILL,
	DRBD2_A_NET_CONF_CONG_EXTENTS,
	DRBD2_A_NET_CONF_TWO_PRIMARIES,
	DRBD2_A_NET_CONF_TCP_CORK,
	DRBD2_A_NET_CONF_ALWAYS_ASBP,
	DRBD2_A_NET_CONF_USE_RLE,
	DRBD2_A_NET_CONF_FENCING,
	DRBD2_A_NET_CONF_CONNECTION_NAME,
	DRBD2_A_NET_CONF_CSUMS_AFTER_CRASH_ONLY,
	DRBD2_A_NET_CONF_SOCK_CHECK_TIMEO,
	DRBD2_A_NET_CONF_TRANSPORT_NAME,
	DRBD2_A_NET_CONF_MAX_BUFFERS,
	DRBD2_A_NET_CONF_ALLOW_REMOTE_READ,
	DRBD2_A_NET_CONF_TLS,
	DRBD2_A_NET_CONF_TLS_PRIVKEY,
	DRBD2_A_NET_CONF_TLS_CERTIFICATE,
	DRBD2_A_NET_CONF_TLS_KEYRING,
	DRBD2_A_NET_CONF_LOAD_BALANCE_PATHS,
	DRBD2_A_NET_CONF_RDMA_CTRL_RCVBUF_SIZE,
	DRBD2_A_NET_CONF_RDMA_CTRL_SNDBUF_SIZE,

	__DRBD2_A_NET_CONF_MAX,
	DRBD2_A_NET_CONF_MAX = (__DRBD2_A_NET_CONF_MAX - 1)
};

enum {
	DRBD2_A_SET_ROLE_PARMS_FORCE = 1,

	__DRBD2_A_SET_ROLE_PARMS_MAX,
	DRBD2_A_SET_ROLE_PARMS_MAX = (__DRBD2_A_SET_ROLE_PARMS_MAX - 1)
};

enum {
	DRBD2_A_RESIZE_PARMS_SIZE = 1,
	DRBD2_A_RESIZE_PARMS_ASSUME_PEER_HAS_SPACE,
	DRBD2_A_RESIZE_PARMS_ASSUME_CLEAN,
	DRBD2_A_RESIZE_PARMS_AL_STRIPES,
	DRBD2_A_RESIZE_PARMS_AL_STRIPE_SIZE,

	__DRBD2_A_RESIZE_PARMS_MAX,
	DRBD2_A_RESIZE_PARMS_MAX = (__DRBD2_A_RESIZE_PARMS_MAX - 1)
};

enum {
	DRBD2_A_START_OV_PARMS_START_SECTOR = 1,
	DRBD2_A_START_OV_PARMS_STOP_SECTOR,

	__DRBD2_A_START_OV_PARMS_MAX,
	DRBD2_A_START_OV_PARMS_MAX = (__DRBD2_A_START_OV_PARMS_MAX - 1)
};

enum {
	DRBD2_A_NEW_CURRENT_UUID_PARMS_CLEAR_BM = 1,
	DRBD2_A_NEW_CURRENT_UUID_PARMS_FORCE_RESYNC,

	__DRBD2_A_NEW_CURRENT_UUID_PARMS_MAX,
	DRBD2_A_NEW_CURRENT_UUID_PARMS_MAX = (__DRBD2_A_NEW_CURRENT_UUID_PARMS_MAX - 1)
};

enum {
	DRBD2_A_DISCONNECT_PARMS_FORCE = 1,

	__DRBD2_A_DISCONNECT_PARMS_MAX,
	DRBD2_A_DISCONNECT_PARMS_MAX = (__DRBD2_A_DISCONNECT_PARMS_MAX - 1)
};

enum {
	DRBD2_A_DETACH_PARMS_FORCE = 1,
	DRBD2_A_DETACH_PARMS_INTENTIONAL_DISKLESS_DETACH,

	__DRBD2_A_DETACH_PARMS_MAX,
	DRBD2_A_DETACH_PARMS_MAX = (__DRBD2_A_DETACH_PARMS_MAX - 1)
};

enum {
	DRBD2_A_DEVICE_CONF_MAX_BIO_SIZE = 1,
	DRBD2_A_DEVICE_CONF_INTENTIONAL_DISKLESS,
	DRBD2_A_DEVICE_CONF_BLOCK_SIZE,
	DRBD2_A_DEVICE_CONF_DISCARD_GRANULARITY,

	__DRBD2_A_DEVICE_CONF_MAX,
	DRBD2_A_DEVICE_CONF_MAX = (__DRBD2_A_DEVICE_CONF_MAX - 1)
};

enum {
	DRBD2_A_RESOURCE_INFO_ROLE = 1,
	DRBD2_A_RESOURCE_INFO_SUSP,
	DRBD2_A_RESOURCE_INFO_SUSP_NOD,
	DRBD2_A_RESOURCE_INFO_SUSP_FEN,
	DRBD2_A_RESOURCE_INFO_SUSP_QUORUM,
	DRBD2_A_RESOURCE_INFO_FAIL_IO,

	__DRBD2_A_RESOURCE_INFO_MAX,
	DRBD2_A_RESOURCE_INFO_MAX = (__DRBD2_A_RESOURCE_INFO_MAX - 1)
};

enum {
	DRBD2_A_DEVICE_INFO_DISK_STATE = 1,
	DRBD2_A_DEVICE_INFO_IS_INTENTIONAL_DISKLESS,
	DRBD2_A_DEVICE_INFO_HAS_QUORUM,
	DRBD2_A_DEVICE_INFO_IS_OPEN,
	DRBD2_A_DEVICE_INFO_BACKING_DEV_PATH,

	__DRBD2_A_DEVICE_INFO_MAX,
	DRBD2_A_DEVICE_INFO_MAX = (__DRBD2_A_DEVICE_INFO_MAX - 1)
};

enum {
	DRBD2_A_CONNECTION_INFO_CONNECTION_STATE = 1,
	DRBD2_A_CONNECTION_INFO_ROLE,

	__DRBD2_A_CONNECTION_INFO_MAX,
	DRBD2_A_CONNECTION_INFO_MAX = (__DRBD2_A_CONNECTION_INFO_MAX - 1)
};

enum {
	DRBD2_A_PEER_DEVICE_INFO_REPL_STATE = 1,
	DRBD2_A_PEER_DEVICE_INFO_DISK_STATE,
	DRBD2_A_PEER_DEVICE_INFO_RESYNC_SUSP_USER,
	DRBD2_A_PEER_DEVICE_INFO_RESYNC_SUSP_PEER,
	DRBD2_A_PEER_DEVICE_INFO_RESYNC_SUSP_DEPENDENCY,
	DRBD2_A_PEER_DEVICE_INFO_IS_INTENTIONAL_DISKLESS,
	DRBD2_A_PEER_DEVICE_INFO_RESYNC_SUSP_MAX_PARALLEL,

	__DRBD2_A_PEER_DEVICE_INFO_MAX,
	DRBD2_A_PEER_DEVICE_INFO_MAX = (__DRBD2_A_PEER_DEVICE_INFO_MAX - 1)
};

enum {
	DRBD2_A_RESOURCE_STATISTICS_WRITE_ORDERING = 1,

	__DRBD2_A_RESOURCE_STATISTICS_MAX,
	DRBD2_A_RESOURCE_STATISTICS_MAX = (__DRBD2_A_RESOURCE_STATISTICS_MAX - 1)
};

enum {
	DRBD2_A_DEVICE_STATISTICS_SIZE = 1,
	DRBD2_A_DEVICE_STATISTICS_READ,
	DRBD2_A_DEVICE_STATISTICS_WRITE,
	DRBD2_A_DEVICE_STATISTICS_AL_WRITES,
	DRBD2_A_DEVICE_STATISTICS_BM_WRITES,
	DRBD2_A_DEVICE_STATISTICS_UPPER_PENDING,
	DRBD2_A_DEVICE_STATISTICS_LOWER_PENDING,
	DRBD2_A_DEVICE_STATISTICS_UPPER_BLOCKED,
	DRBD2_A_DEVICE_STATISTICS_LOWER_BLOCKED,
	DRBD2_A_DEVICE_STATISTICS_AL_SUSPENDED,
	DRBD2_A_DEVICE_STATISTICS_EXPOSED_DATA_UUID,
	DRBD2_A_DEVICE_STATISTICS_CURRENT_UUID,
	DRBD2_A_DEVICE_STATISTICS_DISK_FLAGS,
	DRBD2_A_DEVICE_STATISTICS_HISTORY_UUIDS,

	__DRBD2_A_DEVICE_STATISTICS_MAX,
	DRBD2_A_DEVICE_STATISTICS_MAX = (__DRBD2_A_DEVICE_STATISTICS_MAX - 1)
};

enum {
	DRBD2_A_CONNECTION_STATISTICS_CONGESTED = 1,
	DRBD2_A_CONNECTION_STATISTICS_AP_IN_FLIGHT,
	DRBD2_A_CONNECTION_STATISTICS_RS_IN_FLIGHT,

	__DRBD2_A_CONNECTION_STATISTICS_MAX,
	DRBD2_A_CONNECTION_STATISTICS_MAX = (__DRBD2_A_CONNECTION_STATISTICS_MAX - 1)
};

enum {
	DRBD2_A_PEER_DEVICE_STATISTICS_RECEIVED = 1,
	DRBD2_A_PEER_DEVICE_STATISTICS_SENT,
	DRBD2_A_PEER_DEVICE_STATISTICS_PENDING,
	DRBD2_A_PEER_DEVICE_STATISTICS_UNACKED,
	DRBD2_A_PEER_DEVICE_STATISTICS_OUT_OF_SYNC,
	DRBD2_A_PEER_DEVICE_STATISTICS_RESYNC_FAILED,
	DRBD2_A_PEER_DEVICE_STATISTICS_BITMAP_UUID,
	DRBD2_A_PEER_DEVICE_STATISTICS_FLAGS,
	DRBD2_A_PEER_DEVICE_STATISTICS_RS_TOTAL,
	DRBD2_A_PEER_DEVICE_STATISTICS_OV_START_SECTOR,
	DRBD2_A_PEER_DEVICE_STATISTICS_OV_STOP_SECTOR,
	DRBD2_A_PEER_DEVICE_STATISTICS_OV_POSITION,
	DRBD2_A_PEER_DEVICE_STATISTICS_OV_LEFT,
	DRBD2_A_PEER_DEVICE_STATISTICS_OV_SKIPPED,
	DRBD2_A_PEER_DEVICE_STATISTICS_RS_SAME_CSUM,
	DRBD2_A_PEER_DEVICE_STATISTICS_RS_DT_START_MS,
	DRBD2_A_PEER_DEVICE_STATISTICS_RS_PAUSED_MS,
	DRBD2_A_PEER_DEVICE_STATISTICS_RS_DT0_MS,
	DRBD2_A_PEER_DEVICE_STATISTICS_RS_DB0_SECTORS,
	DRBD2_A_PEER_DEVICE_STATISTICS_RS_DT1_MS,
	DRBD2_A_PEER_DEVICE_STATISTICS_RS_DB1_SECTORS,
	DRBD2_A_PEER_DEVICE_STATISTICS_RS_C_SYNC_RATE,
	DRBD2_A_PEER_DEVICE_STATISTICS_UUID_FLAGS,

	__DRBD2_A_PEER_DEVICE_STATISTICS_MAX,
	DRBD2_A_PEER_DEVICE_STATISTICS_MAX = (__DRBD2_A_PEER_DEVICE_STATISTICS_MAX - 1)
};

enum {
	DRBD2_A_HELPER_INFO_NAME = 1,
	DRBD2_A_HELPER_INFO_STATUS,
	DRBD2_A_HELPER_INFO_PHASE,

	__DRBD2_A_HELPER_INFO_MAX,
	DRBD2_A_HELPER_INFO_MAX = (__DRBD2_A_HELPER_INFO_MAX - 1)
};

enum {
	DRBD2_A_INVALIDATE_PARMS_SYNC_FROM_PEER_NODE_ID = 1,
	DRBD2_A_INVALIDATE_PARMS_RESET_BITMAP,

	__DRBD2_A_INVALIDATE_PARMS_MAX,
	DRBD2_A_INVALIDATE_PARMS_MAX = (__DRBD2_A_INVALIDATE_PARMS_MAX - 1)
};

enum {
	DRBD2_A_PEER_DEVICE_CONF_RESYNC_RATE = 1,
	DRBD2_A_PEER_DEVICE_CONF_C_PLAN_AHEAD,
	DRBD2_A_PEER_DEVICE_CONF_C_DELAY_TARGET,
	DRBD2_A_PEER_DEVICE_CONF_C_FILL_TARGET,
	DRBD2_A_PEER_DEVICE_CONF_C_MAX_RATE,
	DRBD2_A_PEER_DEVICE_CONF_C_MIN_RATE,
	DRBD2_A_PEER_DEVICE_CONF_BITMAP,
	DRBD2_A_PEER_DEVICE_CONF_RESYNC_WITHOUT_REPLICATION,
	DRBD2_A_PEER_DEVICE_CONF_PEER_TIEBREAKER,

	__DRBD2_A_PEER_DEVICE_CONF_MAX,
	DRBD2_A_PEER_DEVICE_CONF_MAX = (__DRBD2_A_PEER_DEVICE_CONF_MAX - 1)
};

enum {
	DRBD2_A_CONNECT_PARMS_TENTATIVE = 1,
	DRBD2_A_CONNECT_PARMS_DISCARD_MY_DATA,

	__DRBD2_A_CONNECT_PARMS_MAX,
	DRBD2_A_CONNECT_PARMS_MAX = (__DRBD2_A_CONNECT_PARMS_MAX - 1)
};

enum {
	DRBD2_A_PATH_INFO_ESTABLISHED = 1,

	__DRBD2_A_PATH_INFO_MAX,
	DRBD2_A_PATH_INFO_MAX = (__DRBD2_A_PATH_INFO_MAX - 1)
};

enum {
	DRBD2_A_RENAME_PARMS_NEW_NAME = 1,

	__DRBD2_A_RENAME_PARMS_MAX,
	DRBD2_A_RENAME_PARMS_MAX = (__DRBD2_A_RENAME_PARMS_MAX - 1)
};

enum {
	DRBD2_A_INVALIDATE_PEER_PARMS_RESET_BITMAP = 1,

	__DRBD2_A_INVALIDATE_PEER_PARMS_MAX,
	DRBD2_A_INVALIDATE_PEER_PARMS_MAX = (__DRBD2_A_INVALIDATE_PEER_PARMS_MAX - 1)
};

enum {
	DRBD2_A_SUSPEND_IO_PARMS_BDEV_FREEZE = 1,
	DRBD2_A_SUSPEND_IO_PARMS_FLUSH_TO_PEERS,

	__DRBD2_A_SUSPEND_IO_PARMS_MAX,
	DRBD2_A_SUSPEND_IO_PARMS_MAX = (__DRBD2_A_SUSPEND_IO_PARMS_MAX - 1)
};

enum {
	DRBD2_A_CBT_START_PARMS_BOOKMARK_ID = 1,

	__DRBD2_A_CBT_START_PARMS_MAX,
	DRBD2_A_CBT_START_PARMS_MAX = (__DRBD2_A_CBT_START_PARMS_MAX - 1)
};

enum {
	DRBD2_A_CBT_INFO_ACTIVE = 1,
	DRBD2_A_CBT_INFO_VALID,
	DRBD2_A_CBT_INFO_BOOKMARK_ID,
	DRBD2_A_CBT_INFO_SINCE,
	DRBD2_A_CBT_INFO_BITMAP_UUID,
	DRBD2_A_CBT_INFO_BITMAP_DAGTAG,

	__DRBD2_A_CBT_INFO_MAX,
	DRBD2_A_CBT_INFO_MAX = (__DRBD2_A_CBT_INFO_MAX - 1)
};

enum {
	DRBD2_A_RESOURCE_CONTEXT = 1,
	DRBD2_A_RESOURCE_INFO,
	DRBD2_A_RESOURCE_STATISTICS,
	DRBD2_A_RESOURCE_NEW_NAME,
	DRBD2_A_RESOURCE_RESOURCE_OPTS,

	__DRBD2_A_RESOURCE_MAX,
	DRBD2_A_RESOURCE_MAX = (__DRBD2_A_RESOURCE_MAX - 1)
};

enum {
	DRBD2_A_DEVICE_CONTEXT = 1,
	DRBD2_A_DEVICE_INFO,
	DRBD2_A_DEVICE_STATISTICS,
	DRBD2_A_DEVICE_DISK_CONF,
	DRBD2_A_DEVICE_DEVICE_CONF,

	__DRBD2_A_DEVICE_MAX,
	DRBD2_A_DEVICE_MAX = (__DRBD2_A_DEVICE_MAX - 1)
};

enum {
	DRBD2_A_CONNECTION_CONTEXT = 1,
	DRBD2_A_CONNECTION_INFO,
	DRBD2_A_CONNECTION_STATISTICS,
	DRBD2_A_CONNECTION_NET_CONF,
	DRBD2_A_CONNECTION_PATH,

	__DRBD2_A_CONNECTION_MAX,
	DRBD2_A_CONNECTION_MAX = (__DRBD2_A_CONNECTION_MAX - 1)
};

enum {
	DRBD2_A_PEER_DEVICE_CONTEXT = 1,
	DRBD2_A_PEER_DEVICE_INFO,
	DRBD2_A_PEER_DEVICE_STATISTICS,
	DRBD2_A_PEER_DEVICE_PEER_DEVICE_CONF,

	__DRBD2_A_PEER_DEVICE_MAX,
	DRBD2_A_PEER_DEVICE_MAX = (__DRBD2_A_PEER_DEVICE_MAX - 1)
};

enum {
	DRBD2_A_PATH_CONTEXT = 1,
	DRBD2_A_PATH_INFO,

	__DRBD2_A_PATH_MAX,
	DRBD2_A_PATH_MAX = (__DRBD2_A_PATH_MAX - 1)
};

enum {
	DRBD2_A_CONTEXT = 1,
	DRBD2_A_SET_DEFAULTS,
	DRBD2_A_STATE_RESULT,
	DRBD2_A_MESSAGE,
	DRBD2_A_TIMEOUT_TYPE,
	DRBD2_A_DISK_CONF,
	DRBD2_A_RESOURCE_OPTS,
	DRBD2_A_NET_CONF,
	DRBD2_A_SET_ROLE_PARMS,
	DRBD2_A_RESIZE_PARMS,
	DRBD2_A_START_OV_PARMS,
	DRBD2_A_NEW_CURRENT_UUID_PARMS,
	DRBD2_A_DISCONNECT_PARMS,
	DRBD2_A_DETACH_PARMS,
	DRBD2_A_DEVICE_CONF,
	DRBD2_A_HELPER,
	DRBD2_A_INVALIDATE_PARMS,
	DRBD2_A_PEER_DEVICE_CONF,
	DRBD2_A_CONNECT_PARMS,
	DRBD2_A_RENAME_PARMS,
	DRBD2_A_INVALIDATE_PEER_PARMS,
	DRBD2_A_SUSPEND_IO_PARMS,
	DRBD2_A_RESOURCE,
	DRBD2_A_DEVICE,
	DRBD2_A_CONNECTION,
	DRBD2_A_PEER_DEVICE,
	DRBD2_A_PATH,
	DRBD2_A_CBT_START_PARMS,
	DRBD2_A_CBT_INFO,

	__DRBD2_A_MAX,
	DRBD2_A_MAX = (__DRBD2_A_MAX - 1)
};

enum {
	DRBD2_A_STATE_CHANGE_ACTION = 1,
	DRBD2_A_STATE_CHANGE_MORE,
	DRBD2_A_STATE_CHANGE_RESOURCE,
	DRBD2_A_STATE_CHANGE_DEVICE,
	DRBD2_A_STATE_CHANGE_CONNECTION,
	DRBD2_A_STATE_CHANGE_PEER_DEVICE,
	DRBD2_A_STATE_CHANGE_PATH,

	__DRBD2_A_STATE_CHANGE_MAX,
	DRBD2_A_STATE_CHANGE_MAX = (__DRBD2_A_STATE_CHANGE_MAX - 1)
};

enum {
	DRBD2_CMD_RESOURCE_NEW = 1,
	DRBD2_CMD_RESOURCE_DEL,
	DRBD2_CMD_RESOURCE_SET,
	DRBD2_CMD_RESOURCE_RENAME,
	DRBD2_CMD_RESOURCE_DOWN,
	DRBD2_CMD_RESOURCE_PRIMARY,
	DRBD2_CMD_RESOURCE_SECONDARY,
	DRBD2_CMD_RESOURCE_SUSPEND_IO,
	DRBD2_CMD_RESOURCE_RESUME_IO,
	DRBD2_CMD_DEVICE_NEW,
	DRBD2_CMD_DEVICE_DEL,
	DRBD2_CMD_DEVICE_ATTACH,
	DRBD2_CMD_DEVICE_DETACH,
	DRBD2_CMD_DISK_SET,
	DRBD2_CMD_DEVICE_RESIZE,
	DRBD2_CMD_DEVICE_OUTDATE,
	DRBD2_CMD_DEVICE_INVALIDATE,
	DRBD2_CMD_DEVICE_NEW_CURRENT_UUID,
	DRBD2_CMD_CONNECTION_NEW,
	DRBD2_CMD_CONNECTION_DEL,
	DRBD2_CMD_CONNECTION_CONNECT,
	DRBD2_CMD_CONNECTION_DISCONNECT,
	DRBD2_CMD_CONNECTION_SET,
	DRBD2_CMD_CONNECTION_FORGET,
	DRBD2_CMD_PATH_NEW,
	DRBD2_CMD_PATH_DEL,
	DRBD2_CMD_PEER_DEVICE_SET,
	DRBD2_CMD_PEER_DEVICE_INVALIDATE,
	DRBD2_CMD_PEER_DEVICE_PAUSE_SYNC,
	DRBD2_CMD_PEER_DEVICE_RESUME_SYNC,
	DRBD2_CMD_PEER_DEVICE_START_OV,
	DRBD2_CMD_TIMEOUT_TYPE_GET,
	DRBD2_CMD_RESOURCE_GET,
	DRBD2_CMD_DEVICE_GET,
	DRBD2_CMD_CONNECTION_GET,
	DRBD2_CMD_PEER_DEVICE_GET,
	DRBD2_CMD_PATH_GET,
	DRBD2_CMD_STATE_GET,
	DRBD2_CMD_STATE_CHANGE_NTF,
	DRBD2_CMD_HELPER_NTF,
	DRBD2_CMD_DEVICE_CBT_FLUSH,
	DRBD2_CMD_DEVICE_CBT_START,
	DRBD2_CMD_DEVICE_CBT_STOP,
	DRBD2_CMD_DEVICE_CBT_STATUS,

	__DRBD2_CMD_MAX,
	DRBD2_CMD_MAX = (__DRBD2_CMD_MAX - 1)
};

#define DRBD2_MCGRP_EVENTS	"events"

#endif /* _UAPI_LINUX_DRBD2_H */
