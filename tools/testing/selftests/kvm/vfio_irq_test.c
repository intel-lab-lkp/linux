// SPDX-License-Identifier: GPL-2.0-only
#include "apic.h"
#include "processor.h"
#include "test_util.h"
#include "kvm_util.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdint.h>
#include <syscall.h>
#include <sys/ioctl.h>
#include <sys/sysinfo.h>
#include <time.h>

#include <sys/eventfd.h>

#include "vfio_pci_util.h"
#include "mercury_device.h"

#define MERCURY_GSI		32
#define MERCURY_IRQ_VECTOR	0x80

#define MERCURY_BAR0_GPA	0xc0000000ul
#define MERCURY_BAR0_SLOT	10

/* Shared variables. */
static bool do_guest_irq = true;

/* Guest-only variables, shared across vCPUs. */
static int irqs_received;
static int irqs_sent;

/* Host-only variables, shared across threads. */
static cpu_set_t possible_mask;
static int min_cpu, max_cpu;
static bool done;
static struct kvm_vcpu *target_vcpu;
static sem_t do_irq;

static bool x2apic;

static void guest_irq_handler(struct ex_regs *regs)
{
	WRITE_ONCE(irqs_received, irqs_received + 1);

	if (x2apic)
		x2apic_write_reg(APIC_EOI, 0);
	else
		xapic_write_reg(APIC_EOI, 0);
}

static void guest_nmi_handler(struct ex_regs *regs)
{
	WRITE_ONCE(irqs_received, irqs_received + 1);
}

#define GUEST_VERIFY_IRQS()							\
do {										\
	int __received;								\
										\
	__received = READ_ONCE(irqs_received);					\
	__GUEST_ASSERT(__received == irqs_sent,					\
			"Sent %u IRQ, received %u IRQs", irqs_sent, __received);\
} while (0)

#define GUEST_WAIT_FOR_IRQ()	\
do {				\
	safe_halt();		\
	GUEST_VERIFY_IRQS();	\
	cli();			\
} while (0)

static void guest_code(uint32_t vcpu_id)
{
	/* GPA is identity mapped. */
	void *mercury_bar0 = (void *)MERCURY_BAR0_GPA;
	uint64_t status;
	int i;

	cli();

	if (x2apic) {
		x2apic_enable();
		GUEST_ASSERT(x2apic_read_reg(APIC_ID) == vcpu_id);
	} else {
		xapic_enable();
		GUEST_ASSERT(xapic_read_reg(APIC_ID) >> 24 == vcpu_id);
	}

	if (vcpu_id == 0) {
		irqs_sent++;
		GUEST_ASSERT(READ_ONCE(do_guest_irq));
		mercury_issue_reset(mercury_bar0);
		GUEST_WAIT_FOR_IRQ();

		status = mercury_get_status(mercury_bar0);
		__GUEST_ASSERT(status & BIT(MERCURY_STATUS_BIT_READY),
			"Expected device ready after reset");
		GUEST_SYNC(irqs_received);
	}

	for ( ; !READ_ONCE(done); ) {
		irqs_sent++;
		if (READ_ONCE(do_guest_irq))
			mercury_force_irq(mercury_bar0);
		GUEST_WAIT_FOR_IRQ();
		GUEST_SYNC(irqs_received);
	}

	sti_nop();

	for (i = 0; i < 1000; i++) {
		mercury_force_irq(mercury_bar0);
		cpu_relax();
	}

	GUEST_VERIFY_IRQS();
	GUEST_SYNC(irqs_received);
}

static void *irq_worker(void *mercury_bar0)
{
	struct kvm_vcpu *vcpu;

	for (;;) {
		sem_wait(&do_irq);

		if (READ_ONCE(done))
			break;

		vcpu = READ_ONCE(target_vcpu);
		while (!vcpu_get_stat(vcpu, blocking))
			cpu_relax();

		mercury_force_irq(mercury_bar0);
	}
	return NULL;
}

static int next_cpu(int cpu)
{
	/*
	 * Advance to the next CPU, skipping those that weren't in the original
	 * affinity set.  Sadly, there is no CPU_SET_FOR_EACH, and cpu_set_t's
	 * data storage is considered as opaque.  Note, if this task is pinned
	 * to a small set of discontiguous CPUs, e.g. 2 and 1023, this loop will
	 * burn a lot cycles and the test will take longer than normal to
	 * complete.
	 */
	do {
		cpu++;
		if (cpu > max_cpu) {
			cpu = min_cpu;
			TEST_ASSERT(CPU_ISSET(cpu, &possible_mask),
				    "Min CPU = %d must always be usable", cpu);
			break;
		}
	} while (!CPU_ISSET(cpu, &possible_mask));

	return cpu;
}

static void *migration_worker(void *__guest_tid)
{
	pid_t guest_tid = (pid_t)(unsigned long)__guest_tid;
	cpu_set_t allowed_mask;
	int r, i, cpu;

	CPU_ZERO(&allowed_mask);

	for (i = 0, cpu = min_cpu; !READ_ONCE(done); i++, cpu = next_cpu(cpu)) {
		CPU_SET(cpu, &allowed_mask);

		r = sched_setaffinity(guest_tid, sizeof(allowed_mask), &allowed_mask);
		TEST_ASSERT(!r, "sched_setaffinity failed, errno = %d (%s)",
			    errno, strerror(errno));

		CPU_CLR(cpu, &allowed_mask);

		usleep((i % 10) + 10);
	}
	return NULL;
}

static void calc_min_max_cpu(void)
{
	int i, cnt, nproc;

	TEST_REQUIRE(CPU_COUNT(&possible_mask) >= 2);

	/*
	 * CPU_SET doesn't provide a FOR_EACH helper, get the min/max CPU that
	 * this task is affined to in order to reduce the time spent querying
	 * unusable CPUs, e.g. if this task is pinned to a small percentage of
	 * total CPUs.
	 */
	nproc = get_nprocs_conf();
	min_cpu = -1;
	max_cpu = -1;
	cnt = 0;

	for (i = 0; i < nproc; i++) {
		if (!CPU_ISSET(i, &possible_mask))
			continue;
		if (min_cpu == -1)
			min_cpu = i;
		max_cpu = i;
		cnt++;
	}

	__TEST_REQUIRE(cnt >= 2, "Only one usable CPU, task migration not possible");
}

static void sanity_check_mercury_device(struct vfio_pci_dev *dev, void *bar0)
{
	uint16_t vendor_id, device_id;
	uint32_t version;

	vendor_id = vfio_pci_get_vendor_id(dev);
	device_id = vfio_pci_get_device_id(dev);

	TEST_ASSERT(vendor_id == MERCURY_VENDOR_ID &&
		    device_id == MERCURY_DEVICE_ID,
		    "Mercury vendor-id/device-id mismatch.  "
		    "Expected vendor: 0x%04x, device: 0x%04x.  "
		    "Got vendor: 0x%04x, device: 0x%04x",
		    MERCURY_VENDOR_ID, MERCURY_DEVICE_ID,
		    vendor_id, device_id);

	version = mercury_read_reg32(bar0, MERCURY_REG_VERSION);
	TEST_ASSERT_EQ(version, MERCURY_ABI_VERSION);
}

static void set_empty_routing(struct kvm_vm *vm, struct kvm_irq_routing *routing)
{
	routing->nr = 0;
	routing->entries[0].gsi = MERCURY_GSI;
	routing->entries[0].type = KVM_IRQ_ROUTING_IRQCHIP;
	routing->entries[0].flags = 0;
	routing->entries[0].u.msi.address_lo = 0;
	routing->entries[0].u.msi.address_hi = 0;
	routing->entries[0].u.msi.data = 0xfe;
	vm_ioctl(vm, KVM_SET_GSI_ROUTING, routing);
}

static void set_gsi_dest(struct kvm_vcpu *vcpu, struct kvm_irq_routing *routing,
			 bool do_nmi)
{
	routing->nr = 1;
	routing->entries[0].gsi = MERCURY_GSI;
	routing->entries[0].type = KVM_IRQ_ROUTING_MSI;
	routing->entries[0].flags = 0;
	routing->entries[0].u.msi.address_lo = (vcpu->id << 12);
	routing->entries[0].u.msi.address_hi = 0;
	if (do_nmi)
		routing->entries[0].u.msi.data = NMI_VECTOR | (4 << 8);
	else
		routing->entries[0].u.msi.data = MERCURY_IRQ_VECTOR;
	vm_ioctl(vcpu->vm, KVM_SET_GSI_ROUTING, routing);
}

static void vcpu_run_and_verify(struct kvm_vcpu *vcpu, int nr_irqs)
{
	struct ucall uc;

	vcpu_run(vcpu);
	TEST_ASSERT_EQ(get_ucall(vcpu, &uc), UCALL_SYNC);
	TEST_ASSERT_EQ(uc.args[1], nr_irqs);
}

int main(int argc, char *argv[])
{
	bool migrate = false, nmi = false, async = false, empty = false;
	pthread_t migration_thread, irq_thread;
	struct kvm_irq_routing *routing;
	struct vfio_pci_dev *dev;
	struct kvm_vcpu *vcpus[2];
	int opt, r, eventfd, i;
	int nr_irqs = 10000;
	struct kvm_vm *vm;
	uint64_t bar_size;
	char *bdf = NULL;
	void *bar;

	sem_init(&do_irq, 0, 0);

	while ((opt = getopt(argc, argv, "had:ei:mnx")) != -1) {
		switch (opt) {
		case 'a':
			async = true;
			break;
		case 'd':
			bdf = strdup(optarg);
			break;
		case 'e':
			empty = true;
			break;
		case 'i':
			nr_irqs = atoi_positive("Number of IRQs", optarg);
			break;
		case 'm':
			migrate = true;
			break;
		case 'n':
			nmi = true;
			break;
		case 'x':
			x2apic = false;
			break;
		case 'h':
		default:
			pr_info("Usage: %s [-h] <-d pci-bdf>\n\n", argv[0]);
			pr_info("\t-d: PCI Domain, Bus, Device, Function in the format DDDD:BB:DD.F\n");
			pr_info("\t-h: print this help screen\n");
			exit(KSFT_SKIP);
		}
	}

	__TEST_REQUIRE(bdf, "Required argument -d <pci-bdf> missing");

	dev = vfio_pci_init(bdf);
	bar = vfio_pci_map_bar(dev, VFIO_PCI_BAR0_REGION_INDEX, &bar_size);
	sanity_check_mercury_device(dev, bar);

	vm = vm_create_with_vcpus(ARRAY_SIZE(vcpus), guest_code, vcpus);
	vm_install_exception_handler(vm, MERCURY_IRQ_VECTOR, guest_irq_handler);
	vm_install_exception_handler(vm, NMI_VECTOR, guest_nmi_handler);

	vcpu_args_set(vcpus[0], 1, 0);
	vcpu_args_set(vcpus[1], 1, 1);

	virt_pg_map(vm, APIC_DEFAULT_GPA, APIC_DEFAULT_GPA);

	vm_set_user_memory_region(vm, MERCURY_BAR0_SLOT, 0, MERCURY_BAR0_GPA,
				  bar_size, bar);
	virt_map(vm, MERCURY_BAR0_GPA, MERCURY_BAR0_GPA,
		 vm_calc_num_guest_pages(VM_MODE_DEFAULT, bar_size));

	routing = kvm_gsi_routing_create();

	eventfd = kvm_new_eventfd();
	vfio_pci_assign_msix(dev, eventfd);
	kvm_assign_irqfd(vm, MERCURY_GSI, eventfd);

	r = sched_getaffinity(0, sizeof(possible_mask), &possible_mask);
	TEST_ASSERT(!r, "sched_getaffinity failed, errno = %d (%s)", errno,
		    strerror(errno));

	if (migrate) {
		calc_min_max_cpu();

		pthread_create(&migration_thread, NULL, migration_worker,
			       (void *)(unsigned long)syscall(SYS_gettid));
	}

	if (nmi || async)
		pthread_create(&irq_thread, NULL, irq_worker, bar);

	set_gsi_dest(vcpus[0], routing, false);
	vcpu_run_and_verify(vcpus[0], 1);

#if 0
	/*
	 * Hack if the user wants to manually mess with interrupt routing while
	 * the test is running, e.g. by modifying smp_affinity in the host.
	 */
	for (i = 1; i < nr_irqs; i++) {
		usleep(1000 * 1000);
		vcpu_run_and_verify(vcpus[0], i + 1);
	}
#endif

	for (i = 1; i < nr_irqs; i++) {
		struct kvm_vcpu *vcpu = vcpus[!!(i & BIT(1))];
		const bool do_nmi = nmi && (i & BIT(2));
		const bool do_empty = empty && (i & BIT(3));
		const bool do_async = nmi || async;

		if (do_empty)
			set_empty_routing(vm, routing);

		set_gsi_dest(vcpu, routing, do_nmi);

		WRITE_ONCE(do_guest_irq, !do_async);
		sync_global_to_guest(vm, do_guest_irq);

		if (do_async) {
			WRITE_ONCE(target_vcpu, vcpu);
			sem_post(&do_irq);
		}

		vcpu_run_and_verify(vcpu, i + 1);
	}

	WRITE_ONCE(done, true);
	sync_global_to_guest(vm, done);
	sem_post(&do_irq);

	for (i = 0; empty && i < ARRAY_SIZE(vcpus); i++) {
		struct kvm_vcpu *vcpu = vcpus[i];

		if (!i)
			set_gsi_dest(vcpu, routing, false);
		set_empty_routing(vm, routing);
		vcpu_run_and_verify(vcpu, nr_irqs);
	}

	set_gsi_dest(vcpus[0], routing, false);

	if (migrate)
		pthread_join(migration_thread, NULL);

	if (nmi || async)
		pthread_join(irq_thread, NULL);

	r = munmap(bar, bar_size);
	TEST_ASSERT(!r, __KVM_SYSCALL_ERROR("munmap()", r));

	vfio_pci_free(dev);

	return 0;
}
