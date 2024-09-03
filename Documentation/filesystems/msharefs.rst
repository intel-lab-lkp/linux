.. SPDX-License-Identifier: GPL-2.0

=====================================================
msharefs - a filesystem to support shared page tables
=====================================================

msharefs is a ram-based filesystem that allows multiple processes to
share page table entries for shared pages.

msharefs is typically mounted like this::

	mount -t msharefs none /sys/fs/mshare

A file created on msharefs creates a new shared region where all
processes mapping that region will map it using shared page table
entries. ioctls are used to initialize or retrieve the start address
and size of a shared region and to map objects in the shared
region. It is important to note that an msharefs file is a control
file for the shared region and does not contain the contents
of the region itself.

Here are the basic steps for using mshare::

1. Mount msharefs on /sys/fs/mshare::

	mount -t msharefs msharefs /sys/fs/mshare

2. mshare regions have alignment and size requirements. Start
   address for the region must be aligned to an address boundary and
   be a multiple of fixed size. This alignment and size requirement
   can be obtained by reading the file ``/sys/fs/mshare/mshare_info``
   which returns a number in text format. mshare regions must be
   aligned to this boundary and be a multiple of this size.

3. For the process creating an mshare region::

a. Create a file on /sys/fs/mshare, for example:

.. code-block:: c

	fd = open("/sys/fs/mshare/shareme",
			O_RDWR|O_CREAT|O_EXCL, 0600);

b. Establish the starting address and size of the region:

.. code-block:: c

	struct mshare_info minfo;

	minfo.start = TB(2);
	minfo.size = BUFFER_SIZE;
	ioctl(fd, MSHAREFS_SET_SIZE, &minfo);

c. Map some memory in the region:

.. code-block:: c

	struct mshare_create mcreate;

	mcreate.addr = TB(2);
	mcreate.size = BUFFER_SIZE;
	mcreate.offset = 0;
	mcreate.prot = PROT_READ | PROT_WRITE;
	mcreate.flags = MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED;
	mcreate.fd = -1;

	ioctl(fd, MSHAREFS_CREATE_MAPPING, &mcreate);

d. Map the mshare region into the process:

.. code-block:: c

	mmap((void *)TB(2), BUF_SIZE,
		PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

e. Write and read to mshared region normally.


4. For processes attaching an mshare region::

a. Open the file on msharefs, for example:

.. code-block:: c

	fd = open("/sys/fs/mshare/shareme", O_RDWR);

b. Get information about mshare'd region from the file:

.. code-block:: c

	struct mshare_info minfo;

	ioctl(fd, MSHAREFS_GET_SIZE, &minfo);

c. Map the mshare'd region into the process:

.. code-block:: c

	mmap(minfo.start, minfo.size,
		PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

5. To delete the mshare region:

.. code-block:: c

		unlink("/sys/fs/mshare/shareme");

