// SPDX-License-Identifier: GPL-2.0
#include "kvm_util.h"
#include "test_util.h"
#include "apic.h"
#include "processor.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/eventfd.h>

static u64 timeout_ns = 2ULL * 1000 * 1000 * 1000;
static bool guest_ready_for_irqs[KVM_MAX_VCPUS];
static bool guest_received_irq[KVM_MAX_VCPUS];
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

	while (!READ_ONCE(done))
		cpu_relax();

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
	struct {
		struct kvm_irq_routing header;
		struct kvm_irq_routing_entry entry;
	} routing = {
		.header.nr = 1,
		.entry = {
			.gsi = gsi,
			.type = KVM_IRQ_ROUTING_MSI,
			.u.msi.address_lo = 0xFEE00000 | (vcpu->id << 12),
			.u.msi.data = vector,
		},
	};

	vm_ioctl(vm, KVM_SET_GSI_ROUTING, &routing.header);
}

static void help(const char *name)
{
	printf("Usage: %s [-h]\n", name);
	printf("\n");
	printf("Tests KVM interrupt routing and delivery via irqfd.\n");
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

	struct kvm_vcpu *vcpus[KVM_MAX_VCPUS];
	pthread_t vcpu_threads[KVM_MAX_VCPUS];
	int nr_irqs = 1000, nr_vcpus = 1;
	int i, j, c, eventfd;
	struct kvm_vm *vm;

	while ((c = getopt(argc, argv, "h")) != -1) {
		switch (c) {
		case 'h':
		default:
			help(argv[0]);
		}
	}

	TEST_REQUIRE(kvm_arch_has_default_irqchip());

	vm = vm_create_with_vcpus(nr_vcpus, guest_code, vcpus);
	vm_install_exception_handler(vm, vector, guest_irq_handler);

	eventfd = kvm_new_eventfd();

	pr_info("Injecting interrupts for GSI %d (guest vector 0x%x) %d times\n",
		gsi, vector, nr_irqs);

	kvm_assign_irqfd(vm, gsi, eventfd);

	for (i = 0; i < nr_vcpus; i++)
		pthread_create(&vcpu_threads[i], NULL, vcpu_thread_main, vcpus[i]);

	for (i = 0; i < nr_vcpus; i++) {
		struct kvm_vcpu *vcpu = vcpus[i];

		while (!SYNC_FROM_GUEST_AND_READ(vm, guest_ready_for_irqs[vcpu->id]))
			continue;
	}

	for (i = 0; i < nr_irqs; i++) {
		struct kvm_vcpu *vcpu = vcpus[i % nr_vcpus];
		struct timespec start;

		kvm_route_msi(vm, gsi, vcpu, vector);

		for (j = 0; j < nr_vcpus; j++)
			TEST_ASSERT(!GUEST_RECEIVED_IRQ(vcpus[j]),
				    "IRQ flag for vCPU %d not clear prior to test",
				    vcpus[j]->id);

		eventfd_write(eventfd, 1);

		clock_gettime(CLOCK_MONOTONIC, &start);
		while (!GUEST_RECEIVED_IRQ(vcpu) &&
		       timespec_to_ns(timespec_elapsed(start)) <= timeout_ns)
			cpu_relax();

		TEST_ASSERT(GUEST_RECEIVED_IRQ(vcpu),
			    "vCPU %d timed out waiting for IRQ (vector 0x%x) from GSI %d\n",
			    vcpu->id, vector, gsi);

		WRITE_AND_SYNC_TO_GUEST(vm, guest_received_irq[vcpu->id], false);
	}

	WRITE_AND_SYNC_TO_GUEST(vm, done, true);

	for (i = 0; i < nr_vcpus; i++)
		pthread_join(vcpu_threads[i], NULL);

	return 0;
}
