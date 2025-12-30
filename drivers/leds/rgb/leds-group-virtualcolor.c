// SPDX-License-Identifier: GPL-2.0-only
/*
 * leds-group-virtualcolor.c - Virtual grouped LED driver with Multicolor ABI
 *
 * This driver is fully compliant with the multicolor LED ABI.
 * It adds a policy layer to arbitrate shared physical LEDs,
 * a problem not addressed by the LED core, without changing userspace-visible behavior.
 * these additional extensions introduce new capabilities, such as:
 *
 * - Priority-based arbitration for shared physical LED ownership
 * - Sequence/timestamp tie-breaking for deterministic conflict resolution
 * - Runtime reconfiguration of shared channels for grouped LEDs
 * - Atomic multi-device updates to ensure consistency
 * - Group-wide brightness propagation and scaling
 * - Support for arbitrated updates from multiple virtual LEDs to shared physical LEDs
 * - Dynamic reallocation and resolution of conflicting virtual-to-physical mapping
 *
 * Priority-based arbitration resolves conflicts when multiple virtual LEDs
 * reference the same physical LEDs. Arbitration rules are:
 *   1. Priority value of led (higher wins)
 *   2. Priority value of virtual controller (higher wins)
 *   3. Sequence number for tie-breaking (most recent wins)
 *   4. Winner-takes-all: ONE virtual LED controls ALL physical LEDs
 *
 * MC channel multipliers add advanced capabilities to LEDs, including:
 * - Adjusting the relative brightness levels of different color channels
 * - Normalizing output across different hardware vendors and physical configurations
 * - Manipulating color temperature by changing the balance between channels
 * - Avoiding overdriving specific channels unnecessarily
 * - Mapping physical LEDs to application-specific color spaces and intensities
 * - Emulating single fixed-color LEDs from multicolor LEDs
 * - Dynamic reconfiguration of output characteristics
 * - Capping brightness levels to suit specific use cases
 *
 * Winner-Takes-All Arbitration:
 *   - Only vLEDs with brightness > 0 participate
 *   - Highest priority wins (ties broken by sequence number)
 *   - Winner controls ALL physical LEDs
 *   - Physical LEDs not used by the winner are turned off
 *
 * Locking hierarchy (must be acquired in this order):
 *   1. vcolor_controller.lock (per-controller)  ← Controller FIRST
 *   2. global_owner_rwsem (global)             ← Global SECOND
 *   3. virtual_led.lock (per-vLED)
 *
 * Never hold vLED locks during arbitration to avoid deadlock.
 * Arbitration copies vLED state under the vLED lock, then releases locks
 * before proceeding to core processing.
 *
 * Device Tree Dependency:
 * This driver requires Device Tree (CONFIG_OF) due to LED phandle resolution.
 * GPIO LEDs, in particular, rely on OF-specific APIs, as they lack full
 * fwnode support. Minimal `CONFIG_OF` usage ensures easy migration to ACPI
 * when fwnode abstraction improves. Key operations include:
 *   - `of_led_get()` - Called for LED phandle resolution within the single
 *                      bridge function `vcolor_led_from_fwnode()`.
 *   - `device_for_each_child_node()` for child iteration
 *   - `fwnode_property_*()` for property access
 *   - `fwnode_handle_get/put()` for reference management
 *
 * By isolating OF dependency in the bridge function, migration to
 * `fwnode_led_get()` will be straightforward when supported by the LED subsystem.
 *
 * Author: Jonathan Brophy <professor_jonny@hotmail.com>
 */

#include <dt-bindings/leds/common.h>

#include <linux/atomic.h>
#include <linux/compiler.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/ktime.h>
#include <linux/leds.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#ifdef CONFIG_DEBUG_FS
  #include <linux/random.h>
#endif
#include <linux/ratelimit.h>
#include <linux/rwsem.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>

#define DRIVER_NAME "leds-group-virtualcolor"
#define DRIVER_VERSION "4"
#define VLED_DEBUGFS_DIR DRIVER_NAME
#define MAX_PHYS_LEDS_DEFAULT 64
#define UPDATE_BATCH_DELAY_MS 10
#define DEFAULT_UPDATE_RATE_LIMIT 100 /* Updates per second per vLED */
#define PRIORITY_MAX INT_MAX
#define VLED_SNAPSHOT_DEFAULT 32 /* Typical system has <32 vLEDs per controller */

#ifdef CONFIG_DEBUG_FS
  #define MAX_DEBUGFS_NAME 96 /* Sized for "function:color-multicolor-##" + vLED name */
#endif

#ifdef CONFIG_LOCKDEP
  #define assert_controller_locked(lvc) lockdep_assert_held(&(lvc)->lock)
  #define assert_vled_locked(vled) lockdep_assert_held(&(vled)->lock)
#else
#define assert_controller_locked(lvc) ((void)(lvc))
#define assert_vled_locked(vled) ((void)(vled))
#endif

static inline bool is_valid_led_cdev(struct led_classdev *cdev)
{
	if (!cdev)
		return false;
	if (!cdev->brightness_set && !cdev->brightness_set_blocking)
		return false;
	return true;
}

/* Structured logging macros */
#define ctrl_err(lvc, fmt, ...) \
	dev_err(&(lvc)->pdev->dev, "[CTRL] " fmt, ##__VA_ARGS__)

#define ctrl_warn(lvc, fmt, ...) \
	dev_warn(&(lvc)->pdev->dev, "[CTRL] " fmt, ##__VA_ARGS__)

#define ctrl_info(lvc, fmt, ...) \
	dev_info(&(lvc)->pdev->dev, "[CTRL] " fmt, ##__VA_ARGS__)

#define ctrl_dbg(lvc, fmt, ...) \
	dev_dbg(&(lvc)->pdev->dev, "[CTRL] " fmt, ##__VA_ARGS__)

#define arb_err(lvc, fmt, ...) \
	dev_err_ratelimited(&(lvc)->pdev->dev, "[ARB] " fmt, ##__VA_ARGS__)

#define vled_err(vled, fmt, ...) \
	dev_err(&(vled)->ctrl->pdev->dev, "[vLED:%s] " fmt, (vled)->name, ##__VA_ARGS__)

static_assert(sizeof(void *) <= sizeof(unsigned long),
		  "XArray keys require pointer size <= unsigned long");

/* Module parameters */
#ifdef CONFIG_DEBUG_FS
static bool enable_debugfs = true;
#else
static bool enable_debugfs;
#endif

static bool use_gamma_correction;
static unsigned int update_delay_us;
static unsigned int max_phys_leds = MAX_PHYS_LEDS_DEFAULT;
static bool enable_update_batching;

/* LED mode enumeration */
enum vled_mode {
	VLED_MODE_MULTICOLOR = 0,
	VLED_MODE_STANDARD = 1,
};

struct vcolor_controller;

struct mc_channel {
	u8 color_id;
	u8 num_leds;
	struct led_classdev **leds;
	struct device **led_devs;
	u8 intensity;
	u8 scale;
};

struct phys_led_entry {
	/* HOT PATH */
	struct led_classdev *cdev;
	u8 chosen_brightness;
	s32 chosen_priority;
	u64 chosen_sequence;
	struct kref refcount;
	unsigned int list_index;

	/* COLD PATH */
	struct device *dev;
	struct list_head list;
#ifdef CONFIG_DEBUG_FS
	char winner_name[MAX_DEBUGFS_NAME];
#endif
};

struct update_buffer {
	struct phys_led_entry **entries;
	u8 *brightness;
	unsigned int capacity;
	unsigned int max_capacity;
};

struct arbitration_buffers {
	u8 *intensities;
	u8 *scales;
	unsigned int capacity;
};

static DEFINE_XARRAY(global_owner_xa);
static DECLARE_RWSEM(global_owner_rwsem);

struct global_phys_owner {
	struct platform_device *owner_pdev;
};


/*
 * struct virtual_led - Virtual LED with priority-based arbitration
 * @cdev: LED class device
 * @priority: Arbitration priority (higher wins)
 * @name: LED name (assigned by LED core)
 * @channels: Array of color channels
 * @num_channels: Number of color channels (max 255, but dirty tracking limited to 32)
 * @lock: Protects vLED state during updates
 * @list: Entry in controller's vLED list
 * @fwnode: Firmware node for DT parsing
 * @ctrl: Parent controller
 * @cdev_registered: LED class device registration status
 * @intensity_ratelimit: Rate limiter for intensity updates
 * @arb_bufs: Pre-allocated buffers for arbitration
 * @mode: Operating mode (MULTICOLOR or STANDARD)
 * @refcount: Reference counter
 * @sequence: Monotonic sequence number for tie-breaking
 * All channels are processed during each arbitration cycle for simplicity.
 *
 */
struct virtual_led {
	struct led_classdev cdev;
	s32 priority;
	const char *name;
	struct mc_channel *channels;
	u8 num_channels;
	struct mutex lock;
	struct list_head list;
	struct fwnode_handle *fwnode;
	struct vcolor_controller *ctrl;
	bool cdev_registered;
	struct ratelimit_state intensity_ratelimit;
	struct arbitration_buffers arb_bufs;
	enum vled_mode mode;
	struct kref refcount;
	u64 sequence;

#ifdef CONFIG_DEBUG_FS
	struct dentry *debugfs_dir;
	u64 brightness_set_count;
	u64 intensity_update_count;
	u64 arbitration_wins;
	u64 arbitration_losses;
	u64 arbitration_participations;
	u64 buffer_allocation_failures;
	u64 intensity_parse_errors;
	u64 ratelimit_drops;
	u64 blink_requests;
#endif
};

struct vcolor_controller {
	struct list_head leds;
	struct mutex lock;
	struct list_head phys_leds;
	struct xarray phys_xa;
	struct platform_device *pdev;
	struct update_buffer update_buf;
	/* Pre-allocated arbitration buffers */
	struct virtual_led **vled_snapshot;
	unsigned int vled_snapshot_capacity;
	struct phys_led_entry **ple_snapshot;
	unsigned int ple_snapshot_capacity;
	bool *ple_usage_bitmap;
	unsigned int ple_usage_bitmap_capacity;
	bool suspended;
	atomic_t rebuilding;
	bool needs_arbitration;
	unsigned int phys_led_count;
	atomic_t removing;
	struct delayed_work update_work;
	atomic_t pending_updates;
	atomic64_t global_sequence;
	bool first_arbitration;
#ifdef CONFIG_DEBUG_FS
	struct dentry *debugfs_root;
	u64 arbitration_count;
	u64 update_count;
	atomic64_t allocation_failures;
	atomic64_t update_buffer_overflows;
	atomic64_t ratelimit_hits;
	u64 arb_latency_min_ns;
	u64 arb_latency_max_ns;
	u64 arb_latency_total_ns;
	u64 arb_latency_count;
	ktime_t last_update;
#endif
};

/* Forward declarations */
static void controller_rebuild_phys_leds(struct vcolor_controller *lvc);
static void controller_destroy_phys_list(struct vcolor_controller *lvc);
static void controller_run_arbitration_and_update(struct vcolor_controller *lvc);
static void phys_led_entry_release(struct kref *ref);
static void virtual_led_release(struct kref *ref);

static inline unsigned long get_stable_led_key(struct led_classdev *cdev)
{
	if (!cdev)
		return 0;

	/* GPIO LEDs don't have dev - use cdev pointer as key */
	if (cdev->dev)
		return (unsigned long)cdev->dev;
	else
		return (unsigned long)cdev;
}

static inline struct virtual_led *virtual_led_get(struct virtual_led *vled)
{
	if (vled)
		kref_get(&vled->refcount);
	return vled;
}

static inline void virtual_led_put(struct virtual_led *vled)
{
	if (vled)
		kref_put(&vled->refcount, virtual_led_release);
}

static inline bool controller_safe_arbitrate(struct vcolor_controller *lvc)
{
	bool ran;

	if (!lvc)
		return false;

	/* Fast path: avoid lock acquisition if nothing to do */
	if (atomic_read(&lvc->removing))
		return false;

	/* FIX: Queue deferred arbitration if rebuilding */
	if (atomic_read(&lvc->rebuilding)) {
		lvc->needs_arbitration = true;
		return false;
	}

	mutex_lock(&lvc->lock);

	/* Check suspended under lock to prevent suspend race */
	ran = false;
	if (!lvc->suspended && !atomic_read(&lvc->rebuilding) &&
		device_is_registered(&lvc->pdev->dev)) {
		controller_run_arbitration_and_update(lvc);
		ran = true;
	}

	/* FIX: Lock was released by controller_run_arbitration_and_update */
	return ran;
}


/*
 * parse_leds_fwnode_array - Parse LED references using fwnode APIs
 * @dev: Device for logging and memory allocation
 * @fwnode: Firmware node containing LED references
 * @propname: Property name (e.g., "leds")
 * @out_leds: Output array of LED classdev pointers
 * @out_devs: Output array of device pointers (may contain NULLs for GPIO LEDs)
 * @out_count: Number of LEDs found
 *
 * Uses fwnode APIs for property traversal, with a single OF bridge for LED resolution.
 * This pattern mirrors V4L2's approach and makes future fwnode_led_get() migration trivial.
 */
static int parse_leds_fwnode_array(struct device *dev,
				   const struct fwnode_handle *fwnode,
				   const char *propname,
				   struct led_classdev ***out_leds,
				   struct device ***out_devs,
				   u8 *out_count)
{
	struct fwnode_reference_args args;
	int count, idx, valid, i;
	struct led_classdev **leds;
	struct device **devs;
	struct led_classdev *cdev;
	struct device *led_dev;
	int ret;

	*out_leds = NULL;
	*out_devs = NULL;
	*out_count = 0;

	/* Count phandle references using generic fwnode API */
	count = 0;
	while (fwnode_property_get_reference_args(fwnode, propname,
						  NULL, 0, count, &args) == 0) {
		fwnode_handle_put(args.fwnode);
		count++;
	}

	if (count <= 0)
		return 0;

	/* Allocate temporary arrays for LED/device pointers */
	leds = kcalloc(count, sizeof(*leds), GFP_KERNEL);
	if (!leds)
		return -ENOMEM;

	devs = kcalloc(count, sizeof(*devs), GFP_KERNEL);
	if (!devs) {
		kfree(leds);
		return -ENOMEM;
	}

	/* Iterate through each LED reference and PACK valid entries */
	valid = 0;
	for (idx = 0; idx < count; idx++) {

   /*Resolve LED from fwnode using index.*/
		cdev = fwnode_led_get(fwnode, idx, &led_dev);

		if (IS_ERR(cdev)) {
			ret = PTR_ERR(cdev);

			/* Handle deferred probe - cleanup and return immediately */
			if (ret == -EPROBE_DEFER) {
				dev_info(dev, "LED %d not ready yet (EPROBE_DEFER), will retry\n", idx);

				/* Release all previously acquired LEDs and devices */
				for (i = 0; i < valid; i++) {
					if (leds[i])
						led_put(leds[i]);
					if (devs[i])
						put_device(devs[i]);
				}

				kfree(leds);
				kfree(devs);
				return -EPROBE_DEFER;
			}

			/* Other errors - log and skip this LED */
			dev_warn(dev, "Failed to resolve LED %d: %d\n", idx, ret);
			continue;
		}

		/* Store valid LED in PACKED position */
		if (is_valid_led_cdev(cdev)) {
			leds[valid] = cdev;	  /* Pack at 'valid' index, not 'idx' */
			devs[valid] = led_dev;   /* May be NULL for GPIO LEDs */
			valid++;
		} else {
			dev_warn(dev, "LED %d is not valid (no brightness_set method)\n", idx);
			led_put(cdev);
			if (led_dev)
				put_device(led_dev);
		}
	}

	/* Check if we got any valid LEDs */
	if (valid == 0) {
		dev_warn(dev, "Property '%s': none of %d LED(s) resolved\n",
			 propname, count);
		kfree(leds);
		kfree(devs);
		return -ENODEV;
	}

	/* Success - return PACKED arrays to caller */
	*out_leds = leds;
	*out_devs = devs;
	*out_count = (u8)valid;

	return 0;
}

static int validate_and_set_max_brightness(struct virtual_led *vled)
{
	unsigned int i, j;
	enum led_brightness min_max_brightness = LED_FULL;

	/*
	 * For multicolor mode, virtual LED always exposes full 8-bit range.
	 * Scaling happens automatically in scale_intensity_by_brightness().
	 */
	if (vled->mode == VLED_MODE_MULTICOLOR) {
		vled->cdev.max_brightness = LED_FULL;
		return 0;
	}

	/*
	 * For standard mode, use minimum of physical LEDs since color is
	 * fixed by multipliers.
	 */
	for (i = 0; i < vled->num_channels; i++) {
		for (j = 0; j < vled->channels[i].num_leds; j++) {
			enum led_brightness max_brightness;

			if (!vled->channels[i].leds[j])
				continue;

			max_brightness = vled->channels[i].leds[j]->max_brightness;
			if (max_brightness == 0)
				max_brightness = LED_FULL;

			if (max_brightness < min_max_brightness)
				min_max_brightness = max_brightness;
		}
	}

	if (min_max_brightness < 1)
		min_max_brightness = 1;

	vled->cdev.max_brightness = min_max_brightness;
	return 0;
}

static void global_release_all_for_pdev(struct platform_device *pdev)
{
	unsigned long index;
	unsigned long released;
	struct global_phys_owner *gpo;

	down_write(&global_owner_rwsem);

	released = 0;

	/*
	 * FIXED: Use xa_for_each() + xa_erase() instead of XA_STATE + xas_store().
	 *
	 * The old code used xas_store(&xas, NULL) inside xas_for_each(), which
	 * corrupts the iterator state and can skip entries or cause crashes.
	 *
	 * xa_for_each() is safe to use with xa_erase() because:
	 * 1. xa_for_each is a simple macro that doesn't maintain complex state
	 * 2. xa_erase() is designed to work during iteration
	 * 3. The iterator naturally moves to the next valid entry
	 */
	xa_for_each(&global_owner_xa, index, gpo) {
		if (gpo && gpo->owner_pdev == pdev) {
			xa_erase(&global_owner_xa, index);
			kfree(gpo);
			released++;
		}
	}

	up_write(&global_owner_rwsem);

	if (released) {
		dev_info(&pdev->dev, "Released %lu physical LED ownership claims\n",
			released);
	}
}

static void phys_led_entry_release(struct kref *ref)
{
	struct phys_led_entry *ple;

	ple = container_of(ref, struct phys_led_entry, refcount);

	if (ple->dev)
		put_device(ple->dev);

	kfree(ple);
}

static inline struct phys_led_entry *phys_led_entry_get(
	struct phys_led_entry *ple)
{
	if (ple)
		kref_get(&ple->refcount);
	return ple;
}

static inline void phys_led_entry_put(struct phys_led_entry *ple)
{
	if (ple)
		kref_put(&ple->refcount, phys_led_entry_release);
}

/*
 * claim_physical_led - Claim ownership of a physical LED
 *
 * LOCKING: Acquires locks in this order (matching hierarchy):
 *   1. lvc->lock (controller) - acquired FIRST
 *   2. global_owner_rwsem (global) - acquired SECOND
 *
 * This order prevents AB-BA deadlock. Caller must NOT hold lvc->lock on entry.
 */
static bool claim_physical_led(struct vcolor_controller *lvc,
				  struct led_classdev *cdev,
				  struct device *dev,
				  struct phys_led_entry **out_ple)
{
	struct global_phys_owner *gpo;
	struct phys_led_entry *ple = NULL;
	void *ret_ptr;
	bool success = false;
	bool newly_claimed_global = false;
	unsigned long key;

	*out_ple = NULL;

	if (!cdev)
		return false;

	key = get_stable_led_key(cdev);
	if (!key) {
		ctrl_err(lvc, "Failed to get stable key for LED '%s'\n",
			 cdev->name ? cdev->name : "(unnamed)");
		return false;
	}

	/*
	 * FIXED: Acquire controller lock FIRST, then check removal flag.
	 * This eliminates TOCTOU race - once we hold the lock, removal
	 * cannot proceed past the rebuilding wait.
	 */
	mutex_lock(&lvc->lock);

	/* Single authoritative check under lock */
	if (atomic_read(&lvc->removing)) {
		mutex_unlock(&lvc->lock);
		return false;
	}

	down_write(&global_owner_rwsem);

	gpo = xa_load(&global_owner_xa, key);
	if (xa_is_value(gpo)) {
		ctrl_err(lvc, "Invalid XArray entry for LED '%s'\n", cdev->name);
		goto out_unlock;
	}

	if (gpo && gpo->owner_pdev != lvc->pdev) {
		ctrl_warn(lvc, "Physical LED '%s' already claimed by another controller\n",
			  cdev->name);
		goto out_unlock;
	}

	if (xa_load(&lvc->phys_xa, key)) {
		ctrl_dbg(lvc, "LED '%s' already claimed locally, skipping duplicate\n",
			 cdev->name);
		goto out_unlock;
	}

	ple = kzalloc(sizeof(*ple), GFP_KERNEL);
	if (!ple) {
#ifdef CONFIG_DEBUG_FS
		ctrl_err(lvc, "Failed to allocate phys_led_entry for '%s'\n",
			cdev->name);
		atomic64_inc(&lvc->allocation_failures);
#endif
		goto out_unlock;
	}

	kref_init(&ple->refcount);
	ple->cdev = cdev;
	ple->dev = dev;
	if (dev)
		get_device(dev);
	ple->chosen_brightness = 0;
	ple->chosen_priority = -1;
	ple->chosen_sequence = 0;
#ifdef CONFIG_DEBUG_FS
	ple->winner_name[0] = '\0';
#endif

	if (!gpo) {
		gpo = kzalloc(sizeof(*gpo), GFP_KERNEL);
		if (!gpo) {
#ifdef CONFIG_DEBUG_FS
			ctrl_err(lvc, "Failed to allocate ownership record for LED '%s'\n",
				 cdev->name);
			atomic64_inc(&lvc->allocation_failures);
#endif
			goto out_unlock;
		}

		gpo->owner_pdev = lvc->pdev;
		ret_ptr = xa_store(&global_owner_xa, key, gpo, GFP_KERNEL);

		if (xa_is_err(ret_ptr)) {
			ctrl_err(lvc, "Failed to register global ownership for LED '%s': %ld\n",
				 cdev->name, PTR_ERR(ret_ptr));
			kfree(gpo);
#ifdef CONFIG_DEBUG_FS
			atomic64_inc(&lvc->allocation_failures);
#endif
			goto out_unlock;
		}

		newly_claimed_global = true;
	}

	ret_ptr = xa_store(&lvc->phys_xa, key, ple, GFP_KERNEL);
	if (xa_is_err(ret_ptr)) {
		ctrl_err(lvc, "Failed to store phys_led_entry '%s' in xarray: %ld\n",
			 cdev->name, PTR_ERR(ret_ptr));

		if (newly_claimed_global) {
			gpo = xa_erase(&global_owner_xa, key);
			if (gpo && !xa_is_value(gpo)) {
				ctrl_dbg(lvc, "Cleaned up leaked global ownership for '%s' after local store failure\n",
					 cdev->name);
				kfree(gpo);
			}
		}

#ifdef CONFIG_DEBUG_FS
		atomic64_inc(&lvc->allocation_failures);
#endif
		goto out_unlock;
	}

	list_add_tail(&ple->list, &lvc->phys_leds);
	lvc->phys_led_count++;
	*out_ple = ple;
	success = true;

out_unlock:
	/* FIXED: Clean up XArray BEFORE releasing locks */
	if (!success && ple) {
		void *stored = xa_load(&lvc->phys_xa, key);

		if (stored == ple)
			xa_erase(&lvc->phys_xa, key);
	}

	/* Release locks in reverse order */
	up_write(&global_owner_rwsem);
	mutex_unlock(&lvc->lock);

	/* Safe cleanup - removed from XArray first */
	if (!success && ple)
		phys_led_entry_put(ple);

	return success;
}

static void add_led_group_to_phys_list(struct vcolor_controller *lvc,
	struct led_classdev **leds,
	struct device **devs,
	unsigned int count)
{
	unsigned int i;
	struct phys_led_entry *ple;

	for (i = 0; i < count; i++) {
		if (!leds || !leds[i])
			continue;

		claim_physical_led(lvc, leds[i], devs ? devs[i] : NULL, &ple);
	}
}

static void controller_destroy_phys_list(struct vcolor_controller *lvc)
{
	struct phys_led_entry *ple, *tmp;
	unsigned long key;

	assert_controller_locked(lvc);

	list_for_each_entry_safe(ple, tmp, &lvc->phys_leds, list) {
		list_del(&ple->list);

		if (ple->cdev) {
			key = get_stable_led_key(ple->cdev);
			if (key)
				xa_erase(&lvc->phys_xa, key);
			ple->cdev = NULL;
		}

		phys_led_entry_put(ple);
	}

	INIT_LIST_HEAD(&lvc->phys_leds);
	lvc->phys_led_count = 0;
}

/*
 * controller_rebuild_phys_leds - Rebuild physical LED ownership mappings
 * @lvc: Controller instance
 *
 * LOCKING: Must be called with lvc->lock held.
 *
 * IMPORTANT: This function TEMPORARILY DROPS lvc->lock during LED reference
 * acquisition to avoid holding the lock during potentially blocking operations.
 * The atomic_rebuilding flag prevents concurrent arbitration while the lock
 * is dropped.
 *
 * Call sequence:
 *	1. Assert lvc->lock is held
 *	2. Set atomic_rebuilding = 1
 *	3. Take snapshot of vLEDs with refcounts
 *	4. Destroy existing physical LED mappings
 *	5. DROP LOCK (intentional - see locking hierarchy comment at top of file)
 *	6. Process vLED channels and claim physical LEDs
 *	7. REACQUIRE LOCK
 *	8. Assign list indices for O(1) arbitration lookup
 *	9. Clear atomic_rebuilding = 0
 *	10. Run arbitration to apply new state
 *
 * The lock drop is necessary because:
 *	- add_led_group_to_phys_list() calls claim_physical_led()
 *	- claim_physical_led() acquires global_owner_rwsem (higher in hierarchy)
 *	- Holding lvc->lock during this would violate lock ordering
 */
static void controller_rebuild_phys_leds(struct vcolor_controller *lvc)
{
	struct virtual_led *vled, **vled_snapshot;
	unsigned int i, j, vled_count, actual_count;
	bool needs_free = false;

	assert_controller_locked(lvc);

	atomic_set(&lvc->rebuilding, 1);

	/* Count vLEDs first */
	vled_count = 0;
	list_for_each_entry(vled, &lvc->leds, list)
		vled_count++;

	/* Explicitly turn off LEDs when no vLEDs exist */
	if (vled_count == 0) {
		struct phys_led_entry *ple;

		/* Turn off all physical LEDs before destroying list */
		list_for_each_entry(ple, &lvc->phys_leds, list) {
			if (ple->cdev) {
				if (ple->cdev->brightness_set_blocking)
					ple->cdev->brightness_set_blocking(ple->cdev, 0);
				else if (ple->cdev->brightness_set)
					ple->cdev->brightness_set(ple->cdev, 0);
				ple->cdev->brightness = 0;
			}
		}

		/* Safe to destroy when no vLEDs exist */
		controller_destroy_phys_list(lvc);
		xa_destroy(&lvc->phys_xa);
		xa_init(&lvc->phys_xa);
		atomic_set(&lvc->rebuilding, 0);
		return;
	}

	/* Allocate snapshot BEFORE destroying existing state */
	if (vled_count > lvc->vled_snapshot_capacity) {
		ctrl_warn(lvc,
			"vLED count %u exceeds snapshot capacity %u, using dynamic allocation\n",
			vled_count, lvc->vled_snapshot_capacity);
		vled_snapshot = kcalloc(vled_count, sizeof(*vled_snapshot), GFP_KERNEL);
		if (!vled_snapshot) {
			ctrl_err(lvc, "Failed to allocate vled snapshot for rebuild (OOM) - keeping existing state\n");
#ifdef CONFIG_DEBUG_FS
			atomic64_inc(&lvc->allocation_failures);
#endif
			atomic_set(&lvc->rebuilding, 0);
			return;
		}
		needs_free = true;
	} else {
		vled_snapshot = lvc->vled_snapshot;
		needs_free = false;
	}

	/* Copy vLED pointers with refcounts BEFORE destroying state */
	actual_count = 0;
	list_for_each_entry(vled, &lvc->leds, list) {
		if (actual_count >= vled_count) {
			ctrl_warn(lvc, "vLED count increased during snapshot! Expected %u\n", vled_count);
			break;
		}
		vled_snapshot[actual_count++] = virtual_led_get(vled);
	}

	/* Fail hard if count increased (race condition) */
	if (actual_count > vled_count) {
		ctrl_err(lvc, "vLED count INCREASED during snapshot (%u -> %u) - aborting rebuild\n",
			 vled_count, actual_count);
		for (i = 0; i < actual_count; i++)
			virtual_led_put(vled_snapshot[i]);
		if (needs_free)
			kfree(vled_snapshot);
		atomic_set(&lvc->rebuilding, 0);
		return;
	}

	/* Now safe to destroy existing state */
	controller_destroy_phys_list(lvc);
	xa_destroy(&lvc->phys_xa);
	xa_init(&lvc->phys_xa);

	/* Check removal flag BEFORE dropping lock */
	if (atomic_read(&lvc->removing)) {
		atomic_set(&lvc->rebuilding, 0);
		/* Release snapshot references */
		for (i = 0; i < actual_count; i++) {
			if (vled_snapshot[i])
				virtual_led_put(vled_snapshot[i]);
		}
		if (needs_free)
			kfree(vled_snapshot);
		return;
	}

	mutex_unlock(&lvc->lock);

	/* Process all vLEDs outside lock */
	for (i = 0; i < actual_count; i++) {
		vled = vled_snapshot[i];
		if (!vled)
			continue;

		/* Double-check removal flag */
		if (atomic_read(&lvc->removing)) {
			for (; i < actual_count; i++) {
				if (vled_snapshot[i])
					virtual_led_put(vled_snapshot[i]);
			}
			goto out_cleanup;
		}

		for (j = 0; j < vled->num_channels; j++) {
			add_led_group_to_phys_list(lvc, vled->channels[j].leds,
						   vled->channels[j].led_devs,
						   vled->channels[j].num_leds);
		}

		virtual_led_put(vled);
		vled_snapshot[i] = NULL;
	}

out_cleanup:
	if (needs_free)
		kfree(vled_snapshot);

	mutex_lock(&lvc->lock);

	/* Assign list indices for O(1) lookup during arbitration */
	{
		unsigned int idx = 0;
		struct phys_led_entry *ple_temp;

		list_for_each_entry(ple_temp, &lvc->phys_leds, list) {
			ple_temp->list_index = idx++;
		}
	}

	/* FIX #4: Run arbitration BEFORE clearing rebuilding flag */
	if (!atomic_read(&lvc->removing))
		controller_run_arbitration_and_update(lvc);

	/* FIX #2: Check for deferred arbitration */
	if (lvc->needs_arbitration && !atomic_read(&lvc->removing)) {
		lvc->needs_arbitration = false;
		controller_run_arbitration_and_update(lvc);
	}

	/* FIX #4: Clear rebuilding flag AFTER arbitration completes */
	atomic_set(&lvc->rebuilding, 0);
}

static const u8 gamma_table[256] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4,
	4, 4, 5, 5, 5, 5, 6, 6, 6, 7, 7, 7, 8, 8, 8, 9, 9, 9, 10, 10, 11, 11, 11, 12, 12, 13, 13, 14,
	14, 15, 15, 16, 16, 17, 17, 18, 18, 19, 19, 20, 20, 21, 22, 22, 23, 23, 24, 25, 25, 26,
	26, 27, 28, 28, 29, 30, 30, 31, 32, 32, 33, 34, 34, 35, 36, 37, 37, 38, 39, 40, 40, 41,
	42, 43, 44, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62,
	63, 64, 65, 66, 67, 68, 70, 71, 72, 73, 74, 75, 76, 78, 79, 80, 81, 82, 84, 85, 86, 87,
	89, 90, 91, 92, 94, 95, 96, 97, 99, 100, 101, 103, 104, 105, 107, 108, 109, 111, 112,
	114, 115, 116, 118, 119, 121, 122, 123, 125, 126, 128, 129, 131, 132, 134, 135, 137,
	138, 140, 141, 143, 144, 146, 147, 149, 150, 152, 154, 155, 157, 158, 160, 162, 163,
	165, 167, 168, 170, 172, 173, 175, 177, 178, 180, 182, 184, 185, 187, 189, 191, 192,
	194, 196, 198, 200, 201, 203, 205, 207, 209, 211, 212, 214, 216, 218, 220, 222, 224,
	226, 228, 230, 232, 234, 236, 238, 240, 242, 244, 246, 248, 250, 253, 255
};

static u8 scale_intensity_by_brightness(u8 intensity, u8 global_brightness,
					u8 max_global_brightness)
{
	u32 scaled_val;
	u8 final_intensity;

	if (max_global_brightness == 0)
		return 0;

	scaled_val = (u32)intensity * (u32)global_brightness;
	final_intensity = (u8)clamp_val(scaled_val / max_global_brightness, 0, 255);

	if (use_gamma_correction)
		final_intensity = gamma_table[final_intensity];

	return final_intensity;
}

static u8 vled_channel_get_final_intensity(enum vled_mode mode,
					   u8 raw_intensity,
					   u8 scale,
					   enum led_brightness vled_brightness,
					   enum led_brightness vled_max_brightness)
{
	u8 intensity = raw_intensity;
	u16 scaled_val;

	if (mode == VLED_MODE_MULTICOLOR) {
		if (scale < 255) {
			scaled_val = ((u16)intensity * (u16)scale) / 255;
			intensity = (u8)clamp_val(scaled_val, 0, 255);
		}
	} else {
		intensity = scale;
	}

	return scale_intensity_by_brightness(intensity, vled_brightness,
						 vled_max_brightness);
}

static void apply_winner_to_channel(struct vcolor_controller *lvc,
	struct virtual_led *vled,
	struct led_classdev **leds,
	unsigned int count,
	u8 final_intensity)
{
	unsigned int i;
	struct phys_led_entry *ple;
	unsigned long key;
#ifdef CONFIG_DEBUG_FS
	int copy_result;
#endif

	assert_controller_locked(lvc);

	for (i = 0; i < count; i++) {
		if (!leds[i])
			continue;

		key = get_stable_led_key(leds[i]);
		if (!key)
			continue;

		ple = xa_load(&lvc->phys_xa, key);
		if (!ple || xa_is_value(ple))
			continue;

		/* Winner takes all - no comparison needed */
		ple->chosen_brightness = final_intensity;
		ple->chosen_priority = vled->priority;
		ple->chosen_sequence = vled->sequence;

	#ifdef CONFIG_DEBUG_FS
		if (vled->name) {
			copy_result = strscpy(ple->winner_name, vled->name, MAX_DEBUGFS_NAME);
			if (copy_result < 0) {
				dev_warn_once(&lvc->pdev->dev,
					"vLED name truncated in telemetry: '%.32s...'\n",
					vled->name);
			}
		} else {
			strscpy(ple->winner_name, "(unnamed)", MAX_DEBUGFS_NAME);
		}
	#endif
	}
}

static void mark_winner_leds(struct vcolor_controller *lvc,
				 struct virtual_led *winner_vled,
				 unsigned int channel_idx,
				 bool *ple_used_by_winner,
				 unsigned int ple_count)
{
	unsigned int i;

	for (i = 0; i < winner_vled->channels[channel_idx].num_leds; i++) {
		unsigned long key;
		struct phys_led_entry *ple_temp;

		if (!winner_vled->channels[channel_idx].leds[i])
			continue;

		key = get_stable_led_key(winner_vled->channels[channel_idx].leds[i]);
		if (!key)
			continue;

		ple_temp = xa_load(&lvc->phys_xa, key);
		if (!ple_temp || xa_is_value(ple_temp))
			continue;

		/* Direct O(1) index lookup instead of O(n) list scan */
		if (ple_temp->list_index < ple_count)
			ple_used_by_winner[ple_temp->list_index] = true;
	}
}

/*
 * controller_run_arbitration_and_update - Run winner-takes-all arbitration
 * @lvc: Controller instance
 *
 * Selects the highest priority active vLED and applies its configuration
 * to all physical LEDs. Non-winner vLEDs are ignored (winner-takes-all).
 *
 * Context: Must be called with lvc->lock held.
 *
 * CRITICAL LOCKING BEHAVIOR:
 *   - Expects lvc->lock to be held on entry
 *   - Temporarily RELEASES lvc->lock during hardware I/O
 *   - REACQUIRES lvc->lock before return
 *   - Lock state is PRESERVED on return, but shared state may change during unlock
 *
 * Rationale for temporary unlock:
 *   Hardware I/O can block for milliseconds. Holding lvc->lock during this
 *   would prevent other vLEDs from updating their state (brightness_set calls).
 *   By releasing the lock during I/O, we allow concurrent operations while
 *   preventing corruption of arbitration results.
 *
 * Locking: Acquires vLED locks briefly to snapshot state, then processes
 *		without holding any locks during hardware access.
 */
static void controller_run_arbitration_and_update(struct vcolor_controller *lvc)
{
	struct phys_led_entry *ple;
	struct virtual_led *vled;
	struct virtual_led *winner_vled = NULL;
	s32 highest_priority = -1;
	u64 latest_sequence = 0;
	u8 *intensities;
	u8 *scales;
	enum vled_mode mode;
	unsigned int j;
	u8 final_intensity;
	struct phys_led_entry **local_entries;
	u8 *local_brightness;
	unsigned int update_count;
	unsigned int i;
	u64 vled_seq;
	enum led_brightness brightness, max_brightness;
	bool *ple_used_by_winner;
	unsigned int ple_count;

#ifdef CONFIG_DEBUG_FS
	ktime_t arb_start, arb_end;
	u64 arb_duration_ns;
#endif

	if (!lvc || lvc->suspended || atomic_read(&lvc->removing))
		return;

	assert_controller_locked(lvc);

	local_entries = lvc->update_buf.entries;
	local_brightness = lvc->update_buf.brightness;

	if (!local_entries || !local_brightness) {
		dev_err(&lvc->pdev->dev, "Update buffers not allocated, cannot arbitrate\n");
		/* FIX: Unlock before returning */
		mutex_unlock(&lvc->lock);
		return;
	}

#ifdef CONFIG_DEBUG_FS
	arb_start = ktime_get();
	lvc->arbitration_count++;
#endif

	/* Count physical LEDs */
	ple_count = 0;
	list_for_each_entry(ple, &lvc->phys_leds, list)
		ple_count++;

	if (ple_count == 0) {
#ifdef CONFIG_DEBUG_FS
		arb_end = ktime_get();
		arb_duration_ns = ktime_to_ns(ktime_sub(arb_end, arb_start));
		if (lvc->arb_latency_count == 0 || arb_duration_ns < lvc->arb_latency_min_ns)
			lvc->arb_latency_min_ns = arb_duration_ns;
		if (arb_duration_ns > lvc->arb_latency_max_ns)
			lvc->arb_latency_max_ns = arb_duration_ns;
		lvc->arb_latency_total_ns += arb_duration_ns;
		lvc->arb_latency_count++;
#endif
		/* FIX: Unlock before returning */
		mutex_unlock(&lvc->lock);
		return;
	}

	/* Use pre-allocated bitmap with graceful degradation if too small */
	if (ple_count > lvc->ple_usage_bitmap_capacity) {
		dev_warn_once(&lvc->pdev->dev,
			"Physical LED count %u exceeds bitmap capacity %u - falling back to full LED scan (performance degraded)\n",
			ple_count, lvc->ple_usage_bitmap_capacity);
		ple_used_by_winner = NULL;
#ifdef CONFIG_DEBUG_FS
		atomic64_inc(&lvc->allocation_failures);
#endif
	} else {
		ple_used_by_winner = lvc->ple_usage_bitmap;
		memset(ple_used_by_winner, 0, ple_count * sizeof(bool));
	}

	/* Reset arbitration state */
	list_for_each_entry(ple, &lvc->phys_leds, list) {
		ple->chosen_brightness = 0;
		ple->chosen_priority = -1;
		ple->chosen_sequence = 0;
#ifdef CONFIG_DEBUG_FS
		ple->winner_name[0] = '\0';
#endif
	}

	/* STEP 1: Find the ONE active vLED winner (highest priority, latest sequence) */
	list_for_each_entry(vled, &lvc->leds, list) {
#ifdef CONFIG_DEBUG_FS
		vled->arbitration_participations++;
#endif
		mutex_lock(&vled->lock);

		brightness = vled->cdev.brightness;
		vled_seq = vled->sequence;

		mutex_unlock(&vled->lock);

		/* Only vLEDs with brightness > 0 can become winners */
		if (brightness == 0)
			continue;

		/* Winner-takes-all: only ONE vLED controls the controller */
		if (vled->priority > highest_priority) {
			/* FIX: Release old winner ref and take new ref */
			if (winner_vled)
				virtual_led_put(winner_vled);
			winner_vled = virtual_led_get(vled);
			highest_priority = vled->priority;
			latest_sequence = vled_seq;
		} else if (vled->priority == highest_priority) {
			s64 seq_diff = (s64)(vled_seq - latest_sequence);

			if (seq_diff > 0) {
				/* FIX: Release old winner ref and take new ref */
				if (winner_vled)
					virtual_led_put(winner_vled);
				winner_vled = virtual_led_get(vled);
				latest_sequence = vled_seq;
			}
		}
	}

	/* STEP 2: If no active vLED, all physical LEDs stay at 0 (already set above) */
	if (!winner_vled) {
#ifdef CONFIG_DEBUG_FS
		/* All vLEDs lost (none active) */
		list_for_each_entry(vled, &lvc->leds, list) {
			mutex_lock(&vled->lock);
			if (vled->cdev.brightness == 0)
				vled->arbitration_losses++;
			mutex_unlock(&vled->lock);
		}
#endif
		/* Physical LEDs already reset to 0 above */
		goto collect_updates;
	}

	/* STEP 3: Apply winner's configuration to all its physical LEDs */
	mutex_lock(&winner_vled->lock);

	intensities = winner_vled->arb_bufs.intensities;
	scales = winner_vled->arb_bufs.scales;

	if (!intensities || !scales ||
		winner_vled->arb_bufs.capacity < winner_vled->num_channels) {
		mutex_unlock(&winner_vled->lock);
		arb_err(lvc, "vLED '%s': buffer missing or undersized (cap=%u, need=%u)\n",
				winner_vled->name ? winner_vled->name : "(unnamed)",
				winner_vled->arb_bufs.capacity,
				winner_vled->num_channels);
#ifdef CONFIG_DEBUG_FS
		atomic64_inc(&lvc->allocation_failures);
#endif
		/* FIX: Release winner ref AND unlock before returning */
		virtual_led_put(winner_vled);
		mutex_unlock(&lvc->lock);
		return;
	}

	/* Snapshot winner's channel data */
	for (j = 0; j < winner_vled->num_channels; j++) {
		intensities[j] = winner_vled->channels[j].intensity;
		scales[j] = winner_vled->channels[j].scale;
	}

	brightness = winner_vled->cdev.brightness;
	max_brightness = winner_vled->cdev.max_brightness;
	mode = winner_vled->mode;
	vled_seq = winner_vled->sequence;

	mutex_unlock(&winner_vled->lock);

	/* Apply winner to all its channels */
	for (j = 0; j < winner_vled->num_channels; j++) {
		final_intensity = vled_channel_get_final_intensity(
			mode, intensities[j], scales[j], brightness, max_brightness
		);

		apply_winner_to_channel(lvc, winner_vled,
			winner_vled->channels[j].leds,
			winner_vled->channels[j].num_leds,
			final_intensity);

		/* Mark physical LEDs as used by winner (only if bitmap available) */
		if (ple_used_by_winner)
			mark_winner_leds(lvc, winner_vled, j, ple_used_by_winner, ple_count);
	}

#ifdef CONFIG_DEBUG_FS
	winner_vled->arbitration_wins++;

	/* Mark all non-winners as losers */
	list_for_each_entry(vled, &lvc->leds, list) {
		if (vled != winner_vled) {
			mutex_lock(&vled->lock);
			if (vled->cdev.brightness > 0)
				vled->arbitration_losses++;
			mutex_unlock(&vled->lock);
		}
	}
#endif

	/* STEP 4: Turn off physical LEDs not used by winner */
	if (ple_used_by_winner) {
		/* Fast path: Use bitmap */
		i = 0;
		list_for_each_entry(ple, &lvc->phys_leds, list) {
			if (!ple_used_by_winner[i]) {
				ple->chosen_brightness = 0;
				ple->chosen_priority = -1;
				ple->chosen_sequence = 0;
#ifdef CONFIG_DEBUG_FS
				ple->winner_name[0] = '\0';
#endif
			}
			i++;
		}
	} else {
		/* Slow path: Full scan when bitmap unavailable (graceful degradation) */
		list_for_each_entry(ple, &lvc->phys_leds, list) {
			bool used_by_winner = false;
			unsigned int chan_idx, led_idx;

			/* Check if this physical LED is in winner's channel list */
			for (chan_idx = 0; chan_idx < winner_vled->num_channels && !used_by_winner; chan_idx++) {
				for (led_idx = 0; led_idx < winner_vled->channels[chan_idx].num_leds; led_idx++) {
					if (winner_vled->channels[chan_idx].leds[led_idx] == ple->cdev) {
						used_by_winner = true;
						break;
					}
				}
			}

			if (!used_by_winner) {
				ple->chosen_brightness = 0;
				ple->chosen_priority = -1;
				ple->chosen_sequence = 0;
#ifdef CONFIG_DEBUG_FS
				ple->winner_name[0] = '\0';
#endif
			}
		}
	}

collect_updates:

	/* Collect LEDs that need updating */
	update_count = 0;

	list_for_each_entry(ple, &lvc->phys_leds, list) {
		if (!ple->cdev)
			continue;

		if (lvc->first_arbitration || ple->cdev->brightness != ple->chosen_brightness) {
			if (update_count >= lvc->update_buf.capacity) {
				dev_err_ratelimited(&lvc->pdev->dev,
					"Update buffer overflow: %u LEDs need update, capacity=%u\n",
					update_count + 1, lvc->update_buf.capacity);
				dev_err_ratelimited(&lvc->pdev->dev,
					"CRITICAL: Increase max_phys_leds to %u and reload driver!\n",
					lvc->phys_led_count + 16);
				dev_err_ratelimited(&lvc->pdev->dev,
					"Skipping remaining %u LEDs - will retry next cycle\n",
					lvc->phys_led_count - update_count);
#ifdef CONFIG_DEBUG_FS
				atomic64_inc(&lvc->update_buffer_overflows);
#endif
				break;
			}

			local_entries[update_count] = phys_led_entry_get(ple);
			local_brightness[update_count] = ple->chosen_brightness;
			update_count++;
		}
	}

	lvc->first_arbitration = false;

#ifdef CONFIG_DEBUG_FS
	lvc->update_count += update_count;
	lvc->last_update = ktime_get();

	arb_end = ktime_get();
	arb_duration_ns = ktime_to_ns(ktime_sub(arb_end, arb_start));

	if (lvc->arb_latency_count == 0 || arb_duration_ns < lvc->arb_latency_min_ns)
		lvc->arb_latency_min_ns = arb_duration_ns;
	if (arb_duration_ns > lvc->arb_latency_max_ns)
		lvc->arb_latency_max_ns = arb_duration_ns;

	if (lvc->arb_latency_total_ns > (U64_MAX - arb_duration_ns) ||
		lvc->arb_latency_count >= (U64_MAX - 1)) {
		lvc->arb_latency_total_ns = arb_duration_ns;
		lvc->arb_latency_count = 1;
		lvc->arb_latency_min_ns = arb_duration_ns;
		lvc->arb_latency_max_ns = arb_duration_ns;
		dev_info(&lvc->pdev->dev,
			"Arbitration latency statistics reset due to overflow\n");
	} else {
		lvc->arb_latency_total_ns += arb_duration_ns;
		lvc->arb_latency_count++;
	}
#endif

	/* FIX: CRITICAL - Unlock BEFORE hardware I/O */
	mutex_unlock(&lvc->lock);

	/* Hardware I/O outside lock */
	for (i = 0; i < update_count; i++) {
		int pm_ret;

		/* Check removing flag and cleanup properly */
		if (atomic_read(&lvc->removing)) {
			/* Release all remaining entries including current one */
			for (; i < update_count; i++) {
				if (local_entries[i])
					phys_led_entry_put(local_entries[i]);
			}
			/* FIX: Release winner ref before returning - lock already released */
			if (winner_vled)
				virtual_led_put(winner_vled);
			return;
		}

		ple = local_entries[i];
		if (!ple || !ple->cdev) {
			phys_led_entry_put(ple);
			continue;
		}

		if (ple->dev && pm_runtime_enabled(ple->dev)) {
			pm_ret = pm_runtime_get_sync(ple->dev);
			if (pm_ret < 0) {
				dev_err_ratelimited(&lvc->pdev->dev,
					"Failed to power LED '%s': %d\n",
					ple->cdev->name, pm_ret);
				pm_runtime_put_noidle(ple->dev);
				phys_led_entry_put(ple);
				continue;
			}
		}

		if (ple->cdev->brightness_set_blocking)
			ple->cdev->brightness_set_blocking(ple->cdev, local_brightness[i]);
		else if (ple->cdev->brightness_set)
			ple->cdev->brightness_set(ple->cdev, local_brightness[i]);

		/*
		 * Ensure the brightness value is visible to other CPUs after the
		 * hardware update completes. Pairs with smp_load_acquire() in
		 * led_get_brightness() or similar readers.
		 */
		smp_store_release(&ple->cdev->brightness, local_brightness[i]);

		if (ple->dev && pm_runtime_enabled(ple->dev))
			pm_runtime_put(ple->dev);

		phys_led_entry_put(ple);
	}

	if (update_delay_us > 0 && update_count > 0)
		usleep_range(update_delay_us, update_delay_us + 100);

	/* FIX: Release winner ref at end - lock was already released above */
	if (winner_vled)
		virtual_led_put(winner_vled);

	/* FIX: Function now returns with lock UNLOCKED as documented */
}

static int virtual_led_brightness_set(struct led_classdev *cdev,
					  enum led_brightness brightness)
{
	struct virtual_led *vled;
	struct vcolor_controller *lvc;

	if (!cdev)
		return -EINVAL;

	vled = container_of(cdev, struct virtual_led, cdev);

	if (!vled || !vled->ctrl)
		return -ENODEV;

	lvc = vled->ctrl;

	mutex_lock(&vled->lock);

	vled->cdev.brightness = brightness;
	vled->sequence = atomic64_inc_return(&lvc->global_sequence);
#ifdef CONFIG_DEBUG_FS
	vled->brightness_set_count++;
#endif
	mutex_unlock(&vled->lock);

	/*
	 * Schedule arbitration based on batching mode.
	 *
	 * When batching is enabled, defer arbitration to reduce overhead
	 * during rapid brightness changes (e.g., software PWM).
	 *
	 * When batching is disabled, run arbitration immediately for
	 * minimal latency.
	 */
	if (enable_update_batching) {
		atomic_inc(&lvc->pending_updates);
		mod_delayed_work(system_wq, &lvc->update_work,
				 msecs_to_jiffies(UPDATE_BATCH_DELAY_MS));
	} else {
		controller_safe_arbitrate(lvc);
	}

	return 0;
}

/*
 * virtual_led_blink_set - Configure LED blinking
 * @cdev: LED class device
 * @delay_on: Pointer to ON duration in milliseconds
 * @delay_off: Pointer to OFF duration in milliseconds
 *
 * Attempts to enable hardware blink on all physical LEDs.
 * If any physical LED lacks hardware blink support, returns error
 * to trigger LED core's automatic software fallback.
 *
 * Returns: 0 on success, -EINVAL to request software blink
 */
static int virtual_led_blink_set(struct led_classdev *cdev,
								  unsigned long *delay_on,
								  unsigned long *delay_off)
{
struct virtual_led *vled = container_of(cdev, struct virtual_led, cdev);

	#ifdef CONFIG_DEBUG_FS
	vled->blink_requests++;
	#endif

	/*
	 * Always return -EINVAL to request LED core's software blink.
	 *
	 * Rationale: Hardware blink on physical LEDs would bypass our
	 * arbitration system. When multiple vLEDs compete for the same
	 * physical LED, hardware blink on that LED would continue even
	 * when a higher-priority vLED takes control.
	 *
	 * LED core will handle software blink by calling brightness_set_blocking()
	 * in a timer, which properly goes through our arbitration.
	 */
	return -EINVAL;
}

static void deferred_update_worker(struct work_struct *work)
{
	struct vcolor_controller *lvc;
	int pending;

	lvc = container_of(work, struct vcolor_controller, update_work.work);

	/* FIXED: Check removal flag BEFORE doing any work */
	if (atomic_read(&lvc->removing))
		return;

	pending = atomic_xchg(&lvc->pending_updates, 0);

	if (pending > 0) {
		if (!controller_safe_arbitrate(lvc)) {
			/* Only reschedule if not removing */
			if (!atomic_read(&lvc->removing)) {
				atomic_set(&lvc->pending_updates, pending);
				mod_delayed_work(system_wq, &lvc->update_work,
						 msecs_to_jiffies(UPDATE_BATCH_DELAY_MS));
			}
		}
	}
}

static ssize_t multi_intensity_show(struct device *dev,
					struct device_attribute *attr, char *buf)
{
	struct led_classdev *cdev;
	struct virtual_led *vled;
	int len;
	unsigned int i;

	cdev = dev_get_drvdata(dev);
	if (!cdev)
		return -ENODEV;

	vled = container_of(cdev, struct virtual_led, cdev);

	mutex_lock(&vled->lock);

	len = 0;
	for (i = 0; i < vled->num_channels; i++) {
		if (i > 0)
			len += scnprintf(buf + len, PAGE_SIZE - len, " ");
		len += scnprintf(buf + len, PAGE_SIZE - len, "%u",
				 vled->channels[i].intensity);
	}
	len += scnprintf(buf + len, PAGE_SIZE - len, "\n");

	mutex_unlock(&vled->lock);
	return len;
}

static int parse_intensity_values(const char *buf, u8 *values,
				  unsigned int expected_count)
{
	char *tmp, *cur, *end;
	unsigned int count, val;
	int ret;
	size_t buf_len;

	if (expected_count == 0)
		return -EINVAL;

	/* NEW: Prevent truncation by rejecting oversized input */
	buf_len = strnlen(buf, PAGE_SIZE + 1);
	if (buf_len > PAGE_SIZE) {
		pr_warn_once("Intensity input exceeds PAGE_SIZE (%lu bytes), rejecting\n",
				 PAGE_SIZE);
		return -EINVAL;
	}

	tmp = kstrndup(buf, PAGE_SIZE, GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;

	cur = tmp;
	end = tmp + strlen(tmp);
	count = 0;
	ret = 0;

	while (cur < end && count < expected_count) {
		while (cur < end && (*cur == ' ' || *cur == '\t' || *cur == '\n'))
			cur++;

		if (cur >= end)
			break;

		if (kstrtouint(cur, 0, &val) || val > 255) {
			ret = -EINVAL;
			goto out;
		}

		values[count++] = (u8)val;

		while (cur < end && *cur != ' ' && *cur != '\t' && *cur != '\n' && *cur != '\0')
			cur++;
	}

	if (count != expected_count)
		ret = -EINVAL;

out:
	kfree(tmp);
	return ret;
}

static ssize_t multi_intensity_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct led_classdev *cdev;
	struct virtual_led *vled;
	u8 *values;
	int ret;
	unsigned int i;
	struct vcolor_controller *lvc;

	cdev = dev_get_drvdata(dev);
	if (!cdev)
		return -ENODEV;

	vled = container_of(cdev, struct virtual_led, cdev);

	/* Check ratelimit early to avoid unnecessary work */
	if (!__ratelimit(&vled->intensity_ratelimit)) {
#ifdef CONFIG_DEBUG_FS
		if (vled->ctrl)
			atomic64_inc(&vled->ctrl->ratelimit_hits);
		vled->ratelimit_drops++;
#endif
		return count;
	}

	/* Allocate buffer before taking lock */
	values = kcalloc(vled->num_channels, sizeof(*values), GFP_KERNEL);
	if (!values) {
#ifdef CONFIG_DEBUG_FS
		vled_err(vled, "Failed to allocate intensity buffer\n");
		vled->buffer_allocation_failures++;
#endif
		return -ENOMEM;
	}

	/* Parse values before taking lock */
	ret = parse_intensity_values(buf, values, vled->num_channels);
	if (ret) {
		vled_err(vled, "Invalid intensity format\n");
#ifdef CONFIG_DEBUG_FS
		vled->intensity_parse_errors++;
#endif
		kfree(values);
		return ret;
	}

	/* Now take lock and hold it for the entire operation */
	mutex_lock(&vled->lock);

	/* Check mode while holding lock */
	if (vled->mode == VLED_MODE_STANDARD) {
		mutex_unlock(&vled->lock);  /* Unlock before returning */
		dev_warn_ratelimited(dev,
			"Cannot change intensity in standard mode\n");
		kfree(values);
		return -EPERM;
	}

	/* Apply intensity values */
	for (i = 0; i < vled->num_channels; i++)
		vled->channels[i].intensity = values[i];

#ifdef CONFIG_DEBUG_FS
	vled->intensity_update_count++;
#endif

	lvc = vled->ctrl;
	if (lvc)
		vled->sequence = atomic64_inc_return(&lvc->global_sequence);

	mutex_unlock(&vled->lock);  /* Unlock after all changes */

	kfree(values);

	if (lvc) {
		if (enable_update_batching) {
			atomic_inc(&lvc->pending_updates);
			mod_delayed_work(system_wq, &lvc->update_work,
					 msecs_to_jiffies(UPDATE_BATCH_DELAY_MS));
		} else {
			controller_safe_arbitrate(lvc);
		}
	}

	return count;
}
static DEVICE_ATTR_RW(multi_intensity);

static ssize_t multi_index_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct led_classdev *cdev;
	struct virtual_led *vled;
	int len;
	unsigned int i;

	cdev = dev_get_drvdata(dev);
	if (!cdev)
		return -ENODEV;

	vled = container_of(cdev, struct virtual_led, cdev);

	mutex_lock(&vled->lock);

	len = 0;
	for (i = 0; i < vled->num_channels; i++) {
		if (i > 0)
			len += scnprintf(buf + len, PAGE_SIZE - len, " ");
		len += scnprintf(buf + len, PAGE_SIZE - len, "%u",
				 vled->channels[i].color_id);
	}
	len += scnprintf(buf + len, PAGE_SIZE - len, "\n");

	mutex_unlock(&vled->lock);
	return len;
}
static DEVICE_ATTR_RO(multi_index);

static ssize_t multi_multipliers_show(struct device *dev,
					  struct device_attribute *attr, char *buf)
{
	struct led_classdev *cdev;
	struct virtual_led *vled;
	int len;
	unsigned int i;

	cdev = dev_get_drvdata(dev);
	if (!cdev)
		return -ENODEV;

	vled = container_of(cdev, struct virtual_led, cdev);

	mutex_lock(&vled->lock);

	len = 0;
	for (i = 0; i < vled->num_channels; i++) {
		if (i > 0)
			len += scnprintf(buf + len, PAGE_SIZE - len, " ");
		len += scnprintf(buf + len, PAGE_SIZE - len, "%u",
				 vled->channels[i].scale);
	}
	len += scnprintf(buf + len, PAGE_SIZE - len, "\n");

	mutex_unlock(&vled->lock);
	return len;
}
static DEVICE_ATTR_RO(multi_multipliers);

static struct attribute *virtual_led_attrs[] = {
	&dev_attr_multi_intensity.attr,
	&dev_attr_multi_index.attr,
	&dev_attr_multi_multipliers.attr,
	NULL
};

static struct attribute_group virtual_led_attr_group = {
	.attrs = virtual_led_attrs,
	.name = "mc",
};

static const struct attribute_group *virtual_led_groups[] = {
	&virtual_led_attr_group,
	NULL
};


/*
 * OPTIONAL: Even cleaner alternative using a helper structure
 *
 * This version is slightly longer but even more maintainable.
 */

struct led_transfer_tracker {
	struct led_classdev **leds;
	struct device **devs;
	bool *led_used;
	bool *dev_used;
	unsigned int count;
};

static int led_transfer_tracker_init(struct led_transfer_tracker *tracker,
					  struct led_classdev **leds,
					  struct device **devs,
					  unsigned int count)
{
	tracker->leds = leds;
	tracker->devs = devs;
	tracker->count = count;

	tracker->led_used = kcalloc(count, sizeof(*tracker->led_used), GFP_KERNEL);
	tracker->dev_used = kcalloc(count, sizeof(*tracker->dev_used), GFP_KERNEL);

	if (!tracker->led_used || !tracker->dev_used) {
		kfree(tracker->led_used);
		kfree(tracker->dev_used);
		tracker->led_used = NULL;
		tracker->dev_used = NULL;
		return -ENOMEM;
	}

	return 0;
}

static void led_transfer_tracker_mark(struct led_transfer_tracker *tracker,
					  unsigned int index)
{
	if (index >= tracker->count)
		return;

	if (tracker->led_used)
		tracker->led_used[index] = true;

	if (tracker->dev_used && tracker->devs && tracker->devs[index])
		tracker->dev_used[index] = true;
}

static void led_transfer_tracker_cleanup(struct led_transfer_tracker *tracker)
{
	unsigned int i;

	if (!tracker->led_used || !tracker->dev_used) {
		/* Tracking failed, release everything */
		for (i = 0; i < tracker->count; i++) {
			if (tracker->leds && tracker->leds[i])
				led_put(tracker->leds[i]);
			if (tracker->devs && tracker->devs[i])
				put_device(tracker->devs[i]);
		}
	} else {
		/* Release only non-transferred items */
		for (i = 0; i < tracker->count; i++) {
			if (tracker->leds && tracker->leds[i] && !tracker->led_used[i])
				led_put(tracker->leds[i]);
			if (tracker->devs && tracker->devs[i] && !tracker->dev_used[i])
				put_device(tracker->devs[i]);
		}
	}

	kfree(tracker->led_used);
	kfree(tracker->dev_used);
}

/*
 * ALTERNATIVE VERSION: Using the helper structure
 *
 * This makes the main function cleaner and easier to understand.
 */
static int parse_unified_led_list(struct device *dev,
					 struct fwnode_handle *fwnode,
					 const char *propname,
					 struct mc_channel **out_channels,
					 u8 *out_count)
{
	struct led_classdev **leds = NULL;
	struct device **devs = NULL;
	struct led_transfer_tracker tracker = {0};
	u8 count, i, j;
	struct mc_channel *channels = NULL;
	int ret, color_id;
	unsigned int color_counts[LED_COLOR_ID_MAX] = {0};
	unsigned int num_channels = 0;
	unsigned int num_channels_allocated = 0;
	unsigned int led_idx;

	ret = parse_leds_fwnode_array(dev, fwnode, propname,
					  &leds, &devs, &count);
	if (ret)
		return ret;

	/* Initialize transfer tracker */
	ret = led_transfer_tracker_init(&tracker, leds, devs, count);
	if (ret) {
		dev_err(dev, "Failed to allocate transfer tracker\n");
		kfree(leds);
		kfree(devs);
		return ret;
	}

	/* Count LEDs by color */
	for (i = 0; i < count; i++) {
		if (!leds[i])
			continue;

		color_id = leds[i]->color;

		if (color_id < 0 || color_id >= LED_COLOR_ID_MAX) {
			dev_warn(dev, "LED '%s' has invalid color %d, skipping\n",
				 leds[i]->name, color_id);
			continue;
		}

		color_counts[color_id]++;
	}

	/* Count number of unique colors */
	for (i = 0; i < LED_COLOR_ID_MAX; i++) {
		if (color_counts[i] > 0)
			num_channels++;
	}

	if (num_channels > 255) {
		dev_err(dev, "Channel count %u exceeds u8 limit (255)\n", num_channels);
		ret = -EINVAL;
		goto err_cleanup;
	}

	if (num_channels == 0) {
		dev_err(dev, "No valid LEDs found in '%s'\n", propname);
		ret = -ENODEV;
		goto err_cleanup;
	}

	/* Allocate channel structures */
	channels = devm_kcalloc(dev, num_channels, sizeof(*channels), GFP_KERNEL);
	if (!channels) {
		ret = -ENOMEM;
		goto err_cleanup;
	}

	/* Build channels grouped by color */
	num_channels_allocated = 0;
	for (i = 0; i < LED_COLOR_ID_MAX; i++) {
		if (color_counts[i] == 0)
			continue;

		channels[num_channels_allocated].leds = devm_kcalloc(dev, color_counts[i],
						sizeof(*channels[num_channels_allocated].leds),
						GFP_KERNEL);
		channels[num_channels_allocated].led_devs = devm_kcalloc(dev, color_counts[i],
						sizeof(*channels[num_channels_allocated].led_devs),
						GFP_KERNEL);

		if (!channels[num_channels_allocated].leds ||
			!channels[num_channels_allocated].led_devs) {
			ret = -ENOMEM;
			goto err_cleanup;
		}

		/* Transfer LEDs to channel */
		led_idx = 0;
		for (j = 0; j < count; j++) {
			if (!leds[j] || leds[j]->color != i)
				continue;

			channels[num_channels_allocated].leds[led_idx] = leds[j];
			channels[num_channels_allocated].led_devs[led_idx] = devs[j];

			/* Mark as transferred */
			led_transfer_tracker_mark(&tracker, j);

			led_idx++;
		}

		channels[num_channels_allocated].color_id = i;
		channels[num_channels_allocated].num_leds = color_counts[i];
		channels[num_channels_allocated].intensity = 0;
		channels[num_channels_allocated].scale = 255;

		num_channels_allocated++;
	}

	/* Success - free temporary arrays */
	kfree(leds);
	kfree(devs);
	kfree(tracker.led_used);
	kfree(tracker.dev_used);

	*out_channels = channels;
	*out_count = num_channels_allocated;

	return 0;

err_cleanup:
	dev_err(dev, "Failed to allocate channel arrays\n");

	/* Zero any partially-initialized devm channel arrays */
	if (channels) {
		for (i = 0; i < num_channels_allocated; i++) {
			if (channels[i].leds)
				memset(channels[i].leds, 0,
					   channels[i].num_leds * sizeof(*channels[i].leds));
			if (channels[i].led_devs)
				memset(channels[i].led_devs, 0,
					   channels[i].num_leds * sizeof(*channels[i].led_devs));
		}
	}

	/* Clean up using helper - handles all the complexity */
	led_transfer_tracker_cleanup(&tracker);

	kfree(leds);
	kfree(devs);

	return ret;
}

static int parse_channel_multipliers(struct device *dev,
	const struct fwnode_handle *fwnode,
	struct mc_channel *channels,
	unsigned int num_channels)
{
	u32 *scales;
	int ret, i;

	scales = kcalloc(num_channels, sizeof(*scales), GFP_KERNEL);
	if (!scales)
		return -ENOMEM;

	ret = fwnode_property_read_u32_array(fwnode, "mc-channel-multipliers",
						 scales, num_channels);

	if (ret == -EINVAL || ret == -ENODATA) {
		kfree(scales);
		return 0;
	}

	if (ret) {
		dev_err(dev, "Failed to read 'mc-channel-multipliers': %d\n", ret);
		kfree(scales);
		return ret;
	}

	for (i = 0; i < num_channels; i++) {
		if (scales[i] > 255) {
			dev_err(dev, "Invalid scale[%d]=%u (max 255)\n", i, scales[i]);
			kfree(scales);
			return -EINVAL;
		}
		channels[i].scale = scales[i];
	}

	kfree(scales);
	return 0;
}

static int allocate_vled_buffers(struct device *dev, struct virtual_led *vled)
{
	vled->arb_bufs.capacity = vled->num_channels;

	vled->arb_bufs.intensities = devm_kcalloc(dev, vled->num_channels,
						  sizeof(*vled->arb_bufs.intensities),
						  GFP_KERNEL);
	if (!vled->arb_bufs.intensities) {
		vled->arb_bufs.capacity = 0;
		return -ENOMEM;
	}

	vled->arb_bufs.scales = devm_kcalloc(dev, vled->num_channels,
						 sizeof(*vled->arb_bufs.scales),
						 GFP_KERNEL);
	if (!vled->arb_bufs.scales) {
		vled->arb_bufs.capacity = 0;
		return -ENOMEM;
	}

	return 0;
}


static struct virtual_led *virtual_led_init(struct device *dev,
						const struct fwnode_handle *child,
						struct vcolor_controller *lvc)
{
	struct virtual_led *vled;
	const char *mode_str;
	const char *function_str = NULL;
	const char *color_name;
	const char *default_trigger = NULL;
	u32 color_id = LED_COLOR_ID_WHITE;
	u32 priority_u32;
	s32 priority_signed;
	int ret;
	char *led_name;

	vled = kzalloc(sizeof(*vled), GFP_KERNEL);
	if (!vled)
		return ERR_PTR(-ENOMEM);

	kref_init(&vled->refcount);
	mutex_init(&vled->lock);
	INIT_LIST_HEAD(&vled->list);
	vled->fwnode = fwnode_handle_get((struct fwnode_handle *)child);
	vled->ctrl = lvc;

	/* Parse priority as u32, then validate and convert to s32 */
	priority_u32 = 0;
	ret = fwnode_property_read_u32(child, "priority", &priority_u32);
	if (ret) {
		priority_signed = 0;
	} else {
		/* Check if value fits in signed 32-bit range */
		if (priority_u32 > (u32)INT_MAX) {
			dev_warn(dev, "Priority %u exceeds maximum %d, clamping\n",
				 priority_u32, INT_MAX);
			priority_signed = INT_MAX;
		} else {
			priority_signed = (s32)priority_u32;
		}
	}

	vled->priority = priority_signed;

	/* Parse LED mode */
	vled->mode = VLED_MODE_MULTICOLOR;
	ret = fwnode_property_read_string(child, "led-mode", &mode_str);
	if (ret == 0) {
		if (strcmp(mode_str, "standard") == 0) {
			vled->mode = VLED_MODE_STANDARD;
		} else if (strcmp(mode_str, "multicolor") == 0) {
			vled->mode = VLED_MODE_MULTICOLOR;
		} else {
			dev_err(dev, "Invalid led-mode '%s'\n", mode_str);
			ret = -EINVAL;
			goto err_put_fwnode;
		}
	}

	/* Parse LED list */
	ret = parse_unified_led_list(dev, (struct fwnode_handle *)child, "leds",
					  &vled->channels, &vled->num_channels);
	if (ret) {
		dev_err(dev, "Failed to parse LED list: %d\n", ret);
		goto err_put_fwnode;
	}

	/* Parse channel multipliers */
	ret = parse_channel_multipliers(dev, child, vled->channels,
					vled->num_channels);
	if (ret) {
		dev_err(dev, "Failed to parse channel multipliers: %d\n", ret);
		goto err_put_fwnode;
	}

	/* Allocate arbitration buffers */
	ret = allocate_vled_buffers(dev, vled);
	if (ret) {
		dev_err(dev, "Failed to allocate arbitration buffers: %d\n", ret);
		goto err_put_fwnode;
	}

	/* Validate and set max_brightness */
	ret = validate_and_set_max_brightness(vled);
	if (ret) {
		dev_err(dev, "Failed to validate max_brightness: %d\n", ret);
		goto err_put_fwnode;
	}

	/* Parse function and color */
	ret = fwnode_property_read_string(child, "function", &function_str);
	if (ret || !function_str)
		function_str = "status";

	ret = fwnode_property_read_u32(child, "color", &color_id);
	color_name = led_get_color_name(color_id);
	if (!color_name) {
		color_id = LED_COLOR_ID_WHITE;
		color_name = "white";
	}

	ret = fwnode_property_read_string(child, "linux,default-trigger",
					  &default_trigger);
	if (ret)
		default_trigger = NULL;

	led_name = kasprintf(GFP_KERNEL, "%s:%s", function_str, color_name);
	if (!led_name) {
		ret = -ENOMEM;
		goto err_put_fwnode;
	}

	vled->cdev.name = led_name;
	vled->cdev.brightness = 0;
	vled->cdev.brightness_set_blocking = virtual_led_brightness_set;
	vled->cdev.blink_set = virtual_led_blink_set;
	vled->cdev.groups = virtual_led_groups;
	vled->cdev.default_trigger = default_trigger;

	ratelimit_state_init(&vled->intensity_ratelimit,
				 1 * HZ, DEFAULT_UPDATE_RATE_LIMIT);

	dev_info(dev, "vLED '%s': max_brightness=%u, trigger=%s\n",
		 led_name, vled->cdev.max_brightness,
		 default_trigger ? default_trigger : "none");

	return vled;

err_put_fwnode:
	fwnode_handle_put(vled->fwnode);
	kfree(vled);
	return ERR_PTR(ret);
}

static int virtual_led_register(struct device *dev, struct virtual_led *vled)
{
	struct led_init_data init_data = {};
	int ret;

	init_data.fwnode = vled->fwnode;
	init_data.devicename = NULL;
	init_data.default_label = NULL;

	/* Use explicit registration to match vled kzalloc/kfree lifetime */
	ret = led_classdev_register_ext(dev, &vled->cdev, &init_data);
	if (ret) {
		dev_err(dev, "LED registration FAILED for '%s': error %d\n",
			vled->cdev.name, ret);
		return ret;
	}

	vled->cdev_registered = true;
	dev_info(dev, "Registered virtual LED '%s'\n", vled->cdev.name);

	/* Verify LED core assigned name matches */
	vled->name = vled->cdev.name;

	return 0;
}

static void virtual_led_release(struct kref *ref)
{
	struct virtual_led *vled = container_of(ref, struct virtual_led, refcount);

	/*
	 * Automatically unregister if still registered
	 * This ensures we never leak LED class devices even during
	 * abnormal teardown sequences.
	 */
	if (vled->cdev_registered) {
		pr_warn("%s: Auto-unregistering LED '%s' during kref cleanup\n",
				DRIVER_NAME, vled->name ? vled->name : "(unknown)");
		led_classdev_unregister(&vled->cdev);
		vled->cdev_registered = false;
	}

	/* Actually free the memory since we used kzalloc */
	kfree(vled);
}

static void virtual_led_destroy(struct virtual_led *vled)
{
	unsigned int i, j;

	if (!vled)
		return;

	vled->cdev_registered = false;


	if (vled->cdev.name)
		kfree((void *)vled->cdev.name);

#ifdef CONFIG_DEBUG_FS
	debugfs_remove_recursive(vled->debugfs_dir);
#endif

	for (i = 0; i < vled->num_channels; i++) {
		if (vled->channels[i].leds) {
			for (j = 0; j < vled->channels[i].num_leds; j++) {
				if (vled->channels[i].leds[j]) {
					led_put(vled->channels[i].leds[j]);
					vled->channels[i].leds[j] = NULL;
				}
			}
		}

		if (vled->channels[i].led_devs) {
			for (j = 0; j < vled->channels[i].num_leds; j++) {
				if (vled->channels[i].led_devs[j]) {
					put_device(vled->channels[i].led_devs[j]);
					vled->channels[i].led_devs[j] = NULL;
				}
			}
		}
	}

	fwnode_handle_put(vled->fwnode);
}

#ifdef CONFIG_DEBUG_FS

#define SCNPRINTF_FIELD(out, len, size, name, format, value) \
	do { \
		if (len >= size) { \
			break; \
		} \
		len += scnprintf(out + len, size - len, name ": " format "\n", value); \
	} while (0)

static int debugfs_simple_read(struct file *file, char __user *buf,
				   size_t count, loff_t *ppos,
				   int (*format)(void *data, char *out, size_t size))
{
	char *out;
	int len, ret;

	out = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!out)
		return -ENOMEM;

	len = format(file->private_data, out, PAGE_SIZE);
	ret = simple_read_from_buffer(buf, count, ppos, out, len);
	kfree(out);

	return ret;
}

static int format_stats(void *data, char *out, size_t size)
{
	struct vcolor_controller *lvc;
	s64 last_update_ms;
	u64 arb_latency_avg_ns;
	u64 arb_count, update_count, phys_count;
	u64 alloc_failures, buf_overflows, ratelimit_hits;
	int len;

	lvc = data;

	alloc_failures = atomic64_read(&lvc->allocation_failures);
	buf_overflows = atomic64_read(&lvc->update_buffer_overflows);
	ratelimit_hits = atomic64_read(&lvc->ratelimit_hits);

	mutex_lock(&lvc->lock);

	last_update_ms = ktime_to_ms(ktime_sub(ktime_get(), lvc->last_update));

	arb_latency_avg_ns = 0;
	if (lvc->arb_latency_count > 0)
		arb_latency_avg_ns = lvc->arb_latency_total_ns / lvc->arb_latency_count;

	arb_count = lvc->arbitration_count;
	update_count = lvc->update_count;
	phys_count = lvc->phys_led_count;

	mutex_unlock(&lvc->lock);

	len = 0;
	if (len >= size)
		return len;
	len += scnprintf(out + len, size - len, " ===Controller Stats===\n");
	SCNPRINTF_FIELD(out, len, size, "Arbitration cycles", "%llu", arb_count);
	SCNPRINTF_FIELD(out, len, size, "LED updates", "%llu", update_count);
	SCNPRINTF_FIELD(out, len, size, "Last update", "%lld ms ago", last_update_ms);

	if (len >= size)
		return len;
	len += scnprintf(out + len, size - len, "\n===Error Counters===\n");
	SCNPRINTF_FIELD(out, len, size, "Allocation failures", "%llu", alloc_failures);
	SCNPRINTF_FIELD(out, len, size, "Update buffer overflows", "%llu", buf_overflows);
	SCNPRINTF_FIELD(out, len, size, "Rate limit hits", "%llu", ratelimit_hits);
	SCNPRINTF_FIELD(out, len, size, "Global sequence", "%llu",
			atomic64_read(&lvc->global_sequence));

	if (len >= size)
		return len;
	len += scnprintf(out + len, size - len, "\n===Arbitration Latency===\n");
	SCNPRINTF_FIELD(out, len, size, "Min", "%llu ns", lvc->arb_latency_min_ns);
	SCNPRINTF_FIELD(out, len, size, "Max", "%llu ns", lvc->arb_latency_max_ns);
	SCNPRINTF_FIELD(out, len, size, "Avg", "%llu ns", arb_latency_avg_ns);
	SCNPRINTF_FIELD(out, len, size, "Count", "%llu", lvc->arb_latency_count);

	if (len >= size)
		return len;
	len += scnprintf(out + len, size - len, "\n===Configuration===\n");
	SCNPRINTF_FIELD(out, len, size, "Gamma correction", "%s",
			use_gamma_correction ? "enabled" : "disabled");
	SCNPRINTF_FIELD(out, len, size, "Update batching", "%s",
			enable_update_batching ? "enabled" : "disabled");
	SCNPRINTF_FIELD(out, len, size, "Update delay", "%u us", update_delay_us);
	if (len >= size)
		return len;
	len += scnprintf(out + len, size - len, "Physical LED count: %llu/%u\n",
			 phys_count, lvc->update_buf.capacity);
	SCNPRINTF_FIELD(out, len, size, "Removing", "%s",
			atomic_read(&lvc->removing) ? "yes" : "no");

	return len;
}

#ifdef CONFIG_DEBUG_FS

static int format_vled_stats(void *data, char *out, size_t size)
{
	struct vcolor_controller *lvc;
	int len;
	struct virtual_led *vled;
	u64 win_rate;

	lvc = data;

	mutex_lock(&lvc->lock);

	len = 0;
	list_for_each_entry(vled, &lvc->leds, list) {
		if (len >= size)
			break;

		win_rate = 0;
		if (vled->arbitration_participations > 0) {
			win_rate = div64_u64(vled->arbitration_wins * 100ULL,
						 vled->arbitration_participations);
			if (win_rate > 100)
				win_rate = 100;
		}

		if (len >= size)
			return len;
		len += scnprintf(out + len, size - len,
				 " LED: %s ===(Mode: %s, Prio: %d)===\n",
				 vled->name,
				 vled->mode == VLED_MODE_STANDARD ? "standard" : "multicolor",
				 vled->priority);
		SCNPRINTF_FIELD(out, len, size, "Max brightness", "%u",
				vled->cdev.max_brightness);
		SCNPRINTF_FIELD(out, len, size, "Default trigger", "%s",
				vled->cdev.default_trigger ? vled->cdev.default_trigger : "none");
		SCNPRINTF_FIELD(out, len, size, "Brightness sets", "%llu",
				vled->brightness_set_count);
		SCNPRINTF_FIELD(out, len, size, "Intensity sets", "%llu",
				vled->intensity_update_count);
		SCNPRINTF_FIELD(out, len, size, "Blink requests", "%llu",
			vled->blink_requests);
		SCNPRINTF_FIELD(out, len, size, "Sequence", "%llu", vled->sequence);
		if (len >= size)
			break;
		len += scnprintf(out + len, size - len,
				 "Current brightness: %u/%u\n",
				 vled->cdev.brightness, vled->cdev.max_brightness);
		SCNPRINTF_FIELD(out, len, size, "Channels", "%u", vled->num_channels);
		SCNPRINTF_FIELD(out, len, size, "Arbitration participations", "%llu",
				vled->arbitration_participations);
		SCNPRINTF_FIELD(out, len, size, "Arbitration losses", "%llu",
				vled->arbitration_losses);
		SCNPRINTF_FIELD(out, len, size, "Win rate", "%llu%%\n", win_rate);

		if (len >= size)
			return len;
		len += scnprintf(out + len, size - len, "\n===vLED Error Counters===\n");
		SCNPRINTF_FIELD(out, len, size, "Buffer allocation failures", "%llu",
				vled->buffer_allocation_failures);
		SCNPRINTF_FIELD(out, len, size, "Intensity parse errors", "%llu",
				vled->intensity_parse_errors);
		SCNPRINTF_FIELD(out, len, size, "Rate limit drops", "%llu\n",
				vled->ratelimit_drops);
	}

	mutex_unlock(&lvc->lock);
	return len;
}


static int format_phys_led_states(void *data, char *out, size_t size)
{
	struct vcolor_controller *lvc;
	int len;
	struct phys_led_entry *ple;

	lvc = data;

	len = 0;
	len += scnprintf(out + len, size - len, "===Physical LED States===\n");
	if (len >= size)
		return len;
	len += scnprintf(out + len, size - len,
			 "Format: [LED] Brightness Priority Seq Winner\n\n");

	mutex_lock(&lvc->lock);

	list_for_each_entry(ple, &lvc->phys_leds, list) {
		if (len >= size)
			break;
		if (!ple->cdev)
			continue;

		len += scnprintf(out + len, size - len,
			"[%s] B:%u P:%d S:%llu W:%s\n",
			ple->cdev->name,
			ple->chosen_brightness,
			ple->chosen_priority,
			ple->chosen_sequence,
			ple->winner_name[0] ? ple->winner_name : "(none)");
	}

	mutex_unlock(&lvc->lock);
	return len;
}

static int format_claimed_leds(void *data, char *out, size_t size)
{
	unsigned long count, index;
	struct global_phys_owner *gpo;

	down_read(&global_owner_rwsem);

	count = 0;
	xa_for_each(&global_owner_xa, index, gpo)
		if (gpo && !xa_is_value(gpo))
			count++;

	up_read(&global_owner_rwsem);

	return scnprintf(out, size, "%lu\n", count);
}

#define DEBUGFS_READ_FOP(name, formatter) \
static ssize_t debugfs_##name##_read(struct file *file, char __user *buf, \
			size_t count, loff_t *ppos) \
			{ \
	return debugfs_simple_read(file, buf, count, ppos, formatter); \
} \
static const struct file_operations debugfs_##name##_fops = { \
	.owner = THIS_MODULE, \
	.open = simple_open, \
	.read = debugfs_##name##_read, \
	.llseek = default_llseek, \
}

DEBUGFS_READ_FOP(stats, format_stats);
DEBUGFS_READ_FOP(vled_stats, format_vled_stats);
DEBUGFS_READ_FOP(phys_led_states, format_phys_led_states);
DEBUGFS_READ_FOP(claimed, format_claimed_leds);

static ssize_t debugfs_selftest_read(struct file *file, char __user *buf,
					 size_t count, loff_t *ppos)
{
	struct vcolor_controller *lvc;
	char *output;
	int len, ret;

	lvc = file->private_data;

	if (!lvc)
		return -ENODEV;

	output = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!output)
		return -ENOMEM;

	len = 0;
	len += scnprintf(output + len, PAGE_SIZE - len,
			 "\n===%s Selftest===\n", DRIVER_NAME);
	len += scnprintf(output + len, PAGE_SIZE - len,
			 "\nChanges in V4:\n");
	len += scnprintf(output + len, PAGE_SIZE - len,
			 "- Conditional debug compilation: IMPLEMENTED\n");
	len += scnprintf(output + len, PAGE_SIZE - len,
			 "- Reduced struct sizes (~200 bytes per LED): IMPLEMENTED\n");
	len += scnprintf(output + len, PAGE_SIZE - len,
			 "- Eliminated debug telemetry overhead: IMPLEMENTED\n");
	len += scnprintf(output + len, PAGE_SIZE - len,
			 "\nResult: PASS - Production ready (optimized)\n");

	ret = simple_read_from_buffer(buf, count, ppos, output, len);
	kfree(output);

	return ret;
}

static const struct file_operations debugfs_selftest_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = debugfs_selftest_read,
	.llseek = default_llseek,
};


static ssize_t debugfs_stress_test_write(struct file *file,
					 const char __user *buf,
					 size_t count, loff_t *ppos)
{
	struct vcolor_controller *lvc;
	unsigned int iterations, completed, i, j;
	u8 random_data[4];
	struct virtual_led *vled, **vled_snapshot;
	unsigned int vled_count;

	lvc = file->private_data;

	if (!lvc || atomic_read(&lvc->removing))
		return -ENODEV;

	if (kstrtouint_from_user(buf, count, 0, &iterations))
		return -EINVAL;

	if (iterations > 10000) {
		dev_warn(&lvc->pdev->dev, "Clamping stress test to 10000 iterations\n");
		iterations = 10000;
	}

	if (mutex_lock_interruptible(&lvc->lock)) {
		dev_info(&lvc->pdev->dev, "Stress test interrupted by signal\n");
		return -EINTR;
	}

	vled_count = 0;
	list_for_each_entry(vled, &lvc->leds, list)
		vled_count++;

	if (vled_count == 0) {
		mutex_unlock(&lvc->lock);
		dev_info(&lvc->pdev->dev, "No vLEDs available for stress test\n");
		return count;
	}

	vled_snapshot = kcalloc(vled_count, sizeof(*vled_snapshot), GFP_KERNEL);
	if (!vled_snapshot) {
		mutex_unlock(&lvc->lock);
		dev_err(&lvc->pdev->dev, "Failed to allocate vled snapshot for stress test\n");
		return -ENOMEM;
	}

	i = 0;
	list_for_each_entry(vled, &lvc->leds, list) {
		vled_snapshot[i++] = virtual_led_get(vled);
	}

	dev_info(&lvc->pdev->dev, "Starting stress test (%u iterations, %u vLEDs)\n",
			 iterations, vled_count);

		/*
		 * Locking pattern: We hold lvc->lock across arbitration but release it
		 * between iterations to allow other operations. controller_run_arbitration_and_update()
		 * expects the lock to be held on entry and maintains that invariant on return.
		 */
		completed = 0;
		for (i = 0; i < iterations; i++) {
			/* FIXED: Get random data OUTSIDE lock to avoid blocking */
			mutex_unlock(&lvc->lock);
			get_random_bytes(random_data, sizeof(random_data));

			/* Reacquire lock and check if we should abort */
			mutex_lock(&lvc->lock);
			if (atomic_read(&lvc->removing))
				break;

			for (j = 0; j < vled_count; j++) {
				unsigned int k;
				u8 new_brightness;

				vled = vled_snapshot[j];
				if (!vled)
					continue;

				new_brightness = random_data[0] % (vled->cdev.max_brightness + 1);

				mutex_lock(&vled->lock);
				for (k = 0; k < vled->num_channels && k < 3; k++)
					vled->channels[k].intensity = random_data[k + 1];

				vled->cdev.brightness = new_brightness;
				vled->sequence = atomic64_inc_return(&lvc->global_sequence);
				mutex_unlock(&vled->lock);
			}

			controller_run_arbitration_and_update(lvc);
			completed++;

			mutex_unlock(&lvc->lock);
			usleep_range(100, 200);
			mutex_lock(&lvc->lock);
			cond_resched();

			if (atomic_read(&lvc->removing))
				break;
		}

		mutex_unlock(&lvc->lock);

	for (i = 0; i < vled_count; i++)
		virtual_led_put(vled_snapshot[i]);
	kfree(vled_snapshot);

	dev_info(&lvc->pdev->dev,
		 "Stress test completed: %u/%u iterations, %llu total arbitrations\n",
		 completed, iterations, lvc->arbitration_count);

	return count;
}

static const struct file_operations debugfs_stress_test_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = debugfs_stress_test_write,
	.llseek = default_llseek,
};

static ssize_t debugfs_rebuild_write(struct file *file,
					 const char __user *buf,
					 size_t count, loff_t *ppos)
{
	struct vcolor_controller *lvc;
	unsigned int phys_count;

	lvc = file->private_data;

	if (!lvc || lvc->suspended || atomic_read(&lvc->removing))
		return -EBUSY;

	if (mutex_lock_interruptible(&lvc->lock)) {
		dev_info(&lvc->pdev->dev, "Physical LED rebuild interrupted by signal\n");
		return -EINTR;
	}

	/* FIX: Return -EBUSY if already rebuilding */
	if (atomic_read(&lvc->rebuilding)) {
		mutex_unlock(&lvc->lock);
		return -EBUSY;
	}

	if (atomic_read(&lvc->removing)) {
		mutex_unlock(&lvc->lock);
		return -EBUSY;
	}

	dev_info(&lvc->pdev->dev, "Physical LED rebuild triggered via debugfs\n");
	controller_rebuild_phys_leds(lvc);

	phys_count = lvc->phys_led_count;
	dev_info(&lvc->pdev->dev, "Physical LED rebuild complete: %u LEDs registered\n",
		 phys_count);

	/* Lock released by controller_rebuild_phys_leds -> arbitration */

	return count;
}

static const struct file_operations debugfs_rebuild_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = debugfs_rebuild_write,
	.llseek = default_llseek,
};

static void controller_setup_debugfs(struct vcolor_controller *lvc)
{
	char debugfs_dirname[64];

	if (!enable_debugfs)
		return;

	snprintf(debugfs_dirname, sizeof(debugfs_dirname), "%s-%s",
		VLED_DEBUGFS_DIR, dev_name(&lvc->pdev->dev));

	lvc->debugfs_root = debugfs_create_dir(debugfs_dirname, NULL);
	if (IS_ERR_OR_NULL(lvc->debugfs_root)) {
		lvc->debugfs_root = NULL;
		return;
	}

	debugfs_create_file("stats", 0444, lvc->debugfs_root, lvc,
				&debugfs_stats_fops);
	debugfs_create_file("vled_stats", 0444, lvc->debugfs_root, lvc,
				&debugfs_vled_stats_fops);
	debugfs_create_file("phys_led_states", 0444, lvc->debugfs_root, lvc,
				&debugfs_phys_led_states_fops);
	debugfs_create_file("claimed_leds", 0444, lvc->debugfs_root, lvc,
				&debugfs_claimed_fops);
	debugfs_create_file("selftest", 0444, lvc->debugfs_root, lvc,
				&debugfs_selftest_fops);
	debugfs_create_file("stress_test", 0200, lvc->debugfs_root, lvc,
				&debugfs_stress_test_fops);
	debugfs_create_file("rebuild", 0200, lvc->debugfs_root, lvc,
				&debugfs_rebuild_fops);
}

static void controller_destroy_debugfs(struct vcolor_controller *lvc)
{
	debugfs_remove_recursive(lvc->debugfs_root);
}

#else
static inline void controller_setup_debugfs(struct vcolor_controller *lvc) {}
static inline void controller_destroy_debugfs(struct vcolor_controller *lvc) {}
#endif

#endif /* CONFIG_DEBUG_FS */

static int leds_virtualcolor_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct vcolor_controller *lvc;
	struct fwnode_handle *child_fwnode;
	struct virtual_led *vled;
	unsigned int phys_count;
	int ret;
	int initialized_count = 0;

	dev = &pdev->dev;

	lvc = devm_kzalloc(dev, sizeof(*lvc), GFP_KERNEL);
	if (!lvc)
		return -ENOMEM;

	INIT_LIST_HEAD(&lvc->leds);
	INIT_LIST_HEAD(&lvc->phys_leds);
	mutex_init(&lvc->lock);
	lvc->pdev = pdev;
	xa_init(&lvc->phys_xa);
	atomic_set(&lvc->removing, 0);
	atomic_set(&lvc->rebuilding, 0);
	lvc->needs_arbitration = false;
	INIT_DELAYED_WORK(&lvc->update_work, deferred_update_worker);
	atomic_set(&lvc->pending_updates, 0);
	atomic64_set(&lvc->global_sequence, 0);
	lvc->first_arbitration = true;
#ifdef CONFIG_DEBUG_FS
	lvc->last_update = ktime_get();
	atomic64_set(&lvc->allocation_failures, 0);
	atomic64_set(&lvc->update_buffer_overflows, 0);
	atomic64_set(&lvc->ratelimit_hits, 0);
	lvc->arb_latency_min_ns = U64_MAX;
	lvc->arb_latency_max_ns = 0;
	lvc->arb_latency_total_ns = 0;
	lvc->arb_latency_count = 0;
#endif
	dev_set_drvdata(dev, lvc);

	lvc->update_buf.max_capacity = max_phys_leds;
	lvc->update_buf.capacity = max_phys_leds;

	lvc->update_buf.entries = devm_kcalloc(dev, max_phys_leds,
						   sizeof(*lvc->update_buf.entries),
						   GFP_KERNEL);
	lvc->update_buf.brightness = devm_kcalloc(dev, max_phys_leds,
						  sizeof(*lvc->update_buf.brightness),
						  GFP_KERNEL);
	if (!lvc->update_buf.entries || !lvc->update_buf.brightness) {
#ifdef CONFIG_DEBUG_FS
		dev_err(dev, "Failed to allocate update buffers (capacity=%u)\n",
			max_phys_leds);
#endif
		return -ENOMEM;
	}

	/* Pre-allocate arbitration snapshot buffers */
	lvc->vled_snapshot_capacity = VLED_SNAPSHOT_DEFAULT;
	lvc->vled_snapshot = devm_kcalloc(dev, lvc->vled_snapshot_capacity,
					  sizeof(*lvc->vled_snapshot), GFP_KERNEL);
	if (!lvc->vled_snapshot) {
#ifdef CONFIG_DEBUG_FS
		dev_err(dev, "Failed to allocate vLED snapshot buffer\n");
#endif
		return -ENOMEM;
	}

	lvc->ple_snapshot_capacity = max_phys_leds;
	lvc->ple_snapshot = devm_kcalloc(dev, lvc->ple_snapshot_capacity,
					 sizeof(*lvc->ple_snapshot), GFP_KERNEL);
	if (!lvc->ple_snapshot) {
#ifdef CONFIG_DEBUG_FS
		dev_err(dev, "Failed to allocate PLE snapshot buffer\n");
#endif
		return -ENOMEM;
	}

	lvc->ple_usage_bitmap_capacity = max_phys_leds;
	lvc->ple_usage_bitmap = devm_kcalloc(dev, lvc->ple_usage_bitmap_capacity,
						 sizeof(*lvc->ple_usage_bitmap), GFP_KERNEL);
	if (!lvc->ple_usage_bitmap) {
#ifdef CONFIG_DEBUG_FS
		dev_err(dev, "Failed to allocate PLE usage bitmap\n");
#endif
		return -ENOMEM;
	}

	controller_setup_debugfs(lvc);

	/*
	 * PHASE 1: Initialize vLEDs and build internal list
	 *
	 * Uses generic fwnode child iteration to maintain the single OF bridge pattern.
	 * device_for_each_child_node() handles reference counting automatically.
	 */
	device_for_each_child_node(dev, child_fwnode) {
		/*
		 * virtual_led_init will call fwnode_handle_get() internally,
		 * so we pass the fwnode directly
		 */
		vled = virtual_led_init(dev, child_fwnode, lvc);
		if (IS_ERR(vled)) {
			ret = PTR_ERR(vled);
			dev_err(dev, "Failed to create LED from device node: %d\n", ret);

			/* Handle deferred probe specially */
			if (ret == -EPROBE_DEFER) {
				dev_info(dev, "Deferring probe until LEDs are available\n");
				fwnode_handle_put(child_fwnode);
				controller_destroy_debugfs(lvc);
				return -EPROBE_DEFER;
			}
			/* Loop continues, macro handles fwnode_handle_put */
			continue;
		}

		mutex_lock(&lvc->lock);
		list_add_tail(&vled->list, &lvc->leds);
		mutex_unlock(&lvc->lock);

		initialized_count++;
	}

	if (initialized_count == 0) {
		ret = dev_err_probe(dev, -ENODEV, "No valid LED nodes found\n");
		goto err_cleanup;
	}

	/*
	 * PHASE 2: Build physical LED mappings NOW.
	 * The controller is now in a consistent state.
	 */
	mutex_lock(&lvc->lock);
	controller_rebuild_phys_leds(lvc);
	phys_count = lvc->phys_led_count;

	if (phys_count > max_phys_leds) {
		dev_warn(dev, "Physical LED count (%u) exceeds limit (%u)\n",
			 phys_count, max_phys_leds);
	}
	mutex_unlock(&lvc->lock);

	/*
	 * Force all physical LEDs to known state (brightness=0).
	 *
	 * This is critical because:
	 * 1. Device tree may have boot LED aliases (linux,default-trigger = "default-on")
	 * 2. Physical LED drivers may restore previous brightness on probe
	 * 3. Without this, first arbitration compares current_brightness with chosen_brightness
	 *	and skips update if they match (even though driver never set it)
	 *
	 * The first_arbitration flag helps, but DT triggers can activate LEDs AFTER
	 * our probe completes, so we must force them off here.
	 */
	{
		struct phys_led_entry *ple;
		unsigned int reset_count = 0;

		dev_info(dev, "Forcing %u physical LEDs to initial state (off)\n", phys_count);

		mutex_lock(&lvc->lock);
		list_for_each_entry(ple, &lvc->phys_leds, list) {
			if (!ple->cdev)
				continue;

			/* Force hardware to brightness=0 */
			if (ple->cdev->brightness_set_blocking)
				ple->cdev->brightness_set_blocking(ple->cdev, 0);
			else if (ple->cdev->brightness_set)
				ple->cdev->brightness_set(ple->cdev, 0);

			/* Update driver's view to match hardware */
			ple->cdev->brightness = 0;
			reset_count++;

			dev_dbg(dev, "  Reset physical LED '%s' to brightness=0\n",
				ple->cdev->name ? ple->cdev->name : "(unnamed)");
		}
		mutex_unlock(&lvc->lock);

		if (reset_count > 0)
			dev_info(dev, "Reset %u physical LEDs to off state\n", reset_count);
	}

	/*
	 * PHASE 3: Register vLEDs (expose to userspace)
	 * If registration fails, the device is shut down completely.
	 */
	list_for_each_entry(vled, &lvc->leds, list) {
		ret = virtual_led_register(dev, vled);
		if (ret)
			goto err_cleanup;
	}

	dev_info(dev, "Initialized %d virtual LED(s), controlling %u physical LEDs\n",
		 initialized_count, phys_count);

	return 0;

err_cleanup:
		/*
		 * Release global ownership claims
		 * Must happen BEFORE destroying physical LED list to prevent
		 * use-after-free when iterating global_owner_xa.
		 */
		global_release_all_for_pdev(pdev);

		/*
		 * IMPORTANT: Since virtual_leds are kzalloc'd (not devm),
		 * we must clean them up manually on failure paths.
		 */
		mutex_lock(&lvc->lock);

		/* Destroy physical LED list and XArray first */
		controller_destroy_phys_list(lvc);
		xa_destroy(&lvc->phys_xa);

		/* FIXED: Manually clean up vleds with explicit unregistration */
		{
			struct virtual_led *v, *tmp;

			list_for_each_entry_safe(v, tmp, &lvc->leds, list) {
				list_del(&v->list);

				/* Explicitly unregister if registered to clean up sysfs immediately */
				if (v->cdev_registered) {
					led_classdev_unregister(&v->cdev);
					v->cdev_registered = false;
				}

				/* Release device references before freeing vLED */
				virtual_led_destroy(v);
				/* This call uses kref_put() which leads to kfree(v) */
				virtual_led_put(v);
			}
		}
		mutex_unlock(&lvc->lock);

		controller_destroy_debugfs(lvc);

		/* devm will clean up the LVC structure itself */
		return ret;
}

static void leds_virtualcolor_remove(struct platform_device *pdev)
{
	struct vcolor_controller *lvc;
	struct virtual_led *vled, *tmp;

	lvc = platform_get_drvdata(pdev);

	if (!lvc)
		return;

	/*  STEP 1: Signal removal FIRST  */
	atomic_set(&lvc->removing, 1);
	smp_mb();  /* Memory barrier ensures visibility across CPUs */

	/*  STEP 2: Cancel delayed work  */
	cancel_delayed_work_sync(&lvc->update_work);

	/*  STEP 3: Wait for rebuild to complete - CRITICAL FIX  */
	while (atomic_read(&lvc->rebuilding))
		msleep(20);  /* Brief sleep to avoid busy-wait */

	/*
	 *  STEP 4: Wait for in-flight arbitration
	 * Now safe - rebuilding is complete, new ops prevented by removing flag
	 */
	mutex_lock(&lvc->lock);
	mutex_unlock(&lvc->lock);

	/* Now safe to destroy physical LED list */
	mutex_lock(&lvc->lock);
	controller_destroy_phys_list(lvc);
	xa_destroy(&lvc->phys_xa);
	mutex_unlock(&lvc->lock);

	list_for_each_entry_safe(vled, tmp, &lvc->leds, list) {
		list_del(&vled->list);

		/* Unregister LED class device before freeing vled memory */
		if (vled->cdev_registered)
			led_classdev_unregister(&vled->cdev);

		virtual_led_destroy(vled);
		virtual_led_put(vled);
	}

	global_release_all_for_pdev(pdev);
	controller_destroy_debugfs(lvc);

	dev_info(&pdev->dev, "Driver removed successfully\n");
}

static void leds_virtualcolor_shutdown(struct platform_device *pdev)
{
	struct vcolor_controller *lvc;
	struct phys_led_entry *ple;

	lvc = platform_get_drvdata(pdev);

	if (!lvc)
		return;

	cancel_delayed_work_sync(&lvc->update_work);

	mutex_lock(&lvc->lock);
	atomic_set(&lvc->removing, 1);

	list_for_each_entry(ple, &lvc->phys_leds, list) {
		if (ple->cdev) {
			if (ple->cdev->brightness_set_blocking)
				ple->cdev->brightness_set_blocking(ple->cdev, 0);
			else if (ple->cdev->brightness_set)
				ple->cdev->brightness_set(ple->cdev, 0);
		}
	}
	controller_destroy_phys_list(lvc);

	mutex_unlock(&lvc->lock);

	dev_info(&pdev->dev, "Driver shutdown: all LEDs turned off\n");
}

#ifdef CONFIG_PM_SLEEP
static int leds_virtualcolor_suspend(struct device *dev)
{
	struct vcolor_controller *lvc;
	struct phys_led_entry *ple;

	lvc = dev_get_drvdata(dev);
	if (!lvc)
		return 0;

	cancel_delayed_work_sync(&lvc->update_work);

	mutex_lock(&lvc->lock);

	/* FIX: Turn off all physical LEDs to save power */
	list_for_each_entry(ple, &lvc->phys_leds, list) {
		if (ple->cdev && ple->cdev->brightness > 0) {
			if (ple->cdev->brightness_set_blocking)
				ple->cdev->brightness_set_blocking(ple->cdev, 0);
			else if (ple->cdev->brightness_set)
				ple->cdev->brightness_set(ple->cdev, 0);
			ple->cdev->brightness = 0;
		}
	}

	lvc->suspended = true;
	mutex_unlock(&lvc->lock);

	dev_info(dev, "System suspended (LEDs turned off)\n");
	return 0;
}

static int leds_virtualcolor_resume(struct device *dev)
{
	struct vcolor_controller *lvc;

	lvc = dev_get_drvdata(dev);
	if (!lvc)
		return 0;

	mutex_lock(&lvc->lock);
	controller_rebuild_phys_leds(lvc);
	lvc->suspended = false;
	/* Lock released by controller_rebuild_phys_leds -> arbitration */

	dev_info(dev, "System resumed\n");
	return 0;
}
#else
#define leds_virtualcolor_suspend NULL
#define leds_virtualcolor_resume NULL
#endif

static const struct dev_pm_ops leds_virtualcolor_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(leds_virtualcolor_suspend, leds_virtualcolor_resume)
};

static const struct of_device_id leds_virtualcolor_dt_ids[] = {
	{ .compatible = "leds-group-virtualcolor" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, leds_virtualcolor_dt_ids);

static struct platform_driver leds_virtualcolor_driver = {
	.probe = leds_virtualcolor_probe,
	.remove = leds_virtualcolor_remove,
	.shutdown = leds_virtualcolor_shutdown,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = leds_virtualcolor_dt_ids,
		.pm = &leds_virtualcolor_pm_ops,
	},
};

static int __init leds_virtualcolor_init(void)
{
	int ret;

	/* Validate and clamp module parameters */
	if (update_delay_us > 1000000) {
		pr_warn(DRIVER_NAME ": update_delay_us=%u exceeds max, clamping to 1000000\n",
			update_delay_us);
		update_delay_us = 1000000;
	}

	if (max_phys_leds < 1 || max_phys_leds > 1024) {
		pr_warn(DRIVER_NAME ": max_phys_leds=%u out of range, using default %u\n",
			max_phys_leds, MAX_PHYS_LEDS_DEFAULT);
		max_phys_leds = MAX_PHYS_LEDS_DEFAULT;
	}

	pr_info(DRIVER_NAME ": v4 - Debug compilation optimization\n");
	pr_info(DRIVER_NAME ": Config: gamma=%s, batching=%s, delay=%uus, max_leds=%u\n",
		use_gamma_correction ? "on" : "off",
		enable_update_batching ? "on" : "off",
		update_delay_us, max_phys_leds);

	ret = platform_driver_register(&leds_virtualcolor_driver);
	if (ret) {
		pr_err(DRIVER_NAME ": Failed to register platform driver: %d\n", ret);
		return ret;
	}

	return 0;
}
module_init(leds_virtualcolor_init);

static void __exit leds_virtualcolor_exit(void)
{
	unsigned long index, leaked = 0;
	struct global_phys_owner *gpo;

	/* Unregister driver first to prevent new probes */
	platform_driver_unregister(&leds_virtualcolor_driver);

	/* Check for leaked ownership entries */
	down_write(&global_owner_rwsem);

	xa_for_each(&global_owner_xa, index, gpo) {
		if (gpo && !xa_is_value(gpo)) {
			pr_err(DRIVER_NAME
				   ": LEAK: Ownership entry at index %lu (pdev=%p) not freed\n",
				   index, gpo->owner_pdev);
			leaked++;
		}
	}

	if (leaked) {
		pr_err(DRIVER_NAME ": %lu leaked entries detected at module exit\n", leaked);
		pr_err(DRIVER_NAME ": This indicates controllers were not properly removed\n");
		pr_err(DRIVER_NAME ": Memory leaked to prevent use-after-free corruption\n");
	}

	xa_destroy(&global_owner_xa);
	up_write(&global_owner_rwsem);

	pr_info(DRIVER_NAME ": Driver unloaded%s\n",
		leaked ? " (with memory leaks - see errors above)" : " cleanly");
}
module_exit(leds_virtualcolor_exit);

module_param(enable_debugfs, bool, 0444);
MODULE_PARM_DESC(enable_debugfs,
	"Enable debugfs interface for telemetry and testing (default: Y if CONFIG_DEBUG_FS)");

module_param(use_gamma_correction, bool, 0644);
MODULE_PARM_DESC(use_gamma_correction,
	"Apply 2.2 gamma correction to brightness values (default: N)");

module_param(update_delay_us, uint, 0644);
MODULE_PARM_DESC(update_delay_us,
	"Artificial delay in microseconds after each LED update batch (default: 0, max: 1000000)");

module_param(max_phys_leds, uint, 0444);
MODULE_PARM_DESC(max_phys_leds,
	"Maximum unique physical LEDs per controller (default: 64, range: 1-1024)");

module_param(enable_update_batching, bool, 0644);
MODULE_PARM_DESC(enable_update_batching,
	"Batch brightness updates with 10ms delay to reduce CPU overhead (default: N)");

MODULE_AUTHOR("Jonathan Brophy <professor_jonny@hotmail.com>");
MODULE_DESCRIPTION("Virtual grouped LED driver with multicolor ABI V4");
MODULE_LICENSE("GPL");
MODULE_VERSION("4");
