.. SPDX-License-Identifier: GPL-2.0

Per-inode open suppression
=========================

A filesystem can avoid OPEN and RELEASE requests for individual inodes by
negotiating FUSE_PER_INODE_NO_OPEN in INIT and setting FUSE_ATTR_NO_OPEN in
``fuse_attr.flags``.  For directories, the flag suppresses OPENDIR and
RELEASEDIR instead.  This allows, for example, cached content files to avoid
open round trips while control files on the same connection retain their
open handlers.  Without the negotiated capability the attribute is ignored.

The kernel updates the hint when it accepts attributes in replies such as
LOOKUP, GETATTR, SETATTR and READDIRPLUS.  STATX replies do not carry
``fuse_attr.flags`` and leave the hint unchanged.  The hint is cached inode
state; it is not independently revalidated on every open.  A server changing
the hint must arrange for fresh attributes to reach the kernel and tolerate
concurrent opens using the previous value.

For an open served locally, the file handle is zero, FOPEN_KEEP_CACHE is
set, and directories also have FOPEN_CACHE_DIR set.  Subsequent requests
identify the object by the node ID and may carry a zero file handle.  The
server must support these requests without per-open state.  Whether OPEN was
sent is recorded for each handle and is not changed by later attribute
updates.  An existing server-opened handle still receives its matching
RELEASE if the inode hint subsequently becomes set.

Remote flock locking is an exception to RELEASE suppression.  When
FUSE_FLOCK_LOCKS is negotiated and a handle has successfully performed a
server-side flock operation, its final close sends RELEASE with
FUSE_RELEASE_FLOCK_UNLOCK and the lock owner, even if OPEN was suppressed.
The server must accept this RELEASE with a zero file handle and no preceding
OPEN, and use the node ID and lock owner to remove any remaining flock locks.
This also applies after an explicit unlock, since the kernel records whether
a flock operation succeeded rather than tracking the server's current locks.

When FUSE_ATOMIC_O_TRUNC is negotiated, an open with O_TRUNC still sends
OPEN and receives a matching RELEASE, so the server can perform truncation.
CREATE also retains its usual open and release semantics.  Connection-wide
no-open behavior selected by an ENOSYS response continues to take precedence.

The hint does not make an inode immutable, grant permissions, or suppress
other operations such as FLUSH, FSYNC, locking or data I/O.  Servers supporting
remote flock must implement the RELEASE cleanup described above.  A server must
only set it when its access policy and file semantics permit the default
open behavior described above.  Servers needing per-open authorization,
nonzero handles, direct I/O, passthrough or other OPEN reply flags must keep
handling OPEN for those inodes.  No additional access-time or open-reference
accounting is performed by this feature.
