// SPDX-License-Identifier: GPL-2.0-only
/// Find a struct cdev, struct miscdevice, struct video_device or struct
/// v4l2_device embedded in memory from the devm_ allocators.  The memory
/// is freed when the driver unbinds, but a file that is open on the node
/// outlives the unbind: for a cdev or a video_device the core touches the
/// object, and the v4l2_device the video_device points to, on every
/// close, and for a miscdevice the driver's file operations usually reach
/// it through file->private_data.  Allocate such state with kzalloc() and
/// free it on the last reference; see
/// Documentation/driver-api/driver-model/devres.rst.
///
/// The first rules need the allocation and a registration of it, or of an
/// address taken inside it, written directly in one function; an
/// allocation inside a condition or behind a cast or a wrapper function
/// is not seen by them. The type rule catches the rest, but with the
/// options below it sees only a struct defined in the .c file itself, not
/// in a header, and only a node that is a direct, non-array member of it,
/// not one nested in another member. So a registration in a helper
/// function, or through any alias of x such as vfd = &x->vdev, is missed
/// when the struct is in a header or the node is not such a member. Not
/// checked: a struct media_device in devm_ memory, which v4l2_release()
/// also reaches; a struct v4l2_subdev with a device node, whose close
/// reaches it; a static node whose file operations reach devm_ state
/// through a global or drvdata pointer; memory freed by devm_add_action()
/// rather than allocated by devm_k*(); a typedef'd struct; and a file or
/// function spatch cannot parse.
/// False positives: a driver that cannot be unbound (suppress_bind_attrs,
/// platform_driver_probe()) on a device that is never removed, a driver
/// whose remove() waits for the last close, a miscdevice driver whose file
/// operations do not use the allocation after open(), a registration of
/// an address that only uses the allocation to reach separate memory
/// (&x->ptr->misc, &table[x->idx]), a v4l2_device that no video_device
/// points to, and, for the type rule, a struct whose embedded node is not
/// registered from this allocation.  When an allocation has several
/// registrations, the report names the first one found.
///
// Confidence: Moderate
// Options: --include-headers-for-types

virtual org
virtual report

@initialize:python@
@@
seen = set()

@alloc depends on org || report exists@
expression x, e, e2;
identifier fn =~ "^devm_k[mzcr]";
position p, q;
@@

x = fn@p(...);
... when != x = e
(
misc_register@q(\(x\|&<+... x ...+>\))
|
cdev_add@q(\(x\|&<+... x ...+>\), ...)
|
cdev_device_add@q(\(x\|&<+... x ...+>\), ...)
|
video_register_device@q(\(x\|&<+... x ...+>\), ...)
|
video_register_device_no_warn@q(\(x\|&<+... x ...+>\), ...)
|
__video_register_device@q(\(x\|&<+... x ...+>\), ...)
|
v4l2_device_register@q(e2, \(x\|&<+... x ...+>\))
)

@allocdecl depends on org || report exists@
identifier x;
type T;
expression e, e2;
identifier fn =~ "^devm_k[mzcr]";
position p, q;
@@

T *x = fn@p(...);
... when != x = e
(
misc_register@q(\(x\|&<+... x ...+>\))
|
cdev_add@q(\(x\|&<+... x ...+>\), ...)
|
cdev_device_add@q(\(x\|&<+... x ...+>\), ...)
|
video_register_device@q(\(x\|&<+... x ...+>\), ...)
|
video_register_device_no_warn@q(\(x\|&<+... x ...+>\), ...)
|
__video_register_device@q(\(x\|&<+... x ...+>\), ...)
|
v4l2_device_register@q(e2, \(x\|&<+... x ...+>\))
)

@embeds depends on org || report@
identifier S, f;
type T = {struct cdev, struct miscdevice, struct video_device,
          struct v4l2_device};
@@

struct S {
  ...
  T f;
  ...
};

@bytype depends on org || report@
identifier embeds.S;
struct S *x;
identifier fn =~ "^devm_k[mzcr]";
position p;
@@

x = fn@p(...)

@script:python depends on org@
p << alloc.p;
q << alloc.q;
@@
if (p[0].file, p[0].line, p[0].column) not in seen:
    seen.add((p[0].file, p[0].line, p[0].column))
    coccilib.org.print_todo(p[0], "WARNING: devm_ memory registered at line %s as a device node or a v4l2_device; an open file outlives unbind" % q[0].line)

@script:python depends on report@
p << alloc.p;
q << alloc.q;
@@
if (p[0].file, p[0].line, p[0].column) not in seen:
    seen.add((p[0].file, p[0].line, p[0].column))
    coccilib.report.print_report(p[0], "WARNING: devm_ memory registered at line %s as a device node or a v4l2_device; an open file outlives unbind" % q[0].line)

@script:python depends on org@
p << allocdecl.p;
q << allocdecl.q;
@@
if (p[0].file, p[0].line, p[0].column) not in seen:
    seen.add((p[0].file, p[0].line, p[0].column))
    coccilib.org.print_todo(p[0], "WARNING: devm_ memory registered at line %s as a device node or a v4l2_device; an open file outlives unbind" % q[0].line)

@script:python depends on report@
p << allocdecl.p;
q << allocdecl.q;
@@
if (p[0].file, p[0].line, p[0].column) not in seen:
    seen.add((p[0].file, p[0].line, p[0].column))
    coccilib.report.print_report(p[0], "WARNING: devm_ memory registered at line %s as a device node or a v4l2_device; an open file outlives unbind" % q[0].line)

@script:python depends on org@
p << bytype.p;
S << embeds.S;
@@
if (p[0].file, p[0].line, p[0].column) not in seen:
    seen.add((p[0].file, p[0].line, p[0].column))
    coccilib.org.print_todo(p[0], "WARNING: devm_ allocation of struct %s, which embeds a cdev, miscdevice, video_device or v4l2_device" % S)

@script:python depends on report@
p << bytype.p;
S << embeds.S;
@@
if (p[0].file, p[0].line, p[0].column) not in seen:
    seen.add((p[0].file, p[0].line, p[0].column))
    coccilib.report.print_report(p[0], "WARNING: devm_ allocation of struct %s, which embeds a cdev, miscdevice, video_device or v4l2_device" % S)
