// SPDX-License-Identifier: GPL-2.0-only
/// Find a cdev, miscdevice, video_device or v4l2_device in devm memory,
/// which a file still open after unbind can use.
///
// Confidence: Moderate
// Comments: Drivers that cannot be unbound while a file is open, and nodes
// without file operations, are false positives.

virtual context
virtual org
virtual report

@initialize:python@
@@
seen = set()

def warn(print_fn, p, msg):
    key = (p.file, p.line, p.column)
    if key not in seen:
        seen.add(key)
        print_fn(p, f"WARNING: {msg}")

@alloc exists@
identifier x;
type T;
expression e, e2;
identifier fn =~ "^devm_k[mzcr]";
position p, q;
@@

(
x = fn@p(...);
|
T *x = fn@p(...);
)
... when != x = e
(
misc_register@q(<+... x ...+>)
|
video_register_device@q(<+... x ...+>, ...)
|
v4l2_device_register@q(e2, <+... x ...+>)
|
cdev_device_add@q(<+... x ...+>, ...)
|
cdev_add@q(<+... x ...+>, ...)
|
__video_register_device@q(<+... x ...+>, ...)
|
video_register_device_no_warn@q(<+... x ...+>, ...)
)

@embeds@
identifier S, f;
type T = {struct cdev, struct miscdevice, struct video_device,
          struct v4l2_device};
@@

struct S {
  ...
  T f;
  ...
};

@bytype@
identifier embeds.S;
struct S *x;
identifier fn =~ "^devm_k[mzcr]";
position p;
@@

x = fn@p(...)

@depends on context@
identifier alloc.fn;
position alloc.p;
@@

* fn@p(...)

@depends on context@
identifier bytype.fn;
position bytype.p;
@@

* fn@p(...)

@script:python depends on org@
p << alloc.p;
q << alloc.q;
@@
warn(coccilib.org.print_todo, p[0],
     f"devm memory registered at line {q[0].line} as a device node or a v4l2_device")

@script:python depends on report@
p << alloc.p;
q << alloc.q;
@@
warn(coccilib.report.print_report, p[0],
     f"devm memory registered at line {q[0].line} as a device node or a v4l2_device")

@script:python depends on org@
p << bytype.p;
S << embeds.S;
@@
warn(coccilib.org.print_todo, p[0],
     f"devm allocation of struct {S}, which embeds a device node or a v4l2_device")

@script:python depends on report@
p << bytype.p;
S << embeds.S;
@@
warn(coccilib.report.print_report, p[0],
     f"devm allocation of struct {S}, which embeds a device node or a v4l2_device")
