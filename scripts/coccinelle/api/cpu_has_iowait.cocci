// SPDX-License-Identifier: GPL-2.0-only
// Replace nr_iowait_cpu() with a boolean accessor after menu stopped using
// the per-CPU count. Preserve the count used by nr_iowait().
//
// The accompanying update to the helper's comment is manual.
//
// Options: --include-headers

virtual patch

@ definition depends on patch @
identifier cpu;
typedef bool;
@@

- unsigned int nr_iowait_cpu(int cpu)
+ bool cpu_has_iowait(int cpu)
  {
-	return atomic_read(&cpu_rq(cpu)->nr_iowait);
+	return atomic_read(&cpu_rq(cpu)->nr_iowait) != 0;
  }

@ prototype_rule depends on patch @
identifier cpu;
typedef bool;
@@

- extern unsigned int nr_iowait_cpu
+ extern bool cpu_has_iowait
  (int cpu);

@ total depends on patch @
expression cpu;
@@

 unsigned int nr_iowait(void)
 {
 <...
- nr_iowait_cpu(cpu)
+ atomic_read(&cpu_rq(cpu)->nr_iowait)
 ...>
 }

@ predicate depends on patch @
expression cpu;
@@

- nr_iowait_cpu(cpu) > 0
+ cpu_has_iowait(cpu)

@ callers depends on patch @
expression cpu;
@@

- nr_iowait_cpu(cpu)
+ cpu_has_iowait(cpu)
