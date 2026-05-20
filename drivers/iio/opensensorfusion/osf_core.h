/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _OSF_CORE_H
#define _OSF_CORE_H

#include <linux/spinlock.h>
#include <linux/types.h>

#include "osf_protocol.h"

#define OSF_MAX_SAMPLE_CHANNELS	3
#define OSF_MAX_CAPABILITIES	16
#define OSF_MAX_LATEST_SAMPLES	OSF_MAX_CAPABILITIES

struct osf_latest_sample {
	u16 sensor_type;
	u16 sensor_index;
	u16 channel_count;
	u16 sample_format;
	u32 scale_nano;
	s32 values[OSF_MAX_SAMPLE_CHANNELS];
	u64 sequence;
	u64 timestamp_us;
	bool valid;
};

struct osf_sample_event {
	u16 sensor_type;
	u16 sensor_index;
	u16 channel_count;
	u16 sample_format;
	u32 scale_nano;
	s32 values[OSF_MAX_SAMPLE_CHANNELS];
	u64 sequence;
	u64 timestamp_us;
	s64 host_timestamp_ns;
};

typedef void (*osf_sample_callback_t)(void *context,
				      const struct osf_sample_event *event);

struct osf_capability_cache {
	u16 capability_count;
	struct osf_capability_entry entries[OSF_MAX_CAPABILITIES];
	u64 sequence;
	bool valid;
};

struct osf_status_cache {
	u32 uptime_s;
	u32 status_flags;
	u32 error_flags;
	u32 dropped_frames;
	u64 sequence;
	bool valid;
};

struct osf_device {
	/* Protects latest sample, capability, status, and callback state. */
	spinlock_t lock;
	struct osf_latest_sample latest_sample;
	struct osf_latest_sample latest_samples[OSF_MAX_LATEST_SAMPLES];
	struct osf_capability_cache capability_cache;
	struct osf_status_cache status_cache;
	u64 last_sequence;
	osf_sample_callback_t sample_callback;
	void *sample_callback_context;
};

void osf_core_init_device(struct osf_device *osf);
int osf_core_receive_frame(struct osf_device *osf, const u8 *buf, size_t len);
int osf_core_read_latest_sample(struct osf_device *osf, u16 sensor_type,
				u16 sensor_index, unsigned int channel,
				s32 *value);
bool osf_core_copy_capability_cache(struct osf_device *osf,
				    struct osf_capability_cache *cache);
bool osf_core_capability_sequence(struct osf_device *osf, u64 *sequence);
void osf_core_set_sample_callback(struct osf_device *osf,
				  osf_sample_callback_t callback,
				  void *context);

#endif
