// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hornet Linux Security Module
 *
 * Author: Blaise Boscaccy <bboscaccy@linux.microsoft.com>
 *
 * Copyright (C) 2025 Microsoft Corporation
 */

#include <linux/lsm_hooks.h>
#include <uapi/linux/lsm.h>
#include <linux/bpf.h>
#include <linux/verification.h>
#include <crypto/public_key.h>
#include <linux/module_signature.h>
#include <crypto/pkcs7.h>
#include <linux/bpf_verifier.h>
#include <linux/sort.h>

#define EBPF_SIG_STRING "~eBPF signature appended~\n"

struct hornet_maps {
	u32 used_idx[MAX_USED_MAPS];
	u32 used_map_cnt;
	bpfptr_t fd_array;
};

static int cmp_idx(const void *a, const void *b)
{
	return *(const u32 *)a - *(const u32 *)b;
}

static int add_used_map(struct hornet_maps *maps, int idx)
{
	int i;

	for (i = 0; i < maps->used_map_cnt; i++)
		if (maps->used_idx[i] == idx)
			return i;

	if (maps->used_map_cnt >= MAX_USED_MAPS)
		return -E2BIG;

	maps->used_idx[maps->used_map_cnt] = idx;
	return maps->used_map_cnt++;
}

static int hornet_find_maps(struct bpf_prog *prog, struct hornet_maps *maps)
{
	struct bpf_insn *insn = prog->insnsi;
	int insn_cnt = prog->len;
	int i;
	int err;

	for (i = 0; i < insn_cnt; i++, insn++) {
		if (insn[0].code == (BPF_LD | BPF_IMM | BPF_DW)) {
			switch (insn[0].src_reg) {
			case BPF_PSEUDO_MAP_IDX_VALUE:
			case BPF_PSEUDO_MAP_IDX:
				err = add_used_map(maps, insn[0].imm);
				if (err < 0)
					return err;
				break;
			default:
				break;
			}
		}
	}
	/* Sort the spare-array indices. This should match the map ordering used during
	 * signature generation
	 */
	sort(maps->used_idx, maps->used_map_cnt, sizeof(*maps->used_idx),
	     cmp_idx, NULL);

	return 0;
}

static int hornet_populate_fd_array(struct hornet_maps *maps, u32 fd_array_cnt)
{
	int i;

	if (fd_array_cnt > MAX_USED_MAPS)
		return -E2BIG;

	for (i = 0; i < fd_array_cnt; i++)
		maps->used_idx[i] = i;

	maps->used_map_cnt = fd_array_cnt;
	return 0;
}

/* kern_sys_bpf is declared as an EXPORT_SYMBOL in kernel/bpf/syscall.c, however no definition is
 * provided in any bpf header files. If/when this function has a proper definition provided
 * somewhere this declaration should be removed
 */
int kern_sys_bpf(int cmd, union bpf_attr *attr, unsigned int size);

static int hornet_verify_lskel(struct bpf_prog *prog, struct hornet_maps *maps,
			       void *sig, size_t sig_len)
{
	int fd;
	u32 i;
	void *buf;
	void *new;
	size_t buf_sz;
	struct bpf_map *map;
	int err = 0;
	int key = 0;
	union bpf_attr attr = {0};

	buf = kmalloc_array(prog->len, sizeof(struct bpf_insn), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	buf_sz = prog->len * sizeof(struct bpf_insn);
	memcpy(buf, prog->insnsi, buf_sz);

	for (i = 0; i < maps->used_map_cnt; i++) {
		err = copy_from_bpfptr_offset(&fd, maps->fd_array,
					      maps->used_idx[i] * sizeof(fd),
					      sizeof(fd));
		if (err < 0)
			continue;
		if (fd < 1)
			continue;

		map = bpf_map_get(fd);
		if (IS_ERR(map))
			continue;

		/* don't allow userspace to change map data used for signature verification */
		if (!map->frozen) {
			attr.map_fd = fd;
			err = kern_sys_bpf(BPF_MAP_FREEZE, &attr, sizeof(attr));
			if (err < 0)
				goto out;
		}

		new = krealloc(buf, buf_sz + map->value_size, GFP_KERNEL);
		if (!new) {
			err = -ENOMEM;
			goto out;
		}
		buf = new;
		new = map->ops->map_lookup_elem(map, &key);
		if (!new) {
			err = -ENOENT;
			goto out;
		}
		memcpy(buf + buf_sz, new, map->value_size);
		buf_sz += map->value_size;
	}

	err = verify_pkcs7_signature(buf, buf_sz, sig, sig_len,
				     VERIFY_USE_SECONDARY_KEYRING,
				     VERIFYING_EBPF_SIGNATURE,
				     NULL, NULL);
out:
	kfree(buf);
	return err;
}

static int hornet_check_binary(struct bpf_prog *prog, union bpf_attr *attr,
			       struct hornet_maps *maps)
{
	struct file *file = get_task_exe_file(current);
	const unsigned long markerlen = sizeof(EBPF_SIG_STRING) - 1;
	void *buf = NULL;
	size_t sz = 0, sig_len, prog_len, buf_sz;
	int err = 0;
	struct module_signature sig;

	buf_sz = kernel_read_file(file, 0, &buf, INT_MAX, &sz, READING_EBPF);
	fput(file);
	if (!buf_sz)
		return -1;

	prog_len = buf_sz;

	if (prog_len > markerlen &&
	    memcmp(buf + prog_len - markerlen, EBPF_SIG_STRING, markerlen) == 0)
		prog_len -= markerlen;

	memcpy(&sig, buf + (prog_len - sizeof(sig)), sizeof(sig));
	sig_len = be32_to_cpu(sig.sig_len);
	prog_len -= sig_len + sizeof(sig);

	err = mod_check_sig(&sig, prog->len * sizeof(struct bpf_insn), "ebpf");
	if (err)
		return err;
	return hornet_verify_lskel(prog, maps, buf + prog_len, sig_len);
}

static int hornet_check_signature(struct bpf_prog *prog, union bpf_attr *attr,
				  struct bpf_token *token)
{
	struct hornet_maps maps = {0};
	int err;

	/* support both sparse arrays and explicit continuous arrays of map fds */
	if (attr->fd_array_cnt)
		err = hornet_populate_fd_array(&maps, attr->fd_array_cnt);
	else
		err = hornet_find_maps(prog, &maps);

	if (err < 0)
		return err;

	maps.fd_array = make_bpfptr(attr->fd_array, false);
	return hornet_check_binary(prog, attr, &maps);
}

static int hornet_bpf_prog_load(struct bpf_prog *prog, union bpf_attr *attr,
				struct bpf_token *token, bool is_kernel)
{
	if (is_kernel)
		return 0;
	return hornet_check_signature(prog, attr, token);
}

static struct security_hook_list hornet_hooks[] __ro_after_init = {
	LSM_HOOK_INIT(bpf_prog_load, hornet_bpf_prog_load),
};

static const struct lsm_id hornet_lsmid = {
	.name = "hornet",
	.id = LSM_ID_HORNET,
};

static int __init hornet_init(void)
{
	pr_info("Hornet: eBPF signature verification enabled\n");
	security_add_hooks(hornet_hooks, ARRAY_SIZE(hornet_hooks), &hornet_lsmid);
	return 0;
}

DEFINE_LSM(hornet) = {
	.name = "hornet",
	.init = hornet_init,
};
