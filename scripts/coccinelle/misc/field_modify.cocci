// SPDX-License-Identifier: GPL-2.0-only
///
/// Replaced below code with the wrapper FIELD_MODIFY(MASK, &reg, val)
/// - reg &= ~MASK;
/// - reg |= FIELD_PREP(MASK, val);
//
// Confidence: High
// Author: Luo Jie <quic_luoj@quicinc.com>
// Copyright (C) 2025 Qualcomm Innovation Center, Inc.
// URL: https://coccinelle.gitlabpages.inria.fr/website
// Keywords: FIELD_PREP, FIELD_MODIFY
// Options: --include-headers

virtual patch

@depends on patch@
identifier reg, val;
constant mask;
symbol FIELD_PREP, FIELD_MODIFY;
@@

- reg &= ~mask;
- reg |= FIELD_PREP(mask, val);
+ FIELD_MODIFY(mask, &reg, val);
