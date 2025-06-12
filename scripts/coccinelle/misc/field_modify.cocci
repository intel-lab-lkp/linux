// SPDX-License-Identifier: GPL-2.0-only
/// Replace below code with the wrapper FIELD_MODIFY(MASK, &reg, val)
/// - reg &= ~MASK;
/// - reg |= FIELD_PREP(MASK, val);
//
// Confidence: High
// Author: Luo Jie <quic_luoj@quicinc.com>
// Copyright: (C) 2025 Qualcomm Innovation Center, Inc.
// Keywords: FIELD_PREP, FIELD_MODIFY
// Options: --include-headers

virtual context
virtual patch
virtual org
virtual report

@ depends on context && !patch && !org && !report@
identifier reg, val;
constant mask;
symbol FIELD_PREP;
@@

* reg &= ~mask;
* reg |= FIELD_PREP(mask, val);

@ depends on !context && patch && !org && !report @
identifier reg, val;
constant mask;
symbol FIELD_PREP, FIELD_MODIFY;
@@

-reg &= ~mask;
-reg |= FIELD_PREP(mask, val);
+FIELD_MODIFY(mask, &reg, val);

@r depends on !context && !patch && (org || report)@
identifier reg, val;
constant mask;
symbol FIELD_PREP;
position p;
@@

 reg &= ~mask;
 reg |= FIELD_PREP@p(mask, val);

@script:python depends on report@
p << r.p;
x << r.reg;
@@

msg="WARNING: Consider using FIELD_MODIFY helper on %s" % (x)
coccilib.report.print_report(p[0], msg)

@script:python depends on org@
p << r.p;
x << r.reg;
@@

msg="WARNING: Consider using FIELD_MODIFY helper on %s" % (x)
msg_safe=msg.replace("[","@(").replace("]",")")
coccilib.org.print_todo(p[0], msg_safe)
