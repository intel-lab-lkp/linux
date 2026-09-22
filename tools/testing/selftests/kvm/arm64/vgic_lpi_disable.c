// SPDX-License-Identifier: GPL-2.0
/*
 * vgic_lpi_disable - Test cross-vCPU LPI disabling race condition
 *
 * Copyright (c) 2026 Yuchao Zhang <ndaugoing@gmail.com>
 *
 * This test verifies that disabling LPIs from a remote vCPU while the
 * target vCPU has in-flight/overflowing LPIs does not lead to use-after-free
 * or kernel panic.
 */

#include <linux/sizes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/sysinfo.h>

#include "kvm_util.h"
#include "delay.h"
#include "gic.h"
#include "gic_v3.h"
#include "gic_v3_its.h"
#include "processor.h"
#include "ucall.h"
#include "vgic.h"

#define TEST_MEMSLOT_INDEX	1
#define GIC_LPI_OFFSET		8192

#define TARGET_VCPU_ID		0
#define DISABLER_VCPU_ID	1

static size_t nr_iterations = 200;
static gpa_t gpa_base;

static struct kvm_vm *vm;
static struct kvm_vcpu **vcpus;
static int its_fd;

static struct test_data {
	bool		request_vcpus_stop;
	u32		nr_cpus;
	u32		nr_devices;
	u32		nr_event_ids;

	gpa_t		device_table;
	gpa_t		collection_table;
	gpa_t		cmdq_base;
	void		*cmdq_base_va;
	gpa_t		itt_tables;

	gpa_t		lpi_prop_table;
	gpa_t		lpi_pend_tables;
} test_data = {
	.nr_cpus	= 2,
	.nr_devices	= 1,
	.nr_event_ids	= 64,
};

static void guest_irq_handler(struct ex_regs *regs)
{
	u32 intid = gic_get_and_ack_irq();

	if (intid == IAR_SPURIOUS)
		return;

	GUEST_ASSERT(intid >= GIC_LPI_OFFSET);
	gic_set_eoi(intid);
}

static void guest_setup_its_mappings(void)
{
	u32 device_id, event_id, intid = GIC_LPI_OFFSET;
	u32 nr_events = test_data.nr_event_ids;
	u32 nr_devices = test_data.nr_devices;

	/* Map collection 0 to TARGET_VCPU_ID */
	its_send_mapc_cmd(test_data.cmdq_base_va, TARGET_VCPU_ID, TARGET_VCPU_ID, true);

	/* Map all LPIs to TARGET_VCPU_ID to force LR overflow */
	for (device_id = 0; device_id < nr_devices; device_id++) {
		gpa_t itt_base = test_data.itt_tables + (device_id * SZ_64K);

		its_send_mapd_cmd(test_data.cmdq_base_va, device_id,
				  itt_base, SZ_64K, true);

		for (event_id = 0; event_id < nr_events; event_id++) {
			its_send_mapti_cmd(test_data.cmdq_base_va, device_id,
					   event_id, TARGET_VCPU_ID, intid++);
		}
	}
}

static void guest_invalidate_all_rdists(void)
{
	int i;

	for (i = 0; i < test_data.nr_cpus; i++)
		its_send_invall_cmd(test_data.cmdq_base_va, i);
}

static void guest_setup_gic(void)
{
	static atomic_int nr_cpus_ready;
	u32 cpuid = guest_get_vcpuid();

	gic_init(GIC_V3, test_data.nr_cpus);
	gic_rdist_enable_lpis(test_data.lpi_prop_table, SZ_64K,
			      test_data.lpi_pend_tables + (cpuid * SZ_64K));

	atomic_fetch_add(&nr_cpus_ready, 1);

	if (cpuid > 0)
		return;

	while (atomic_load(&nr_cpus_ready) < test_data.nr_cpus)
		cpu_relax();

	its_init(test_data.collection_table, SZ_64K,
		 test_data.device_table, SZ_64K,
		 test_data.cmdq_base, SZ_64K);

	guest_setup_its_mappings();
	guest_invalidate_all_rdists();

	/* SYNC to ensure ITS setup is complete */
	for (cpuid = 0; cpuid < test_data.nr_cpus; cpuid++)
		its_send_sync_cmd(test_data.cmdq_base_va, cpuid);
}

static inline void *test_gicr_base_cpu(u32 cpu)
{
	return (void *)(GICR_BASE_GPA + cpu * SZ_64K * 2);
}

static void test_gicv3_gicr_wait_for_rwp(u32 cpu)
{
	unsigned int count = 100000;

	while (readl(test_gicr_base_cpu(cpu) + GICR_CTLR) & GICR_CTLR_RWP) {
		GUEST_ASSERT(count--);
		udelay(10);
	}
}

static void guest_code(size_t nr_lpis)
{
	u32 cpuid = guest_get_vcpuid();

	guest_setup_gic();

	if (cpuid == TARGET_VCPU_ID) {
		local_irq_enable();
		GUEST_SYNC(0);

		while (!READ_ONCE(test_data.request_vcpus_stop))
			cpu_relax();
	} else {
		GUEST_SYNC(0);

		for (size_t i = 0; i < nr_iterations; i++) {
			/* Remotely disable LPIs on target vCPU */
			writel(0, test_gicr_base_cpu(TARGET_VCPU_ID) + GICR_CTLR);
			test_gicv3_gicr_wait_for_rwp(TARGET_VCPU_ID);

			for (int d = 0; d < 50; d++)
				cpu_relax();

			/* Remotely re-enable LPIs on target vCPU */
			writel(GICR_CTLR_ENABLE_LPIS,
			       test_gicr_base_cpu(TARGET_VCPU_ID) + GICR_CTLR);
			test_gicv3_gicr_wait_for_rwp(TARGET_VCPU_ID);
		}

		WRITE_ONCE(test_data.request_vcpus_stop, true);
	}

	GUEST_DONE();
}

static void setup_memslot(void)
{
	size_t pages;
	size_t sz;

	sz = (3 + test_data.nr_devices) * SZ_64K;
	sz += (1 + test_data.nr_cpus) * SZ_64K;

	pages = sz / vm->page_size;
	gpa_base = ((vm_compute_max_gfn(vm) + 1) * vm->page_size) - sz;
	vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS, gpa_base,
				    TEST_MEMSLOT_INDEX, pages, 0);
}

#define LPI_PROP_DEFAULT_PRIO	0xa0

static void configure_lpis(void)
{
	size_t nr_lpis = test_data.nr_devices * test_data.nr_event_ids;
	u8 *tbl = addr_gpa2hva(vm, test_data.lpi_prop_table);
	size_t i;

	for (i = 0; i < nr_lpis; i++) {
		tbl[i] = LPI_PROP_DEFAULT_PRIO |
			 LPI_PROP_GROUP1 |
			 LPI_PROP_ENABLED;
	}
}

static void setup_test_data(void)
{
	size_t pages_per_64k = vm_calc_num_guest_pages(vm->mode, SZ_64K);
	u32 nr_devices = test_data.nr_devices;
	u32 nr_cpus = test_data.nr_cpus;
	gpa_t cmdq_base;

	test_data.device_table = vm_phy_pages_alloc(vm, pages_per_64k,
						    gpa_base,
						    TEST_MEMSLOT_INDEX);

	test_data.collection_table = vm_phy_pages_alloc(vm, pages_per_64k,
							gpa_base,
							TEST_MEMSLOT_INDEX);

	cmdq_base = vm_phy_pages_alloc(vm, pages_per_64k, gpa_base,
				       TEST_MEMSLOT_INDEX);
	virt_map(vm, cmdq_base, cmdq_base, pages_per_64k);
	test_data.cmdq_base = cmdq_base;
	test_data.cmdq_base_va = (void *)cmdq_base;

	test_data.itt_tables = vm_phy_pages_alloc(vm, pages_per_64k * nr_devices,
						  gpa_base, TEST_MEMSLOT_INDEX);

	test_data.lpi_prop_table = vm_phy_pages_alloc(vm, pages_per_64k,
						      gpa_base, TEST_MEMSLOT_INDEX);
	configure_lpis();

	test_data.lpi_pend_tables = vm_phy_pages_alloc(vm, pages_per_64k * nr_cpus,
						       gpa_base, TEST_MEMSLOT_INDEX);

	sync_global_to_guest(vm, test_data);
}

static void setup_gic(void)
{
	its_fd = vgic_its_setup(vm);
}

static void signal_lpi(u32 device_id, u32 event_id)
{
	gpa_t db_addr = GITS_BASE_GPA + GITS_TRANSLATER;

	struct kvm_msi msi = {
		.address_lo	= db_addr,
		.address_hi	= db_addr >> 32,
		.data		= event_id,
		.devid		= device_id,
		.flags		= KVM_MSI_VALID_DEVID,
	};

	__vm_ioctl(vm, KVM_SIGNAL_MSI, &msi);
}

static pthread_barrier_t test_setup_barrier;

static atomic_bool stop_lpi_thread;

static void *lpi_worker_thread(void *data)
{
	u32 device_id = (size_t)data;
	u32 event_id;

	pthread_barrier_wait(&test_setup_barrier);

	while (!atomic_load(&stop_lpi_thread)) {
		for (event_id = 0; event_id < test_data.nr_event_ids; event_id++)
			signal_lpi(device_id, event_id);
		usleep(100);
	}

	return NULL;
}

static void *vcpu_worker_thread(void *data)
{
	struct kvm_vcpu *vcpu = data;
	struct ucall uc;

	while (true) {
		vcpu_run(vcpu);

		switch (get_ucall(vcpu, &uc)) {
		case UCALL_SYNC:
			pthread_barrier_wait(&test_setup_barrier);
			continue;
		case UCALL_DONE:
			if (vcpu == vcpus[DISABLER_VCPU_ID]) {
				atomic_store(&stop_lpi_thread, true);
				write_guest_global(vm, test_data.request_vcpus_stop, true);
			}
			return NULL;
		case UCALL_ABORT:
			REPORT_GUEST_ASSERT(uc);
			break;
		default:
			TEST_FAIL("Unknown ucall: %lu", uc.cmd);
		}
	}

	return NULL;
}

static void run_test(void)
{
	pthread_t *vcpu_threads;
	pthread_t lpi_thread;
	u32 i;

	pthread_barrier_init(&test_setup_barrier, NULL, test_data.nr_cpus + 1);

	vcpu_threads = malloc(sizeof(pthread_t) * test_data.nr_cpus);
	TEST_ASSERT(vcpu_threads, "Failed to allocate vcpu_threads");

	for (i = 0; i < test_data.nr_cpus; i++)
		pthread_create(&vcpu_threads[i], NULL, vcpu_worker_thread, vcpus[i]);

	pthread_create(&lpi_thread, NULL, lpi_worker_thread, (void *)(size_t)0);

	pthread_join(lpi_thread, NULL);
	for (i = 0; i < test_data.nr_cpus; i++)
		pthread_join(vcpu_threads[i], NULL);

	free(vcpu_threads);
}

static void setup_vm(void)
{
	int i;

	vm = vm_create_with_vcpus(test_data.nr_cpus, guest_code, vcpus);

	vm_init_descriptor_tables(vm);
	for (i = 0; i < test_data.nr_cpus; i++)
		vcpu_init_descriptor_tables(vcpus[i]);

	vm_install_exception_handler(vm, VECTOR_IRQ_CURRENT, guest_irq_handler);

	setup_memslot();
	setup_gic();
	setup_test_data();
}

static void destroy_vm(void)
{
	close(its_fd);
	kvm_vm_free(vm);
}

static void help(const char *name)
{
	pr_info("Usage: %s [-i iterations] [-e event_ids]\n", name);
	pr_info(" -i: number of iterations to toggle GICR_CTLR.EnableLPIs (default %lu)\n",
		nr_iterations);
	pr_info(" -e: number of event IDs/LPIs to inject (default %u)\n",
		test_data.nr_event_ids);
}

int main(int argc, char **argv)
{
	int opt;

	TEST_REQUIRE(kvm_supports_vgic_v3());

	while ((opt = getopt(argc, argv, "i:e:h")) != -1) {
		switch (opt) {
		case 'i':
			nr_iterations = atoi_positive("iterations", optarg);
			break;
		case 'e':
			test_data.nr_event_ids = atoi_positive("event_ids", optarg);
			break;
		case 'h':
		default:
			help(argv[0]);
			exit(opt == 'h' ? 0 : 1);
		}
	}

	vcpus = malloc(sizeof(struct kvm_vcpu *) * test_data.nr_cpus);
	TEST_ASSERT(vcpus, "Failed to allocate vcpus array");

	setup_vm();
	run_test();
	destroy_vm();

	free(vcpus);

	pr_info("Completed %lu iterations of remote LPI disable successfully\n",
		nr_iterations);

	return 0;
}
