/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LINUX_KEXEC_INTERNAL_H
#define LINUX_KEXEC_INTERNAL_H

#include <linux/kexec.h>

struct kimage *do_kimage_alloc_init(void);
int sanity_check_segment_list(struct kimage *image);
void kimage_free_page_list(struct list_head *list);
void kimage_free(struct kimage *image);
int kimage_load_segment(struct kimage *image, struct kexec_segment *segment);
void kimage_terminate(struct kimage *image);
int kimage_is_destination_range(struct kimage *image,
				unsigned long start, unsigned long end);

/*
 * Whatever is used to serialize accesses to the kexec_crash_image needs to be
 * NMI safe, as __crash_kexec() can happen during nmi_panic(), so here we use a
 * "simple" atomic variable that is acquired with a cmpxchg().
 */
extern atomic_t __kexec_lock;
static inline bool kexec_trylock(void)
{
	return atomic_cmpxchg_acquire(&__kexec_lock, 0, 1) == 0;
}
static inline void kexec_unlock(void)
{
	atomic_set_release(&__kexec_lock, 0);
}

/*
 * Different than kexec/kdump loading/unloading/crash or kexec jumping/shrinking
 * which usually rarely happen, there will be many crash hotplug events notified
 * during one short period, e.g one memory board is hot added and memory regions
 * are online. So mutex lock  __crash_hotplug_lock is used to serialize the crash
 * hotplug handling specificially.
 * */
extern struct mutex __crash_hotplug_lock;
#define crash_hotplug_lock() mutex_lock(&__crash_hotplug_lock)
#define crash_hotplug_unlock() mutex_unlock(&__crash_hotplug_lock)

#ifdef CONFIG_KEXEC_FILE
#include <linux/purgatory.h>
void kimage_file_post_load_cleanup(struct kimage *image);
extern char kexec_purgatory[];
extern size_t kexec_purgatory_size;
#else /* CONFIG_KEXEC_FILE */
static inline void kimage_file_post_load_cleanup(struct kimage *image) { }
#endif /* CONFIG_KEXEC_FILE */
#endif /* LINUX_KEXEC_INTERNAL_H */
