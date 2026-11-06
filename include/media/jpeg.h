/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _MEDIA_JPEG_H_
#define _MEDIA_JPEG_H_

#include <linux/types.h>

/* JPEG markers */
#define JPEG_MARKER_TEM		0x01
#define JPEG_MARKER_SOF0	0xc0
#define JPEG_MARKER_DHT		0xc4
#define JPEG_MARKER_RST		0xd0
#define JPEG_MARKER_SOI		0xd8
#define JPEG_MARKER_EOI		0xd9
#define JPEG_MARKER_SOS		0xda
#define JPEG_MARKER_DQT		0xdb
#define JPEG_MARKER_DRI		0xdd
#define JPEG_MARKER_DHP		0xde
#define JPEG_MARKER_APP0	0xe0
#define JPEG_MARKER_COM		0xfe

/* JPEG baseline sample precision (bits per sample) */
#define JPEG_SAMPLE_PRECISION_BITS	8

/* Number of components: greyscale (1) and colour YCbCr (3) */
#define JPEG_COMP_MONO			1
#define JPEG_COMP_COLOR			3

/* Chroma subsampling factors encoded in SOF0 sampling byte */
#define JPEG_SAMPLING_H1V1		0x11
#define JPEG_SAMPLING_H2V2		0x22

/* Quantization table destination identifiers */
#define JPEG_QTABLE_LUMA		0
#define JPEG_QTABLE_CHROMA		1

/* SOS spectral selection and approximation fields (baseline JPEG) */
#define JPEG_SPECTRAL_START		0x00
#define JPEG_SPECTRAL_END		0x3f
#define JPEG_APPROX_HIGH_LOW		0x00

/* SOS table selector bytes (luma: DC=0/AC=0, chroma: DC=1/AC=1) */
#define JPEG_TABLE_SEL_LUMA		0x00
#define JPEG_TABLE_SEL_CHROMA		0x11

/*
 * JFIF APP0 payload fields, as specified by the JFIF spec referenced from
 * ITU-T T.81 Annex B. See: https://www.w3.org/Graphics/JPEG/itu-t81.pdf
 */
#define JFIF_IDENT_TERM			0x00
#define JFIF_VERSION_MAJOR		0x01
#define JFIF_VERSION_MINOR		0x01
#define JFIF_UNITS_NONE			0x00
#define JFIF_DENSITY_HI			0x00
#define JFIF_DENSITY_LO			0x01
#define JFIF_THUMBNAIL_SIZE		0x00

/* Marker segment header: marker and length (ITU-T T.81 B.1.1.4) */
struct jpeg_seg_hdr {
	__be16 marker;
	__be16 length;	/* bytes from the length field to the end of the segment */
} __packed;

/* SOF0 frame header up to the component specifications (ITU-T T.81 B.2.2) */
struct jpeg_sof_hdr {
	struct jpeg_seg_hdr seg;
	u8 precision;
	__be16 height;
	__be16 width;
	u8 num_components;
} __packed;

/* SOF0 frame component specification, one per component */
struct jpeg_sof_comp {
	u8 id;
	u8 sampling;
	u8 qtable;
} __packed;

/* SOS scan header up to the component specifications (ITU-T T.81 B.2.3) */
struct jpeg_sos_hdr {
	struct jpeg_seg_hdr seg;
	u8 num_components;
} __packed;

/* SOS scan component specification, one per component */
struct jpeg_sos_comp {
	u8 id;
	u8 tables;	/* DC and AC entropy table selectors */
} __packed;

/* SOS fields after the component specifications */
struct jpeg_sos_tail {
	u8 spectral_start;
	u8 spectral_end;
	u8 approx;
} __packed;

#endif /* _MEDIA_JPEG_H_ */
