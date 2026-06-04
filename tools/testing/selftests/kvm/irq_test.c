// SPDX-License-Identifier: GPL-2.0
#include "kvm_util.h"
#include "test_util.h"
#include <linux/sizes.h>
#include "apic.h"
#include "processor.h"
#include "proc_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/sysinfo.h>
#include <libvfio.h>

static u64 timeout_ns = 2ULL * 1000 * 1000 * 1000;
static bool guest_ready_for_irqs[KVM_MAX_VCPUS];
static bool guest_received_irq[KVM_MAX_VCPUS];
static bool irq_affinity;
static bool block_vcpus;
static bool done;

#define GUEST_RECEIVED_IRQ(__vcpu)	\
	SYNC_FROM_GUEST_AND_READ((__vcpu)->vm, guest_received_irq[(__vcpu)->id])

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
		if (block_vcpus)
			hlt();
		else
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

static int vfio_setup_msi(struct vfio_pci_device *device)
{
	const int flags = MAP_SHARED | MAP_ANONYMOUS;
	const int prot = PROT_READ | PROT_WRITE;
	struct dma_region *region;

	/* A driver is required to generate an MSI. */
	TEST_REQUIRE(device->driver.ops);

	/* Set up a DMA-able region for the driver to use. */
	region = &device->driver.region;
	region->iova = 0;
	region->size = SZ_2M;
	region->vaddr = kvm_mmap(region->size, prot, flags, -1);
	TEST_ASSERT(region->vaddr != MAP_FAILED, "mmap() failed\n");
	iommu_map(device->iommu, region);

	vfio_pci_driver_init(device);
	return device->driver.msi;
}

static void trigger_interrupt(struct vfio_pci_device *device, int eventfd)
{
	if (device)
		vfio_pci_driver_send_msi(device);
	else
		eventfd_write(eventfd, 1);
}


static void kvm_route_msi(struct kvm_vm *vm, u32 gsi, struct kvm_vcpu *vcpu,
			  u8 vector)
{
	struct {
		struct kvm_irq_routing head;
		struct kvm_irq_routing_entry entry;
	} routing_data = {};

	struct kvm_irq_routing *routes = &routing_data.head;

	routes->nr = 1;
	routes->entries[0].gsi = gsi;
	routes->entries[0].type = KVM_IRQ_ROUTING_MSI;
	routes->entries[0].u.msi.address_lo = 0xFEE00000 | (vcpu->id << 12);
	routes->entries[0].u.msi.data = vector;

	vm_ioctl(vm, KVM_SET_GSI_ROUTING, routes);
}

static void help(const char *name)
{
	printf("Usage: %s [-a] [-b] [-d <segment:bus:device.function>] [-h]\n", name);
	printf("\n");
	printf("Tests KVM IRQ injection via irqfd using an emulated eventfd.\n");
	printf("-a	Randomly affinitize the device's host IRQ to different physical CPUs throughout the test\n");
	printf("-b	Block vCPUs (e.g. HLT) instead of spinning in guest-mode\n");
	printf("-d	Use a VFIO device to send MSI-X interrupts instead of using an emulated eventfd\n");
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
	u32 gsi = kvm_random_u64_in_range(&kvm_rng, 24, KVM_MAX_IRQ_ROUTES - 1);
	u8 vector = kvm_random_u64_in_range(&kvm_rng, 32, UINT8_MAX);

	pthread_t vcpu_threads[KVM_MAX_VCPUS];
	struct kvm_vcpu *vcpus[KVM_MAX_VCPUS];
	struct vfio_pci_device *device = NULL;
	int nr_irqs = 1000, nr_vcpus = 1;
	const char *device_bdf = NULL;
	FILE *irq_affinity_fp = NULL;
	int i, j, c, msix, eventfd;
	struct iommu *iommu;
	struct kvm_vm *vm;
	unsigned int irq;
	int irq_cpu;

	while ((c = getopt(argc, argv, "abd:h")) != -1) {
		switch (c) {
		case 'a':
			irq_affinity = true;
			break;
		case 'b':
			block_vcpus = true;
			break;
		case 'd':
			device_bdf = optarg;
			break;
		case 'h':
		default:
			help(argv[0]);
		}
	}

	TEST_REQUIRE(kvm_arch_has_default_irqchip());

	vm = vm_create_with_vcpus(nr_vcpus, guest_code, vcpus);
	vm_install_exception_handler(vm, vector, guest_irq_handler);

	if (device_bdf) {
		iommu = iommu_init(default_iommu_mode);
		device = vfio_pci_device_init(device_bdf, iommu);
		msix = vfio_setup_msi(device);
		irq = vfio_msix_to_host_irq(device_bdf, msix);
		eventfd = device->msi_eventfds[msix];
		printf("Using device %s MSI-X[%d] (IRQ-%u)\n", device_bdf, msix,
		       irq);
		if (irq_affinity)
			irq_affinity_fp = open_proc_irq_smp_affinity_list(irq);
	} else {
		TEST_ASSERT(!irq_affinity,
			    "Setting IRQ affinity (-a) requires a backing device (-d)");
		eventfd = kvm_new_eventfd();
	}

	printf("Injecting interrupts for GSI %d (Vector 0x%x) %d times\n",
	       gsi, vector, nr_irqs);

	kvm_assign_irqfd(vm, gsi, eventfd);

	sync_global_to_guest(vm, block_vcpus);

	for (i = 0; i < nr_vcpus; i++)
		pthread_create(&vcpu_threads[i], NULL, vcpu_thread_main, vcpus[i]);

	for (i = 0; i < nr_vcpus; i++) {
		struct kvm_vcpu *vcpu = vcpus[i];

		while (!SYNC_FROM_GUEST_AND_READ(vm, guest_ready_for_irqs[vcpu->id]))
			continue;
	}

	/*
	 * Suppress a false positive maybe-uninitialized compiler warning due
	 * to conditional changes in IRQ affinity.
	 */
	irq_cpu = -1;

	for (i = 0; i < nr_irqs; i++) {
		struct kvm_vcpu *vcpu = vcpus[i % nr_vcpus];
		struct timespec start;

		kvm_route_msi(vm, gsi, vcpu, vector);

		if (irq_affinity_fp) {
			irq_cpu = kvm_random_u64(&kvm_rng) % get_nprocs();
			write_proc_irq_smp_affinity_list(irq_affinity_fp, irq, irq_cpu);
		}

		for (j = 0; j < nr_vcpus; j++)
			TEST_ASSERT(!GUEST_RECEIVED_IRQ(vcpus[j]),
				    "IRQ flag for vCPU %d not clear prior to test",
				    vcpus[j]->id);

		trigger_interrupt(device, eventfd);

		clock_gettime(CLOCK_MONOTONIC, &start);
		for (;;) {
			if (GUEST_RECEIVED_IRQ(vcpu))
				break;

			if (timespec_to_ns(timespec_elapsed(start)) > timeout_ns) {
				printf("Timeout waiting for interrupt!\n");
				printf("  vCPU: %d\n", vcpu->id);
				if (irq_affinity_fp) {
					printf("  irq_cpu: %d\n", irq_cpu);
					print_proc_irq_smp_affinity(irq);
					print_proc_irq_effective_affinity(irq);
				}

				TEST_FAIL("vCPU %d timed out waiting for IRQ from GSI %d (Vector 0x%x) !\n",
					  vcpu->id, gsi, vector);
			}
		}

		WRITE_AND_SYNC_TO_GUEST(vm, guest_received_irq[vcpu->id], false);
	}

	WRITE_AND_SYNC_TO_GUEST(vm, done, true);

	for (i = 0; i < nr_vcpus; i++) {
		/*
		 * Verify that sending an interrupt to a halted vCPU wakes it
		 * up. If the vCPU does not wake up, the call to pthread_join(),
		 * below, will hang.
		 */
		if (block_vcpus) {
			kvm_route_msi(vm, gsi, vcpus[i], vector);
			trigger_interrupt(device, eventfd);
		}

		pthread_join(vcpu_threads[i], NULL);
	}

	if (irq_affinity_fp)
		fclose(irq_affinity_fp);

	printf("Test passed!\n");

	return 0;
}
