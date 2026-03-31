// SPDX-License-Identifier: GPL-2.0
#include "kvm_util.h"
#include "test_util.h"
#include "irq_util.h"
#include "apic.h"
#include "processor.h"

#include <stdint.h>
#include <pthread.h>
#include <ctype.h>
#include <time.h>
#include <linux/vfio.h>
#include <linux/sizes.h>
#include <sys/sysinfo.h>

#include <libvfio.h>

static bool done;

static bool guest_ready_for_irqs[KVM_MAX_VCPUS];
static bool guest_received_irq[KVM_MAX_VCPUS];
#define TIMEOUT_NS (2ULL * 1000 * 1000 * 1000)

static u32 guest_get_vcpu_id(void)
{
	return x2apic_read_reg(APIC_ID);
}

static void guest_irq_handler(struct ex_regs *regs)
{
	WRITE_ONCE(guest_received_irq[guest_get_vcpu_id()], true);

	x2apic_write_reg(APIC_EOI, 0);
}

static void guest_code(void)
{
	x2apic_enable();

	sti_nop();

	WRITE_ONCE(guest_ready_for_irqs[guest_get_vcpu_id()], true);

	while (!READ_ONCE(done)) {
		cpu_relax();
	}

	GUEST_DONE();
}

static void *vcpu_thread_main(void *arg)
{
	struct kvm_vcpu *vcpu = arg;
	struct ucall uc;

	vcpu_run(vcpu);
	TEST_ASSERT_EQ(UCALL_DONE, get_ucall(vcpu, &uc));

	return NULL;
}

static void kvm_route_msi(struct kvm_vm *vm, u32 gsi, struct kvm_vcpu *vcpu,
			  u8 vector)
{
	u8 buf[sizeof(struct kvm_irq_routing) + sizeof(struct kvm_irq_routing_entry)] = {};
	struct kvm_irq_routing *routes = (void *)&buf;

	routes->nr = 1;
	routes->entries[0].gsi = gsi;
	routes->entries[0].type = KVM_IRQ_ROUTING_MSI;
	routes->entries[0].u.msi.address_lo = 0xFEE00000 | (vcpu->id << 12);
	routes->entries[0].u.msi.data = vector;

	vm_ioctl(vm, KVM_SET_GSI_ROUTING, routes);
}

static int setup_msi(struct vfio_pci_device *device)
{
	TEST_REQUIRE(device->msix_info.count > 0);
	vfio_pci_msix_enable(device, 0, 1);
	return 0;
}

static void send_msi(struct vfio_pci_device *device, int msi)
{
	vfio_pci_irq_trigger(device, VFIO_PCI_MSIX_IRQ_INDEX, msi);
}

static void help(const char *name)
{
	printf("Usage: %s [-a] [-h] segment:bus:device.function\n",
	       name);
	printf("\n");
	printf("  -a: Randomly affinitize the device IRQ to different CPUs\n"
	       "      throughout the test.\n");
	printf("\n");
	exit(KSFT_FAIL);
}

int main(int argc, char **argv)
{
	/*
	 * Pick a random vector and a random GSI to use for device IRQ.
	 *
	 * Pick an IRQ vector in range [32, UINT8_MAX]. Min value is 32 because
	 * Linux/x86 reserves vectors 0-31 for exceptions and architecture
	 * defined NMIs and interrupts.
	 *
	 * Pick a GSI in range [24, KVM_MAX_IRQ_ROUTES - 1]. The min value is 24
	 * because KVM reserves GSIs 0-15 for legacy ISA IRQs and 16-23 only go
	 * to the IOAPIC. The max is KVM_MAX_IRQ_ROUTES - 1, because
	 * KVM_MAX_IRQ_ROUTES is exclusive.
	*/
	u32 gsi = 24 + rand() % (KVM_MAX_IRQ_ROUTES - 1 - 24);
	u8 vector = 32 + rand() % (UINT8_MAX - 32);

	/* Test configuration (overridable by command line flags). */
	bool irq_affinity = false;
	int nr_irqs = 1000;
	int nr_vcpus = 1;

	struct kvm_vcpu *vcpus[KVM_MAX_VCPUS];
	pthread_t vcpu_threads[KVM_MAX_VCPUS];
	u64 irq_count, pin_count, piw_count;
	struct vfio_pci_device *device;
	struct iommu *iommu;
	const char *device_bdf;
	FILE *irq_affinity_fp;
	int i, j, c, msi, irq;
	struct kvm_vm *vm;
	int irq_cpu;
	int ret;

	device_bdf = vfio_selftests_get_bdf(&argc, argv);

	while ((c = getopt(argc, argv, "ah")) != -1) {
		switch (c) {
		case 'a':
			irq_affinity = true;
			break;
		case 'h':
		default:
			help(argv[0]);
		}
	}

	vm = vm_create_with_vcpus(nr_vcpus, guest_code, vcpus);
	vm_install_exception_handler(vm, vector, guest_irq_handler);

	iommu = iommu_init(default_iommu_mode);
	device = vfio_pci_device_init(device_bdf, iommu);
	msi = setup_msi(device);
	irq = get_irq_number(device_bdf, msi);

	irq_count = get_irq_count(irq);
	pin_count = get_irq_count_by_name("PIN:");
	piw_count = get_irq_count_by_name("PIW:");

	printf("%s %s MSI-X[%d] (IRQ-%d) %d times\n",
               "Notifying the eventfd for",
	       device_bdf, msi, irq, nr_irqs);

	kvm_assign_irqfd(vm, gsi, device->msi_eventfds[msi]);

	for (i = 0; i < nr_vcpus; i++)
		pthread_create(&vcpu_threads[i], NULL, vcpu_thread_main, vcpus[i]);

	for (i = 0; i < nr_vcpus; i++) {
		struct kvm_vcpu *vcpu = vcpus[i];

		while (!READ_FROM_GUEST(vm, guest_ready_for_irqs[vcpu->id]))
			continue;
	}

	if (irq_affinity) {
		char path[PATH_MAX];

		snprintf(path, sizeof(path), "/proc/irq/%d/smp_affinity_list", irq);
		irq_affinity_fp = fopen(path, "w");
		TEST_ASSERT(irq_affinity_fp, "fopen(%s) failed", path);
	}

	/* Set a consistent seed so that test are repeatable. */
	srand(0);

	for (i = 0; i < nr_irqs; i++) {
		struct kvm_vcpu *vcpu = vcpus[i % nr_vcpus];
		struct timespec start;

		kvm_route_msi(vm, gsi, vcpu, vector);

		if (irq_affinity && vcpu->id == 0) {
			irq_cpu = rand() % get_nprocs();

			ret = fprintf(irq_affinity_fp, "%d\n", irq_cpu);
			TEST_ASSERT(ret > 0, "Failed to affinitize IRQ-%d to CPU %d", irq, irq_cpu);
		}

		for (j = 0; j < nr_vcpus; j++) {
			TEST_ASSERT(
				!READ_FROM_GUEST(vm, guest_received_irq[vcpu->id]),
				"IRQ flag for vCPU %d not clear prior to test",
				vcpu->id);
		}

		send_msi(device, msi);

		clock_gettime(CLOCK_MONOTONIC, &start);
		for (;;) {
			if (READ_FROM_GUEST(vm, guest_received_irq[vcpu->id]))
				break;

			if (timespec_to_ns(timespec_elapsed(start)) > TIMEOUT_NS) {
				printf("Timeout waiting for interrupt!\n");
				printf("  vCPU: %d\n", vcpu->id);
				if (irq_affinity)
					printf("  irq_cpu: %d\n", irq_cpu);

				TEST_FAIL("vCPU never received IRQ!\n");
			}
		}

		WRITE_TO_GUEST(vm, guest_received_irq[vcpu->id], false);
	}

	WRITE_TO_GUEST(vm, done, true);

	for (i = 0; i < nr_vcpus; i++) {
		pthread_join(vcpu_threads[i], NULL);
	}

	if (irq_affinity)
		fclose(irq_affinity_fp);

	printf("Host interrupts handled:\n");
	printf("  IRQ-%d: %lu\n", irq, get_irq_count(irq) - irq_count);
	printf("  Posted-interrupt notification events: %lu\n",
	       get_irq_count_by_name("PIN:") - pin_count);
	printf("  Posted-interrupt wakeup events: %lu\n",
	       get_irq_count_by_name("PIW:") - piw_count);

	vfio_pci_device_cleanup(device);
	iommu_cleanup(iommu);

	return 0;
}
