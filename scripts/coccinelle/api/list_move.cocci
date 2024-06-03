// SPDX-License-Identifier: GPL-2.0-only
//
// Use list_move or list_move_tail rather than list_del or list_del_init
// followed by either list_add or list_add_tail.
//
// Copyright (c) 2023, Intel Corporation
// Confidence: High
// Options: --no-includes --include-headers
//
// Keywords: list_move, list_move_tail
//

virtual context
virtual org
virtual patch
virtual report

@rmove depends on !patch@
expression E, H;
position p;
@@

(
- list_del@p(E);
|
- list_del_init@p(E);
)
- list_add(E, H);
+ list_move(E, H);

@rmove_tail depends on !patch@
expression E, H;
position p;
@@

(
- list_del@p(E);
|
- list_del_init@p(E);
)
- list_add_tail(E, H);
+ list_move_tail(E, H);

@script:python depends on report@
p << rmove.p;
@@

coccilib.report.print_report(p[0], "WARNING opportunity for list_move()")

@script:python depends on org@
p << rmove.p;
@@

coccilib.org.print_todo(p[0], "WARNING opportunity for list_move()")

@script:python depends on report@
p << rmove_tail.p;
@@

coccilib.report.print_report(p[0], "WARNING opportunity for list_move_tail()")

@script:python depends on org@
p << rmove_tail.p;
@@

coccilib.org.print_todo(p[0], "WARNING opportunity for list_move_tail()")

