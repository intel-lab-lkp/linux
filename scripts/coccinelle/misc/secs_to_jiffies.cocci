// SPDX-License-Identifier: GPL-2.0-only
///
/// Find usages of:
/// - msecs_to_jiffies(value*1000)
/// - msecs_to_jiffies(value*MSEC_PER_SEC)
///
// Confidence: High
// Copyright: (C) 2024 Easwar Hariharan, Microsoft
// Keywords: secs, seconds, jiffies
//

virtual patch

@depends on patch@
constant C;
@@

-msecs_to_jiffies
+secs_to_jiffies
 (C
- * \( 1000 \| MSEC_PER_SEC \)
 )

@depends on patch@
expression E;
@@

-msecs_to_jiffies
+secs_to_jiffies
 (E
- * \( 1000 \| MSEC_PER_SEC \)
 )
