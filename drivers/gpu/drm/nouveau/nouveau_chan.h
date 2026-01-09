/* SPDX-License-Identifier: MIT */
#ifndef __NOUVEAU_CHAN_H__
#define __NOUVEAU_CHAN_H__
#include <nvif/object.h>
#include <nvif/event.h>
#include <nvif/chan.h>
#include <nvif/class.h>
struct nvif_device;

struct nouveau_channel {
	struct nvif_chan chan;

	struct nouveau_cli *cli;
	struct nouveau_vmm *vmm;

	struct nvif_mem mem_userd;
	struct nvif_object *userd;

	int runlist;
	int chid;
	u64 inst;
	u32 token;

	struct nvif_object vram;
	struct nvif_object gart;
	struct nvif_object nvsw;

	struct {
		struct nouveau_bo *buffer;
		struct nouveau_vma *vma;
		struct nvif_object ctxdma;
		u64 addr;
		u64 plength;
		u64 ioffset;
		u64 ilength;
	} push;

	void *fence;
	struct {
		int max;
		int free;
		int cur;
		int put;
	} dma;
	u32 user_get;
	u32 user_put;

	struct {
		struct nouveau_bo *bo;
		struct nouveau_vma *vma;
	} sema;

	struct nvif_object user;
	struct nvif_object blit;

	struct nvif_event kill;
	atomic_t killed;
};

int nouveau_channels_init(struct nouveau_drm *);
void nouveau_channels_fini(struct nouveau_drm *);

int  nouveau_channel_new(struct nouveau_cli *, bool priv, u64 runm,
			 u32 vram, u32 gart, struct nouveau_channel **);
void nouveau_channel_del(struct nouveau_channel **);
int  nouveau_channel_idle(struct nouveau_channel *);
void nouveau_channel_kill(struct nouveau_channel *);

/* Maximum GPFIFO entries per channel. */
#define NV50_CHANNEL_GPFIFO_ENTRIES_MAX_COUNT		(0x02000 / 8)
#define MAXWELL_CHANNEL_GPFIFO_ENTRIES_MAX_COUNT	(0x40000 / 8)

static inline u32 nouveau_channel_get_gpfifo_entries_count(u32 oclass)
{
	if (oclass < NV50_CHANNEL_GPFIFO)
		return 0;

	if (oclass >= MAXWELL_CHANNEL_GPFIFO_A)
		return MAXWELL_CHANNEL_GPFIFO_ENTRIES_MAX_COUNT;

	return NV50_CHANNEL_GPFIFO_ENTRIES_MAX_COUNT;
}

extern int nouveau_vram_pushbuf;

#endif
