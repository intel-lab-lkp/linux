// SPDX-License-Identifier: GPL-2.0-only
///
/// Find usages of:
/// - msecs_to_jiffies(value*1000)
/// - msecs_to_jiffies(value*MSEC_PER_SEC)
///
// Confidence: High
// Copyright: (C) 2024 Easwar Hariharan Microsoft
//
// Keywords: secs, seconds, jiffies
//

@@ constant C; @@

- msecs_to_jiffies(C * 1000)
+ secs_to_jiffies(C)

@@ constant C; @@

- msecs_to_jiffies(C * MSEC_PER_SEC)
+ secs_to_jiffies(C)
