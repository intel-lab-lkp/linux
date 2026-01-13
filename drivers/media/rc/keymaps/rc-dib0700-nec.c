// SPDX-License-Identifier: GPL-2.0+
// rc-dvb0700-big.c - Keytable for devices in dvb0700
//
// Copyright (c) 2010 by Mauro Carvalho Chehab
//
// TODO: This table is a real mess, as it merges RC codes from several
// devices into a big table. It also has both RC-5 and NEC codes inside.
// It should be broken into small tables, and the protocols should properly
// be identificated.
//
// The table were imported from dib0700_devices.c.

#include <media/rc-map.h>
#include <linux/module.h>

/* Key codes for the Pixelview SBTVD remote */
static struct rc_map_table pixelview_table[] = {
	{ 0x866b13, KEY_MUTE },
	{ 0x866b12, KEY_POWER },
	{ 0x866b01, KEY_NUMERIC_1 },
	{ 0x866b02, KEY_NUMERIC_2 },
	{ 0x866b03, KEY_NUMERIC_3 },
	{ 0x866b04, KEY_NUMERIC_4 },
	{ 0x866b05, KEY_NUMERIC_5 },
	{ 0x866b06, KEY_NUMERIC_6 },
	{ 0x866b07, KEY_NUMERIC_7 },
	{ 0x866b08, KEY_NUMERIC_8 },
	{ 0x866b09, KEY_NUMERIC_9 },
	{ 0x866b00, KEY_NUMERIC_0 },
	{ 0x866b0d, KEY_CHANNELUP },
	{ 0x866b19, KEY_CHANNELDOWN },
	{ 0x866b10, KEY_VOLUMEUP },
	{ 0x866b0c, KEY_VOLUMEDOWN },

	{ 0x866b0a, KEY_CAMERA },
	{ 0x866b0b, KEY_ZOOM },
	{ 0x866b1b, KEY_BACKSPACE },
	{ 0x866b15, KEY_ENTER },

	{ 0x866b1d, KEY_UP },
	{ 0x866b1e, KEY_DOWN },
	{ 0x866b0e, KEY_LEFT },
	{ 0x866b0f, KEY_RIGHT },

	{ 0x866b18, KEY_RECORD },
	{ 0x866b1a, KEY_STOP },
};

/* Key codes for the EvolutePC TVWay+ remote */
static struct rc_map_table evolutepc_table[] = {
	{ 0x7a00, KEY_MENU },
	{ 0x7a01, KEY_RECORD },
	{ 0x7a02, KEY_PLAY },
	{ 0x7a03, KEY_STOP },
	{ 0x7a10, KEY_CHANNELUP },
	{ 0x7a11, KEY_CHANNELDOWN },
	{ 0x7a12, KEY_VOLUMEUP },
	{ 0x7a13, KEY_VOLUMEDOWN },
	{ 0x7a40, KEY_POWER },
	{ 0x7a41, KEY_MUTE },
};

/* Key codes for the Elgato EyeTV Diversity silver remote */
static struct rc_map_table elgato_table[] = {
	{ 0x4501, KEY_POWER },
	{ 0x4502, KEY_MUTE },
	{ 0x4503, KEY_NUMERIC_1 },
	{ 0x4504, KEY_NUMERIC_2 },
	{ 0x4505, KEY_NUMERIC_3 },
	{ 0x4506, KEY_NUMERIC_4 },
	{ 0x4507, KEY_NUMERIC_5 },
	{ 0x4508, KEY_NUMERIC_6 },
	{ 0x4509, KEY_NUMERIC_7 },
	{ 0x450a, KEY_NUMERIC_8 },
	{ 0x450b, KEY_NUMERIC_9 },
	{ 0x450c, KEY_LAST },
	{ 0x450d, KEY_NUMERIC_0 },
	{ 0x450e, KEY_ENTER },
	{ 0x450f, KEY_RED },
	{ 0x4510, KEY_CHANNELUP },
	{ 0x4511, KEY_GREEN },
	{ 0x4512, KEY_VOLUMEDOWN },
	{ 0x4513, KEY_OK },
	{ 0x4514, KEY_VOLUMEUP },
	{ 0x4515, KEY_YELLOW },
	{ 0x4516, KEY_CHANNELDOWN },
	{ 0x4517, KEY_BLUE },
	{ 0x4518, KEY_LEFT }, /* Skip backwards */
	{ 0x4519, KEY_PLAYPAUSE },
	{ 0x451a, KEY_RIGHT }, /* Skip forward */
	{ 0x451b, KEY_REWIND },
	{ 0x451c, KEY_L }, /* Live */
	{ 0x451d, KEY_FASTFORWARD },
	{ 0x451e, KEY_STOP }, /* 'Reveal' for Teletext */
	{ 0x451f, KEY_MENU }, /* KEY_TEXT for Teletext */
	{ 0x4540, KEY_RECORD }, /* Font 'Size' for Teletext */
	{ 0x4541, KEY_SCREEN }, /*  Full screen toggle, 'Hold' for Teletext */
	{ 0x4542, KEY_SELECT }, /* Select video input, 'Select' for Teletext */
};

static struct rc_map_list pixelview_map = {
	.map = {
		.scan     = pixelview_table,
		.size     = ARRAY_SIZE(pixelview_table),
		.rc_proto = RC_PROTO_NEC,
		.name     = "RC_MAP_PIXELVIEW",
	}
};

static struct rc_map_list evolutepc_map = {
	.map = {
		.scan     = evolutepc_table,
		.size     = ARRAY_SIZE(evolutepc_table),
		.rc_proto = RC_PROTO_NEC,
		.name     = "RC_MAP_EVOLUTEPC",
	}
};

static struct rc_map_list elgato_map = {
	.map = {
		.scan     = elgato_table,
		.size     = ARRAY_SIZE(elgato_table),
		.rc_proto = RC_PROTO_NEC,
		.name     = "RC_MAP_ELGATO",
	}
};

static int __init init_rc_map(void)
{
	int ret;

	ret = rc_map_register(&pixelview_map);
	if (ret)
		return ret;

	ret = rc_map_register(&evolutepc_map);
	if (ret)
		goto unregister_pixelview;

	ret = rc_map_register(&elgato_map);
	if (ret)
		goto unregister_evolutepc;

	return 0;

unregister_evolutepc:
	rc_map_unregister(&evolutepc_map);
unregister_pixelview:
	rc_map_unregister(&pixelview_map);

	return ret;
}

static void __exit exit_rc_map(void)
{
	rc_map_unregister(&elgato_map);
	rc_map_unregister(&evolutepc_map);
	rc_map_unregister(&pixelview_map);
}

module_init(init_rc_map)
module_exit(exit_rc_map)

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mauro Carvalho Chehab");
MODULE_DESCRIPTION("dib0700-nec remote controller keytable");
