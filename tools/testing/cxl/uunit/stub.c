// SPDX-License-Identifier: GPL-2.0
#include "pre.h"
#include "linux/cpuhotplug.h"
#include "linux/rwsem.h"
#include "linux/kobject.h"
#include "linux/device/bus.h"
#include "drivers/cxl/cxl.h"
#include "post.h"

#include <string.h>
#include <stdlib.h>

/* All modules require one of these. */
struct module __this_module;

/*
 * All stubs must be place in a section for the associated kernel source file that defines it.
 * All sections must be sorted by pathname.
 */

/*
 * arch
 * These cannot be mapped to a specific file, since archs are free to define them wherever they
 * want in their arch directory.
 */
void __udelay(unsigned long usecs) { }
unsigned long __stack_chk_guard;
#ifdef CONFIG_ARCH_HAS_CPU_CACHE_INVALIDATE_MEMREGION
int cpu_cache_invalidate_memregion(int res_desc) { return 0; }
bool cpu_cache_has_invalidate_memregion(void) { return true; }
#endif

#ifdef CONFIG_ARM64
/* arch/arm64/kernel/alternative.c */
void alt_cb_patch_nops(struct alt_instr *alt, __le32 *origptr, __le32 *updptr, int nr_inst) {}

/* arch/arm64/lib/copy_from_user.S */
unsigned long __arch_copy_from_user(void *to, const void *from, unsigned long n) { return n; }
DECLARE_BITMAP(system_cpucaps, ARM64_NCAPS);
#endif

#ifdef CONFIG_X86_64
/* arch/x86/kernel/cpu/common.c */
struct pcpu_hot pcpu_hot;

/* arch/x86/kernel/setup_percpu.c */
unsigned long this_cpu_off;
#endif

/* drivers/base/core.c */
int device_add(struct device *dev) { return 0; }
struct device *get_device(struct device *dev) { return NULL; }
void put_device(struct device *dev) { }
void device_unregister(struct device *dev) { }
int device_for_each_child(struct device *dev, void *data,
		int (*fn)(struct device *dev, void *data)) { return 0; }
struct device *device_find_child(struct device *dev, void *data,
		int (*match)(struct device *dev, void *data)) { return NULL; }
struct device *device_find_child_by_name(struct device *parent, const char *name)
{ return NULL; }
const char *dev_driver_string(const struct device *dev) { return NULL; }
void device_del(struct device *dev) { }
int device_match_name(struct device *dev, const void *name) { return 0; }
void device_initialize(struct device *dev) { }
int dev_set_name(struct device *dev, const char* name, ...) { return 0; }
void device_remove_groups(struct device *dev, const struct attribute_group **groups) { }
int device_register(struct device *dev) { return 0; }
int device_add_groups(struct device *dev, const struct attribute_group **groups) { return 0; }
struct kobject *virtual_device_parent(struct device *dev) { return NULL; }
struct kset *devices_kset;

/* drivers/base/dd.c */
int device_attach(struct device *dev) { return 0; }
void device_release_driver(struct device *dev) { }
int device_driver_attach(struct device_driver *drv, struct device *dev) { return 0; }
void device_driver_detach(struct device *dev) { }
int driver_attach(struct device_driver *drv) { return 0; }
void driver_detach(struct device_driver *drv) { }
void device_initial_probe(struct device *dev) { }
void deferred_probe_extend_timeout(void) { }

/* drivers/base/devres.c */
int __devm_add_action(struct device *dev, void (*action)(void *), void *data, const char *name) { return 0; }
void devm_release_action(struct device *dev, void (*action)(void *), void *data) { }
void *devm_kmalloc(struct device *dev, size_t size, gfp_t gfp) { return NULL; }
void devm_remove_action(struct device *dev, void (*action)(void *), void *data) { }

/* drivers/base/module.c */
void module_add_driver(struct module *mod, struct device_driver *drv) { }
void module_remove_driver(struct device_driver *drv) { }

/* drivers/base/power/runtime.c */
int __pm_runtime_idle(struct device *dev, int rpmflags) { return 0; }

/* drivers/char/random.c */
void get_random_bytes(void *buf, size_t len) { }

/* drivers/cxl/core/mbox.c */
int cxl_mem_get_poison(struct cxl_memdev *cxlmd, u64 offset, u64 len,
		struct cxl_region *cxlr) { return 0; }

/* drivers/cxl/core/memdev.c */
bool is_cxl_memdev(const struct device *dev) { return false; }

/* drivers/cxl/core/pmem.c */
struct cxl_nvdimm_bridge *cxl_find_nvdimm_bridge(struct cxl_memdev *cxlmd) { return NULL; }

/* drivers/cxl/core/port.c */
struct attribute_group cxl_base_attribute_group;
struct bus_type cxl_bus_type;
DECLARE_RWSEM(cxl_region_rwsem);
struct cxl_port *to_cxl_port(const struct device *dev)
{
	return container_of(dev, struct cxl_port, dev);
}
struct cxl_root_decoder *to_cxl_root_decoder(struct device *dev)
{
	return container_of(dev, struct cxl_root_decoder, cxlsd.cxld.dev);
}
struct cxl_switch_decoder *to_cxl_switch_decoder(struct device *dev)
{
	return container_of(dev, struct cxl_switch_decoder, cxld.dev);
}
struct cxl_endpoint_decoder *to_cxl_endpoint_decoder(struct device *dev) { return NULL; }
struct cxl_decoder *to_cxl_decoder(struct device *dev) { return NULL; }
bool is_endpoint_decoder(struct device *dev) { return true; }
bool is_root_decoder(struct device *dev) { return true; }
bool is_switch_decoder(struct device *dev) { return true; }
int __cxl_driver_register(struct cxl_driver *cxl_drv, struct module *owner,
		const char *modname) { return 0; }
void cxl_driver_unregister(struct cxl_driver *cxl_drv) { }
int cxl_decoder_add_locked(struct cxl_decoder *cxld, int *target_map) { return 0; }
int cxl_decoder_autoremove(struct device *host, struct cxl_decoder *cxld) { return 0; }
struct cxl_switch_decoder *cxl_switch_decoder_alloc(struct cxl_port *port,
						    unsigned int nr_targets) { return NULL; }
struct cxl_endpoint_decoder *cxl_endpoint_decoder_alloc(struct cxl_port *port) { return NULL; }
int cxl_num_decoders_committed(struct cxl_port *port) { return 0; }

/* drivers/cxl/core/regs.c */
int cxl_map_component_regs(const struct cxl_register_map *map, struct cxl_component_regs *regs,
			   unsigned long map_mask) { return 0; }
void cxl_probe_component_regs(struct device *dev, void *base, struct cxl_component_reg_map *map) { }

/* drivers/irqchip/irq-gic-v3.c */
struct static_key_false gic_nonsecure_priorities;

/* fs/kernfs/dir.c */
void kernfs_get(struct kernfs_node *kn) { }
void kernfs_put(struct kernfs_node *kn) { }

/* fs/seq_file.c */
void seq_printf(struct seq_file *m, const char *f, ...) { }

/* fs/sysfs/file.c */
int sysfs_create_file_ns(struct kobject *kobj, const struct attribute *attr, const void *ns) { return 0; }
void sysfs_remove_file_ns(struct kobject *kobj, const struct attribute *attr, const void *ns) { }
int sysfs_create_dir_ns(struct kobject *kobj, const void *ns) { return 0; }
int sysfs_move_dir_ns(struct kobject *kobj, struct kobject *new_parent_kobj, const void *new_ns) { return 0; }
int sysfs_rename_dir_ns(struct kobject *kobj, const char *new_name, const void *new_ns) { return 0; }
int sysfs_create_groups(struct kobject *kobj, const struct attribute_group **groups) { return 0; }
void sysfs_remove_groups(struct kobject *kobj, const struct attribute_group **groups) { }
int sysfs_update_group(struct kobject *kobj, const struct attribute_group *grp) { return 0; }
int sysfs_create_link(struct kobject *kobj, struct kobject *target, const char *name) { return 0; }
void sysfs_remove_link(struct kobject *kobj, const char *name) { }
void sysfs_remove_dir(struct kobject *kobj) { }
int sysfs_emit(char *buf, const char *fmt, ...)
{
	va_list argptr;
	int rc;

	va_start(argptr, fmt);
	rc = vsprintf(buf, fmt, argptr);
	va_end(argptr);

	return rc;
}
bool sysfs_streq(const char *s1, const char *s2) { return true; }

/* include linux/bitfield.h */
/* Resolves undefined reference error when compiling at -O0 */
void __bad_mask(void) { }

/* include/linux/dev_printk.h, lib/dynamic_debug.c */
void _dev_err(const struct device *dev, const char *fmt, ...) { }
void _dev_warn(const struct device *dev, const char *fmt, ...) { }
void _dev_info(const struct device *dev, const char *fmt, ...) { }
void __dynamic_dev_dbg(struct _ddebug *descriptor, const struct device *dev,
		const char *fmt, ...) { }
void __dynamic_pr_debug(struct _ddebug *descriptor, const char *fmt, ...) { }
void __warn_printk(const char *fmt, ...) { }
int _printk(const char *fmt, ...) { return 0; }
const char *kvasprintf_const(gfp_t gfp, const char *fmt, va_list ap) { return NULL; }

/* kernel/cpu.c */
int __cpuhp_setup_state(enum cpuhp_state state, const char *name, bool invoke,
		int (*startup)(unsigned int cpu),
		int (*teardown)(unsigned int cpu), bool multi_instance) { return 0; }

/* kernel/locking/mutex.c */
void mutex_lock(struct mutex *lock) { }
void mutex_unlock(struct mutex *lock) { }
void __mutex_init(struct mutex *lock, const char *name, struct lock_class_key *key) { }

/* kernel/locking/rwsem.c */
void __init_rwsem(struct rw_semaphore *sem, const char *name, struct lock_class_key *key) { }
void up_read(struct rw_semaphore *sem) { }
void down_read(struct rw_semaphore *sem) { }
int down_read_interruptible(struct rw_semaphore *sem) { return 0; }
void up_write(struct rw_semaphore *sem) { }
void down_write(struct rw_semaphore *sem) { }
int down_write_killable(struct rw_semaphore *sem) { return 0; }

/* kernel/locking/spinlock.c */
void _raw_spin_lock(raw_spinlock_t *lock) { }
void _raw_spin_lock_irq(raw_spinlock_t *lock) { }
void _raw_spin_lock_bh(raw_spinlock_t *lock) { }
void _raw_spin_unlock(raw_spinlock_t *lock) { }
void _raw_spin_unlock_irq(raw_spinlock_t *lock) { }
void _raw_spin_unlock_bh(raw_spinlock_t *lock) { }
unsigned long _raw_spin_lock_irqsave(raw_spinlock_t *lock) { return 0; }
void _raw_spin_unlock_irqrestore(raw_spinlock_t *lock, unsigned long flags) { }

/* kernel/notifier.c */
int blocking_notifier_chain_register(struct blocking_notifier_head *nh, struct notifier_block *n) { return 0; }
int blocking_notifier_chain_unregister(struct blocking_notifier_head *nh, struct notifier_block *n) { return 0; }
int blocking_notifier_call_chain(struct blocking_notifier_head *nh, unsigned long val, void *v) { return 0; }

/* kernel/panic.c */
void __stack_chk_fail(void) { }
void warn_slowpath_fmt(const char *file, const int line, unsigned taint, const char *fmt, ...) { }

/* kernel/rcu/tree.c */
void call_rcu(struct rcu_head *head, rcu_callback_t func) { }

/* kernel/rcu/tree_plugin.h */
void __rcu_read_lock(void) { }
void __rcu_read_unlock(void) { }

/* kernel/resource.c */
int insert_resource(struct resource *parent, struct resource *new) { return 0; }
int remove_resource(struct resource *old) { return 0; }
struct resource *alloc_free_mem_region(struct resource *base, unsigned long size,
		unsigned long align, const char *name) { return NULL; }
struct resource *__request_region(struct resource *parent,
				  resource_size_t start, resource_size_t n,
				  const char *name, int flags) { return NULL; }
void __release_region(struct resource *parent, resource_size_t start, resource_size_t n) { }
int walk_iomem_res_desc(unsigned long desc, unsigned long flags, u64 start, u64 end,
		void *arg, int (*func)(struct resource *, void *)) { return 0; }

/* kernel/sched/core.c */
void dynamic_preempt_schedule(void) { }
int dynamic_might_resched(void) { return 0; }
int wake_up_process(struct task_struct *p) { return 0; }
void schedule(void) { }
//void preempt_schedule(void) { }
//DEFINE_STATIC_CALL(preempt_schedule, preempt_schedule);

/* kernel/time/timer.c */
unsigned int g_msecs;
void msleep(unsigned int msecs) { g_msecs += msecs; }
void usleep_range_state(unsigned long min, unsigned long max, unsigned int state) { }

/* lib/bitmap.c */
void __bitmap_clear(unsigned long *map, unsigned int start, int len) { }

/* lib/ctype.c */
const unsigned char _ctype[1];

/* lib/dump_stack.c */
asmlinkage void dump_stack_lvl(const char *log_lvl) { }

/* lib/kobject_uevent.c */
int kobject_uevent(struct kobject *kobj, enum kobject_action) { return 0; }
int kobject_uevent_env(struct kobject *kobj, enum kobject_action, char *envp[]) { return 0; }
int kobject_synth_uevent(struct kobject *kobj, const char *buf, size_t count) { return 0; }

/* lib/logic_iomem.c */
#ifndef CONFIG_GENERIC_IOREMAP
void *ioremap(phys_addr_t offset, size_t size) { return NULL; }
#endif
void iounmap(volatile void *addr) { }

/* lib/memregion.c */
int memregion_alloc(gfp_t gfp) { return 0; }
void memregion_free(int id) { }

/* lib/smp_processor_id.c */
unsigned int debug_smp_processor_id(void) { return 0; }

/* lib/string.c */
char *strnchr(const char *s, size_t count, int c) { return NULL; }

/* lib/string_helpers.c */
char *strreplace(char *str, char old, char new) { return str; }

/* lib/usercopy.c */
#ifndef INLINE_COPY_FROM_USER
unsigned long _copy_from_user(void *to, const void *from, unsigned long n) { return n; }
#endif

/* mm/maccess.c */
void __copy_overflow(int size, unsigned long count) { }

/* mm/percpu.c */
unsigned long __per_cpu_offset[NR_CPUS];

/* mm/slab_common.c */
void *__kmalloc(size_t size, gfp_t flags) { return malloc(size); }
void kfree(const void *objp) { free((void *)objp); }
void kfree_const(const void *x) { free((void *)x); }
struct kmem_cache {
	unsigned int obj_size;
};
struct kmem_cache *kmem_cache_create(const char *name, unsigned int size, unsigned int align,
		slab_flags_t flags, void (*ctor)(void *))
{
	struct kmem_cache *s = calloc(1, sizeof(*s));
	s->obj_size = size;
	return s;
}
void *kmem_cache_alloc(struct kmem_cache *s, gfp_t flags) { return malloc(s->obj_size); }
void *kmem_cache_alloc_lru(struct kmem_cache *s, struct list_lru *lru, gfp_t gfpflags) { return malloc(s->obj_size); }
void kmem_cache_free(struct kmem_cache *s, void *objp) { free(objp); }
struct kmem_cache *kmalloc_caches[NR_KMALLOC_TYPES][KMALLOC_SHIFT_HIGH + 1];
void *kmalloc_trace(struct kmem_cache *s, gfp_t flags, size_t size) { return NULL; }

/* mm/usercopy.c */
void __check_object_size(const void *ptr, unsigned long n, bool to_user) { }

/* mm/util.c */
char *kstrdup(const char *s, gfp_t gfp) { return strdup(s); }
const char *kstrdup_const(const char *s, gfp_t gfp) { return strdup(s); }
char *kstrndup(const char *s, size_t max, gfp_t gfp) { return strndup(s, max); }

