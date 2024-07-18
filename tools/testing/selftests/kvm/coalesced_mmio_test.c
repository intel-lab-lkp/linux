// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2024 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Test the KVM_CREATE_COALESCED_MMIO_BUFFER, KVM_REGISTER_COALESCED_MMIO2 and
 * KVM_UNREGISTER_COALESCED_MMIO2 ioctls by making sure that MMIO writes to
 * associated zones end up in the correct ring buffer. Also test that we don't
 * exit to userspace when there is space in the corresponding buffer.
 */

#include <kvm_util.h>
#include <ucall_common.h>

#define PAGE_SIZE 4096

/*
 * Somewhat arbitrary location and slot, intended to not overlap anything.
 */
#define MEM_REGION_SLOT         10
#define MEM_REGION_GPA          0xc0000000UL
#define MEM_REGION_SIZE         (PAGE_SIZE * 2)
#define MEM_REGION_PAGES        DIV_ROUND_UP(MEM_REGION_SIZE, PAGE_SIZE)

#define COALESCING_ZONE1_GPA    MEM_REGION_GPA
#define COALESCING_ZONE1_SIZE   PAGE_SIZE
#define COALESCING_ZONE2_GPA    (COALESCING_ZONE1_GPA + COALESCING_ZONE1_SIZE)
#define COALESCING_ZONE2_SIZE   PAGE_SIZE

#define MMIO_WRITE_DATA         0xbadbeef
#define MMIO_WRITE_DATA2        0xbadc0de

#define BATCH_SIZE              4

static void guest_code(void)
{
	uint64_t *gpa;

	/*
	 * The first write should result in an exit
	 */
	gpa = (uint64_t *)(MEM_REGION_GPA);
	WRITE_ONCE(*gpa, MMIO_WRITE_DATA);

	/*
	 * These writes should be stored in a coalescing ring buffer and only
	 * the last one should result in an exit.
	 */
	for (int i = 0; i < KVM_COALESCED_MMIO_MAX; i++) {
		gpa = (uint64_t *)(COALESCING_ZONE1_GPA + i * sizeof(*gpa));
		WRITE_ONCE(*gpa, MMIO_WRITE_DATA + i);

		/* Let's throw a PIO into the mix */
		if (i == KVM_COALESCED_MMIO_MAX / 2)
			GUEST_SYNC(0);
	}

	/*
	 * These writes should be stored in two separate ring buffers and they
	 * shouldn't result in an exit.
	 */
	for (int i = 0; i < BATCH_SIZE; i++) {
		gpa = (uint64_t *)(COALESCING_ZONE1_GPA + i * sizeof(*gpa));
		WRITE_ONCE(*gpa, MMIO_WRITE_DATA + i);

		gpa = (uint64_t *)(COALESCING_ZONE2_GPA + i * sizeof(*gpa));
		WRITE_ONCE(*gpa, MMIO_WRITE_DATA2 + i);
	}

	GUEST_SYNC(0);

	/*
	 * These writes should be stored in the same ring buffer and they
	 * shouldn't result in an exit.
	 */
	for (int i = 0; i < BATCH_SIZE; i++) {
		if (i < BATCH_SIZE / 2)
			gpa = (uint64_t *)(COALESCING_ZONE1_GPA + i * sizeof(*gpa));
		else
			gpa = (uint64_t *)(COALESCING_ZONE2_GPA + i * sizeof(*gpa));

		WRITE_ONCE(*gpa, MMIO_WRITE_DATA2 + i);
	}

	GUEST_SYNC(0);

	/*
	 * This last write should result in an exit because the host should
	 * have disabled I/O coalescing by now.
	 */
	gpa = (uint64_t *)(COALESCING_ZONE1_GPA);
	WRITE_ONCE(*gpa, MMIO_WRITE_DATA);
}

static void assert_mmio_write(struct kvm_vcpu *vcpu, uint64_t addr, uint64_t value)
{
	uint64_t data;

	TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_MMIO);
	TEST_ASSERT(vcpu->run->mmio.is_write, "Got MMIO read, not MMIO write");

	memcpy(&data, vcpu->run->mmio.data, vcpu->run->mmio.len);
	TEST_ASSERT_EQ(vcpu->run->mmio.phys_addr, addr);
	TEST_ASSERT_EQ(value, data);
}

static void assert_ucall_exit(struct kvm_vcpu *vcpu, uint64_t command)
{
	uint64_t cmd;
	struct ucall uc;

	TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);
	cmd = get_ucall(vcpu, &uc);
	TEST_ASSERT_EQ(cmd, command);
}

static void assert_ring_entries(struct kvm_coalesced_mmio_ring *ring,
				uint32_t nentries,
				uint64_t addr,
				uint64_t value)
{
	uint64_t data;

	for (int i = READ_ONCE(ring->first); i < nentries; i++) {
		TEST_ASSERT_EQ(READ_ONCE(ring->coalesced_mmio[i].len),
			       sizeof(data));
		memcpy(&data, ring->coalesced_mmio[i].data,
		       READ_ONCE(ring->coalesced_mmio[i].len));

		TEST_ASSERT_EQ(READ_ONCE(ring->coalesced_mmio[i].phys_addr),
			       addr + i * sizeof(data));
		TEST_ASSERT_EQ(data, value + i);
	}
}

int main(int argc, char *argv[])
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	uint64_t gpa;
	struct kvm_coalesced_mmio_ring *ring, *ring2;
	struct kvm_coalesced_mmio_zone2 zone, zone2;
	int ring_fd, ring_fd2;
	int r;

	TEST_REQUIRE(kvm_has_cap(KVM_CAP_COALESCED_MMIO2));
	TEST_REQUIRE(kvm_has_cap(KVM_CAP_READONLY_MEM));
	TEST_ASSERT(BATCH_SIZE * 2 <= KVM_COALESCED_MMIO_MAX,
		"KVM_COALESCED_MMIO_MAX too small");

	vm = vm_create_with_one_vcpu(&vcpu, guest_code);

	vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS, MEM_REGION_GPA,
				    MEM_REGION_SLOT, MEM_REGION_PAGES,
				    KVM_MEM_READONLY);

	gpa = vm_phy_pages_alloc(vm, MEM_REGION_PAGES, MEM_REGION_GPA,
				 MEM_REGION_SLOT);
	TEST_ASSERT(gpa == MEM_REGION_GPA, "Failed vm_phy_pages_alloc");

	virt_map(vm, MEM_REGION_GPA, MEM_REGION_GPA, MEM_REGION_PAGES);

	/*
	 * Test that allocating an fd and memory mapping it works
	 */
	ring_fd = __vm_ioctl(vm, KVM_CREATE_COALESCED_MMIO_BUFFER, NULL);
	TEST_ASSERT(ring_fd != -1, "Failed KVM_CREATE_COALESCED_MMIO_BUFFER");

	ring = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		    ring_fd, 0);
	TEST_ASSERT(ring != MAP_FAILED, "Failed to allocate ring buffer");

	/*
	 * Test that trying to map the same fd again fails
	 */
	ring2 = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		     ring_fd, 0);
	TEST_ASSERT(ring2 == MAP_FAILED && errno == EBUSY,
		    "Mapping the same fd again should fail with EBUSY");

	/*
	 * Test that the first and last ring indices are zero
	 */
	TEST_ASSERT_EQ(READ_ONCE(ring->first), 0);
	TEST_ASSERT_EQ(READ_ONCE(ring->last), 0);

	/*
	 * Run the vCPU and make sure the first MMIO write results in a
	 * userspace exit since we have not setup MMIO coalescing yet.
	 */
	vcpu_run(vcpu);
	assert_mmio_write(vcpu, MEM_REGION_GPA, MMIO_WRITE_DATA);

	/*
	 * Let's actually setup MMIO coalescing now...
	 */
	zone.addr = COALESCING_ZONE1_GPA;
	zone.size = COALESCING_ZONE1_SIZE;
	zone.buffer_fd = ring_fd;
	r = __vm_ioctl(vm, KVM_REGISTER_COALESCED_MMIO2, &zone);
	TEST_ASSERT(r != -1, "Failed KVM_REGISTER_COALESCED_MMIO2");

	/*
	 * The guest will start doing MMIO writes in the coalesced regions but
	 * will also do a ucall when the buffer is half full. The first
	 * userspace exit should be due to the ucall and not an MMIO exit.
	 */
	vcpu_run(vcpu);
	assert_ucall_exit(vcpu, UCALL_SYNC);
	TEST_ASSERT_EQ(READ_ONCE(ring->first), 0);
	TEST_ASSERT_EQ(READ_ONCE(ring->last), KVM_COALESCED_MMIO_MAX / 2 + 1);

	/*
	 * Run the guest again. Next exit should be when the buffer is full.
	 * One entry always remains unused.
	 */
	vcpu_run(vcpu);
	assert_mmio_write(vcpu,
	    COALESCING_ZONE1_GPA + (KVM_COALESCED_MMIO_MAX - 1) * sizeof(uint64_t),
	    MMIO_WRITE_DATA + KVM_COALESCED_MMIO_MAX - 1);
	TEST_ASSERT_EQ(READ_ONCE(ring->first), 0);
	TEST_ASSERT_EQ(READ_ONCE(ring->last), KVM_COALESCED_MMIO_MAX - 1);

	assert_ring_entries(ring, KVM_COALESCED_MMIO_MAX - 1,
			    COALESCING_ZONE1_GPA, MMIO_WRITE_DATA);

	/*
	 * Let's setup another region with a separate buffer
	 */
	ring_fd2 = __vm_ioctl(vm, KVM_CREATE_COALESCED_MMIO_BUFFER, NULL);
	TEST_ASSERT(ring_fd != -1, "Failed KVM_CREATE_COALESCED_MMIO_BUFFER");

	ring2 = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		     ring_fd2, 0);
	TEST_ASSERT(ring2 != MAP_FAILED, "Failed to allocate ring buffer");

	zone2.addr = COALESCING_ZONE2_GPA;
	zone2.size = COALESCING_ZONE2_SIZE;
	zone2.buffer_fd = ring_fd2;
	r = __vm_ioctl(vm, KVM_REGISTER_COALESCED_MMIO2, &zone2);
	TEST_ASSERT(r != -1, "Failed KVM_REGISTER_COALESCED_MMIO2");

	/*
	 * Move the consumer pointer of the first ring forward.
	 *
	 * When re-entering the vCPU the guest will write BATCH_SIZE
	 * times to each MMIO zone.
	 */
	WRITE_ONCE(ring->first,
		(READ_ONCE(ring->first) + BATCH_SIZE) % KVM_COALESCED_MMIO_MAX);

	vcpu_run(vcpu);
	assert_ucall_exit(vcpu, UCALL_SYNC);

	TEST_ASSERT_EQ(READ_ONCE(ring->first), BATCH_SIZE);
	TEST_ASSERT_EQ(READ_ONCE(ring->last),
	  (KVM_COALESCED_MMIO_MAX - 1 + BATCH_SIZE) % KVM_COALESCED_MMIO_MAX);
	TEST_ASSERT_EQ(READ_ONCE(ring2->first), 0);
	TEST_ASSERT_EQ(READ_ONCE(ring2->last), BATCH_SIZE);

	assert_ring_entries(ring, BATCH_SIZE, COALESCING_ZONE1_GPA, MMIO_WRITE_DATA);
	assert_ring_entries(ring2, BATCH_SIZE, COALESCING_ZONE2_GPA, MMIO_WRITE_DATA2);

	/*
	 * Unregister zone 2 and register it again but this time use the same
	 * ring buffer used for zone 1.
	 */
	r = __vm_ioctl(vm, KVM_UNREGISTER_COALESCED_MMIO2, &zone2);
	TEST_ASSERT(r != -1, "Failed KVM_UNREGISTER_COALESCED_MMIO2");

	zone2.buffer_fd = ring_fd;
	r = __vm_ioctl(vm, KVM_REGISTER_COALESCED_MMIO2, &zone2);
	TEST_ASSERT(r != -1, "Failed KVM_REGISTER_COALESCED_MMIO2");

	/*
	 * Enter the vCPU again. This time writes to both regions should go
	 * to the same ring buffer.
	 */
	WRITE_ONCE(ring->first,
		(READ_ONCE(ring->first) + BATCH_SIZE) % KVM_COALESCED_MMIO_MAX);

	vcpu_run(vcpu);
	assert_ucall_exit(vcpu, UCALL_SYNC);

	TEST_ASSERT_EQ(READ_ONCE(ring->first), BATCH_SIZE * 2);
	TEST_ASSERT_EQ(READ_ONCE(ring->last),
	  (KVM_COALESCED_MMIO_MAX - 1 + BATCH_SIZE * 2) % KVM_COALESCED_MMIO_MAX);

	WRITE_ONCE(ring->first,
		(READ_ONCE(ring->first) + BATCH_SIZE) % KVM_COALESCED_MMIO_MAX);

	/*
	 * Test that munmap and close work.
	 */
	r = munmap(ring, PAGE_SIZE);
	TEST_ASSERT(r == 0, "Failed to munmap()");
	r = close(ring_fd);
	TEST_ASSERT(r == 0, "Failed to close()");

	r = munmap(ring2, PAGE_SIZE);
	TEST_ASSERT(r == 0, "Failed to munmap()");
	r = close(ring_fd2);
	TEST_ASSERT(r == 0, "Failed to close()");

	/*
	 * close() should have also deregistered all I/O regions associated
	 * with the ring buffer automatically. Make sure that when the guest
	 * writes to the region again this results in an immediate exit.
	 */
	vcpu_run(vcpu);
	assert_mmio_write(vcpu, COALESCING_ZONE1_GPA, MMIO_WRITE_DATA);

	return 0;
}
