// SPDX-License-Identifier: GPL-2.0
/*
 * Secure Launch late validation/setup, securityfs exposure and finalization.
 *
 * Copyright (c) 2026 Apertus Solutions, LLC
 * Copyright (c) 2026 Assured Information Security, Inc.
 * Copyright (c) 2026, Oracle and/or its affiliates.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/linkage.h>
#include <linux/mm.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/security.h>
#include <linux/memblock.h>
#include <linux/tpm.h>
#include <asm/segment.h>
#include <asm/sections.h>
#include <crypto/sha2.h>
#include <linux/efi.h>
#include <linux/slr_table.h>
#include <linux/slaunch.h>

/*
 * The macro DECLARE_TXT_PUB_READ_U is used to read values from the TXT
 * public registers as unsigned values.
 */
#define DECLARE_TXT_PUB_READ_U(size, fmt, msg_size)			\
static ssize_t txt_pub_read_u##size(unsigned int offset,		\
		loff_t *read_offset,					\
		size_t read_len,					\
		char __user *buf)					\
{									\
	char msg_buffer[msg_size];					\
	u##size reg_value = 0;						\
	void __iomem *txt;						\
									\
	txt = ioremap(TXT_PUB_CONFIG_REGS_BASE,				\
			TXT_NR_CONFIG_PAGES * PAGE_SIZE);		\
	if (!txt)							\
		return -EFAULT;						\
	memcpy_fromio(&reg_value, txt + offset, sizeof(u##size));	\
	iounmap(txt);							\
	snprintf(msg_buffer, msg_size, fmt, reg_value);			\
	return simple_read_from_buffer(buf, read_len, read_offset,	\
			&msg_buffer, msg_size);				\
}

DECLARE_TXT_PUB_READ_U(8, "%#04x\n", 6);
DECLARE_TXT_PUB_READ_U(32, "%#010x\n", 12);
DECLARE_TXT_PUB_READ_U(64, "%#018llx\n", 20);

#define DECLARE_TXT_FOPS(reg_name, reg_offset, reg_size)		\
static ssize_t txt_##reg_name##_read(struct file *flip,			\
		char __user *buf, size_t read_len, loff_t *read_offset)	\
{									\
	return txt_pub_read_u##reg_size(reg_offset, read_offset,	\
			read_len, buf);					\
}									\
static const struct file_operations reg_name##_ops = {			\
	.read = txt_##reg_name##_read,					\
}

DECLARE_TXT_FOPS(sts, TXT_CR_STS, 64);
DECLARE_TXT_FOPS(ests, TXT_CR_ESTS, 8);
DECLARE_TXT_FOPS(errorcode, TXT_CR_ERRORCODE, 32);
DECLARE_TXT_FOPS(didvid, TXT_CR_DIDVID, 64);
DECLARE_TXT_FOPS(e2sts, TXT_CR_E2STS, 64);
DECLARE_TXT_FOPS(ver_emif, TXT_CR_VER_EMIF, 32);
DECLARE_TXT_FOPS(scratchpad, TXT_CR_SCRATCHPAD, 64);

/*
 * Securityfs exposure
 */
struct memfile {
	char *name;
	void *addr;
	size_t size;
};

static struct memfile sl_evtlog = { "eventlog", NULL, 0 };
static void *txt_heap;
static struct txt_heap_event_log_pointer2_1_element *evtlog21;
static DEFINE_MUTEX(sl_evt_log_mutex);
static struct tcg_efi_specid_event_head *efi_head;

static ssize_t sl_evtlog_read(struct file *file, char __user *buf,
			      size_t count, loff_t *pos)
{
	ssize_t size;

	if (!sl_evtlog.addr)
		return 0;

	mutex_lock(&sl_evt_log_mutex);
	size = simple_read_from_buffer(buf, count, pos, sl_evtlog.addr,
				       sl_evtlog.size);
	mutex_unlock(&sl_evt_log_mutex);

	return size;
}

static ssize_t sl_evtlog_write(struct file *file, const char __user *buf,
			       size_t datalen, loff_t *ppos)
{
	ssize_t result;
	char *data;

	if (!sl_evtlog.addr)
		return 0;

	/* No partial writes. */
	result = -EINVAL;
	if (*ppos != 0)
		goto out;

	data = memdup_user(buf, datalen);
	if (IS_ERR(data)) {
		result = PTR_ERR(data);
		goto out;
	}

	mutex_lock(&sl_evt_log_mutex);
	if (evtlog21)
		result = tpm2_log_event(evtlog21, sl_evtlog.addr,
					sl_evtlog.size, datalen, data);
	else
		result = tpm_log_event(sl_evtlog.addr, sl_evtlog.size,
				       datalen, data);
	mutex_unlock(&sl_evt_log_mutex);

	kfree(data);
out:
	return result;
}

static const struct file_operations sl_evtlog_ops = {
	.read = sl_evtlog_read,
	.write = sl_evtlog_write,
	.llseek = default_llseek,
};

struct sfs_file {
	const char *name;
	const struct file_operations *fops;
};

#define SL_TXT_ENTRY_COUNT	7
static const struct sfs_file sl_txt_files[] = {
	{ "sts", &sts_ops },
	{ "ests", &ests_ops },
	{ "errorcode", &errorcode_ops },
	{ "didvid", &didvid_ops },
	{ "ver_emif", &ver_emif_ops },
	{ "scratchpad", &scratchpad_ops },
	{ "e2sts", &e2sts_ops }
};

/* sysfs file handles */
static struct dentry *slaunch_dir;
static struct dentry *event_file;
static struct dentry *txt_dir;
static struct dentry *txt_entries[SL_TXT_ENTRY_COUNT];

static long slaunch_expose_securityfs(void)
{
	long ret = 0;
	int i;

	slaunch_dir = securityfs_create_dir("slaunch", NULL);
	if (IS_ERR(slaunch_dir))
		return PTR_ERR(slaunch_dir);

	if (slaunch_get_flags() & SL_FLAG_ARCH_TXT) {
		txt_dir = securityfs_create_dir("txt", slaunch_dir);
		if (IS_ERR(txt_dir)) {
			ret = PTR_ERR(txt_dir);
			goto remove_slaunch;
		}

		for (i = 0; i < ARRAY_SIZE(sl_txt_files); i++) {
			txt_entries[i] =
				securityfs_create_file(sl_txt_files[i].name, 0440, txt_dir,
						       NULL, sl_txt_files[i].fops);
			if (IS_ERR(txt_entries[i])) {
				ret = PTR_ERR(txt_entries[i]);
				goto remove_files;
			}
		}
	}

	if (sl_evtlog.addr) {
		event_file = securityfs_create_file(sl_evtlog.name, 0440,
						    slaunch_dir, NULL,
						    &sl_evtlog_ops);
		if (IS_ERR(event_file)) {
			ret = PTR_ERR(event_file);
			goto remove_files;
		}
	}

	return 0;

remove_files:
	if (slaunch_get_flags() & SL_FLAG_ARCH_TXT) {
		while (--i >= 0)
			securityfs_remove(txt_entries[i]);
		securityfs_remove(txt_dir);
	}

remove_slaunch:
	securityfs_remove(slaunch_dir);

	return ret;
}

static void slaunch_teardown_securityfs(void)
{
	int i;

	securityfs_remove(event_file);
	if (sl_evtlog.addr) {
		memunmap(sl_evtlog.addr);
		sl_evtlog.addr = NULL;
	}
	sl_evtlog.size = 0;

	if (slaunch_get_flags() & SL_FLAG_ARCH_TXT) {
		for (i = 0; i < ARRAY_SIZE(sl_txt_files); i++)
			securityfs_remove(txt_entries[i]);

		securityfs_remove(txt_dir);

		if (txt_heap) {
			memunmap(txt_heap);
			txt_heap = NULL;
		}
	}

	securityfs_remove(slaunch_dir);
}

static void __init slaunch_intel_evtlog(void __iomem *txt)
{
	struct slr_entry_log_info *log_info;
	struct txt_os_mle_data *params;
	struct slr_table *slrt;
	void *os_sinit_data;
	u64 base, size;

	memcpy_fromio(&base, txt + TXT_CR_HEAP_BASE, sizeof(base));
	memcpy_fromio(&size, txt + TXT_CR_HEAP_SIZE, sizeof(size));

	/* now map TXT heap */
	txt_heap = memremap(base, size, MEMREMAP_WB);
	if (!txt_heap)
		slaunch_reset(txt, "Error memremap TXT heap failed\n", SL_ERROR_HEAP_MAP);

	params = (struct txt_os_mle_data *)slaunch_txt_get_heap_table(txt_heap,
								      TXT_OS_MLE_DATA_TABLE);

	/* Get the SLRT and remap it */
	slrt = memremap(params->slrt, sizeof(*slrt), MEMREMAP_WB);
	if (!slrt)
		slaunch_reset(txt, "Error memremap SLR Table failed\n", SL_ERROR_SLRT_MAP);
	size = slrt->size;
	memunmap(slrt);

	slrt = memremap(params->slrt, size, MEMREMAP_WB);
	if (!slrt)
		slaunch_reset(txt, "Error memremap SLR Table failed\n", SL_ERROR_SLRT_MAP);

	log_info = slr_next_entry_by_tag(slrt, NULL, SLR_ENTRY_LOG_INFO);
	if (!log_info)
		slaunch_reset(txt, "Error SLR Table missing entry\n", SL_ERROR_SLRT_MISSING_ENTRY);

	sl_evtlog.size = log_info->size;
	sl_evtlog.addr = memremap(log_info->addr, log_info->size, MEMREMAP_WB);
	if (!sl_evtlog.addr)
		slaunch_reset(txt, "Error memremap TPM event log failed\n", SL_ERROR_EVENTLOG_MAP);

	memunmap(slrt);

	/* Determine if this is TPM 1.2 or 2.0 event log */
	if (memcmp(sl_evtlog.addr + sizeof(struct tcg_pcr_event),
		   TCG_SPECID_SIG, sizeof(TCG_SPECID_SIG)))
		return; /* looks like it is not 2.0 */

	/* For TPM 2.0 logs, the extended heap element must be located */
	os_sinit_data = slaunch_txt_get_heap_table(txt_heap, TXT_OS_SINIT_DATA_TABLE);

	evtlog21 = txt_find_log2_1_element(os_sinit_data);

	/*
	 * If this fails, things are in really bad shape. Any attempt to write
	 * events to the log will fail.
	 */
	if (!evtlog21)
		slaunch_reset(txt, "Error locate TPM20 event log element failed\n",
			      SL_ERROR_TPM_INVALID_LOG20);

	/* Save pointer to the EFI SpecID log header */
	efi_head = (struct tcg_efi_specid_event_head *)(sl_evtlog.addr +
							sizeof(struct tcg_pcr_event));
}

static void __init slaunch_tpm_open_locality2(void __iomem *txt)
{
	struct tpm_chip *tpm;
	int rc;

	tpm = tpm_default_chip();
	if (!tpm)
		slaunch_reset(txt, "Could not get default TPM chip\n", SL_ERROR_TPM_INIT);

	rc = tpm_chip_set_locality(tpm, 2);
	if (rc)
		slaunch_reset(txt, "Could not set TPM chip locality 2\n", SL_ERROR_TPM_INIT);
}

static int __init slaunch_module_init(void)
{
	void __iomem *txt;

	/* Check to see if Secure Launch happened */
	if ((slaunch_get_flags() & (SL_FLAG_ACTIVE|SL_FLAG_ARCH_TXT)) !=
	    (SL_FLAG_ACTIVE | SL_FLAG_ARCH_TXT))
		return 0;

	txt = ioremap(TXT_PRIV_CONFIG_REGS_BASE, TXT_NR_CONFIG_PAGES *
		      PAGE_SIZE);
	if (!txt)
		panic("Error ioremap of TXT priv registers\n");

	/* Only Intel TXT is supported at this point */
	slaunch_intel_evtlog(txt);
	slaunch_tpm_open_locality2(txt);
	iounmap(txt);

	return slaunch_expose_securityfs();
}

static void __exit slaunch_module_exit(void)
{
	slaunch_teardown_securityfs();
}

late_initcall(slaunch_module_init);
__exitcall(slaunch_module_exit);
