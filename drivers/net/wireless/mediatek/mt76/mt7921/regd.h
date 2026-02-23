/* SPDX-License-Identifier: ISC */
/* Copyright (C) 2025 MediaTek Inc. */

#ifndef __MT7921_REGD_H
#define __MT7921_REGD_H

#include "mt7921.h"

void mt7921_regd_update(struct mt792x_dev *dev);
void mt7921_regd_notifier(struct wiphy *wiphy,
			  struct regulatory_request *request);
bool mt7921_regd_clc_supported(struct mt792x_dev *dev);

#endif
