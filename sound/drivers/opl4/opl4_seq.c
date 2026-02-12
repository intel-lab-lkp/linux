// SPDX-License-Identifier: GPL-2.0-or-later OR LicenseRef-BSD-variant1
/*
 * OPL4 sequencer functions
 *
 * Copyright (c) 2003 by Clemens Ladisch <clemens@ladisch.de>
 * All rights reserved.
 */

#include "opl4_local.h"
#include <linux/init.h>
#include <linux/moduleparam.h>
#include <linux/module.h>
#include <sound/initval.h>

MODULE_AUTHOR("Clemens Ladisch <clemens@ladisch.de>");
MODULE_DESCRIPTION("OPL4 wavetable synth driver");
MODULE_LICENSE("Dual BSD/GPL");

int volume_boost = 8;

module_param(volume_boost, int, 0644);
MODULE_PARM_DESC(volume_boost, "Additional volume for OPL4 wavetable sounds.");

static int snd_opl4_seq_use_inc(struct snd_opl4 *opl4)
{
	if (!try_module_get(opl4->card->module))
		return -EFAULT;
	return 0;
}

static void snd_opl4_seq_use_dec(struct snd_opl4 *opl4)
{
	module_put(opl4->card->module);
}

static int snd_opl4_seq_use(void *private_data, struct snd_seq_port_subscribe *info)
{
	struct snd_opl4 *opl4 = private_data;
	int err;

	scoped_guard(mutex, &opl4->access_mutex) {
		if (opl4->used)
			return -EBUSY;
		opl4->used++;

		if (info->sender.client != SNDRV_SEQ_CLIENT_SYSTEM) {
			err = snd_opl4_seq_use_inc(opl4);
			if (err < 0)
				return err;
		}
	}

	snd_opl4_synth_reset(opl4);
	return 0;
}

static int snd_opl4_seq_unuse(void *private_data, struct snd_seq_port_subscribe *info)
{
	struct snd_opl4 *opl4 = private_data;

	snd_opl4_synth_shutdown(opl4);

	scoped_guard(mutex, &opl4->access_mutex) {
		opl4->used--;
	}

	if (info->sender.client != SNDRV_SEQ_CLIENT_SYSTEM)
		snd_opl4_seq_use_dec(opl4);
	return 0;
}

static const struct snd_midi_op opl4_ops = {
	.note_on =		snd_opl4_note_on,
	.note_off =		snd_opl4_note_off,
	.note_terminate =	snd_opl4_terminate_note,
	.control =		snd_opl4_control,
	.sysex =		snd_opl4_sysex,
};

static int snd_opl4_seq_event_input(struct snd_seq_event *ev, int direct,
				    void *private_data, int atomic, int hop)
{
	struct snd_opl4 *opl4 = private_data;

	snd_midi_process_event(&opl4_ops, ev, opl4->chset);
	return 0;
}

static void snd_opl4_seq_free_port(void *private_data)
{
	struct snd_opl4 *opl4 = private_data;

	snd_midi_channel_free_set(opl4->chset);
}

static int snd_opl4_seq_probe(struct snd_seq_device *dev)
{
	struct snd_opl4 *opl4;
	int client;
	struct snd_seq_port_callback pcallbacks;

	opl4 = *(struct snd_opl4 **)SNDRV_SEQ_DEVICE_ARGPTR(dev);
	if (!opl4)
		return -EINVAL;

	if (snd_yrw801_detect(opl4) < 0)
		return -ENODEV;

	opl4->chset = snd_midi_channel_alloc_set(16);
	if (!opl4->chset)
		return -ENOMEM;
	opl4->chset->private_data = opl4;

	/* allocate new client */
	client = snd_seq_create_kernel_client(opl4->card, opl4->seq_dev_num,
					      "OPL4 Wavetable");
	if (client < 0) {
		snd_midi_channel_free_set(opl4->chset);
		return client;
	}
	opl4->seq_client = client;
	opl4->chset->client = client;

	/* create new port */
	memset(&pcallbacks, 0, sizeof(pcallbacks));
	pcallbacks.owner = THIS_MODULE;
	pcallbacks.use = snd_opl4_seq_use;
	pcallbacks.unuse = snd_opl4_seq_unuse;
	pcallbacks.event_input = snd_opl4_seq_event_input;
	pcallbacks.private_free = snd_opl4_seq_free_port;
	pcallbacks.private_data = opl4;

	opl4->chset->port = snd_seq_event_port_attach(client, &pcallbacks,
						      SNDRV_SEQ_PORT_CAP_WRITE |
						      SNDRV_SEQ_PORT_CAP_SUBS_WRITE,
						      SNDRV_SEQ_PORT_TYPE_MIDI_GENERIC |
						      SNDRV_SEQ_PORT_TYPE_MIDI_GM |
						      SNDRV_SEQ_PORT_TYPE_HARDWARE |
						      SNDRV_SEQ_PORT_TYPE_SYNTHESIZER,
						      16, 24,
						      "OPL4 Wavetable Port");
	if (opl4->chset->port < 0) {
		int err = opl4->chset->port;
		snd_midi_channel_free_set(opl4->chset);
		snd_seq_delete_kernel_client(client);
		opl4->seq_client = -1;
		return err;
	}
	return 0;
}

static void snd_opl4_seq_remove(struct snd_seq_device *dev)
{
	struct snd_opl4 *opl4;

	opl4 = *(struct snd_opl4 **)SNDRV_SEQ_DEVICE_ARGPTR(dev);
	if (!opl4)
		return;

	if (opl4->seq_client >= 0) {
		snd_seq_delete_kernel_client(opl4->seq_client);
		opl4->seq_client = -1;
	}
}

static struct snd_seq_driver opl4_seq_driver = {
	.probe = snd_opl4_seq_probe,
	.remove = snd_opl4_seq_remove,
	.driver = {
		.name = KBUILD_MODNAME,
	},
	.id = SNDRV_SEQ_DEV_ID_OPL4,
	.argsize = sizeof(struct snd_opl4 *),
};

module_snd_seq_driver(opl4_seq_driver);
