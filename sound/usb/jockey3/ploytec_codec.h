/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *   ALSA driver for Reloop Jockey 3 devices
 *
 *   Copyright (c) 2026 by Frank van de Pol <fvdpol@gmail.com>
 */

#ifndef PLOYTEC_CODEC_H
#define PLOYTEC_CODEC_H

#include <linux/types.h>

/* Ploytec Protocol Constants */
#define PLOYTEC_PKT_SIZE 512
#define PLOYTEC_MIDI_IDLE_BYTE 0xFD

/* Playback (EP 0x05) */
#define PLOYTEC_EP_PCM_OUT 0x05
#define PLOYTEC_PLAYBACK_FRAMES 10
#define PLOYTEC_PLAYBACK_FRAME_SIZE 48

/* Capture (EP 0x86) */
#define PLOYTEC_EP_PCM_IN 0x86
#define PLOYTEC_CAPTURE_FRAMES 8
#define PLOYTEC_CAPTURE_FRAME_SIZE 64

/* MIDI (EP 0x83) */
#define PLOYTEC_EP_MIDI_IN 0x83

/* Bit-shuffling codec for S24_3LE (3 bytes per sample) */
void ploytec_encode_s24_3le(u8 *dest, const u8 *src);
void ploytec_decode_s24_3le(u8 *dest, const u8 *src);

#endif /* PLOYTEC_CODEC_H */
