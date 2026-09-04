.. SPDX-License-Identifier: GPL-2.0

===========
FUSE Caches
===========

Introduction
============

This document summarises the different types of caches used in FUSE. For each
cache type, it documents the rules to insert data into it. It also documents the
rules for validating and invalidating data in the cache.

symlink caching
===============

Whenever there's a link resolution request for a FUSE filesystem, the VFS will
call into ``fuse_get_link()``, the ``->get_link()`` inode operation. This
function will then send a ``FUSE_READLINK`` request to the user-space FUSE
server.

The server can ask the kernel to cache all link resolutions by setting the
``FUSE_CACHE_SYMLINKS`` flag during the ``FUSE_INIT`` negotiation. If this flag
is set, when the VFS calls into the ``->get_link()`` operation, FUSE will
immediately call ``__page_get_link()``. The first time this is done for a
specific inode, it will result in sending the ``FUSE_READLINK`` request to
user-space. But the result returned from this request will then be added into
the page-cache. The next time this link needs to be resolved, it will use the
link resolution already cached, and will only fallback to user-space if the
folio isn't up-to-date.

Attributes caching
==================

Inode attributes may be obtained from user-space by different FUSE operations.
For example, ``FUSE_LOOKUP``, ``FUSE_GETATTR``, and also several other
operations that create file system objects (e.g. ``FUSE_MKDIR``). These
attributes obtained from user-space are cached by the kernel. They have,
however, a timeout associated and once it expires, they are invalidated. The
next time the attributes are needed, a request (``FUSE_GETATTR``) will be sent
to the FUSE server.

The ``FUSE_GETATTR`` request can be sent to user-space in three different
scenarios:

#. if the attributes for the inode aren't yet available in the kernel;
#. if they are not valid any more (timed-out, or have been invalidated), or
#. if there is an explicit request for forcing the request to be sent (for
   example, by using the ``AT_STATX_FORCE_SYNC`` flag in ``statx``).

Regarding the attributes invalidation, they may happen in several occasions:

- Upon user-space request for invalidation:

  - A ``FUSE_NOTIFY_INVAL_INODE`` will invalidate ``STATX_BASIC_STATS``;
  - ``FUSE_NOTIFY_DELETE`` and ``FUSE_NOTIFY_INVAL_ENTRY`` invalidate
    ``FUSE_STATX_MODDIR``.

- When setting (or removing) an ACL on an inode ``STATX_CTIME`` is invalidated;
  if the ``FUSE_POSIX_ACL`` flag was set by the FUSE server,
  ``STATX_BASIC_STATS`` will also be invalidated.
- On a ``->rename()`` operation, both the old and the new entities will have
  it's ctime invalidated (``STATX_CTIME``). Also, the directories for both the
  old and the new entities will also have their attributes invalidated
  (``FUSE_STATX_MODDIR``)
- When creating or deleting a new file system object (``->link()/->unlink()``,
  ``->symlink()``, ``->mkdir()/->rmdir()``, ``->tmpfile()``, or
  ``->atomic_open()``), the directory where the object is created/deleted will
  have it's attributes invalidated (``FUSE_STATX_MODDIR``).
- If a ``->link()`` operation is interrupted by a signal (``EINTR``) the inode
  being linked will have it's attributes invalidated (``STATX_BASIC_STATS``).
- When doing a readdir (``->iterate_shared()`` operation) and the directory
  contents is not cached, ``STATX_ATIME`` attributes will be invalidated.
- When doing a symlink resolution (by sending a ``FUSE_READLINK`` request)
  ``STATX_ATIME`` will be invalidated.
- When doing a ``->flush()`` (i.e. sending a ``FUSE_FLUSH`` request) and
  writeback cache is enabled, ``STATX_BLOCKS`` will be invalidated.
- When truncating a file on open using ``O_TRUNC`` open flag (and the FUSE
  server has set ``FUSE_ATOMIC_O_TRUNC`` during ``FUSE_INIT``), then
  ``FUSE_STATX_MODSIZE`` will be invalidated.
- When setting attributes in an inode (``->setattr()``) and there's a signal
  that interrupts the operation (``EINTR``), then ``STATX_BASIC_STATS`` will be
  invalidated.
- When data is read from a file, ``STATX_ATIME`` will be invalidated (unless the
  file system is read-only).
- When data is written info a file, ``FUSE_STATX_MODSIZE`` is invalidated.

ACL caching
===========

FUSE has allowed the usage of POSIX Access Control Lists (ACLs) for a long time,
as they can be set and accessed simply as extended attributes. However, it was
only with the introduction of the ``FUSE_POSIX_ACL`` flag that ACLs started to
be fully supported. Without this flag being set during the ``FUSE_INIT``
negotiation, ACLs can still be set, but the VFS won't use them for performing
permission checks - that would be the user-space server's responsibility.

Also, without setting ``FUSE_POSIX_ACL``, ACLs will not be cached by the kernel.
In this case, new inodes ``i_acl`` and ``i_default_acl`` fields will be set to
``ACL_DONT_CACHE``.

On the other hand, if the ``FUSE_POSIX_ACL`` flag is set then, when an inode ACL
is accessed, VFS will first check if it's already cached. If it is not, FUSE
``->get_acl()`` operation (``fuse_get_acl()``) is called, which will eventually
send a user-space request. Future accesses to this inode ACL will use the cached
data.

Setting an ACL in an inode will also result in sending a request to the FUSE
server for setting it. But this operation won't immediately cache the ACL -- it
will only be cached after it is accessed again and requested from user-space.

On the other hand, ACLs will be removed from the cache in the following
situations:

- When setting an ACL in an inode (and the ``FUSE_POSIX_ACL`` flag is set),
  previously cached ACLs for this inode will be invalidated.
- When invalidating an inode through the ``FUSE_NOTIFY_INVAL_INODE`` operation.
- When ``->d_revalidate()`` is called for a dentry that requires a lookup (e.g.
  it has expired) and that lookup operation is successful.
- When the VFS needs to check access rights for an inode (by calling
  ``->permission()``), attributes may need to be refreshed. If that happens, any
  cached ACLs for that inode will be invalidated.
- After setting an inode attribute (i.e. operation ``FUSE_SETATTR`` is sent to
  user-space), the user-space server may have also updated the ACLs. Thus, any
  cached ACLs for this inode are also invalidated.
- While processing ``FUSE_READDIRPLUS`` and an already existing dentry needs to
  be updated.
- In general, when there is the need to send a ``FUSE_STATX`` or
  ``FUSE_GETATTR`` to user-space (e.g. when attributes expired).

readdir caching
===============

When opening a directory a ``FUSE_OPENDIR`` will be sent to the FUSE server, and
server will be responsible for setting the open flags related with caching,
namely ``FOPEN_KEEP_CACHE`` and ``FOPEN_CACHE_DIR``.

If neither flags are set by the user-space FUSE server, then every ``readdir``
will result in a ``FUSE_READDIR`` (or ``FUSE_READDIRPLUS``) request being sent.
If ``FOPEN_CACHE_DIR`` is set by the server, then the result of a ``readdir``
will be cached by the kernel and reused for the current open. If
``FOPEN_KEEP_CACHE`` is also set, the cache will be kept and reused in the
future, when the directory is open again for reading.

The readdir cache will also expire and reset if the inode's ``mtime`` or
``iversion`` don't match the cached values, or if the FUSE connection ``epoch``
doesn't match the cache ``epoch``.

dentry caching
==============

TBD

data caching
============

TBD

