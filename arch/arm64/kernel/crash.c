// SPDX-License-Identifier: GPL-2.0-only
/*
 * Architecture specific functions for kexec based crash dumps.
 */

#define pr_fmt(fmt)	"crash hp: " fmt

#include <linux/kexec.h>
#include <linux/elf.h>
#include <linux/vmalloc.h>
#include <linux/crash_core.h>

#include <asm/kexec.h>

#ifdef CONFIG_CRASH_HOTPLUG

int arch_crash_hotplug_support(struct kimage *image, unsigned long kexec_flags)
{
#ifdef CONFIG_KEXEC_FILE
	if (image->file_mode)
		return 1;
#endif
	/*
	 * For kexec_load syscall, crash hotplug support requires
	 * KEXEC_CRASH_HOTPLUG_SUPPORT flag to be passed by userspace.
	 */
	return kexec_flags & KEXEC_CRASH_HOTPLUG_SUPPORT;
}

unsigned int arch_crash_get_elfcorehdr_size(void)
{
	unsigned int phdr_cnt;

	/* A program header for possible CPUs, vmcoreinfo and kernel_map */
	phdr_cnt = 2 + num_possible_cpus();
	if (IS_ENABLED(CONFIG_MEMORY_HOTPLUG))
		phdr_cnt += CONFIG_CRASH_MAX_MEMORY_RANGES;

	return sizeof(Elf64_Ehdr) + phdr_cnt * sizeof(Elf64_Phdr);
}

/**
 * update_crash_elfcorehdr() - Recreate the elfcorehdr and replace it with old
 *			       elfcorehdr in the kexec segment array.
 * @image: the active struct kimage
 */
static void update_crash_elfcorehdr(struct kimage *image)
{
	void *elfbuf = NULL, *old_elfcorehdr;
	unsigned long mem, memsz;
	unsigned long elfsz = 0;

	/*
	 * Create the new elfcorehdr reflecting the changes to CPU and/or
	 * memory resources.
	 */
	if (crash_prepare_headers(true, &elfbuf, &elfsz, NULL)) {
		pr_err("unable to create new elfcorehdr");
		goto out;
	}

	/*
	 * Obtain address and size of the elfcorehdr segment, and
	 * check it against the new elfcorehdr buffer.
	 */
	mem = image->segment[image->elfcorehdr_index].mem;
	memsz = image->segment[image->elfcorehdr_index].memsz;
	if (elfsz > memsz) {
		pr_err("update elfcorehdr elfsz %lu > memsz %lu",
			elfsz, memsz);
		goto out;
	}

	/*
	 * Copy new elfcorehdr over the old elfcorehdr at destination.
	 */
	old_elfcorehdr = (void *)__va(mem);
	if (!old_elfcorehdr) {
		pr_err("mapping elfcorehdr segment failed\n");
		goto out;
	}

	/*
	 * Temporarily invalidate the crash image while the
	 * elfcorehdr is updated.
	 */
	xchg(&kexec_crash_image, NULL);
	memcpy_flushcache(old_elfcorehdr, elfbuf, elfsz);
	xchg(&kexec_crash_image, image);
	pr_debug("updated elfcorehdr\n");

out:
	vfree(elfbuf);
}

/**
 * arch_crash_handle_hotplug_event() - Handle hotplug elfcorehdr changes
 * @image: a pointer to kexec_crash_image
 * @arg: struct memory_notify handler for memory hotplug case and
 *       NULL for CPU hotplug case.
 *
 * Update the kdump image based on the type of hotplug event:
 * - CPU add and remove: No action is needed.
 * - Memory add/remove: Update the elfcorehdr to reflect the current memory layout.
 *
 * Prepare the new elfcorehdr and replace the existing elfcorehdr.
 */
void arch_crash_handle_hotplug_event(struct kimage *image, void *arg)
{
	switch (image->hp_action) {
	case KEXEC_CRASH_HP_ADD_CPU:
		fallthrough;
	case KEXEC_CRASH_HP_REMOVE_CPU:
		if (image->file_mode || image->elfcorehdr_updated)
			return;
		fallthrough;
	case KEXEC_CRASH_HP_ADD_MEMORY:
	case KEXEC_CRASH_HP_REMOVE_MEMORY:
		update_crash_elfcorehdr(image);
		return;
	default:
		pr_warn_once("Unknown hotplug action\n");
	}
}
#endif /* CONFIG_CRASH_HOTPLUG */
