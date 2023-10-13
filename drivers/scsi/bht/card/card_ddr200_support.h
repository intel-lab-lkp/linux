/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: card_ddr200_support.h
 *
 * Abstract: the functon declaration about checking whether the card supports DDR200/DDR225 mode
 *
 * Version: 1.00
 *
 * Author: Fred
 *
 * Environment:	OS Independent
 *
 * History:
 *
 * 12/17/2021   Creation    Fred
 */

#ifndef _CARD_DDR200_SUPPORT_H
#define _CARD_DDR200_SUPPORT_H

#include "../include/card.h"

#define MAX_DDR200_CHECK_METHOD 0x4
#define SANDISK		0x0
#define LEXAR		0x1
#define TRANSEND	0x2
#define PHISON		0x3
#define KINGSTON	0x4

bool sandisk_ddr_support(sd_card_t *card, bool ddr_mode);
bool lexar_transend_ddr200_support(sd_card_t *card);
bool phison_kingston_ddr200_support(sd_card_t *card);
bool sd_ddr_support(sd_card_t *card);
bool manuefecture_ddr200_support(sd_card_t *card, u32 check_methood);

#endif
