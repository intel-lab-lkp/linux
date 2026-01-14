// SPDX-License-Identifier: GPL-2.0
/*
 * SCMI - System Telemetry Driver
 *
 * Copyright (C) 2026 ARM Ltd.
 */

#include <linux/atomic.h>
#include <linux/bitfield.h>
#include <linux/ctype.h>
#include <linux/dcache.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/kstrtox.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/scmi_protocol.h>
#include <linux/slab.h>
#include <linux/sprintf.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include <uapi/linux/scmi.h>

#define TLM_FS_MAGIC		0x75C01C80
#define TLM_FS_NAME		"stlmfs"
#define TLM_FS_MNT		"arm_telemetry"

#define MAX_AVAILABLE_INTERV_CHAR_LENGTH	25
#define MAX_BULK_LINE_CHAR_LENGTH		64

static struct kmem_cache *stlmfs_inode_cachep;

static DEFINE_MUTEX(stlmfs_mtx);
static struct super_block *stlmfs_sb;

static atomic_t scmi_tlm_instance_count = ATOMIC_INIT(0);

struct scmi_tlm_setup;

struct scmi_tlm_priv {
	char *buf;
	size_t buf_sz;
	int buf_len;
	int (*bulk_retrieve)(struct scmi_tlm_setup *tsp,
			     int res_id, int *num_samples,
			     struct scmi_telemetry_de_sample *samples);
};

/**
 * struct scmi_tlm_buffer  - Output Telemetry buffer descriptor
 * @used: Current number of used bytes in @buf
 * @buf: Actual buffer for output data
 *
 * This describes an output buffer which will be made available to each r/w
 * entry file_operations.
 */
struct scmi_tlm_buffer {
	size_t used;
#define SCMI_TLM_MAX_BUF_SZ	128
	unsigned char buf[SCMI_TLM_MAX_BUF_SZ];
};

/**
 * struct scmi_tlm_setup  - Telemetry setup descriptor
 * @dev: A reference to the related device
 * @ph: A reference to the protocol handle to be used with the ops
 * @rinfo: A reference to the resource info descriptor
 * @ops: A reference to the protocol ops
 */
struct scmi_tlm_setup {
	struct device *dev;
	struct scmi_protocol_handle *ph;
	const struct scmi_telemetry_res_info __private *rinfo;
	const struct scmi_telemetry_proto_ops *ops;
};

/**
 * struct scmi_tlm_class  - Telemetry class descriptor
 * @name: A string to be used for filesystem dentry name.
 * @mode: Filesystem mode mask.
 * @flags: Optional misc flags that can slighly modify provided @f_op behaviour;
 *	   this way the same @scmi_tlm_class can be used to describe multiple
 *	   entries in the filesystem whose @f_op behaviour is very similar.
 * @f_op: Optional file ops attached to this object. Used to initialized inodes.
 * @i_op: Optional inode ops attached to this object. Used to initialize inodes.
 *
 * This structure describes a class of telemetry entities that will be
 * associated with filesystem inodes having the same behaviour, i.e. the same
 * @f_op and @i_op: this way it will be possible to statically define a set of
 * common descriptors to describe all the possible behaviours and then link it
 * to the effective inodes that will be created to support the set of DEs
 * effectively discovered at run-time via SCMI.
 */
struct scmi_tlm_class {
	const char *name;
	umode_t mode;
	int flags;
#define	TLM_IS_STATE	BIT(0)
#define	TLM_IS_GROUP	BIT(1)
#define	TLM_IS_DYNAMIC	BIT(2)
#define IS_STATE(_f)	((_f) & TLM_IS_STATE)
#define IS_GROUP(_f)	((_f) & TLM_IS_GROUP)
#define IS_DYNAMIC(_f)	((_f) & TLM_IS_DYNAMIC)
	const struct file_operations *f_op;
	const struct inode_operations *i_op;
};

#define TLM_ANON_CLASS(_n, _f, _m, _fo, _io)	\
	{					\
		.name = _n,			\
		.flags = _f,			\
		.f_op = _fo,			\
		.i_op = _io,			\
		.mode = _m,			\
	}

#define DEFINE_TLM_CLASS(_tag, _ns, _fl, _mo, _fop, _iop)	\
	static const struct scmi_tlm_class _tag =		\
		TLM_ANON_CLASS(_ns, _fl, _mo, _fop, _iop)

/**
 * struct scmi_tlm_inode  - Telemetry node descriptor
 * @tsp: A reference to a structure holding data needed to interact with
 *	 the SCMI instance associated to this inode.
 * @cls: A reference to the @scmi_tlm_class describing the behaviour of this
 *	 inode.
 * @priv: Generic private data reference.
 * @de: SCMI DE data reference.
 * @grp: SCMI Group data reference.
 * @info: SCMI instance information data reference.
 * @vfs_inode: The embedded VFS inode that will be initialized and plugged
 *	       into the live filesystem at mount time.
 *
 * This structure is used to describe each SCMI Telemetry entity discovered
 * at probe time, store its related SCMI data, and link to the proper
 * telemetry class @scmi_tlm_class.
 */
struct scmi_tlm_inode {
	struct scmi_tlm_setup *tsp;
	const struct scmi_tlm_class *cls;
	union {
		const void *priv;
		const struct scmi_telemetry_de *de;
		const struct scmi_telemetry_group *grp;
		const struct scmi_telemetry_info *info;
	};
	struct inode vfs_inode;
};

#define to_tlm_inode(t)	container_of(t, struct scmi_tlm_inode, vfs_inode)

#define	MAX_INST_NAME		32

#define TOP_NODES_NUM		32
#define NODES_PER_DE_NUM	12
#define NODES_PER_GRP_NUM	 9

/**
 * struct scmi_tlm_instance  - Telemetry instance descriptor
 * @id: Progressive number identifying this probed instance; it will be used
 *	to name the top node at the root of this instance.
 * @res_enumerated: A flag to indicate if full resources enumeration has been
 *		    successfully performed.
 * @name: Name to be used for the top root node of the instance. (tlm_<id>)
 * @node: A node to link this in the list of all instances.
 * @sb: A reference to the current super_block.
 * @tsp: A reference to the SCMI instance data.
 * @top_cls: A class to represent the top node behaviour.
 * @top_dentry: A reference to the top dentry for this instance.
 * @des_dentry: A reference to the DES dentry for this instance.
 * @grps_dentry: A reference to the groups dentry for this instance.
 * @compo_dentry: A reference to the components dentry for this instance.
 * @info: A handy reference to this instance SCMI Telemetry info data.
 *
 */
struct scmi_tlm_instance {
	int id;
	bool res_enumerated;
	char name[MAX_INST_NAME];
	struct list_head node;
	struct super_block *sb;
	struct scmi_tlm_setup *tsp;
	struct scmi_tlm_class top_cls;
	struct dentry *top_dentry;
	struct dentry *des_dentry;
	struct dentry *grps_dentry;
	struct dentry *compo_dentry;
	const struct scmi_telemetry_info *info;
};

static int scmi_telemetry_instance_register(struct super_block *sb,
					    struct scmi_tlm_instance *ti);

static LIST_HEAD(scmi_telemetry_instances);

#define TYPES_ARRAY_SZ		256

static const char *compo_types[TYPES_ARRAY_SZ] = {
	"unspec",
	"cpu",
	"cluster",
	"gpu",
	"npu",
	"interconnnect",
	"mem_cntrl",
	"l1_cache",
	"l2_cache",
	"l3_cache",
	"ll_cache",
	"sys_cache",
	"disp_cntrl",
	"ipu",
	"chiplet",
	"package",
	"soc",
	"system",
	"smcu",
	"accel",
	"battery",
	"charger",
	"pmic",
	"board",
	"memory",
	"periph",
	"periph_subc",
	"lid",
	"display",
	"res_29",
	"res_30",
	"res_31",
	"res_32",
	"res_33",
	"res_34",
	"res_35",
	"res_36",
	"res_37",
	"res_38",
	"res_39",
	"res_40",
	"res_41",
	"res_42",
	"res_43",
	"res_44",
	"res_45",
	"res_46",
	"res_47",
	"res_48",
	"res_49",
	"res_50",
	"res_51",
	"res_52",
	"res_53",
	"res_54",
	"res_55",
	"res_56",
	"res_57",
	"res_58",
	"res_59",
	"res_60",
	"res_61",
	"res_62",
	"res_63",
	"res_64",
	"res_65",
	"res_66",
	"res_67",
	"res_68",
	"res_69",
	"res_70",
	"res_71",
	"res_72",
	"res_73",
	"res_74",
	"res_75",
	"res_76",
	"res_77",
	"res_78",
	"res_79",
	"res_80",
	"res_81",
	"res_82",
	"res_83",
	"res_84",
	"res_85",
	"res_86",
	"res_87",
	"res_88",
	"res_89",
	"res_90",
	"res_91",
	"res_92",
	"res_93",
	"res_94",
	"res_95",
	"res_96",
	"res_97",
	"res_98",
	"res_99",
	"res_100",
	"res_101",
	"res_102",
	"res_103",
	"res_104",
	"res_105",
	"res_106",
	"res_107",
	"res_108",
	"res_109",
	"res_110",
	"res_111",
	"res_112",
	"res_113",
	"res_114",
	"res_115",
	"res_116",
	"res_117",
	"res_118",
	"res_119",
	"res_120",
	"res_121",
	"res_122",
	"res_123",
	"res_124",
	"res_125",
	"res_126",
	"res_127",
	"res_128",
	"res_129",
	"res_130",
	"res_131",
	"res_132",
	"res_133",
	"res_134",
	"res_135",
	"res_136",
	"res_137",
	"res_138",
	"res_139",
	"res_140",
	"res_141",
	"res_142",
	"res_143",
	"res_144",
	"res_145",
	"res_146",
	"res_147",
	"res_148",
	"res_149",
	"res_150",
	"res_151",
	"res_152",
	"res_153",
	"res_154",
	"res_155",
	"res_156",
	"res_157",
	"res_158",
	"res_159",
	"res_160",
	"res_161",
	"res_162",
	"res_163",
	"res_164",
	"res_165",
	"res_166",
	"res_167",
	"res_168",
	"res_169",
	"res_170",
	"res_171",
	"res_172",
	"res_173",
	"res_174",
	"res_175",
	"res_176",
	"res_177",
	"res_178",
	"res_179",
	"res_180",
	"res_181",
	"res_182",
	"res_183",
	"res_184",
	"res_185",
	"res_186",
	"res_187",
	"res_188",
	"res_189",
	"res_190",
	"res_191",
	"res_192",
	"res_193",
	"res_194",
	"res_195",
	"res_196",
	"res_197",
	"res_198",
	"res_199",
	"res_200",
	"res_201",
	"res_202",
	"res_203",
	"res_204",
	"res_205",
	"res_206",
	"res_207",
	"res_208",
	"res_209",
	"res_210",
	"res_211",
	"res_212",
	"res_213",
	"res_214",
	"res_215",
	"res_216",
	"res_217",
	"res_218",
	"res_219",
	"res_220",
	"res_221",
	"res_222",
	"res_223",
	"oem_224",
	"oem_225",
	"oem_226",
	"oem_227",
	"oem_228",
	"oem_229",
	"oem_230",
	"oem_231",
	"oem_232",
	"oem_233",
	"oem_234",
	"oem_235",
	"oem_236",
	"oem_237",
	"oem_238",
	"oem_239",
	"oem_240",
	"oem_241",
	"oem_242",
	"oem_243",
	"oem_244",
	"oem_245",
	"oem_246",
	"oem_247",
	"oem_248",
	"oem_249",
	"oem_250",
	"oem_251",
	"oem_252",
	"oem_253",
	"oem_254",
	"oem_255",
};

static const char *unit_types[TYPES_ARRAY_SZ] = {
	"none",
	"unspec",
	"celsius",
	"fahrenheit",
	"kelvin",
	"volts",
	"amps",
	"watts",
	"joules",
	"coulombs",
	"va",
	"nits",
	"lumens",
	"lux",
	"candelas",
	"kpa",
	"psi",
	"newtons",
	"cfm",
	"rpm",
	"hertz",
	"seconds",
	"minutes",
	"hours",
	"days",
	"weeks",
	"mils",
	"inches",
	"feet",
	"cubic_inches",
	"cubic_feet",
	"meters",
	"cubic_centimeters",
	"cubic_meters",
	"liters",
	"fluid_ounces",
	"radians",
	"steradians",
	"revolutions",
	"cycles",
	"gravities",
	"ounces",
	"pounds",
	"foot_pounds",
	"ounce_inches",
	"gauss",
	"gilberts",
	"henries",
	"farads",
	"ohms",
	"siemens",
	"moles",
	"becquerels",
	"ppm",
	"decibels",
	"dba",
	"dbc",
	"grays",
	"sieverts",
	"color_temp_kelvin",
	"bits",
	"bytes",
	"words",
	"dwords",
	"qwords",
	"percentage",
	"pascals",
	"counts",
	"grams",
	"newton_meters",
	"hits",
	"misses",
	"retries",
	"overruns",
	"underruns",
	"collisions",
	"packets",
	"messages",
	"chars",
	"errors",
	"corrected_err",
	"uncorrectable_err",
	"square_mils",
	"square_inches",
	"square_feet",
	"square_centimeters",
	"square_meters",
	"radians_per_secs",
	"beats_per_minute",
	"meters_per_secs_squared",
	"meters_per_secs",
	"cubic_meter_per_secs",
	"millimeters_mercury",
	"radians_per_secs_squared",
	"state",
	"bps",
	"res_96",
	"res_97",
	"res_98",
	"res_99",
	"res_100",
	"res_101",
	"res_102",
	"res_103",
	"res_104",
	"res_105",
	"res_106",
	"res_107",
	"res_108",
	"res_109",
	"res_110",
	"res_111",
	"res_112",
	"res_113",
	"res_114",
	"res_115",
	"res_116",
	"res_117",
	"res_118",
	"res_119",
	"res_120",
	"res_121",
	"res_122",
	"res_123",
	"res_124",
	"res_125",
	"res_126",
	"res_127",
	"res_128",
	"res_129",
	"res_130",
	"res_131",
	"res_132",
	"res_133",
	"res_134",
	"res_135",
	"res_136",
	"res_137",
	"res_138",
	"res_139",
	"res_140",
	"res_141",
	"res_142",
	"res_143",
	"res_144",
	"res_145",
	"res_146",
	"res_147",
	"res_148",
	"res_149",
	"res_150",
	"res_151",
	"res_152",
	"res_153",
	"res_154",
	"res_155",
	"res_156",
	"res_157",
	"res_158",
	"res_159",
	"res_160",
	"res_161",
	"res_162",
	"res_163",
	"res_164",
	"res_165",
	"res_166",
	"res_167",
	"res_168",
	"res_169",
	"res_170",
	"res_171",
	"res_172",
	"res_173",
	"res_174",
	"res_175",
	"res_176",
	"res_177",
	"res_178",
	"res_179",
	"res_180",
	"res_181",
	"res_182",
	"res_183",
	"res_184",
	"res_185",
	"res_186",
	"res_187",
	"res_188",
	"res_189",
	"res_190",
	"res_191",
	"res_192",
	"res_193",
	"res_194",
	"res_195",
	"res_196",
	"res_197",
	"res_198",
	"res_199",
	"res_200",
	"res_201",
	"res_202",
	"res_203",
	"res_204",
	"res_205",
	"res_206",
	"res_207",
	"res_208",
	"res_209",
	"res_210",
	"res_211",
	"res_212",
	"res_213",
	"res_214",
	"res_215",
	"res_216",
	"res_217",
	"res_218",
	"res_219",
	"res_220",
	"res_221",
	"res_222",
	"res_223",
	"res_224",
	"res_225",
	"res_226",
	"res_227",
	"res_228",
	"res_229",
	"res_230",
	"res_231",
	"res_232",
	"res_233",
	"res_234",
	"res_235",
	"res_236",
	"res_237",
	"res_238",
	"res_239",
	"res_240",
	"res_241",
	"res_242",
	"res_243",
	"res_244",
	"res_245",
	"res_246",
	"res_247",
	"res_248",
	"res_249",
	"res_250",
	"res_251",
	"res_252",
	"res_253",
	"res_254",
	"oem_unit",
};

static struct inode *stlmfs_get_inode(struct super_block *sb)
{
	struct inode *inode = new_inode(sb);

	if (inode) {
		inode->i_ino = get_next_ino();
		simple_inode_init_ts(inode);
	}

	return inode;
}

static int stlmfs_failed_creating(struct dentry *dentry)
{
	simple_done_creating(dentry);

	return -ENOMEM;
}

static struct dentry *
stlmfs_create_dentry(struct super_block *sb, struct scmi_tlm_setup *tsp,
		     struct dentry *parent, const struct scmi_tlm_class *cls,
		     const void *priv)
{
	struct scmi_tlm_inode *tlmi;
	struct dentry *dentry;
	struct inode *inode;

	if (!parent)
		parent = sb->s_root;

	if (IS_ERR(parent))
		return parent;

	dentry = simple_start_creating(parent, cls->name);
	if (IS_ERR(dentry))
		return dentry;

	inode = stlmfs_get_inode(sb);
	if (unlikely(!inode)) {
		dev_err(tsp->dev,
			"out of free dentries, cannot create '%s'",
			cls->name);
		return ERR_PTR(stlmfs_failed_creating(dentry));
	}

	if (S_ISDIR(cls->mode)) {
		inode->i_op = cls->i_op ?: &simple_dir_inode_operations;
		inode->i_fop = cls->f_op ?: &simple_dir_operations;
	} else {
		inode->i_op = cls->i_op ?: &simple_dir_inode_operations;
		inode->i_fop = cls->f_op;
	}

	inode->i_mode = cls->mode;
	inode_init_owner(&nop_mnt_idmap, inode, NULL, inode->i_mode);
	inode->i_private = (void *)priv;

	tlmi = to_tlm_inode(inode);

	tlmi->cls = cls;
	tlmi->tsp = tsp;
	tlmi->priv = priv;

	d_make_persistent(dentry, inode);

	simple_done_creating(dentry);

	return dentry;
}

static inline int
__scmi_tlm_generic_open(struct inode *ino, struct file *filp,
			int (*bulk_op)(struct scmi_tlm_setup *tsp,
				       int res_id, int *num_samples,
				       struct scmi_telemetry_de_sample *samples))
{
	struct scmi_tlm_priv *tp;

	tp = kzalloc(sizeof(*tp), GFP_KERNEL);
	if (!tp)
		return -ENOMEM;

	tp->bulk_retrieve = bulk_op;

	filp->private_data = tp;

	return nonseekable_open(ino, filp);
}

static int scmi_tlm_priv_release(struct inode *ino, struct file *filp)
{
	struct scmi_tlm_priv *tp = filp->private_data;

	kfree(tp->buf);
	kfree(tp);

	return 0;
}

static inline const struct scmi_telemetry_res_info *
scmi_telemetry_res_info_get(struct scmi_tlm_setup *tsp)
{
	const struct scmi_telemetry_res_info *rinfo;

	if (tsp->rinfo)
		return ACCESS_PRIVATE(tsp, rinfo);

	rinfo = tsp->ops->res_get(tsp->ph);
	/* Cache the retrieved resource info value */
	smp_store_mb(tsp->rinfo, rinfo);

	return rinfo;
}

static ssize_t scmi_tlm_all_des_write(struct file *filp,
				      const char __user *buf,
				      size_t count, loff_t *ppos)
{
	struct scmi_tlm_inode *tlmi = to_tlm_inode(file_inode(filp));
	struct scmi_tlm_setup *tsp = tlmi->tsp;
	const struct scmi_tlm_class *cls = tlmi->cls;
	bool enable;
	int ret;

	ret = kstrtobool_from_user(buf, count, &enable);
	if (ret)
		return ret;

	/* When !IS_STATE imply that is a tstamp_enable operation */
	if (IS_STATE(cls->flags) && !enable) {
		ret = tsp->ops->all_disable(tsp->ph, false);
		if (ret)
			return ret;
	} else {
		const struct scmi_telemetry_res_info *rinfo;

		rinfo = scmi_telemetry_res_info_get(tsp);
		if (!rinfo)
			return -ENODEV;

		for (int i = 0; i < rinfo->num_des; i++) {
			ret = tsp->ops->state_set(tsp->ph, false,
						  rinfo->des[i]->info->id,
						  IS_STATE(cls->flags) ? &enable : NULL,
						  !IS_STATE(cls->flags) ? &enable : NULL);
			if (ret)
				return ret;
		}
	}

	return count;
}

static const struct file_operations all_des_fops = {
	.open = nonseekable_open,
	.write = scmi_tlm_all_des_write,
};

static ssize_t scmi_tlm_obj_enable_write(struct file *filp,
					 const char __user *buf,
					 size_t count, loff_t *ppos)
{
	struct scmi_tlm_inode *tlmi = to_tlm_inode(file_inode(filp));
	struct scmi_tlm_setup *tsp = tlmi->tsp;
	const struct scmi_tlm_class *cls = tlmi->cls;
	bool enabled, is_group = IS_GROUP(cls->flags);
	int ret, res_id;

	ret = kstrtobool_from_user(buf, count, &enabled);
	if (ret)
		return ret;

	res_id = !is_group ? tlmi->de->info->id : tlmi->grp->info->id;
	ret = tsp->ops->state_set(tsp->ph, is_group, res_id,
				  IS_STATE(cls->flags) ? &enabled : NULL,
				  !IS_STATE(cls->flags) ? &enabled : NULL);
	if (ret)
		return ret;

	return count;
}

static ssize_t scmi_tlm_obj_enable_read(struct file *filp, char __user *buf,
					size_t count, loff_t *ppos)
{
	struct scmi_tlm_inode *tlmi = to_tlm_inode(file_inode(filp));
	const bool *enabled_state, *tstamp_enabled_state;
	char o_buf[2];
	bool enabled;

	if (!IS_GROUP(tlmi->cls->flags)) {
		enabled_state = &tlmi->de->enabled;
		tstamp_enabled_state = &tlmi->de->tstamp_enabled;
	} else {
		enabled_state = &tlmi->grp->enabled;
		tstamp_enabled_state = &tlmi->grp->tstamp_enabled;
	}

	enabled = IS_STATE(tlmi->cls->flags) ? *enabled_state : *tstamp_enabled_state;
	o_buf[0] = enabled ? 'Y' : 'N';
	o_buf[1] = '\n';

	return simple_read_from_buffer(buf, count, ppos, o_buf, 2);
}

static const struct file_operations obj_enable_fops = {
	.open = nonseekable_open,
	.write = scmi_tlm_obj_enable_write,
	.read = scmi_tlm_obj_enable_read,
};

static int scmi_tlm_open(struct inode *ino, struct file *filp)
{
	struct scmi_tlm_buffer *data;

	/* Allocate some per-open buffer */
	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	filp->private_data = data;

	return nonseekable_open(ino, filp);
}

static int scmi_tlm_release(struct inode *ino, struct file *filp)
{
	kfree(filp->private_data);

	return 0;
}

static ssize_t
scmi_tlm_update_interval_read(struct file *filp, char __user *buf,
			      size_t count, loff_t *ppos)
{
	struct scmi_tlm_inode *tlmi = to_tlm_inode(file_inode(filp));
	struct scmi_tlm_buffer *data = filp->private_data;
	unsigned int active_update_interval;

	if (!data)
		return 0;

	if (!IS_GROUP(tlmi->cls->flags))
		active_update_interval = tlmi->info->active_update_interval;
	else
		active_update_interval = tlmi->grp->active_update_interval;

	if (!data->used)
		data->used =
			scnprintf(data->buf, SCMI_TLM_MAX_BUF_SZ, "%u, %d\n",
				  SCMI_TLM_GET_UPDATE_INTERVAL_SECS(active_update_interval),
				  SCMI_TLM_GET_UPDATE_INTERVAL_EXP(active_update_interval));

	return simple_read_from_buffer(buf, count, ppos, data->buf, data->used);
}

static ssize_t
scmi_tlm_update_interval_write(struct file *filp, const char __user *buf,
			       size_t count, loff_t *ppos)
{
	struct scmi_tlm_inode *tlmi = to_tlm_inode(file_inode(filp));
	struct scmi_tlm_setup *tsp = tlmi->tsp;
	struct scmi_tlm_buffer *data = filp->private_data;
	bool is_group = IS_GROUP(tlmi->cls->flags);
	unsigned int update_interval_ms = 0, secs = 0;
	int ret, grp_id, exp = -3;
	char *p, *token;

	if (count >= SCMI_TLM_MAX_BUF_SZ)
		return -ENOSPC;

	if (copy_from_user(data->buf, buf, count))
		return -EFAULT;

	p = data->buf;
	token = strsep(&p, ",");
	if (!token || iscntrl(token[0]))
		return -EINVAL;

	ret = kstrtouint(strim(token), 0, &secs);
	if (ret)
		return ret;

	if (p) {
		token = p;
		if (!token || iscntrl(token[0]))
			return -EINVAL;

		ret = kstrtoint(strim(token), 0, &exp);
		if (ret)
			return ret;
	}

	update_interval_ms = SCMI_TLM_BUILD_UPDATE_INTERVAL(secs, exp);

	grp_id = !is_group ? SCMI_TLM_GRP_INVALID : tlmi->grp->info->id;
	ret = tsp->ops->collection_configure(tsp->ph, grp_id, !is_group, NULL,
					     &update_interval_ms, NULL);
	if (ret)
		return ret;

	return count;
}

static const struct file_operations current_interval_fops = {
	.open = scmi_tlm_open,
	.read = scmi_tlm_update_interval_read,
	.write = scmi_tlm_update_interval_write,
	.release = scmi_tlm_release,
};

static ssize_t scmi_tlm_de_read(struct file *filp, char __user *buf,
				size_t count, loff_t *ppos)
{
	struct scmi_tlm_inode *tlmi = to_tlm_inode(file_inode(filp));
	struct scmi_tlm_setup *tsp = tlmi->tsp;
	struct scmi_tlm_buffer *data = filp->private_data;
	int ret;

	if (!data)
		return 0;

	if (!data->used) {
		struct scmi_telemetry_de_sample sample;

		sample.id = tlmi->de->info->id;
		ret = tsp->ops->de_data_read(tsp->ph, &sample);
		if (ret)
			return ret;

		data->used = scnprintf(data->buf, SCMI_TLM_MAX_BUF_SZ,
				       "%llu: %016llX\n", sample.tstamp,
				       sample.val);
	}

	return simple_read_from_buffer(buf, count, ppos, data->buf, data->used);
}

static const struct file_operations de_read_fops = {
	.open = scmi_tlm_open,
	.read = scmi_tlm_de_read,
	.release = scmi_tlm_release,
};

static ssize_t
scmi_tlm_enable_read(struct file *filp, char __user *buf, size_t count,
		     loff_t *ppos)
{
	struct scmi_tlm_inode *tlmi = to_tlm_inode(file_inode(filp));
	char o_buf[2];

	o_buf[0] = tlmi->info->enabled ? 'Y' : 'N';
	o_buf[1] = '\n';

	return simple_read_from_buffer(buf, count, ppos, o_buf, 2);
}

static ssize_t
scmi_tlm_enable_write(struct file *filp, const char __user *buf, size_t count,
		      loff_t *ppos)
{
	struct scmi_tlm_inode *tlmi = to_tlm_inode(file_inode(filp));
	enum scmi_telemetry_collection mode = SCMI_TLM_ONDEMAND;
	struct scmi_tlm_setup *tsp = tlmi->tsp;
	bool enabled;
	int ret;

	ret = kstrtobool_from_user(buf, count, &enabled);
	if (ret)
		return ret;

	ret = tsp->ops->collection_configure(tsp->ph, SCMI_TLM_GRP_INVALID, true,
					     &enabled, NULL, &mode);
	if (ret)
		return ret;

	return count;
}

static const struct file_operations tlm_enable_fops = {
	.open = nonseekable_open,
	.read = scmi_tlm_enable_read,
	.write = scmi_tlm_enable_write,
};

static ssize_t
scmi_tlm_intrv_discrete_read(struct file *filp, char __user *buf,
			     size_t count, loff_t *ppos)
{
	struct scmi_tlm_inode *tlmi = to_tlm_inode(file_inode(filp));
	bool discrete;
	char o_buf[2];

	discrete = !IS_GROUP(tlmi->cls->flags) ?
		tlmi->info->intervals->discrete : tlmi->grp->intervals->discrete;

	o_buf[0] = discrete ? 'Y' : 'N';
	o_buf[1] = '\n';

	return simple_read_from_buffer(buf, count, ppos, o_buf, 2);
}

static const struct file_operations intrv_discrete_fops = {
	.open = nonseekable_open,
	.read = scmi_tlm_intrv_discrete_read,
};

static ssize_t
scmi_tlm_reset_write(struct file *filp, const char __user *buf, size_t count,
		     loff_t *ppos)
{
	struct scmi_tlm_inode *tlmi = to_tlm_inode(file_inode(filp));
	int ret;

	ret = tlmi->tsp->ops->reset(tlmi->tsp->ph);
	if (ret)
		return ret;

	return count;
}

static const struct file_operations reset_fops = {
	.open = nonseekable_open,
	.write = scmi_tlm_reset_write,
};

static int sa_u32_get(void *data, u64 *val)
{
	*val = *(u32 *)data;
	return 0;
}

static int sa_u32_set(void *data, u64 val)
{
	*(u32 *)data = val;
	return 0;
}

static int sa_u32_open(struct inode *ino, struct file *filp)
{
	return simple_attr_open(ino, filp, sa_u32_get, sa_u32_set, "%u\n");
}

static int sa_s32_open(struct inode *ino, struct file *filp)
{
	return simple_attr_open(ino, filp, sa_u32_get, sa_u32_set, "%d\n");
}

static int sa_x32_open(struct inode *ino, struct file *filp)
{
	return simple_attr_open(ino, filp, sa_u32_get, sa_u32_set, "0x%X\n");
}

static const struct file_operations sa_x32_ro_fops = {
	.open = sa_x32_open,
	.read = simple_attr_read,
	.release = simple_attr_release,
};

static const struct file_operations sa_u32_ro_fops = {
	.open = sa_u32_open,
	.read = simple_attr_read,
	.release = simple_attr_release,
};

static const struct file_operations sa_s32_ro_fops = {
	.open = sa_s32_open,
	.read = simple_attr_read,
	.release = simple_attr_release,
};

static ssize_t
scmi_de_impl_version_read(struct file *filp, char __user *buf, size_t count,
			  loff_t *ppos)
{
	struct scmi_tlm_inode *tlmi = to_tlm_inode(file_inode(filp));
	struct scmi_tlm_buffer *data = filp->private_data;

	if (!data)
		return 0;

	if (!data->used)
		data->used = scnprintf(data->buf, SCMI_TLM_MAX_BUF_SZ,
				       "%pUL\n", tlmi->info->base.de_impl_version);

	return simple_read_from_buffer(buf, count, ppos, data->buf, data->used);
}

static const struct file_operations de_impl_vers_fops = {
	.open = scmi_tlm_open,
	.read = scmi_de_impl_version_read,
	.release = scmi_tlm_release,
};

static ssize_t scmi_string_read(struct file *filp, char __user *buf,
				size_t count, loff_t *ppos)
{
	struct scmi_tlm_priv *tp = filp->private_data;

	/*
	 * Note that tp->buf is a scratch buffer, filled once, used to support
	 * multiple chunked read and freed in scmi_tlm_priv_release.
	 */
	if (!tp->buf) {
		struct scmi_tlm_inode *tlmi = to_tlm_inode(file_inode(filp));
		const char *str = tlmi->priv;

		tp->buf = kasprintf(GFP_KERNEL, "%s\n", str);
		if (!tp->buf)
			return -ENOMEM;

		tp->buf_len = strlen(tp->buf) + 1;
	}

	return simple_read_from_buffer(buf, count, ppos, tp->buf, tp->buf_len);
}

static int scmi_tlm_priv_open(struct inode *ino, struct file *filp)
{
	return __scmi_tlm_generic_open(ino, filp, NULL);
}

static const struct file_operations string_ro_fops = {
	.open = scmi_tlm_priv_open,
	.read = scmi_string_read,
	.release = scmi_tlm_priv_release,
};

static ssize_t scmi_available_interv_read(struct file *filp, char __user *buf,
					  size_t count, loff_t *ppos)
{
	struct scmi_tlm_priv *tp = filp->private_data;

	/*
	 * Note that tp->buf is a scratch buffer, filled once, used to support
	 * multiple chunked read and freed in scmi_tlm_priv_release.
	 */
	if (!tp->buf) {
		struct scmi_tlm_inode *tlmi = to_tlm_inode(file_inode(filp));
		struct scmi_tlm_intervals *intervals;
		int len = 0;

		intervals = !IS_GROUP(tlmi->cls->flags) ?
			tlmi->info->intervals : tlmi->grp->intervals;
		tp->buf_len = intervals->num * MAX_AVAILABLE_INTERV_CHAR_LENGTH;
		tp->buf = kzalloc(tp->buf_len, GFP_KERNEL);
		if (!tp->buf)
			return -ENOMEM;

		for (int i = 0; i < intervals->num; i++) {
			u32 ivl;

			ivl = intervals->update_intervals[i];
			len += scnprintf(tp->buf + len, tp->buf_len - len,
					 "%u,%d ",
					 SCMI_TLM_GET_UPDATE_INTERVAL_SECS(ivl),
					 SCMI_TLM_GET_UPDATE_INTERVAL_EXP(ivl));
		}
		tp->buf[len - 1] = '\n';
	}

	return simple_read_from_buffer(buf, count, ppos, tp->buf, tp->buf_len);
}

static const struct file_operations available_interv_fops = {
	.open = scmi_tlm_priv_open,
	.read = scmi_available_interv_read,
	.release = scmi_tlm_priv_release,
};

static const struct scmi_tlm_class tlm_tops[] = {
	TLM_ANON_CLASS("all_des_enable", TLM_IS_STATE,
		       S_IFREG | S_IWUSR, &all_des_fops, NULL),
	TLM_ANON_CLASS("all_des_tstamp_enable", 0,
		       S_IFREG | S_IWUSR, &all_des_fops, NULL),
	TLM_ANON_CLASS("current_update_interval_ms", 0,
		       S_IFREG | S_IRUSR | S_IWUSR, &current_interval_fops, NULL),
	TLM_ANON_CLASS("intervals_discrete", 0,
		       S_IFREG | S_IRUSR, &intrv_discrete_fops, NULL),
	TLM_ANON_CLASS("available_update_intervals_ms", 0,
		       S_IFREG | S_IRUSR, &available_interv_fops, NULL),
	TLM_ANON_CLASS("de_implementation_version", 0,
		       S_IFREG | S_IRUSR, &de_impl_vers_fops, NULL),
	TLM_ANON_CLASS("tlm_enable", 0,
		       S_IFREG | S_IRUSR | S_IWUSR, &tlm_enable_fops, NULL),
	TLM_ANON_CLASS(NULL, 0, 0, NULL, NULL),
};

DEFINE_TLM_CLASS(reset_tlmo, "reset", 0, S_IFREG | S_IWUSR, &reset_fops, NULL);

DEFINE_TLM_CLASS(des_dir_cls, "des", 0,
		 S_IFDIR | S_IRWXU, NULL, NULL);
DEFINE_TLM_CLASS(name_tlmo, "name", 0,
		 S_IFREG | S_IRUSR, &string_ro_fops, NULL);
DEFINE_TLM_CLASS(ena_tlmo, "enable", TLM_IS_STATE,
		 S_IFREG | S_IRUSR | S_IWUSR, &obj_enable_fops, NULL);
DEFINE_TLM_CLASS(tstamp_ena_tlmo, "tstamp_enable", 0,
		 S_IFREG | S_IRUSR | S_IWUSR, &obj_enable_fops, NULL);
DEFINE_TLM_CLASS(type_tlmo, "type", 0,
		 S_IFREG | S_IRUSR, &sa_u32_ro_fops, NULL);
DEFINE_TLM_CLASS(unit_tlmo, "unit", 0,
		 S_IFREG | S_IRUSR, &sa_u32_ro_fops, NULL);
DEFINE_TLM_CLASS(unit_exp_tlmo, "unit_exp", 0,
		 S_IFREG | S_IRUSR, &sa_s32_ro_fops, NULL);
DEFINE_TLM_CLASS(instance_id_tlmo, "instance_id", 0,
		 S_IFREG | S_IRUSR, &sa_u32_ro_fops, NULL);
DEFINE_TLM_CLASS(compo_type_tlmo, "compo_type", 0,
		 S_IFREG | S_IRUSR, &sa_u32_ro_fops, NULL);
DEFINE_TLM_CLASS(compo_inst_id_tlmo, "compo_instance_id", 0,
		 S_IFREG | S_IRUSR, &sa_u32_ro_fops, NULL);
DEFINE_TLM_CLASS(tstamp_exp_tlmo, "tstamp_exp", 0,
		 S_IFREG | S_IRUSR, &sa_s32_ro_fops, NULL);
DEFINE_TLM_CLASS(persistent_tlmo, "persistent", 0,
		 S_IFREG | S_IRUSR, &sa_u32_ro_fops, NULL);
DEFINE_TLM_CLASS(value_tlmo, "value", 0,
		 S_IFREG | S_IRUSR, &de_read_fops, NULL);

static inline struct dentry *
stlmfs_lookup_by_name(struct dentry *parent, const char *dname)
{
	struct qstr qstr;

	qstr.name = dname;
	qstr.len = strlen(dname);
	qstr.hash = full_name_hash(parent, qstr.name, qstr.len);

	return d_lookup(parent, &qstr);
}

static int scmi_telemetry_de_populate(struct super_block *sb,
				      struct scmi_tlm_setup *tsp,
				      struct dentry *parent,
				      const struct scmi_telemetry_de *de,
				      bool fully_enumerated)
{
	struct scmi_tlm_de_info *dei = de->info;

	stlmfs_create_dentry(sb, tsp, parent, &ena_tlmo, de);
	stlmfs_create_dentry(sb, tsp, parent, &value_tlmo, de);
	if (!fully_enumerated)
		return 0;

	if (de->name_support)
		stlmfs_create_dentry(sb, tsp, parent, &name_tlmo, dei->name);

	if (de->tstamp_support) {
		stlmfs_create_dentry(sb, tsp, parent, &tstamp_ena_tlmo, de);
		stlmfs_create_dentry(sb, tsp, parent, &tstamp_exp_tlmo,
				     &dei->tstamp_exp);
	}

	stlmfs_create_dentry(sb, tsp, parent, &type_tlmo, &dei->type);
	stlmfs_create_dentry(sb, tsp, parent, &unit_tlmo, &dei->unit);
	stlmfs_create_dentry(sb, tsp, parent, &unit_exp_tlmo, &dei->unit_exp);
	stlmfs_create_dentry(sb, tsp, parent, &instance_id_tlmo, &dei->instance_id);
	stlmfs_create_dentry(sb, tsp, parent, &compo_type_tlmo, &dei->compo_type);
	stlmfs_create_dentry(sb, tsp, parent, &compo_inst_id_tlmo,
			     &dei->compo_instance_id);
	stlmfs_create_dentry(sb, tsp, parent, &persistent_tlmo, &dei->persistent);

	return 0;
}

static int
scmi_telemetry_des_lazy_enumerate(struct scmi_tlm_instance *ti,
				  const struct scmi_telemetry_res_info *rinfo)
{
	struct scmi_tlm_setup *tsp = ti->tsp;
	struct super_block *sb = ti->sb;

	for (int i = 0; i < rinfo->num_des; i++) {
		const struct scmi_telemetry_de *de = rinfo->des[i];
		struct dentry *de_dir_dentry;
		int ret;

		struct scmi_tlm_class *de_tlm_cls __free(kfree) =
			kzalloc(sizeof(*de_tlm_cls), GFP_KERNEL);
		if (!de_tlm_cls)
			return -ENOMEM;

		de_tlm_cls->name = kasprintf(GFP_KERNEL, "0x%08X", de->info->id);
		if (!de_tlm_cls->name)
			return -ENOMEM;

		de_tlm_cls->mode = S_IFDIR | S_IRWXU;
		de_tlm_cls->flags = TLM_IS_DYNAMIC;
		de_dir_dentry = stlmfs_create_dentry(sb, tsp, ti->des_dentry,
						     de_tlm_cls, de);

		ret = scmi_telemetry_de_populate(sb, tsp, de_dir_dentry, de,
						 rinfo->fully_enumerated);
		if (ret)
			return ret;

		retain_and_null_ptr(de_tlm_cls);
	}

	ti->res_enumerated = true;

	dev_info(tsp->dev, "Found %d Telemetry DE resources.\n", rinfo->num_des);

	return 0;
}

static int scmi_telemetry_des_initialize(struct scmi_tlm_instance *ti)
{
	const struct scmi_telemetry_res_info *rinfo;

	rinfo = scmi_telemetry_res_info_get(ti->tsp);
	if (!rinfo)
		return -ENODEV;

	return scmi_telemetry_des_lazy_enumerate(ti, rinfo);
}

DEFINE_TLM_CLASS(version_tlmo, "version", 0,
		 S_IFREG | S_IRUSR, &sa_x32_ro_fops, NULL);

static int scmi_tlm_bulk_on_demand(struct scmi_tlm_setup *tsp,
				   int res_id, int *num_samples,
				   struct scmi_telemetry_de_sample *samples)
{
	return tsp->ops->des_bulk_read(tsp->ph, res_id, num_samples, samples);
}

static int scmi_tlm_data_open(struct inode *ino, struct file *filp)
{
	return __scmi_tlm_generic_open(ino, filp, scmi_tlm_bulk_on_demand);
}

static int scmi_tlm_buffer_fill(struct device *dev, char *buf, size_t size,
				int *len, int num,
				struct scmi_telemetry_de_sample *samples)
{
	int idx, bytes = 0;

	/* Loop till there space for the next line */
	for (idx = 0; idx < num && size - bytes >= MAX_BULK_LINE_CHAR_LENGTH; idx++) {
		bytes += scnprintf(buf + bytes, size - bytes,
				   "0x%08X %llu %016llX\n", samples[idx].id,
				   samples[idx].tstamp, samples[idx].val);
	}

	if (idx < num) {
		dev_err(dev, "Bulk buffer truncated !\n");
		return -ENOSPC;
	}

	if (len)
		*len = bytes;

	return 0;
}

static int scmi_tlm_bulk_buffer_allocate_and_fill(struct scmi_tlm_inode *tlmi,
						  struct scmi_tlm_priv *tp)
{
	struct scmi_tlm_setup *tsp = tlmi->tsp;
	const struct scmi_tlm_class *cls = tlmi->cls;
	struct scmi_telemetry_de_sample *samples;
	bool is_group = IS_GROUP(cls->flags);
	int ret, num_samples, res_id;

	num_samples = !is_group ? tlmi->info->base.num_des :
		tlmi->grp->info->num_des;
	tp->buf_sz = num_samples * MAX_BULK_LINE_CHAR_LENGTH;
	tp->buf = kzalloc(tp->buf_sz, GFP_KERNEL);
	if (!tp->buf)
		return -ENOMEM;

	res_id = is_group ? tlmi->grp->info->id : SCMI_TLM_GRP_INVALID;
	samples = kcalloc(num_samples, sizeof(*samples), GFP_KERNEL);
	if (!samples) {
		kfree(tp->buf);
		return -ENOMEM;
	}

	ret = tp->bulk_retrieve(tsp, res_id, &num_samples, samples);
	if (ret) {
		kfree(tp->buf);
		kfree(samples);
		return ret;
	}

	/*
	 * Note that tp->buf is a scratch buffer, filled once, used to support
	 * multiple chunked read and freed in scmi_tlm_priv_release.
	 */
	ret = scmi_tlm_buffer_fill(tsp->dev, tp->buf, tp->buf_sz, &tp->buf_len,
				   num_samples, samples);
	kfree(samples);

	return ret;
}

static ssize_t scmi_tlm_generic_data_read(struct file *filp, char __user *buf,
					  size_t count, loff_t *ppos)
{
	struct scmi_tlm_inode *tlmi = to_tlm_inode(file_inode(filp));
	struct scmi_tlm_priv *tp = filp->private_data;
	int ret;

	if (!tp->buf) {
		ret = scmi_tlm_bulk_buffer_allocate_and_fill(tlmi, tp);
		if (ret)
			return ret;
	}

	return simple_read_from_buffer(buf, count, ppos, tp->buf, tp->buf_len);
}

static const struct file_operations scmi_tlm_data_fops = {
	.owner = THIS_MODULE,
	.open = scmi_tlm_data_open,
	.read = scmi_tlm_generic_data_read,
	.release = scmi_tlm_priv_release,
};

DEFINE_TLM_CLASS(data_tlmo, "des_bulk_read", 0,
		 S_IFREG | S_IRUSR, &scmi_tlm_data_fops, NULL);

static int scmi_tlm_bulk_single_read(struct scmi_tlm_setup *tsp,
				     int res_id, int *num_samples,
				     struct scmi_telemetry_de_sample *samples)
{
	return tsp->ops->des_sample_get(tsp->ph, res_id, num_samples, samples);
}

static int scmi_tlm_single_read_open(struct inode *ino, struct file *filp)
{
	return __scmi_tlm_generic_open(ino, filp, scmi_tlm_bulk_single_read);
}

static const struct file_operations scmi_tlm_single_sample_fops = {
	.owner = THIS_MODULE,
	.open = scmi_tlm_single_read_open,
	.read = scmi_tlm_generic_data_read,
	.release = scmi_tlm_priv_release,
};

DEFINE_TLM_CLASS(single_sample_tlmo, "des_single_sample_read", 0,
		 S_IFREG | S_IRUSR, &scmi_tlm_single_sample_fops, NULL);

static const struct scmi_tlm_class tlm_grps[] = {
	TLM_ANON_CLASS("enable", TLM_IS_STATE | TLM_IS_GROUP,
		       S_IFREG | S_IRUSR | S_IWUSR, &obj_enable_fops, NULL),
	TLM_ANON_CLASS("tstamp_enable", TLM_IS_GROUP,
		       S_IFREG | S_IRUSR | S_IWUSR, &obj_enable_fops, NULL),
	TLM_ANON_CLASS(NULL, 0, 0, NULL, NULL),
};

DEFINE_TLM_CLASS(grp_data_tlmo, "des_bulk_read", TLM_IS_GROUP,
		 S_IFREG | S_IRUSR, &scmi_tlm_data_fops, NULL);

DEFINE_TLM_CLASS(groups_dir_cls, "groups", 0, S_IFDIR | S_IRWXU, NULL, NULL);

DEFINE_TLM_CLASS(grp_single_sample_tlmo, "des_single_sample_read", TLM_IS_GROUP,
		 S_IFREG | S_IRUSR, &scmi_tlm_single_sample_fops, NULL);

DEFINE_TLM_CLASS(grp_composing_des_tlmo, "composing_des", TLM_IS_GROUP,
		 S_IFREG | S_IRUSR, &string_ro_fops, NULL);

DEFINE_TLM_CLASS(grp_current_interval_tlmo, "current_update_interval_ms",
		 TLM_IS_GROUP, S_IFREG | S_IRUSR | S_IWUSR,
		 &current_interval_fops, NULL);

DEFINE_TLM_CLASS(grp_available_interval_tlmo, "available_update_intervals_ms",
		 TLM_IS_GROUP, S_IFREG | S_IRUSR, &available_interv_fops, NULL);

DEFINE_TLM_CLASS(grp_intervals_discrete_tlmo, "intervals_discrete",
		 TLM_IS_GROUP, S_IFREG | S_IRUSR, &intrv_discrete_fops, NULL);

static long
scmi_tlm_info_get_ioctl(const struct scmi_tlm_inode *tlmi, unsigned long arg)
{
	const struct scmi_telemetry_info *info = tlmi->priv;
	void * __user uptr = (void * __user)arg;

	if (copy_to_user(uptr, &info->base, sizeof(info->base)))
		return -EFAULT;

	return 0;
}

static long
scmi_tlm_intervals_get_ioctl(const struct scmi_tlm_inode *tlmi,
			     unsigned long arg, bool is_group)
{
	struct scmi_tlm_intervals ivs, *tlm_ivs;
	void * __user uptr = (void * __user)arg;

	if (copy_from_user(&ivs, uptr, sizeof(ivs)))
		return -EFAULT;

	if (!is_group) {
		const struct scmi_telemetry_info *info = tlmi->priv;

		tlm_ivs = info->intervals;
	} else {
		const struct scmi_telemetry_group *grp = tlmi->priv;

		tlm_ivs = grp->intervals;
	}

	if (ivs.num != tlm_ivs->num)
		return -EINVAL;

	if (copy_to_user(uptr, tlm_ivs,
			 sizeof(*tlm_ivs) + sizeof(u32) * ivs.num))
		return -EFAULT;

	return 0;
}

static long
scmi_tlm_de_config_set_ioctl(const struct scmi_tlm_inode *tlmi,
			     unsigned long arg, bool all)
{
	const struct scmi_telemetry_res_info *rinfo;
	void * __user uptr = (void * __user)arg;
	struct scmi_tlm_setup *tsp = tlmi->tsp;
	struct scmi_tlm_de_config tcfg = {};
	int ret;

	if (copy_from_user(&tcfg, uptr, sizeof(tcfg)))
		return -EFAULT;

	if (!all)
		return tsp->ops->state_set(tsp->ph, false, tcfg.id,
					   (bool *)&tcfg.enable,
					   (bool *)&tcfg.t_enable);

	rinfo = scmi_telemetry_res_info_get(tsp);
	for (int i = 0; i < rinfo->num_des; i++) {
		ret = tsp->ops->state_set(tsp->ph, false,
					  rinfo->des[i]->info->id,
					  (bool *)&tcfg.enable,
					  (bool *)&tcfg.t_enable);
		if (ret)
			return ret;
	}

	return 0;
}

static long
scmi_tlm_de_config_get_ioctl(const struct scmi_tlm_inode *tlmi,
			     unsigned long arg)
{
	struct scmi_tlm_setup *tsp = tlmi->tsp;
	void * __user uptr = (void * __user)arg;
	struct scmi_tlm_de_config tcfg = {};
	int ret;

	if (copy_from_user(&tcfg, uptr, sizeof(tcfg)))
		return -EFAULT;

	ret = tsp->ops->state_get(tsp->ph, tcfg.id,
				  (bool *)&tcfg.enable, (bool *)&tcfg.t_enable);
	if (ret)
		return ret;

	if (copy_to_user(uptr, &tcfg, sizeof(tcfg)))
		return -EFAULT;

	return 0;
}

static long
scmi_tlm_config_get_ioctl(const struct scmi_tlm_inode *tlmi, unsigned long arg,
			  bool is_group)
{
	void * __user uptr = (void * __user)arg;
	struct scmi_tlm_config cfg;

	if (!is_group) {
		const struct scmi_telemetry_info *info = tlmi->priv;

		cfg.enable = !!info->enabled;
		cfg.current_update_interval = info->active_update_interval;
	} else {
		const struct scmi_telemetry_group *grp = tlmi->priv;

		cfg.enable = !!grp->enabled;
		cfg.t_enable = !!grp->tstamp_enabled;
		cfg.current_update_interval = grp->active_update_interval;
	}

	if (copy_to_user(uptr, &cfg, sizeof(cfg)))
		return -EFAULT;

	return 0;
}

static long
scmi_tlm_config_set_ioctl(const struct scmi_tlm_inode *tlmi, unsigned long arg,
			  bool is_group)
{
	struct scmi_tlm_setup *tsp = tlmi->tsp;
	void * __user uptr = (void * __user)arg;
	struct scmi_tlm_config cfg = {};
	bool grp_ignore;
	int res_id;

	if (copy_from_user(&cfg, uptr, sizeof(cfg)))
		return -EFAULT;

	if (!is_group) {
		res_id = SCMI_TLM_GRP_INVALID;
		grp_ignore = true;
	} else {
		const struct scmi_telemetry_group *grp = tlmi->priv;
		int ret;

		res_id = grp->info->id;
		grp_ignore = false;
		ret = tsp->ops->state_set(tsp->ph, true, res_id,
					  (bool *)&cfg.enable,
					  (bool *)&cfg.t_enable);
		if (ret)
			return ret;
	}

	return tsp->ops->collection_configure(tsp->ph, res_id, grp_ignore,
					      (bool *)&cfg.enable,
					      &cfg.current_update_interval,
					      NULL);
}

static long
scmi_tlm_de_info_get_ioctl(const struct scmi_tlm_inode *tlmi, unsigned long arg)
{
	struct scmi_tlm_setup *tsp = tlmi->tsp;
	void * __user uptr = (void * __user)arg;
	const struct scmi_telemetry_de *de;
	struct scmi_tlm_de_info dei;

	if (copy_from_user(&dei, uptr, sizeof(dei)))
		return -EFAULT;

	de = tsp->ops->de_lookup(tsp->ph, dei.id);
	if (!de)
		return -EINVAL;

	if (copy_to_user(uptr, de->info, sizeof(*de->info)))
		return -EFAULT;

	return 0;
}

static long
scmi_tlm_des_list_get_ioctl(const struct scmi_tlm_inode *tlmi, unsigned long arg)
{
	struct scmi_tlm_setup *tsp = tlmi->tsp;
	const struct scmi_telemetry_res_info *rinfo;
	void * __user uptr = (void * __user)arg;
	struct scmi_tlm_des_list dsl;

	rinfo = scmi_telemetry_res_info_get(tsp);
	if (copy_from_user(&dsl, uptr, sizeof(dsl)))
		return -EFAULT;

	if (dsl.num_des < rinfo->num_des)
		return -EINVAL;

	if (copy_to_user(uptr, &rinfo->num_des, sizeof(rinfo->num_des)))
		return -EFAULT;

	if (copy_to_user(uptr + sizeof(rinfo->num_des), rinfo->dei_store,
			 rinfo->num_des * sizeof(*rinfo->dei_store)))
		return -EFAULT;

	return 0;
}

static long
scmi_tlm_de_value_get_ioctl(const struct scmi_tlm_inode *tlmi, unsigned long arg)
{
	struct scmi_tlm_setup *tsp = tlmi->tsp;
	void * __user uptr = (void * __user)arg;
	struct scmi_tlm_de_sample sample;
	int ret;

	if (copy_from_user(&sample, uptr, sizeof(sample)))
		return -EFAULT;

	ret = tsp->ops->de_data_read(tsp->ph,
				     (struct scmi_telemetry_de_sample *)&sample);
	if (ret)
		return ret;

	if (copy_to_user(uptr, &sample, sizeof(sample)))
		return -EFAULT;

	return 0;
}

static long
scmi_tlm_grp_info_get_ioctl(const struct scmi_tlm_inode *tlmi, unsigned long arg)
{
	const struct scmi_telemetry_group *grp = tlmi->priv;
	void * __user uptr = (void * __user)arg;

	if (copy_to_user(uptr, grp->info, sizeof(*grp->info)))
		return -EFAULT;

	return 0;
}

static long
scmi_tlm_grp_desc_get_ioctl(const struct scmi_tlm_inode *tlmi, unsigned long arg)
{
	const struct scmi_telemetry_group *grp = tlmi->priv;
	unsigned int num_des = grp->info->num_des;
	void * __user uptr = (void * __user)arg;
	struct scmi_tlm_grp_desc grp_desc;

	if (copy_from_user(&grp_desc, uptr, sizeof(grp_desc)))
		return -EFAULT;

	if (grp_desc.num_des < num_des)
		return -EINVAL;

	if (copy_to_user(uptr, &num_des, sizeof(num_des)))
		return -EFAULT;

	if (copy_to_user(uptr + sizeof(num_des), grp->des,
			 sizeof(*grp->des) * num_des))
		return -EFAULT;

	return 0;
}

static long
scmi_tlm_grps_list_get_ioctl(const struct scmi_tlm_inode *tlmi, unsigned long arg)
{
	struct scmi_tlm_setup *tsp = tlmi->tsp;
	const struct scmi_telemetry_res_info *rinfo;
	void * __user uptr = (void * __user)arg;
	struct scmi_tlm_grps_list gsl;

	if (copy_from_user(&gsl, uptr, sizeof(gsl)))
		return -EFAULT;

	rinfo = scmi_telemetry_res_info_get(tsp);
	if (gsl.num_grps < rinfo->num_groups)
		return -EINVAL;

	if (copy_to_user(uptr, &rinfo->num_groups, sizeof(rinfo->num_groups)))
		return -EFAULT;

	if (copy_to_user(uptr + sizeof(rinfo->num_groups), rinfo->grps_store,
			 rinfo->num_groups * sizeof(*rinfo->grps_store)))
		return -EFAULT;

	return 0;
}

static long scmi_tlm_des_read_ioctl(const struct scmi_tlm_inode *tlmi,
				    unsigned long arg, bool single,
				    bool is_group)
{
	struct scmi_tlm_setup *tsp = tlmi->tsp;
	void * __user uptr = (void * __user)arg;
	struct scmi_tlm_data_read bulk;
	int ret, grp_id = SCMI_TLM_GRP_INVALID;

	if (copy_from_user(&bulk, uptr, sizeof(bulk)))
		return -EFAULT;

	struct scmi_tlm_data_read *bulk_ptr __free(kfree) =
		kzalloc(struct_size(bulk_ptr, samples, bulk.num_samples),
			GFP_KERNEL);
	if (!bulk_ptr)
		return -ENOMEM;

	if (is_group) {
		const struct scmi_telemetry_group *grp = tlmi->priv;

		grp_id = grp->info->id;
	}

	bulk_ptr->num_samples = bulk.num_samples;
	if (!single)
		ret = tsp->ops->des_bulk_read(tsp->ph, grp_id,
					      &bulk_ptr->num_samples,
			  (struct scmi_telemetry_de_sample *)bulk_ptr->samples);
	else
		ret = tsp->ops->des_sample_get(tsp->ph, grp_id,
					       &bulk_ptr->num_samples,
					       (struct scmi_telemetry_de_sample *)bulk_ptr->samples);
	if (ret)
		return ret;

	if (copy_to_user(uptr, bulk_ptr, sizeof(*bulk_ptr) +
			 bulk_ptr->num_samples * sizeof(bulk_ptr->samples[0])))
		return -EFAULT;

	return 0;
}

static long scmi_tlm_unlocked_ioctl(struct file *filp, unsigned int cmd,
				    unsigned long arg)
{
	struct scmi_tlm_inode *tlmi = to_tlm_inode(file_inode(filp));
	bool is_group = IS_GROUP(tlmi->cls->flags);

	switch (cmd) {
	case SCMI_TLM_GET_INFO:
		if (is_group)
			return -EOPNOTSUPP;
		return scmi_tlm_info_get_ioctl(tlmi, arg);
	case SCMI_TLM_GET_CFG:
		return scmi_tlm_config_get_ioctl(tlmi, arg, is_group);
	case SCMI_TLM_SET_CFG:
		return scmi_tlm_config_set_ioctl(tlmi, arg, is_group);
	case SCMI_TLM_GET_INTRVS:
		return scmi_tlm_intervals_get_ioctl(tlmi, arg, is_group);
	case SCMI_TLM_GET_DE_CFG:
		if (is_group)
			return -EOPNOTSUPP;
		return scmi_tlm_de_config_get_ioctl(tlmi, arg);
	case SCMI_TLM_SET_DE_CFG:
		if (is_group)
			return -EOPNOTSUPP;
		return scmi_tlm_de_config_set_ioctl(tlmi, arg, false);
	case SCMI_TLM_GET_DE_INFO:
		if (is_group)
			return -EOPNOTSUPP;
		return scmi_tlm_de_info_get_ioctl(tlmi, arg);
	case SCMI_TLM_GET_DE_LIST:
		if (is_group)
			return -EOPNOTSUPP;
		return scmi_tlm_des_list_get_ioctl(tlmi, arg);
	case SCMI_TLM_GET_DE_VALUE:
		if (is_group)
			return -EOPNOTSUPP;
		return scmi_tlm_de_value_get_ioctl(tlmi, arg);
	case SCMI_TLM_SET_ALL_CFG:
		return scmi_tlm_de_config_set_ioctl(tlmi, arg, true);
	case SCMI_TLM_GET_GRP_LIST:
		if (is_group)
			return -EOPNOTSUPP;
		return scmi_tlm_grps_list_get_ioctl(tlmi, arg);
	case SCMI_TLM_GET_GRP_INFO:
		if (!is_group)
			return -EOPNOTSUPP;
		return scmi_tlm_grp_info_get_ioctl(tlmi, arg);
	case SCMI_TLM_GET_GRP_DESC:
		if (!is_group)
			return -EOPNOTSUPP;
		return scmi_tlm_grp_desc_get_ioctl(tlmi, arg);
	case SCMI_TLM_SINGLE_SAMPLE:
		return scmi_tlm_des_read_ioctl(tlmi, arg, true, is_group);
	case SCMI_TLM_BULK_READ:
		return scmi_tlm_des_read_ioctl(tlmi, arg, false, is_group);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations scmi_tlm_ctrl_fops = {
	.owner = THIS_MODULE,
	.open = nonseekable_open,
	.unlocked_ioctl = scmi_tlm_unlocked_ioctl,
};

DEFINE_TLM_CLASS(ctrl_tlmo, "control", 0,
		 S_IFREG | S_IRUSR | S_IWUSR, &scmi_tlm_ctrl_fops, NULL);
DEFINE_TLM_CLASS(grp_ctrl_tlmo, "control", TLM_IS_GROUP,
		 S_IFREG | S_IRUSR | S_IWUSR, &scmi_tlm_ctrl_fops, NULL);

static int scmi_telemetry_groups_initialize(struct scmi_tlm_instance *ti)
{
	const struct scmi_telemetry_res_info *rinfo;
	struct scmi_tlm_setup *tsp = ti->tsp;
	struct super_block *sb = ti->sb;
	struct device *dev = tsp->dev;
	struct dentry *grp_dir_dentry;

	if (ti->info->base.num_groups == 0)
		return 0;

	rinfo = scmi_telemetry_res_info_get(tsp);
	if (!rinfo)
		return -ENODEV;

	for (int i = 0; i < rinfo->num_groups; i++) {
		const struct scmi_telemetry_group *grp = &rinfo->grps[i];

		struct scmi_tlm_class *grp_tlm_cls __free(kfree) =
			kzalloc(sizeof(*grp_tlm_cls), GFP_KERNEL);
		if (!grp_tlm_cls)
			return -ENOMEM;

		grp_tlm_cls->name = kasprintf(GFP_KERNEL, "%u", grp->info->id);
		if (!grp_tlm_cls->name)
			return -ENOMEM;

		grp_tlm_cls->mode = S_IFDIR | S_IRWXU;
		grp_tlm_cls->flags = TLM_IS_DYNAMIC;

		grp_dir_dentry = stlmfs_create_dentry(sb, tsp, ti->grps_dentry,
						      grp_tlm_cls, grp);

		for (const struct scmi_tlm_class *gto = tlm_grps; gto->name; gto++)
			stlmfs_create_dentry(sb, tsp, grp_dir_dentry, gto, grp);

		stlmfs_create_dentry(sb, tsp, grp_dir_dentry,
				     &grp_composing_des_tlmo, grp->des_str);

		stlmfs_create_dentry(sb, tsp, grp_dir_dentry, &grp_ctrl_tlmo, grp);
		stlmfs_create_dentry(sb, tsp, grp_dir_dentry, &grp_data_tlmo, grp);
		stlmfs_create_dentry(sb, tsp, grp_dir_dentry,
				     &grp_single_sample_tlmo, grp);

		if (ti->info->per_group_config_support) {
			stlmfs_create_dentry(sb, tsp, grp_dir_dentry,
					     &grp_current_interval_tlmo, grp);
			stlmfs_create_dentry(sb, tsp, grp_dir_dentry,
					     &grp_available_interval_tlmo, grp);
			stlmfs_create_dentry(sb, tsp, grp_dir_dentry,
					     &grp_intervals_discrete_tlmo, grp);
		}

		retain_and_null_ptr(grp_tlm_cls);
	}

	dev_info(dev, "Found %d Telemetry GROUPS resources.\n",
		 rinfo->num_groups);

	return 0;
}

static struct scmi_tlm_instance *scmi_tlm_init(struct scmi_tlm_setup *tsp,
					       int instance_id)
{
	struct device *dev = tsp->dev;
	struct scmi_tlm_instance *ti;

	ti = devm_kzalloc(dev, sizeof(*ti), GFP_KERNEL);
	if (!ti)
		return ERR_PTR(-ENOMEM);

	ti->info = tsp->ops->info_get(tsp->ph);
	if (!ti->info)
		return dev_err_ptr_probe(dev,
					 -EINVAL, "invalid Telemetry info !\n");

	ti->id = instance_id;
	ti->tsp = tsp;

	return ti;
}

static int scmi_telemetry_probe(struct scmi_device *sdev)
{
	const struct scmi_handle *handle = sdev->handle;
	struct scmi_protocol_handle *ph;
	struct device *dev = &sdev->dev;
	struct scmi_tlm_instance *ti;
	struct scmi_tlm_setup *tsp;
	struct super_block *sb;
	const void *ops;

	if (!handle)
		return -ENODEV;

	ops = handle->devm_protocol_get(sdev, sdev->protocol_id, &ph);
	if (IS_ERR(ops))
		return dev_err_probe(dev, PTR_ERR(ops),
				     "Cannot access protocol:0x%X\n",
				     sdev->protocol_id);

	tsp = devm_kzalloc(dev, sizeof(*tsp), GFP_KERNEL);
	if (!tsp)
		return -ENOMEM;

	tsp->dev = dev;
	tsp->ops = ops;
	tsp->ph = ph;

	ti = scmi_tlm_init(tsp, atomic_fetch_inc(&scmi_tlm_instance_count));
	if (IS_ERR(ti))
		return PTR_ERR(ti);

	mutex_lock(&stlmfs_mtx);
	list_add(&ti->node, &scmi_telemetry_instances);
	sb = stlmfs_sb;
	mutex_unlock(&stlmfs_mtx);

	/*
	 * If the file system was already mounted by the time this
	 * instance was probed, register explicitly, since the list
	 * has been scanned already.
	 */
	if (sb) {
		int ret;

		ret = scmi_telemetry_instance_register(sb, ti);
		if (ret) {
			dev_err(dev, "Failed to register instance %u at probe.\n",
				ti->id);
			return ret;
		}
	}

	return 0;
}

static const struct scmi_device_id scmi_id_table[] = {
	{ SCMI_PROTOCOL_TELEMETRY, "telemetry" },
	{ }
};
MODULE_DEVICE_TABLE(scmi, scmi_id_table);

static struct scmi_driver scmi_telemetry_driver = {
	.name = "scmi-telemetry-driver",
	.probe = scmi_telemetry_probe,
	.id_table = scmi_id_table,
};

static struct inode *stlmfs_alloc_inode(struct super_block *sb)
{
	struct scmi_tlm_inode *tlmi;

	tlmi = alloc_inode_sb(sb, stlmfs_inode_cachep, GFP_KERNEL);
	if (!tlmi)
		return NULL;

	tlmi->cls = NULL;

	return &tlmi->vfs_inode;
}

static void stlmfs_free_inode(struct inode *inode)
{
	struct scmi_tlm_inode *tlmi = to_tlm_inode(inode);

	if (tlmi->cls && IS_DYNAMIC(tlmi->cls->flags)) {
		kfree(tlmi->cls->name);
		kfree(tlmi->cls);
	}

	kmem_cache_free(stlmfs_inode_cachep, tlmi);
}

static const struct super_operations tlm_sops = {
	.statfs = simple_statfs,
	.alloc_inode = stlmfs_alloc_inode,
	.free_inode = stlmfs_free_inode,
};

static struct dentry *stlmfs_create_root_dentry(struct super_block *sb)
{
	struct dentry *dentry;
	struct inode *inode;

	inode = stlmfs_get_inode(sb);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	inode->i_op = &simple_dir_inode_operations;
	inode->i_fop = &simple_dir_operations;
	inode_init_owner(&nop_mnt_idmap, inode, NULL, S_IFDIR | S_IRWXU);

	dentry = d_make_root(inode);
	if (!dentry)
		return ERR_PTR(-ENOMEM);

	return dentry;
}

static int scmi_telemetry_de_subdir_symlink(struct super_block *sb,
					    struct scmi_tlm_setup *tsp,
					    const struct scmi_telemetry_de *de,
					    struct dentry *parent)
{
	struct dentry *dentry;
	struct inode *inode;
	int ret;

	if (IS_ERR(parent))
		return 0;

	char *name __free(kfree) = kasprintf(GFP_KERNEL, "0x%08X", de->info->id);
	if (!name)
		return -ENOMEM;

	char *link __free(kfree) =
		kasprintf(GFP_KERNEL, "../../../../../des/0x%08X", de->info->id);
	if (!link)
		return -ENOMEM;

	dentry = simple_start_creating(parent, name);
	if (IS_ERR(dentry))
		return PTR_ERR(dentry);

	inode = stlmfs_get_inode(sb);
	if (unlikely(!inode)) {
		dev_err(tsp->dev,
			"out of free dentries, cannot create '%s'", name);
		return stlmfs_failed_creating(dentry);
	}

	inode->i_mode = S_IFLNK | 0777;
	inode->i_op = &simple_symlink_inode_operations;
	inode_init_owner(&nop_mnt_idmap, inode, NULL, inode->i_mode);
	inode->i_link = no_free_ptr(link);

	//d_add(dentry, inode);
	d_make_persistent(dentry, inode);

	simple_done_creating(dentry);

	return ret;
}

static struct dentry *
scmi_telemetry_topology_path_get(struct super_block *sb,
				 struct scmi_tlm_setup *tsp,
				 struct dentry *parent, const char *dname)
{
	struct dentry *dentry;

	dentry = stlmfs_lookup_by_name(parent, dname);
	if (!dentry) {
		struct scmi_tlm_class *dir_tlm_cls __free(kfree) =
			kzalloc(sizeof(*dir_tlm_cls), GFP_KERNEL);
		if (!dir_tlm_cls)
			return NULL;

		dir_tlm_cls->name = kasprintf(GFP_KERNEL, "%s", dname);
		if (!dir_tlm_cls->name)
			return NULL;

		dir_tlm_cls->mode = S_IFDIR | S_IRWXU;
		dir_tlm_cls->flags = TLM_IS_DYNAMIC;

		dentry = stlmfs_create_dentry(sb, tsp, parent,
					      dir_tlm_cls, NULL);
		if (!IS_ERR(dentry))
			retain_and_null_ptr(dir_tlm_cls);
	}

	return dentry;
}

static int scmi_telemetry_topology_add_node(struct super_block *sb,
					    struct scmi_tlm_instance *ti,
					    const struct scmi_telemetry_de *de)
{
	struct dentry *ctype, *cinst, *cunit, *dinst;
	struct scmi_tlm_de_info *dei = de->info;
	char inst_str[32];
	int ret;

	/* by_compo_type/<COMPO_TYPE_STR>/ */
	ctype = scmi_telemetry_topology_path_get(sb, ti->tsp, ti->compo_dentry,
						 compo_types[dei->compo_type]);
	if (!ctype)
		return -ENOMEM;

	/* by_compo_type/<COMPO_TYPE_STR>/<N>/ */
	snprintf(inst_str, 32, "%u", dei->compo_instance_id);
	cinst = scmi_telemetry_topology_path_get(sb, ti->tsp, ctype, inst_str);
	dput(ctype);
	if (!cinst)
		return -ENOMEM;

	/* by_compo_type/<COMPO_TYPE_STR>/<N>/<DE_UNIT_TYPE_STR>/ */
	cunit = scmi_telemetry_topology_path_get(sb, ti->tsp, cinst,
						 unit_types[dei->unit]);
	dput(cinst);
	if (!cunit)
		return -ENOMEM;

	/* by_compo_type/<COMPO_TYPE_STR>/<N>/<DE_UNIT_TYPE_STR>/<N> */
	snprintf(inst_str, 32, "%u", dei->instance_id);
	dinst = scmi_telemetry_topology_path_get(sb, ti->tsp, cunit, inst_str);
	dput(cunit);
	if (!dinst)
		return -ENOMEM;

	ret = scmi_telemetry_de_subdir_symlink(sb, ti->tsp, de, dinst);
	dput(dinst);

	return ret;
}

DEFINE_TLM_CLASS(compo_dir_cls, "components", 0, S_IFDIR | S_IRWXU, NULL, NULL);

static int scmi_telemetry_topology_view_add(struct scmi_tlm_instance *ti)
{
	const struct scmi_telemetry_res_info *rinfo;
	struct scmi_tlm_setup *tsp = ti->tsp;
	struct device *dev = tsp->dev;

	rinfo = scmi_telemetry_res_info_get(tsp);
	if (!rinfo || !rinfo->fully_enumerated)
		return -ENODEV;

	ti->compo_dentry =
		stlmfs_create_dentry(ti->sb, tsp, ti->top_dentry, &compo_dir_cls, NULL);

	for (int i = 0; i < rinfo->num_des; i++) {
		int ret;

		ret = scmi_telemetry_topology_add_node(ti->sb, ti, rinfo->des[i]);
		if (ret)
			dev_err(dev, "Fail to add node %s to topology. Skip.\n",
				rinfo->des[i]->info->name);
	}

	return 0;
}

static int scmi_tlm_root_dentries_initialize(struct scmi_tlm_instance *ti)
{
	struct scmi_tlm_setup *tsp = ti->tsp;
	struct super_block *sb = ti->sb;

	scnprintf(ti->name, MAX_INST_NAME, "tlm_%d", ti->id);

	/* Allocate top instance node */
	ti->top_cls.name = ti->name;
	ti->top_cls.mode = S_IFDIR | S_IRWXU;

	/* Create the root of this instance */
	ti->top_dentry = stlmfs_create_dentry(sb, tsp, sb->s_root, &ti->top_cls, NULL);
	for (const struct scmi_tlm_class *tlmo = tlm_tops; tlmo->name; tlmo++)
		stlmfs_create_dentry(sb, tsp, ti->top_dentry, tlmo, ti->info);

	if (ti->info->reset_support)
		stlmfs_create_dentry(sb, tsp, ti->top_dentry, &reset_tlmo, NULL);

	stlmfs_create_dentry(sb, tsp, ti->top_dentry, &version_tlmo,
			     &ti->info->base.version);
	stlmfs_create_dentry(sb, tsp, ti->top_dentry, &data_tlmo, ti->info);
	stlmfs_create_dentry(sb, tsp, ti->top_dentry, &single_sample_tlmo, ti->info);
	stlmfs_create_dentry(sb, tsp, ti->top_dentry, &ctrl_tlmo, ti->info);
	ti->des_dentry =
		stlmfs_create_dentry(sb, tsp, ti->top_dentry, &des_dir_cls, NULL);
	ti->grps_dentry =
		stlmfs_create_dentry(sb, tsp, ti->top_dentry, &groups_dir_cls, NULL);

	return 0;
}

static int scmi_telemetry_instance_register(struct super_block *sb,
					    struct scmi_tlm_instance *ti)
{
	int ret;

	ti->sb = sb;
	ret = scmi_tlm_root_dentries_initialize(ti);
	if (ret)
		return ret;

	ret = scmi_telemetry_des_initialize(ti);
	if (ret)
		return ret;

	ret = scmi_telemetry_groups_initialize(ti);
	if (ret) {
		dev_warn(ti->tsp->dev,
			 "Failed to initialize groups for instance %s.\n",
			 ti->top_cls.name);
	}

	ret = scmi_telemetry_topology_view_add(ti);
	if (ret)
		dev_warn(ti->tsp->dev,
			 "Failed to create topology view for instance %s.\n",
			 ti->top_cls.name);

	return 0;
}

static int stlmfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct scmi_tlm_instance *ti;
	struct dentry *root_dentry;
	int ret;

	sb->s_magic = TLM_FS_MAGIC;
	sb->s_blocksize = PAGE_SIZE;
	sb->s_blocksize_bits = PAGE_SHIFT;
	sb->s_op = &tlm_sops;

	root_dentry = stlmfs_create_root_dentry(sb);
	if (IS_ERR(root_dentry))
		return PTR_ERR(root_dentry);

	sb->s_root = root_dentry;

	mutex_lock(&stlmfs_mtx);
	list_for_each_entry(ti, &scmi_telemetry_instances, node) {
		mutex_unlock(&stlmfs_mtx);
		ret = scmi_telemetry_instance_register(sb, ti);
		if (ret)
			dev_err(ti->tsp->dev,
				"Failed to register instance %u.\n", ti->id);
		mutex_lock(&stlmfs_mtx);
	}
	stlmfs_sb = sb;
	mutex_unlock(&stlmfs_mtx);

	return 0;
}

static int stlmfs_get_tree(struct fs_context *fc)
{
	return get_tree_single(fc, stlmfs_fill_super);
}

static const struct fs_context_operations stlmfs_fc_ops = {
	.get_tree = stlmfs_get_tree,
};

static int stlmfs_init_fs_context(struct fs_context *fc)
{
	fc->ops = &stlmfs_fc_ops;

	return 0;
}

static void stlmfs_kill_sb(struct super_block *sb)
{
	kill_anon_super(sb);
}

static struct file_system_type scmi_telemetry_fs = {
	.owner = THIS_MODULE,
	.name = TLM_FS_NAME,
	.kill_sb = stlmfs_kill_sb,
	.init_fs_context = stlmfs_init_fs_context,
	.fs_flags = 0,
};

static void stlmfs_init_once(void *arg)
{
	struct scmi_tlm_inode *tlmi = arg;

	inode_init_once(&tlmi->vfs_inode);
}

static int __init scmi_telemetry_init(void)
{
	int ret;

	ret = sysfs_create_mount_point(fs_kobj, TLM_FS_MNT);
	if (ret && ret != -EEXIST)
		return ret;

	stlmfs_inode_cachep = kmem_cache_create("stlmfs_inode_cache",
						sizeof(struct scmi_tlm_inode), 0,
						SLAB_RECLAIM_ACCOUNT | SLAB_ACCOUNT,
						stlmfs_init_once);
	if (!stlmfs_inode_cachep) {
		ret = -ENOMEM;
		goto out_mnt;
	}

	ret = register_filesystem(&scmi_telemetry_fs);
	if (ret)
		goto out_kmem;

	ret = scmi_register(&scmi_telemetry_driver);
	if (ret)
		goto out_reg;

	return 0;

out_reg:
	unregister_filesystem(&scmi_telemetry_fs);
out_kmem:
	kmem_cache_destroy(stlmfs_inode_cachep);
out_mnt:
	sysfs_remove_mount_point(fs_kobj, TLM_FS_MNT);

	return ret;
}
module_init(scmi_telemetry_init);

static void __exit scmi_telemetry_exit(void)
{
	int ret;

	scmi_unregister(&scmi_telemetry_driver);
	ret = unregister_filesystem(&scmi_telemetry_fs);
	if (ret)
		pr_err("Failed to unregister %s\n", TLM_FS_NAME);

	sysfs_remove_mount_point(fs_kobj, TLM_FS_MNT);
	kmem_cache_destroy(stlmfs_inode_cachep);
}
module_exit(scmi_telemetry_exit);

MODULE_AUTHOR("Cristian Marussi <cristian.marussi@arm.com>");
MODULE_DESCRIPTION("ARM SCMI Telemetry Driver");
MODULE_LICENSE("GPL");
