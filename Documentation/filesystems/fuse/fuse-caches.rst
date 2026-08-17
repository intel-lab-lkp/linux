.. SPDX-License-Identifier: GPL-2.0

===========
FUSE Caches
===========

Introduction
============

This document summarises the different types of caches that are used in FUSE.
For each cache type, it attempts to document the rules that are followed to
insert, validate and invalidate data into the cache.

symlink caching
===============

Whenever there's a link resolution request, the VFS will call into
``fuse_get_link()`` which will then send a ``FUSE_READLINK`` request to the
user-space FUSE server. However, the server can ask the kernel to cache all
links resolutions by setting the ``FUSE_CACHE_SYMLINKS`` flag during the
``FUSE_INIT`` negotiation.

If this flag is set, FUSE will immediately call into the VFS
``__page_get_link()`` from the ``->get_link()`` inode operation. The first time
this is done for a specific link, it will end-up sending the ``FUSE_READLINK``
to user-space but the link contents will then be added into page-cache. The next
time the link needs to be resolved, it will use the link content that is already
cached, and will only fallback into sending the request to use-space if the
folio isn't up-to-date.

Attributes caching
==================

Attributes obtained from user-space, for example when an inode is first
looked-up, are cached in the kernel. However, these attributes have a timeout
associated and once expired they are invalidated.

Thus, the ``FUSE_GETATTR`` operation will be sent to user-space only if the
attributes aren't yet available, the attributes aren't valid (timeout), or if
there is an explicit request for doing so (for example, by using the
``AT_STATX_FORCE_SYNC`` flag in ``statx``). This may happen in the following
situations:

#. An explicit request from VFS to get the attributes for an inode (through the
   ``->getattr()`` callback).
#. When an ``->llseek()`` is requested to FUSE with a type of request
   (``whence``):

   -  ``SEEK_{HOLE,DATA}`` and the user-space doesn't implement the
      ``FUSE_LSEEK`` operation (it has returned ``ENOSYS``), or
   -  ``SEEK_END``

#. When doing a buffered read past EOF or automatic page cache invalidation mode
   is enabled (``FUSE_AUTO_INVAL_DATA``).
#. When doing a buffered write with write-back cache enabled
   (``FUSE_CAP_WRITEBACK_CACHE``).

ACL caching
===========

FUSE has allowed the usage of POSIX ACLs for a long time as they could be set
and accessed simply as extended attributes. However, it was only with the
addition of the ``FUSE_POSIX_ACL`` flag that ACLs started to be fully supported.
Without this flag, ACLs can still be set, but the VFS won't use them for
performing permission checks - that would be the user-space server's
responsibility.

Also, without setting ``FUSE_POSIX_ACL``, ACLs will not be cached by the kernel.
In this case, new inodes ``i_acl`` and ``i_default_acl`` fields will be set to
``ACL_DONT_CACHE``.

On the other hand, if ``FUSE_POSIX_ACL`` is set during ``FUSE_INIT``, when an
ACL is accessed the VFS layer will first check if it's already cached. If it is
not, FUSE ``->get_acl`` operation is called, which will eventually send a
user-space request. Future accesses to this inode ACL will then use the cached
data.

Setting an ACL in an inode, however, won't cache it immediately. It will send
user-space a request with the new ACL, and the FUSE server may perform some
modifications before storing it.

On the other hand, ACLs will be removed for the cache in the following
situations:

-  When setting an ACL in an inode and the user-space server has set the
   ``FUSE_POSIX_ACL`` flag, all previously cached ACLs for this inode will be
   invalidated.
-  When invalidating an inode through the ``FUSE_NOTIFY_INVAL_INODE`` operation.
-  When ``->d_revalidate()`` is called for a dentry that requires a lookup (e.g.
   it has expired) and that lookup operation is successful.
-  When the VFS needs to check access rights for an inode (by calling
   ``->permission()``), attributes may need to be refreshed. If that happens,
   any cached ACLs for that inode will be invalidated.
-  After setting an inode attribute (i.e. operation ``FUSE_SETATTR`` is sent to
   user-space), the user-space server may have also updated the ACLs, so any
   cached ACLs for this inode are also invalidated.
-  While processing ``FUSE_READDIRPLUS`` and a new dentry is added (unless this
   dentry is already being looked up (``DCACHE_PAR_LOOKUP``))
-  In general, when there is the need to sent a ``FUSE_STATX`` or
   ``FUSE_GETATTR`` to user-space (e.g. because the attributes have expired).
   This may happen in the following cases:

   -  When doing an ``->llseek()`` on a file with ``SEEK_END``, ``SEEK_HOLE`` or
      ``SEEK_DATA``.
   -  When the ``FUSE_AUTO_INVAL_DATA`` flag is set at ``INIT`` time (to
      automatically invalidate cached pages), and a buffered read
      (``->read_iter()``) past EOF is done on a non-passthrough file.
   -  When the ``FUSE_WRITEBACK_CACHE`` flag is set at ``INIT`` time, and a
      buffered write (``->write_iter()``) past EOF is done on a non-passthrough
      file.
   -  When the ``FUSE_AUTO_INVAL_DATA`` flag is set at ``INIT`` time and the VFS
      needs to read a directory contents (``->iterate_shared()``) for a
      directory that is allowed to be cached.

readdir caching
===============

When opening a directory for doing a readdir, a ``FUSE_OPENDIR`` will be sent
and the user-space server will be responsible for setting the open flags related
with caching, namely ``FOPEN_KEEP_CACHE`` and ``FOPEN_CACHE_DIR``.

If neither flags are set by the user-space FUSE server, then every ``readdir``
will result in a ``FUSE_READDIR`` (or ``FUSE_READDIRPLUS``) request being sent.
If ``FOPEN_CACHE_DIR`` is set by the server, then the result of a ``readdir``
will be cached by the kernel and reused. However, if ``FOPEN_KEEP_CACHE`` isn't
also set, the cache will be invalidated next time the directory is open.

The readdir cache will also expire and resetted in the following situations if:

-  The inode ``mtime`` doesn't match the cache ``mtime``,
-  The inode ``iversion`` doesn't match the cache ``iversion``,
-  The FUSE connection ``epoch`` doesn't match the cache ``epoch``.

dentry caching
==============

TBD

data caching
============

TBD
