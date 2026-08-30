.. SPDX-License-Identifier: GPL-2.0

============================
Configurable Error Injection
============================

Overview
--------

Configurable error injection allows injecting specific block layer status codes
for sector ranges of a block device.  Errors can be injected unconditionally, or
with a given probability.  Instead of, or before, failing a bio it can also be
held back for a while to model a slow device.

To use configurable error injection, CONFIG_BLK_ERROR_INJECTION must be enabled.

The only interface is the error_injection debugfs file, which is created for
each registered gendisk.  Writes to this file are used to create or delete rules
and reads return a list of the current error injection sites.

Options
-------

The following options specify the operations:

===================	=======================================================
add			add a new rule
removeall		remove all existing rules
===================	=======================================================

The following options specify the details of the rule for the add operation:

===================	=======================================================
op=<string>		block layer operation this rule applies to.  This uses
			the XYZ for each REQ_OP_XYZ operation, e.g. READ, WRITE
			or DISCARD. Mandatory.
status=<string>		Status to return.  This uses XYZ for each BLK_STS_XYZ
			code, e.g. IOERR or MEDIUM. Mandatory unless delay_us
			is given.
start=<number>		First block layer sector the rule applies to.
			Optional, defaults to 0.
nr_sectors=<number>	Number of sectors this rule applies.
			Optional, defaults to the remainder of the device.
chance=<number>		Only return a failure with a likelihood of 1/chance.
			Optional, defaults to 1 (always).
delay_us=<number>	Hold the bio back for this many microseconds.  Without
			status the bio is then submitted to the device as
			usual, with status it is failed once the delay has
			expired.  Optional, defaults to 0 (no delay).
			Values above 600 seconds are rejected.
===================	=======================================================

Delays
------

A delayed bio is held before it is submitted, so the device itself never sees a
slow I/O: the delay is not visible to the driver, to the I/O statistics, or to
anything else below submission such as writeback throttling.  Throttling by
blk-throttle happens before a bio can be delayed, so it is not affected either.
What it does exercise is everything waiting above the block layer, for instance
io_uring cancellation, hung task detection, and filesystem or userspace
timeouts.  Because the low level driver is not involved, a delay does not reach
the blk-mq timeout handler or SCSI error handling.

Once a bio has been delayed no rule is evaluated for it a second time, not when
its delay expires and it is submitted below the injection hook, and not when
the block layer splits it and resubmits the remainder above the hook.  A bio
that matched a delay rule therefore never gets an error from another rule, even
one covering the same sectors, and is held for the delay once rather than once
per split.  Put the delay and the status in a single rule to fail a bio after
holding it back.

A delayed bio is issued after bios submitted while it was held, which reorders
the I/O stream.  On zoned devices this breaks sequential write ordering: zone
write plugging happens below the injection hook, so the writes issued while a
write is held reach the zone out of order and are failed as misaligned.  Only
delay reads there.

Bios that must not block are never delayed.  A bio with REQ_NOWAIT set is
submitted, or failed with the rule's status, immediately.

The delay is a lower bound for anything longer than a timer tick, and the timer
wheel adds further slack as the delay grows.  Values shorter than a tick are of
little use: they expire on the next tick, which is anywhere between now and one
tick away.

Removing rules does not release bios that are already being delayed by them;
those run out on their own.  A delayed bio whose disk is removed in the meantime
is not submitted until its delay expires, by which point the queue no longer
accepts I/O, so it fails with EIO.

Example
-------

Return BLK_STS_IOERR for one in 10 reads of sector 0 of /dev/nvme0n1:

	$ echo 'add,op=READ,start=0,status=IOERR,chance=10' > /sys/kernel/debug/block/nvme0n1/error_injection

Return BLK_STS_MEDIUM for every write to /dev/nvme0n1:

	$ echo 'add,op=WRITE,start=0,status=MEDIUM' > /sys/kernel/debug/block/nvme0n1/error_injection

Delay every read of /dev/nvme0n1 by 10 milliseconds, then issue it normally:

	$ echo 'add,op=READ,delay_us=10000' > /sys/kernel/debug/block/nvme0n1/error_injection

Fail one in 100 writes with BLK_STS_TIMEOUT, but only after 30 seconds:

	$ echo 'add,op=WRITE,status=TIMEOUT,chance=100,delay_us=30000000' > /sys/kernel/debug/block/nvme0n1/error_injection

Remove all rules for /dev/nvme0n1:

	$ echo 'removeall' > /sys/kernel/debug/block/nvme0n1/error_injection
