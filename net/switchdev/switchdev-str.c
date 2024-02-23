// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/if_bridge.h>
#include <net/switchdev.h>

static ssize_t switchdev_str_write_id(char *buf, size_t len, unsigned long id,
				      const char *const *names, size_t n_names)
{
	if (id < n_names && names[id])
		return snprintf(buf, len, "%s", names[id]);

	return snprintf(buf, len, "UNKNOWN<%lu>", id);
}

ssize_t switchdev_attr_str(const struct switchdev_attr *attr,
			   char *buf, size_t len)
{
#define _ATTR_ID_STRINGER(_id) [SWITCHDEV_ATTR_ID_ ## _id] = #_id
	static const char *const attr_id_strs[] = {
		SWITCHDEV_ATTR_ID_MAPPER(_ATTR_ID_STRINGER)
	};
#undef _ATTR_ID_STRINGER

	static const char *const stp_state_strs[] = {
		[BR_STATE_DISABLED] = "disabled",
		[BR_STATE_LISTENING] = "listening",
		[BR_STATE_LEARNING] = "learning",
		[BR_STATE_FORWARDING] = "forwarding",
		[BR_STATE_BLOCKING] = "blocking",
	};

	char *cur = buf;
	ssize_t n;

	n = switchdev_str_write_id(cur, len, attr->id, attr_id_strs,
				   ARRAY_SIZE(attr_id_strs));
	if (n < 0)
		return n;

	cur += n;
	len -= n;

	n = snprintf(cur, len, "(flags %#x orig %s) ", attr->flags,
		     attr->orig_dev ? netdev_name(attr->orig_dev) : "(null)");
	if (n < 0)
		return n;

	cur += n;
	len -= n;

	switch (attr->id) {
	case SWITCHDEV_ATTR_ID_PORT_STP_STATE:
		n = switchdev_str_write_id(cur, len, attr->u.stp_state,
					   stp_state_strs, ARRAY_SIZE(stp_state_strs));
		break;
	case SWITCHDEV_ATTR_ID_PORT_MST_STATE:
		n = snprintf(cur, len, "msti %u", attr->u.mst_state.msti);
		if (n < 0)
			return n;

		cur += n;
		len -= n;

		n = switchdev_str_write_id(cur, len, attr->u.mst_state.state,
					   stp_state_strs, ARRAY_SIZE(stp_state_strs));
		break;
	case SWITCHDEV_ATTR_ID_PORT_PRE_BRIDGE_FLAGS:
	case SWITCHDEV_ATTR_ID_PORT_BRIDGE_FLAGS:
		n = snprintf(cur, len, "val %#lx mask %#lx",
			     attr->u.brport_flags.val,
			     attr->u.brport_flags.mask);
		break;
	case SWITCHDEV_ATTR_ID_PORT_MROUTER:
	case SWITCHDEV_ATTR_ID_BRIDGE_MROUTER:
		n = snprintf(cur, len, "%s",
			     attr->u.mrouter ? "enabled" : "disabled");
		break;
	case SWITCHDEV_ATTR_ID_BRIDGE_AGEING_TIME:
		n = snprintf(cur, len, "%ums",
			     jiffies_to_msecs(clock_t_to_jiffies(attr->u.ageing_time)));
		break;
	case SWITCHDEV_ATTR_ID_BRIDGE_VLAN_FILTERING:
		n = snprintf(cur, len, "%s",
			     attr->u.vlan_filtering ? "enabled" : "disabled");
		break;
	case SWITCHDEV_ATTR_ID_BRIDGE_VLAN_PROTOCOL:
		n = snprintf(cur, len, "%#x", attr->u.vlan_protocol);
		break;
	case SWITCHDEV_ATTR_ID_BRIDGE_MC_DISABLED:
		n = snprintf(cur, len, "%s",
			     attr->u.mc_disabled ? "active" : "inactive");
		break;
	case SWITCHDEV_ATTR_ID_BRIDGE_MST:
		n = snprintf(cur, len, "%s",
			     attr->u.mst ? "enabled" : "disabled");
		break;
	case SWITCHDEV_ATTR_ID_VLAN_MSTI:
		n = snprintf(cur, len, "vid %u msti %u",
			     attr->u.vlan_msti.vid, attr->u.vlan_msti.msti);
		break;
	default:
		/* Trim trailing space */
		return --cur - buf;
	}

	if (n < 0)
		return n;

	cur += n;
	return cur - buf;
}
EXPORT_SYMBOL_GPL(switchdev_attr_str);

ssize_t switchdev_obj_str(const struct switchdev_obj *obj,
			  char *buf, size_t len)
{
#define _OBJ_ID_STRINGER(_id) [SWITCHDEV_OBJ_ID_ ## _id] = #_id
	static const char *const obj_id_strs[] = {
		SWITCHDEV_OBJ_ID_MAPPER(_OBJ_ID_STRINGER)
	};
#undef _OBJ_ID_STRINGER

	const struct switchdev_obj_port_vlan *vlan;
	const struct switchdev_obj_port_mdb *mdb;
	char *cur = buf;
	ssize_t n;

	n = switchdev_str_write_id(cur, len, obj->id, obj_id_strs,
				   ARRAY_SIZE(obj_id_strs));
	if (n < 0)
		return n;

	cur += n;
	len -= n;

	n = snprintf(cur, len, "(flags %#x orig %s) ", obj->flags,
		     obj->orig_dev ? netdev_name(obj->orig_dev) : "(null)");
	if (n < 0)
		return n;

	cur += n;
	len -= n;

	switch (obj->id) {
	case SWITCHDEV_OBJ_ID_PORT_VLAN:
		vlan = SWITCHDEV_OBJ_PORT_VLAN(obj);
		n = snprintf(cur, len, "vid %u flags %#x%s", vlan->vid,
			     vlan->flags, vlan->changed ? "(changed)" : "");
		break;
	case SWITCHDEV_OBJ_ID_PORT_MDB:
	case SWITCHDEV_OBJ_ID_HOST_MDB:
		mdb = SWITCHDEV_OBJ_PORT_MDB(obj);
		n = snprintf(cur, len, "vid %u addr %pM", mdb->vid, mdb->addr);
		break;
	default:
		/* Trim trailing space */
		return --cur - buf;
	}

	if (n < 0)
		return n;

	cur += n;
	return cur - buf;
}
EXPORT_SYMBOL_GPL(switchdev_obj_str);

ssize_t switchdev_fdb_info_str(enum switchdev_notifier_type nt,
			       const struct switchdev_notifier_fdb_info *fdbi,
			       char *buf, size_t len)
{
	switch (nt) {
	case SWITCHDEV_FDB_FLUSH_TO_BRIDGE:
		return snprintf(buf, len, "vid %u", fdbi->vid);
	case SWITCHDEV_FDB_ADD_TO_BRIDGE:
	case SWITCHDEV_FDB_DEL_TO_BRIDGE:
	case SWITCHDEV_FDB_ADD_TO_DEVICE:
	case SWITCHDEV_FDB_DEL_TO_DEVICE:
	case SWITCHDEV_FDB_OFFLOADED:
	case SWITCHDEV_VXLAN_FDB_ADD_TO_BRIDGE:
	case SWITCHDEV_VXLAN_FDB_DEL_TO_BRIDGE:
	case SWITCHDEV_VXLAN_FDB_ADD_TO_DEVICE:
	case SWITCHDEV_VXLAN_FDB_DEL_TO_DEVICE:
	case SWITCHDEV_VXLAN_FDB_OFFLOADED:
		return snprintf(buf, len, "vid %u addr %pM%s%s%s%s",
				fdbi->vid, fdbi->addr,
				fdbi->added_by_user ? " added_by_user" : "",
				fdbi->is_local ? " is_local" : "",
				fdbi->locked ? " locked" : "",
				fdbi->offloaded ? " offloaded" : "");
	default:
		break;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(switchdev_fdb_info_str);

ssize_t switchdev_brport_str(const struct switchdev_brport *brport,
			     char *buf, size_t len)
{
	return snprintf(buf, len, "dev %s%s",
			brport->dev ? netdev_name(brport->dev) : "(null)",
			brport->tx_fwd_offload ? " tx_fwd_offload" : "");
}
EXPORT_SYMBOL_GPL(switchdev_brport_str);

ssize_t switchdev_notifier_str(enum switchdev_notifier_type nt,
			       const struct switchdev_notifier_info *info,
			       char *buf, size_t len)
{
#define _TYPE_STRINGER(_id) [SWITCHDEV_ ## _id] = #_id
	static const char *const type_strs[] = {
		SWITCHDEV_TYPE_MAPPER(_TYPE_STRINGER)
	};
#undef _TYPE_STRINGER

	const struct switchdev_notifier_port_attr_info *attri;
	const struct switchdev_notifier_brport_info *brporti;
	const struct switchdev_notifier_port_obj_info *obji;
	const struct switchdev_notifier_fdb_info *fdbi;
	char *cur = buf;
	ssize_t n;

	n = switchdev_str_write_id(cur, len, nt, type_strs,
				   ARRAY_SIZE(type_strs));
	if (n < 0)
		return n;

	cur += n;
	len -= n;

	if (len > 0) {
		*cur++ = ' ';
		len--;
	}

	switch (nt) {
	case SWITCHDEV_FDB_FLUSH_TO_BRIDGE:
	case SWITCHDEV_FDB_ADD_TO_BRIDGE:
	case SWITCHDEV_FDB_DEL_TO_BRIDGE:
	case SWITCHDEV_FDB_ADD_TO_DEVICE:
	case SWITCHDEV_FDB_DEL_TO_DEVICE:
	case SWITCHDEV_FDB_OFFLOADED:
	case SWITCHDEV_VXLAN_FDB_ADD_TO_BRIDGE:
	case SWITCHDEV_VXLAN_FDB_DEL_TO_BRIDGE:
	case SWITCHDEV_VXLAN_FDB_ADD_TO_DEVICE:
	case SWITCHDEV_VXLAN_FDB_DEL_TO_DEVICE:
	case SWITCHDEV_VXLAN_FDB_OFFLOADED:
		fdbi = container_of(info, typeof(*fdbi), info);
		n = switchdev_fdb_info_str(nt, fdbi, cur, len);
		break;
	case SWITCHDEV_PORT_OBJ_ADD:
	case SWITCHDEV_PORT_OBJ_DEL:
		obji = container_of(info, typeof(*obji), info);
		n = switchdev_obj_str(obji->obj, cur, len);
		break;
	case SWITCHDEV_PORT_ATTR_SET:
		attri = container_of(info, typeof(*attri), info);
		n = switchdev_attr_str(attri->attr, cur, len);
		break;
	case SWITCHDEV_BRPORT_OFFLOADED:
	case SWITCHDEV_BRPORT_UNOFFLOADED:
	case SWITCHDEV_BRPORT_REPLAY:
		brporti = container_of(info, typeof(*brporti), info);
		n = switchdev_brport_str(&brporti->brport, cur, len);
		break;
	default:
		/* Trim trailing space */
		return --cur - buf;
	}

	if (n < 0)
		return n;

	cur += n;
	return cur - buf;
}
EXPORT_SYMBOL_GPL(switchdev_notifier_str);
