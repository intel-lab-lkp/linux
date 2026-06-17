/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *   ALSA driver for Reloop Jockey 3 devices
 *   Ploytec USB Protocol Handling
 *
 *   Copyright (c) 2026 by Frank van de Pol <fvdpol@gmail.com>
 */

#ifndef PLOYTEC_PROTO_H
#define PLOYTEC_PROTO_H

#include <linux/types.h>
#include <linux/usb.h>

/* Ploytec Protocol Constants */
#define PLOYTEC_PKT_SIZE 512
#define PLOYTEC_MIDI_IDLE_BYTE 0xFD

/* Playback & MIDI Out (EP 0x05) */
#define PLOYTEC_EP_PCM_OUT 0x05
#define PLOYTEC_PLAYBACK_FRAMES 10
#define PLOYTEC_PLAYBACK_FRAME_SIZE 48
#define PLOYTEC_MIDI_OUT_OFFSET 480
#define PLOYTEC_SYNC_BYTE_OFFSET 481
#define PLOYTEC_SYNC_BYTE_VALUE 0xFF

/* Capture (EP 0x86) */
#define PLOYTEC_EP_PCM_IN 0x86
#define PLOYTEC_CAPTURE_FRAMES 8
#define PLOYTEC_CAPTURE_FRAME_SIZE 64

/* MIDI In (EP 0x83) */
#define PLOYTEC_EP_MIDI_IN 0x83

/* Protocol Commands */
#define PLOYTEC_REQ_FIRMWARE 0x56
#define PLOYTEC_REQ_STATUS   0x49
#define PLOYTEC_SET_RATE     0x01
#define PLOYTEC_SET_RATE_VAL 0x22

/* Status Bits */
#define PLOYTEC_STATUS_READY 0x20

/* Bit-shuffling codec for S24_3LE (3 bytes per sample) */
void ploytec_encode_s24_3le(u8 *dest, const u8 *src);
void ploytec_decode_s24_3le(u8 *dest, const u8 *src);

/* Protocol Helpers */
void ploytec_prepare_out_packet(u8 *buf);
int ploytec_handshake_step(struct usb_device *dev, void *xfer_buf);
int ploytec_set_rate(struct usb_device *dev, void *xfer_buf, u32 rate);

#endif /* PLOYTEC_PROTO_H */
