// SPDX-License-Identifier: GPL-2.0
/*
 * Kexec PE image loader

 * Copyright (C) 2025 Red Hat, Inc
 */

#define pr_fmt(fmt)	"kexec_file(Image): " fmt

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/list.h>
#include <linux/kernel.h>
#include <linux/vmalloc.h>
#include <linux/kexec.h>
#include <linux/pe.h>
#include <linux/string.h>
#include <linux/bpf.h>
#include <linux/filter.h>
#include <linux/decompress/generic.h>
#include <asm/byteorder.h>
#include <asm/cpufeature.h>
#include <asm/image.h>
#include <asm/memory.h>


static LIST_HEAD(phase_head);

struct parsed_phase {
	struct list_head head;
	struct list_head res_head;
};

static struct parsed_phase *cur_phase;

static char *kexec_res_names[3] = {"kernel", "initrd", "cmdline"};

struct kexec_res {
	struct list_head node;
	char *name;
	char *buf;
	int size;
	bool kmalloc;
	bool deferred_free;
};

static struct parsed_phase *alloc_new_phase(void)
{
	struct parsed_phase *phase = kzalloc(sizeof(struct parsed_phase), GFP_KERNEL);

	INIT_LIST_HEAD(&phase->head);
	INIT_LIST_HEAD(&phase->res_head);
	list_add_tail(&phase->head, &phase_head);

	return phase;
}

struct mem_range_result {
	refcount_t usage;
	/*
	 * Pointer to a kernel space, which is written by kfunc and read by
	 * bpf-prog. Hence kfunc guarantees its validation.
	 */
	char *buf;
	uint32_t size;     // Size of decompressed data
	int status;        // Status code (0 for success)
};

#define MAX_KEXEC_RES_SIZE	(1 << 29)

BTF_KFUNCS_START(bpf_kexec_ids)
BTF_ID_FLAGS(func, bpf_kexec_carrier, KF_TRUSTED_ARGS)
BTF_ID_FLAGS(func, bpf_kexec_decompress, KF_TRUSTED_ARGS | KF_ACQUIRE)
BTF_ID_FLAGS(func, bpf_kexec_result_release, KF_RELEASE)
BTF_KFUNCS_END(bpf_kexec_ids)

static const struct btf_kfunc_id_set kexec_kfunc_set = {
	.owner = THIS_MODULE,
	.set = &bpf_kexec_ids,
};

/*
 * Copy the partial decompressed content in [buf, buf + len) to dst.
 * If the dst size is beyond the capacity, return 0 to indicate the
 * decompress method that something is wrong.
 */
//to do
static long flush_buffer(void *buf, unsigned long len)
{

	//return len to indicate everything goest smoothly
	return 0;
}


__bpf_kfunc_start_defs();

/*
 * @name should be one of : kernel, initrd, cmdline
 */
__bpf_kfunc int bpf_kexec_carrier(const char *name, struct mem_range_result *r)
{
	struct kexec_res *res;
	int ret = 0;

	if (!r) {
		pr_err("%s, receive invalid range\n", __func__);
		return -EINVAL;
	}

	if (!r || !name)
		return -EINVAL;
	if (r->size == 0 || r->size > MAX_KEXEC_RES_SIZE) {
		pr_err("Invalid resource size: 0x%x\n", r->size);
		return -EINVAL;
	}

	res = kzalloc(sizeof(struct kexec_res), GFP_KERNEL);
	if (!res)
		return -ENOMEM;

	for (int i = 0; i < ARRAY_SIZE(kexec_res_names); i++) {
		if (!strcmp(kexec_res_names[i], name))
			res->name = kexec_res_names[i];
	}

	if (res->name == NULL) {
		pr_err("Invalid resource name: %s, should be 'kernel', 'initrd', 'cmdline'\n", name);
		kfree(res);
		return -EINVAL;
	}

	res->buf = vmalloc(r->size);
	if (!res->buf) {
		kfree(res);
		return -ENOMEM;
	}
	ret = copy_from_kernel_nofault(res->buf, r->buf, r->size);
	if (unlikely(ret < 0)) {
		kfree(res->buf);
		kfree(res);
		return -EINVAL;
	}
	res->size = r->size;

	INIT_LIST_HEAD(&res->node);
	list_add_tail(&res->node, &cur_phase->res_head);
	return 0;
}

__bpf_kfunc struct mem_range_result *bpf_kexec_decompress(char *image_gz_payload, int image_gz_sz,
			unsigned int expected_decompressed_sz)
{
	decompress_fn decompressor;
	//todo, use flush to cap the memory size used by decompression
	long (*flush)(void*, unsigned long) = NULL;
	struct mem_range_result *range;
	const char *name;
	void *output_buf;
	char *input_buf;
	int ret;

	range = kmalloc(sizeof(struct mem_range_result), GFP_KERNEL);
	if (!range) {
		pr_err("fail to allocate mem_range_result\n");
		return NULL;
	}
	refcount_set(&range->usage, 1);

	input_buf = vmalloc(image_gz_sz);
	if (!input_buf) {
		pr_err("fail to allocate input buffer\n");
		kfree(range);
		return NULL;
	}

	ret = copy_from_kernel_nofault(input_buf, image_gz_payload, image_gz_sz);
	if (ret < 0) {
		pr_err("Error when copying from 0x%px, size:0x%x\n",
				image_gz_payload, image_gz_sz);
		kfree(range);
		vfree(input_buf);
		return NULL;
	}

	output_buf = vmalloc(expected_decompressed_sz);
	if (!output_buf) {
		pr_err("fail to allocate output buffer\n");
		kfree(range);
		vfree(input_buf);
		return NULL;
	}

	decompressor = decompress_method(input_buf, image_gz_sz, &name);
	if (!decompressor) {
		pr_err("Can not find decompress method\n");
		kfree(range);
		vfree(input_buf);
		vfree(output_buf);
		return NULL;
	}
	//to do, use flush
	ret = decompressor(image_gz_payload, image_gz_sz, NULL, NULL,
				output_buf, NULL, NULL);

	/* Update the range map */
	if (ret == 0) {
		range->buf = output_buf;
		range->size = expected_decompressed_sz;
		range->status = 0;
	} else {
		pr_err("Decompress error\n");
		vfree(output_buf);
		kfree(range);
		return NULL;
	}
	pr_info("%s, return range 0x%lx\n", __func__, range);
	return range;
}

__bpf_kfunc int bpf_kexec_result_release(struct mem_range_result *result)
{
	if (!result) {
		pr_err("%s, receive invalid range\n", __func__);
		return -EINVAL;
	}

	if (refcount_dec_and_test(&result->usage)) {
		vfree(result->buf);
		kfree(result);
	}

	return 0;
}

__bpf_kfunc_end_defs();

static bool is_valid_pe(const char *kernel_buf, unsigned long kernel_len)
{
	struct mz_hdr *mz;
	struct pe_hdr *pe;

	if (!kernel_buf)
		return false;
	mz = (struct mz_hdr *)kernel_buf;
	if (mz->magic != MZ_MAGIC)
		return false;
	pe = (struct pe_hdr *)(kernel_buf + mz->peaddr);
	if (pe->magic != PE_MAGIC)
		return false;
	if (pe->opt_hdr_size == 0) {
		pr_err("optional header is missing\n");
		return false;
	}

	return true;
}

static bool is_valid_format(const char *kernel_buf, unsigned long kernel_len)
{
	return is_valid_pe(kernel_buf, kernel_len);
}

/*
 * The UEFI Terse Executable (TE) image has MZ header.
 */
static int pe_image_probe(const char *kernel_buf, unsigned long kernel_len)
{
	return is_valid_pe(kernel_buf, kernel_len) ? 0 : -1;
}

static int get_pe_section(char *file_buf, const char *sect_name,
		char **sect_start, unsigned long *sect_sz)
{
	struct pe_hdr *pe_hdr;
	struct pe32plus_opt_hdr *opt_hdr;
	struct section_header *sect_hdr;
	int section_nr, i;
	struct mz_hdr *mz = (struct mz_hdr *)file_buf;

	*sect_start = NULL;
	*sect_sz = 0;
	pe_hdr = (struct pe_hdr *)(file_buf + mz->peaddr);
	section_nr = pe_hdr->sections;
	opt_hdr = (struct pe32plus_opt_hdr *)(file_buf + mz->peaddr + sizeof(struct pe_hdr));
	sect_hdr = (struct section_header *)((char *)opt_hdr + pe_hdr->opt_hdr_size);

	for (i = 0; i < section_nr; i++) {
		if (strcmp(sect_hdr->name, sect_name) == 0) {
			*sect_start = file_buf + sect_hdr->data_addr;
			*sect_sz = sect_hdr->raw_data_size;
			return 0;
		}
		sect_hdr++;
	}

	return -1;
}

static bool pe_has_bpf_section(char *file_buf, unsigned long pe_sz)
{
	char *sect_start = NULL;
	unsigned long sect_sz = 0;
	int ret;

	ret = get_pe_section(file_buf, ".bpf", &sect_start, &sect_sz);
	if (ret < 0)
		return false;
	return true;
}

/* Load a ELF */
static int arm_bpf_prog(char *bpf_elf, unsigned long sz)
{
	return 0;
}

static void disarm_bpf_prog(void)
{
}

/*
 * In eBPF, functions can only pass up to five arguments through R1 to R5.
 * If five arguments are not enough, considering parse_zboot(struct pt_regs *regs)
 *
 * optimize("O0") prevents inline, compiler constant propagation
 */
__attribute__((used, optimize("O0"))) void bpf_handle_pefile(char *image, int image_sz,
			char *initrd, int initrd_sz, char *cmdline)
{
}

__attribute__((used, optimize("O0"))) void bpf_post_handle_pefile(void)
{
}

/*
 * PE file may be nested and should be unfold one by one.
 * Query 'kernel', 'initrd', 'cmdline' in cur_phase, as they are inputs for the
 * next phase.
 */
static int prepare_nested_pe(char **kernel, unsigned long *kernel_len, char **initrd,
		unsigned long *initrd_len, char **cmdline)
{
	struct kexec_res *res;
	int ret = -1;

	*kernel = NULL;
	*kernel_len = 0;

	list_for_each_entry(res, &cur_phase->res_head, node) {
		if (res->name == kexec_res_names[0]) {
			*kernel = res->buf;
			*kernel_len = res->size;
			ret = 0;
		} else if (res->name == kexec_res_names[1]) {
			*initrd = res->buf;
			*initrd_len = res->size;
		} else if (res->name == kexec_res_names[2]) {
			*cmdline = res->buf;
		}
	}

	return ret;
}

static void *pe_image_load(struct kimage *image,
				char *kernel, unsigned long kernel_len,
				char *initrd, unsigned long initrd_len,
				char *cmdline, unsigned long cmdline_len)
{
	char *parsed_kernel = NULL;
	unsigned long parsed_len;
	char *linux_start, *initrd_start, *cmdline_start, *bpf_start;
	unsigned long linux_sz, initrd_sz, cmdline_sz, bpf_sz;
	struct parsed_phase *phase, *phase_tmp;
	struct kexec_res *res, *res_tmp;
	void *ldata;
	int ret;

	linux_start = kernel;
	linux_sz = kernel_len;
	initrd_start = initrd;
	initrd_sz = initrd_len;
	cmdline_start = cmdline;
	cmdline_sz = cmdline_len;

	while(is_valid_format(linux_start, linux_sz) &&
	      pe_has_bpf_section(linux_start, linux_sz)) {

		get_pe_section(linux_start, ".bpf", &bpf_start, &bpf_sz);
		if (!!bpf_sz) {
			/* load and attach bpf-prog */
			ret = arm_bpf_prog(bpf_start, bpf_sz);
			if (ret) {
				pr_err("Fail to load .bpf section\n");
				goto err;
			}
		}
		cur_phase = alloc_new_phase();
		/*
		 * bpf-prog fentry, which handle above buffers, and
		 * bpf_carrier_helper() fills each phase
		 */
		bpf_handle_pefile(linux_start, linux_sz, initrd_start, initrd_sz,
					cmdline_start);

		prepare_nested_pe(&linux_start, &linux_sz, &initrd_start,
					&initrd_sz, &cmdline_start);
		/* bpf-prog fentry */
		bpf_post_handle_pefile();
		/*
		 * detach the current bpf-prog from their attachment points.
		 * It also a point to free any registered interim resource.
		 * Any resource except attached to phase is interim.
		 */
		disarm_bpf_prog();
	}

	/* the rear of parsed phase contains the result */
	list_for_each_entry_reverse(phase, &phase_head, head) {
		if (initrd != NULL && cmdline != NULL && parsed_kernel != NULL)
			break;
		list_for_each_entry(res, &phase->res_head, node) {
			if (!strcmp(res->name, "kernel") && !parsed_kernel) {
				parsed_kernel = res->buf;
				parsed_len = res->size;
				res->deferred_free = true;
			} else if (!strcmp(res->name, "initrd") && !initrd) {
				initrd = res->buf;
				initrd_len = res->size;
				res->deferred_free = true;
			} else if (!strcmp(res->name, "cmdline") && !cmdline) {
				cmdline = res->buf;
				cmdline_len = res->size;
				res->deferred_free = true;
			}
		}

	}

	if (initrd == NULL || cmdline == NULL || parsed_kernel == NULL) {
		char *c, buf[64];

		c = buf;
		if (parsed_kernel == NULL) {
			strcpy(c, "kernel ");
			c += strlen("kernel ");
		}
		if (initrd == NULL) {
			strcpy(c, "initrd ");
			c += strlen("initrd ");
		}
		if (cmdline == NULL) {
			strcpy(c, "cmdline ");
			c += strlen("cmdline ");
		}
		c = '\0';
		pr_err("Can not extract data for %s", buf);
		ret = -EINVAL;
		goto err;
	}
	/*
	 * image's kernel_buf, initrd_buf, cmdline_buf are set. Now they should
	 * be updated to the new content.
	 */
	if (image->kernel_buf != parsed_kernel) {
		vfree(image->kernel_buf);
		image->kernel_buf = parsed_kernel;
		image->kernel_buf_len = parsed_len;
	}
	if (image->initrd_buf != initrd) {
		vfree(image->initrd_buf);
		image->initrd_buf = initrd;
		image->initrd_buf_len = initrd_len;
	}
	if (image->cmdline_buf != cmdline) {
		kfree(image->cmdline_buf);
		image->cmdline_buf = cmdline;
		image->cmdline_buf_len = cmdline_len;
	}
	ret = arch_kexec_kernel_image_probe(image, image->kernel_buf,
					    image->kernel_buf_len);
	if (ret) {
		pr_err("Fail to find suitable image loader\n");
		goto err;
	}
	ldata = kexec_image_load_default(image);
	if (IS_ERR(ldata)) {
		ret = PTR_ERR(ldata);
		goto err;
	}
	image->image_loader_data = ldata;

err:
	list_for_each_entry_safe(phase, phase_tmp, &phase_head, head) {
		list_for_each_entry_safe(res, res_tmp, &phase->res_head, node) {
			list_del(&res->node);
			/* defer to kimage_file_post_load_cleanup() */
			if (!res->deferred_free) {
				if (res->kmalloc)
					kfree(res->buf);
				else
					vfree(res->buf);
			}
			kfree(res);
		}
		list_del(&phase->head);
		kfree(phase);
	}

	return ERR_PTR(ret);
}

const struct kexec_file_ops kexec_pe_image_ops = {
	.probe = pe_image_probe,
	.load = pe_image_load,
#ifdef CONFIG_KEXEC_IMAGE_VERIFY_SIG
	.verify_sig = kexec_kernel_verify_pe_sig,
#endif
};

static int __init bpf_kfunc_init(void)
{
	int ret;

	ret = register_btf_kfunc_id_set(BPF_PROG_TYPE_TRACING, &kexec_kfunc_set);
	if (!!ret)
		pr_err("Fail to register btf for kexec_kfunc_set\n");
	return ret;
}
late_initcall(bpf_kfunc_init);
