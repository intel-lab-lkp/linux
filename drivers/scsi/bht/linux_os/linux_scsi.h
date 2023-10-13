/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2015 BHT Inc.
 *
 * File Name: linux_scsi.h
 *
 * Abstract: SCSI function
 *
 * Version: 1.00
 *
 * Author: Peter.Guo
 *
 * Environment:	Linux
 *
 * History:
 *
 * 5/20/2015		Creation	Peter.Guo
 */

#ifndef _BHT_SCSI_H
#define _BHT_SCSI_H

#define LINUX_SCSI_MAX_QUEUE_DPETH	32

bool bht_scsi_init(bht_dev_ext_t *pdx, struct device *dev);
void bht_scsi_uinit(bht_dev_ext_t *pdx);

typedef struct {
	byte ErrorCode;
	byte Reserved;
	byte SenseKey;
	byte Infomation[4];
	byte AdditionalSenseLength;
	byte CommandSpecInfo[4];
	byte AdditionalSenseCode;
	byte AdditionalSenseCodeQualifier;
	byte FieldReplaceable;
	byte SenseKeySpec[3];
} SENSE_DATA, *PSENSE_DATA;

typedef struct {
	byte DeviceType:5;
	byte DeviceTypeQualifier:3;
	byte DeviceTypeModifier:7;
	byte RemovableMedia:1;
	byte Versions;
	byte ResponseDataFormat:4;
	byte HiSupport:1;
	byte NormACA:1;
	byte ReservedBit:1;
	byte AERC:1;
	byte AdditionalLength;
	byte Reserved[2];
	byte SoftReset:1;
	byte CommandQueue:1;
	byte Reserved2:1;
	byte LinkedCommands:1;
	byte Synchronous:1;
	byte Wide16Bit:1;
	byte Wide32Bit:1;
	byte RelativeAddressing:1;
	byte VendorId[8];
	byte ProductId[16];
	byte ProductRevisionLevel[4];
	byte VendorSpecific[20];
	byte IUS:1;
	byte QAS:1;
	byte Clocking:2;
	byte Reserved3_first_byte:4;
	byte Reserved3[39];
} _INQUIRYDATA;

typedef struct {
	u32 LogicalBlockAddress;
	u32 BytesPerBlock;
} READ_CAPACITY_DATA, *PREAD_CAPACITY_DATA;

typedef struct {
	byte DataLength;
	byte MediumType;
	byte Reserved;
	byte BlockDescLength;
} MODE_PAGE_HEADER;

typedef struct {
	u8 PageCode;
	u8 PageLength;
	u16 TracksPerZone;
	u16 AltSectorsPerZone;
	u16 AltTracksPerZone;
	u16 AltTracksPerVolume;
	u16 SectorsPerTrack;
	u16 BytesPerSector;
	u16 Interleave;
	u16 TrackSkew;
	u16 CylinderSkew;
	u8 flags;
	u8 reserved[3];
} MODE_PAGE3;
/*
 * Sense Data Format - Page 4
 */
typedef struct {
	u8 PageCode;
	u8 PageLength;
	u16 CylindersHigh;
	u8 CylindersLow;
	u8 Heads;
	u16 WritePrecompHigh;
	u8 WritePrecompLow;
	u16 ReducedWriteCurrentHigh;
	u8 ReducedWriteCurrentLow;
	u16 StepRate;
	u16 LandingZoneHigh;
	u8 LandingZoneLow;
	u8 flags;
	u8 RotationalOffset;
	u8 Reserved;
	u16 MediumRotationRate;
	u8 Reserved2[2];
} MODE_PAGE4;

typedef struct {
	u8 PageCode;
	u8 PageLength;
	u8 Flags;
	u8 RetensionPriority;
	u8 DisablePrefetchTransfer[2];
	u8 MinimumPrefetch[2];
	u8 MaximumPrefetch[2];
	u8 MaximumPrefetchCeiling[2];
	u8 temp[7];
} MODE_PAGE8;

typedef struct {
	byte PageCode:6;
	byte Reserved:1;
	byte PageSavable:1;
	byte PageLength;

	byte ReadDisableCache:1;
	byte MultiplicationFactor:1;
	byte WriteCacheEnable:1;
	byte Reserved2:5;
	byte WriteRetensionPriority:4;
	byte ReadRetensionPriority:4;
	byte DisablePrefetchTransfer[2];
	byte MinimumPrefetch[2];
	byte MaximumPrefetch[2];
	byte MaximumPrefetchCeiling[2];
	byte FSW_LBCSS_RDA;
	byte NumberofCacheSegments;
	byte CacheSegmentSize[2];
	byte Reserved16;
	byte NonCacheSegmentSize[3];
} MODE_PAGE_8;

typedef struct {
	byte PageCode:6;
	byte Reserved:1;
	byte PageSavable:1;
	byte PageLength;

	byte Reserved2;
	byte ProtocolIdentifier;
	byte SynchronousTransferTimeout[2];
	byte Reserved6[2];
} MODEPAGE19;

typedef struct {
	MODE_PAGE_HEADER hdr;
	union {
		MODEPAGE19 pg_9;
		MODE_PAGE_8 pg_8;
		MODE_PAGE8 pg8;
		MODE_PAGE3 pg3;
		MODE_PAGE4 pg4;
	} pdata;
} MODE_PAGE_DATA;

/* Sense codes */
#define SCSI_SENSE_NO_SENSE         0x00
#define SCSI_SENSE_RECOVERED_ERROR  0x01
#define SCSI_SENSE_NOT_READY        0x02
#define SCSI_SENSE_MEDIUM_ERROR     0x03
#define SCSI_SENSE_HARDWARE_ERROR   0x04
#define SCSI_SENSE_ILLEGAL_REQUEST  0x05
#define SCSI_SENSE_UNIT_ATTENTION   0x06
#define SCSI_SENSE_DATA_PROTECT     0x07
#define SCSI_SENSE_BLANK_CHECK      0x08
#define SCSI_SENSE_UNIQUE           0x09
#define SCSI_SENSE_COPY_ABORTED     0x0A
#define SCSI_SENSE_ABORTED_COMMAND  0x0B
#define SCSI_SENSE_EQUAL            0x0C
#define SCSI_SENSE_VOL_OVERFLOW     0x0D
#define SCSI_SENSE_MISCOMPARE       0x0E
#define SCSI_SENSE_RESERVED         0x0F

/* Additional Sense codes */
#define SCSI_ADSENSE_NO_SENSE                              0x00
#define SCSI_ADSENSE_NO_SEEK_COMPLETE                      0x02
#define SCSI_ADSENSE_LUN_NOT_READY                         0x04
#define SCSI_ADSENSE_LUN_COMMUNICATION                     0x08
#define SCSI_ADSENSE_WRITE_ERROR                           0x0C
#define SCSI_ADSENSE_TRACK_ERROR                           0x14
#define SCSI_ADSENSE_SEEK_ERROR                            0x15
#define SCSI_ADSENSE_REC_DATA_NOECC                        0x17
#define SCSI_ADSENSE_REC_DATA_ECC                          0x18
#define SCSI_ADSENSE_PARAMETER_LIST_LENGTH                 0x1A
#define SCSI_ADSENSE_ILLEGAL_COMMAND                       0x20
#define SCSI_ADSENSE_ILLEGAL_BLOCK                         0x21
#define SCSI_ADSENSE_INVALID_CDB                           0x24
#define SCSI_ADSENSE_INVALID_LUN                           0x25
#define SCSI_ADSENSE_INVALID_FIELD_PARAMETER_LIST          0x26
#define SCSI_ADSENSE_WRITE_PROTECT                         0x27
#define SCSI_ADSENSE_MEDIUM_CHANGED                        0x28
#define SCSI_ADSENSE_BUS_RESET                             0x29
#define SCSI_ADSENSE_PARAMETERS_CHANGED                    0x2A
#define SCSI_ADSENSE_INSUFFICIENT_TIME_FOR_OPERATION       0x2E
#define SCSI_ADSENSE_INVALID_MEDIA                         0x30
#define SCSI_ADSENSE_NO_MEDIA_IN_DEVICE                    0x3a
#define SCSI_ADSENSE_POSITION_ERROR                        0x3b
#define SCSI_ADSENSE_OPERATING_CONDITIONS_CHANGED          0x3f
#define SCSI_ADSENSE_OPERATOR_REQUEST                      0x5a
#define SCSI_ADSENSE_FAILURE_PREDICTION_THRESHOLD_EXCEEDED 0x5d
#define SCSI_ADSENSE_ILLEGAL_MODE_FOR_THIS_TRACK           0x64
#define SCSI_ADSENSE_COPY_PROTECTION_FAILURE               0x6f
#define SCSI_ADSENSE_POWER_CALIBRATION_ERROR               0x73
#define SCSI_ADSENSE_VENDOR_UNIQUE                         0x80
#define SCSI_ADSENSE_MUSIC_AREA                            0xA0
#define SCSI_ADSENSE_DATA_AREA                             0xA1
#define SCSI_ADSENSE_VOLUME_OVERFLOW                       0xA7

/* for legacy apps : */

extern struct kmem_cache *bht_srb_ext_cachep;
extern mempool_t *bht_sd_mem_pool;

#endif
