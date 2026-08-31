==============================
Deadline IO scheduler tunables
==============================

This little file attempts to document how the deadline io scheduler works.
In particular, it will clarify the meaning of the exposed tunables that may be
of interest to power users.

Selecting IO schedulers
-----------------------
Refer to Documentation/block/switching-sched.rst for information on
selecting an io scheduler on a per-device basis.

------------------------------------------------------------------------------

read_expire	(in ms)
-----------------------

The goal of the deadline io scheduler is to attempt to guarantee a start
service time for a request. As we focus mainly on read latencies, this is
tunable. When a read request first enters the io scheduler, it is assigned
a deadline that is the current time + the read_expire value in units of
milliseconds.


write_expire	(in ms)
-----------------------

Similar to read_expire mentioned above, but for writes.


fifo_batch	(number of requests)
------------------------------------

Requests are grouped into ``batches`` of a particular data direction (read or
write) which are serviced in increasing sector order.  To limit extra seeking,
deadline expiries are only checked between batches.  fifo_batch controls the
maximum number of requests per batch.

This parameter tunes the balance between per-request latency and aggregate
throughput.  When low latency is the primary concern, smaller is better (where
a value of 1 yields first-come first-served behaviour).  Increasing fifo_batch
generally improves throughput, at the cost of latency variation.


writes_starved	(number of dispatches)
--------------------------------------

When we have to move requests from the io scheduler queue to the block
device dispatch queue, we always give a preference to reads. However, we
don't want to starve writes indefinitely either. So writes_starved controls
how many times we give preference to reads over writes. When that has been
done writes_starved number of times, we dispatch some writes based on the
same criteria as reads.


front_merges	(bool)
----------------------

Sometimes it happens that a request enters the io scheduler that is contiguous
with a request that is already on the queue. Either it fits in the back of that
request, or it fits at the front. That is called either a back merge candidate
or a front merge candidate. Due to the way files are typically laid out,
back merges are much more common than front merges. For some work loads, you
may even know that it is a waste of time to spend any time attempting to
front merge requests. Setting front_merges to 0 disables this functionality.
Front merges may still occur due to the cached last_merge hint, but since
that comes at basically 0 cost we leave that on. We simply disable the
rbtree front sector lookup when the io scheduler merge function is called.


prio_enable	(bool)
----------------------

Whether to enable I/O priority support that distinguishes real-time (RT),
best-effort (BE) and idle requests.  When enabled (the default), requests are
filed into separate per-priority buckets and dispatched in priority order: lower
priority requests are deferred while any higher priority requests are pending,
subject to the prio_aging_expire aging mechanism described below.  When disabled,
every request is filed in the best-effort bucket, the priority aging path is
bypassed, and the scheduler dispatches from that single bucket.  This lets
systems that do not want RT/BE/IDLE distinction opt out of the extra overhead.
Switching the value drains all in-flight I/O (queue freeze and quiesce) to avoid
priority inversion during the transition.  This parameter can also be set at
module load time via the prio_enable module parameter.


prio_aging_expire	(in ms)
------------------------------

To prevent lower priority requests from being starved indefinitely by a steady
stream of higher priority requests, the deadline scheduler ages pending
requests.  prio_aging_expire is the time after which a best-effort or idle
request that has been waiting longer than this threshold may be dispatched even
though real-time requests are still pending.  The default is 10000 ms (10 s).

This parameter only takes effect when prio_enable is enabled and there are
requests queued in at least two distinct priority buckets.  The value must be
positive: zero or negative values are rejected with -EINVAL, since a value of
zero would dispatch best-effort and idle requests ahead of pending real-time
requests through "now - 0 == now", a classic priority inversion.


Nov 11 2002, Jens Axboe <jens.axboe@oracle.com>
