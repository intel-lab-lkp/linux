.. SPDX-License-Identifier: GPL-2.0

=====================================================
Msharefs - A filesystem to support shared page tables
=====================================================

What is msharefs?
-----------------

msharefs is a pseudo filesystem that allows multiple processes to
share page table entries for shared pages. To enable support for
msharefs the kernel must be compiled with CONFIG_MSHARE set.

msharefs is typically mounted like this::

	mount -t msharefs none /sys/fs/mshare

A file created on msharefs creates a new shared region where all
processes mapping that region will map it using shared page table
entries. Once the size of the region has been established via
ftruncate() or fallocate(), the region can be mapped into processes
and ioctls used to map and unmap objects within it. Note that an
msharefs file is a control file and accessing mapped objects within
a shared region through read or write of the file is not permitted.

How to use mshare
-----------------

Here are the basic steps for using mshare:

  1. Mount msharefs on /sys/fs/mshare::

	mount -t msharefs msharefs /sys/fs/mshare

  2. mshare regions have alignment and size requirements. Start
     address for the region must be aligned to an address boundary and
     be a multiple of fixed size. This alignment and size requirement
     can be obtained by reading the file ``/sys/fs/mshare/mshare_info``
     which returns a number in text format. mshare regions must be
     aligned to this boundary and be a multiple of this size.

  3. For the process creating an mshare region:

    a. Create a file on /sys/fs/mshare, for example::

        fd = open("/sys/fs/mshare/shareme",
                        O_RDWR|O_CREAT|O_EXCL, 0600);

    b. Establish the size of the region::

        fallocate(fd, 0, 0, BUF_SIZE);

      or::

        ftruncate(fd, BUF_SIZE);

    c. Map some memory in the region::

	struct mshare_create mcreate;

	mcreate.region_offset = 0;
	mcreate.size = BUF_SIZE;
	mcreate.offset = 0;
	mcreate.prot = PROT_READ | PROT_WRITE;
	mcreate.flags = MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED;
	mcreate.fd = -1;

	ioctl(fd, MSHAREFS_CREATE_MAPPING, &mcreate);

    d. Map the mshare region into the process::

	mmap(NULL, BUF_SIZE,
		PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    e. Write and read to mshared region normally.


  4. For processes attaching an mshare region:

    a. Open the msharefs file, for example::

	fd = open("/sys/fs/mshare/shareme", O_RDWR);

    b. Get the size of the mshare region from the file::

        fstat(fd, &sb);
        mshare_size = sb.st_size;

    c. Map the mshare region into the process::

	mmap(NULL, mshare_size,
		PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

  5. To delete the mshare region::

		unlink("/sys/fs/mshare/shareme");
