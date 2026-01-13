// SPDX-License-Identifier: GPL-2.0-only
/// Convert sysctl table initializations to use SYSCTL_ENTRY and SYSCTL_RANGE_ENTRY macros
///
// Confidence: Medium
// Options: --no-includes --include-headers

// Virtual rules for different modes
virtual patch
virtual context
virtual report
virtual org

// =============================================================================
// STRUCT DECLARATIONS with extra1/extra2 - Type-specific rules
// =============================================================================

// int type
@rule_struct_range_int depends on patch@
expression E1, E2, E3, E4;
identifier D, I;
@@
- struct ctl_table I = {
-  .procname = E1,
-  .data = &D,
-  .maxlen = sizeof(int),
-  .mode = E2,
-  .proc_handler = proc_dointvec_minmax,
-  .extra1 = E3,
-  .extra2 = E4
- };
+ struct ctl_table I = SYSCTL_RANGE_ENTRY(E1, &D, int, E2, E3, E4);

// unsigned int -> uint
@rule_struct_range_uint depends on patch@
expression E1, E2, E3, E4;
identifier D, I;
@@
- struct ctl_table I = {
-  .procname = E1,
-  .data = &D,
-  .maxlen = sizeof(unsigned int),
-  .mode = E2,
-  .proc_handler = proc_douintvec_minmax,
-  .extra1 = E3,
-  .extra2 = E4
- };
+ struct ctl_table I = SYSCTL_RANGE_ENTRY(E1, &D, uint, E2, E3, E4);

// unsigned -> uint
@rule_struct_range_unsigned depends on patch@
expression E1, E2, E3, E4;
identifier D, I;
@@
- struct ctl_table I = {
-  .procname = E1,
-  .data = &D,
-  .maxlen = sizeof(unsigned),
-  .mode = E2,
-  .proc_handler = proc_douintvec_minmax,
-  .extra1 = E3,
-  .extra2 = E4
- };
+ struct ctl_table I = SYSCTL_RANGE_ENTRY(E1, &D, uint, E2, E3, E4);

// long type
@rule_struct_range_long depends on patch@
expression E1, E2, E3, E4;
identifier D, I;
@@
- struct ctl_table I = {
-  .procname = E1,
-  .data = &D,
-  .maxlen = sizeof(long),
-  .mode = E2,
-  .proc_handler = proc_dolongvec_minmax,
-  .extra1 = E3,
-  .extra2 = E4
- };
+ struct ctl_table I = SYSCTL_RANGE_ENTRY(E1, &D, long, E2, E3, E4);

// unsigned long -> ulong
@rule_struct_range_ulong depends on patch@
expression E1, E2, E3, E4;
identifier D, I;
@@
- struct ctl_table I = {
-  .procname = E1,
-  .data = &D,
-  .maxlen = sizeof(unsigned long),
-  .mode = E2,
-  .proc_handler = proc_doulongvec_minmax,
-  .extra1 = E3,
-  .extra2 = E4
- };
+ struct ctl_table I = SYSCTL_RANGE_ENTRY(E1, &D, ulong, E2, E3, E4);

// unsigned char -> u8
@rule_struct_range_u8 depends on patch@
expression E1, E2, E3, E4;
identifier D, I;
@@
- struct ctl_table I = {
-  .procname = E1,
-  .data = &D,
-  .maxlen = sizeof(unsigned char),
-  .mode = E2,
-  .proc_handler = proc_dou8vec_minmax,
-  .extra1 = E3,
-  .extra2 = E4
- };
+ struct ctl_table I = SYSCTL_RANGE_ENTRY(E1, &D, u8, E2, E3, E4);

// =============================================================================
// STRUCT DECLARATIONS without extra1/extra2 - Type-specific rules
// Only match standard proc_handler: proc_dointvec, proc_douintvec, etc.
// =============================================================================

// int type
@rule_struct_simple_int depends on patch@
expression E1, E2;
identifier D, I;
@@
- struct ctl_table I = {
-  .procname = E1,
-  .data = &D,
-  .maxlen = sizeof(int),
-  .mode = E2,
-  .proc_handler = proc_dointvec
- };
+ struct ctl_table I = SYSCTL_ENTRY(E1, &D, int, E2);

// unsigned int -> uint
@rule_struct_simple_uint depends on patch@
expression E1, E2;
identifier D, I;
@@
- struct ctl_table I = {
-  .procname = E1,
-  .data = &D,
-  .maxlen = sizeof(unsigned int),
-  .mode = E2,
-  .proc_handler = proc_douintvec
- };
+ struct ctl_table I = SYSCTL_ENTRY(E1, &D, uint, E2);

// unsigned -> uint
@rule_struct_simple_unsigned depends on patch@
expression E1, E2;
identifier D, I;
@@
- struct ctl_table I = {
-  .procname = E1,
-  .data = &D,
-  .maxlen = sizeof(unsigned),
-  .mode = E2,
-  .proc_handler = proc_douintvec
- };
+ struct ctl_table I = SYSCTL_ENTRY(E1, &D, uint, E2);

// long type
@rule_struct_simple_long depends on patch@
expression E1, E2;
identifier D, I;
@@
- struct ctl_table I = {
-  .procname = E1,
-  .data = &D,
-  .maxlen = sizeof(long),
-  .mode = E2,
-  .proc_handler = proc_dolongvec
- };
+ struct ctl_table I = SYSCTL_ENTRY(E1, &D, long, E2);

// unsigned long -> ulong
@rule_struct_simple_ulong depends on patch@
expression E1, E2;
identifier D, I;
@@
- struct ctl_table I = {
-  .procname = E1,
-  .data = &D,
-  .maxlen = sizeof(unsigned long),
-  .mode = E2,
-  .proc_handler = proc_doulongvec
- };
+ struct ctl_table I = SYSCTL_ENTRY(E1, &D, ulong, E2);

// unsigned char -> u8
@rule_struct_simple_u8 depends on patch@
expression E1, E2;
identifier D, I;
@@
- struct ctl_table I = {
-  .procname = E1,
-  .data = &D,
-  .maxlen = sizeof(unsigned char),
-  .mode = E2,
-  .proc_handler = proc_dou8vec
- };
+ struct ctl_table I = SYSCTL_ENTRY(E1, &D, u8, E2);

// =============================================================================
// ARRAY ELEMENTS with extra1/extra2 - Type-specific rules
// =============================================================================

// int type
@rule_array_range_int depends on patch@
expression E1, E2, E3, E4;
identifier D;
@@
- {
-  .procname = E1,
-  .data = &D,
-  .maxlen = sizeof(int),
-  .mode = E2,
-  .proc_handler = proc_dointvec_minmax,
-  .extra1 = E3,
-  .extra2 = E4
- }
+ SYSCTL_RANGE_ENTRY(E1, &D, int, E2, E3, E4)

// unsigned int -> uint
@rule_array_range_uint depends on patch@
expression E1, E2, E3, E4;
identifier D;
@@
- {
-  .procname = E1,
-  .data = &D,
-  .maxlen = sizeof(unsigned int),
-  .mode = E2,
-  .proc_handler = proc_douintvec_minmax,
-  .extra1 = E3,
-  .extra2 = E4
- }
+ SYSCTL_RANGE_ENTRY(E1, &D, uint, E2, E3, E4)

// unsigned -> uint
@rule_array_range_unsigned depends on patch@
expression E1, E2, E3, E4;
identifier D;
@@
- {
-  .procname = E1,
-  .data = &D,
-  .maxlen = sizeof(unsigned),
-  .mode = E2,
-  .proc_handler = proc_dointvec_minmax,
-  .extra1 = E3,
-  .extra2 = E4
- }
+ SYSCTL_RANGE_ENTRY(E1, &D, uint, E2, E3, E4)

// long type
@rule_array_range_long depends on patch@
expression E1, E2, E3, E4;
identifier D;
@@
- {
-  .procname = E1,
-  .data = &D,
-  .maxlen = sizeof(long),
-  .mode = E2,
-  .proc_handler = proc_dolongvec_minmax,
-  .extra1 = E3,
-  .extra2 = E4
- }
+ SYSCTL_RANGE_ENTRY(E1, &D, long, E2, E3, E4)

// unsigned long -> ulong
@rule_array_range_ulong depends on patch@
expression E1, E2, E3, E4;
identifier D;
@@
- {
-  .procname = E1,
-  .data = &D,
-  .maxlen = sizeof(unsigned long),
-  .mode = E2,
-  .proc_handler = proc_doulongvec_minmax,
-  .extra1 = E3,
-  .extra2 = E4
- }
+ SYSCTL_RANGE_ENTRY(E1, &D, ulong, E2, E3, E4)

// unsigned char -> u8
@rule_array_range_u8 depends on patch@
expression E1, E2, E3, E4;
identifier D;
@@
- {
-  .procname = E1,
-  .data = &D,
-  .maxlen = sizeof(unsigned char),
-  .mode = E2,
-  .proc_handler = proc_dou8vec_minmax,
-  .extra1 = E3,
-  .extra2 = E4
- }
+ SYSCTL_RANGE_ENTRY(E1, &D, u8, E2, E3, E4)

// =============================================================================
// ARRAY ELEMENTS without extra1/extra2 - Type-specific rules
// =============================================================================

// int type
@rule_array_simple_int depends on patch@
expression E1, E2;
identifier D;
@@
- {
-  .procname = E1,
-  .data = &D,
-  .maxlen = sizeof(int),
-  .mode = E2,
-  .proc_handler = proc_dointvec
- }
+ SYSCTL_ENTRY(E1, &D, int, E2)

// unsigned int -> uint
@rule_array_simple_uint depends on patch@
expression E1, E2;
identifier D;
@@
- {
-  .procname = E1,
-  .data = &D,
-  .maxlen = sizeof(unsigned int),
-  .mode = E2,
-  .proc_handler = proc_douintvec
- }
+ SYSCTL_ENTRY(E1, &D, uint, E2)

// unsigned -> uint
@rule_array_simple_unsigned depends on patch@
expression E1, E2;
identifier D;
@@
- {
-  .procname = E1,
-  .data = &D,
-  .maxlen = sizeof(unsigned),
-  .mode = E2,
-  .proc_handler = proc_dointvec
- }
+ SYSCTL_ENTRY(E1, &D, uint, E2)

// long type
@rule_array_simple_long depends on patch@
expression E1, E2;
identifier D;
@@
- {
-  .procname = E1,
-  .data = &D,
-  .maxlen = sizeof(long),
-  .mode = E2,
-  .proc_handler = proc_dolongvec
- }
+ SYSCTL_ENTRY(E1, &D, long, E2)

// unsigned long -> ulong
@rule_array_simple_ulong depends on patch@
expression E1, E2;
identifier D;
@@
- {
-  .procname = E1,
-  .data = &D,
-  .maxlen = sizeof(unsigned long),
-  .mode = E2,
-  .proc_handler = proc_doulongvec
- }
+ SYSCTL_ENTRY(E1, &D, ulong, E2)

// unsigned char -> u8
@rule_array_simple_u8 depends on patch@
expression E1, E2;
identifier D;
@@
- {
-  .procname = E1,
-  .data = &D,
-  .maxlen = sizeof(unsigned char),
-  .mode = E2,
-  .proc_handler = proc_dou8vec
- }
+ SYSCTL_ENTRY(E1, &D, u8, E2)

// =============================================================================
// CONTEXT MODE - Generic rule to show all matches
// =============================================================================

@rule_context_range depends on context@
expression E1, E2, E3, E4;
identifier D;
identifier H = {proc_dointvec_minmax, proc_douintvec_minmax, proc_dolongvec_minmax, proc_doulongvec_minmax, proc_dou8vec_minmax};
type T;
@@
* {
*  .procname = E1,
*  .data = &D,
*  .maxlen = sizeof(T),
*  .mode = E2,
*  .proc_handler = H,
*  .extra1 = E3,
*  .extra2 = E4
* }

@rule_context_simple depends on context@
expression E1, E2;
identifier D;
identifier H = {proc_dointvec, proc_douintvec, proc_dolongvec, proc_doulongvec, proc_dou8vec};
type T;
@@
* {
*  .procname = E1,
*  .data = &D,
*  .maxlen = sizeof(T),
*  .mode = E2,
*  .proc_handler = H
* }

// =============================================================================
// REPORT MODE - Generic rule to report all matches
// =============================================================================

@rule_report_range depends on report@
expression E1, E2, E3, E4;
identifier D;
identifier H = {proc_dointvec_minmax, proc_douintvec_minmax, proc_dolongvec_minmax, proc_doulongvec_minmax, proc_dou8vec_minmax};
type T;
position p;
@@
{
  .procname@p = E1,
  .data = &D,
  .maxlen = sizeof(T),
  .mode = E2,
  .proc_handler = H,
  .extra1 = E3,
  .extra2 = E4
}

@script:python depends on report@
p << rule_report_range.p;
@@
msg = "INFO: ctl_table initialization can use SYSCTL_RANGE_ENTRY"
coccilib.report.print_report(p[0], msg)

@rule_report_simple depends on report@
expression E1, E2;
identifier D;
identifier H = {proc_dointvec, proc_douintvec, proc_dolongvec, proc_doulongvec, proc_dou8vec};
type T;
position p;
@@
{
  .procname@p = E1,
  .data = &D,
  .maxlen = sizeof(T),
  .mode = E2,
  .proc_handler = H
}

@script:python depends on report@
p << rule_report_simple.p;
@@
msg = "INFO: ctl_table initialization can use SYSCTL_ENTRY"
coccilib.report.print_report(p[0], msg)

// =============================================================================
// ORG MODE - Generic rule for org-mode output
// =============================================================================

@rule_org_range depends on org@
expression E1, E2, E3, E4;
identifier D;
identifier H = {proc_dointvec_minmax, proc_douintvec_minmax, proc_dolongvec_minmax, proc_doulongvec_minmax, proc_dou8vec_minmax};
type T;
position p;
@@
{
  .procname@p = E1,
  .data = &D,
  .maxlen = sizeof(T),
  .mode = E2,
  .proc_handler = H,
  .extra1 = E3,
  .extra2 = E4
}

@script:python depends on org@
p << rule_org_range.p;
@@
msg = "INFO: ctl_table initialization can use SYSCTL_RANGE_ENTRY"
coccilib.org.print_todo(p[0], msg)

@rule_org_simple depends on org@
expression E1, E2;
identifier D;
identifier H = {proc_dointvec, proc_douintvec, proc_dolongvec, proc_doulongvec, proc_dou8vec};
type T;
position p;
@@
{
  .procname@p = E1,
  .data = &D,
  .maxlen = sizeof(T),
  .mode = E2,
  .proc_handler = H
}

@script:python depends on org@
p << rule_org_simple.p;
@@
msg = "INFO: ctl_table initialization can use SYSCTL_ENTRY"
coccilib.org.print_todo(p[0], msg)

