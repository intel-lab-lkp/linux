// SPDX-License-Identifier: GPL-2.0-only
/*
 * Test code for the s390x kvm ucontrol interface
 *
 * Copyright IBM Corp. 2024
 *
 * Authors:
 *  Christoph Schlameuss <schlameuss@linux.ibm.com>
 */
#include "debug_print.h"
#include "kselftest_harness.h"
#include "kvm_util.h"
#include "processor.h"
#include "sie.h"

#include <linux/capability.h>
#include <linux/sizes.h>

#define PGM_SEGMENT_TRANSLATION 0x10

#define VM_MEM_SIZE (4 * SZ_1M)
#define VM_MEM_EXT_SIZE (2 * SZ_1M)
#define VM_MEM_MAX (VM_MEM_SIZE + VM_MEM_EXT_SIZE)

#define PAGES_PER_SEGMENT 4

/* so directly declare capget to check caps without libcap */
int capget(cap_user_header_t header, cap_user_data_t data);

/**
 * In order to create user controlled virtual machines on S390,
 * check KVM_CAP_S390_UCONTROL and use the flag KVM_VM_S390_UCONTROL
 * as privileged user (SYS_ADMIN).
 */
void require_ucontrol_admin(void)
{
	struct __user_cap_data_struct data[_LINUX_CAPABILITY_U32S_3];
	struct __user_cap_header_struct hdr = {
		.version = _LINUX_CAPABILITY_VERSION_3,
	};
	int rc;

	rc = capget(&hdr, data);
	TEST_ASSERT_EQ(0, rc);
	TEST_REQUIRE((data->effective & CAP_TO_MASK(CAP_SYS_ADMIN)) > 0);

	TEST_REQUIRE(kvm_has_cap(KVM_CAP_S390_UCONTROL));
}

/* Test program setting some registers and looping */
extern char test_gprs_asm[];
asm("test_gprs_asm:\n"
	"xgr	%r0, %r0\n"
	"lgfi	%r1,1\n"
	"lgfi	%r2,2\n"
	"lgfi	%r3,3\n"
	"lgfi	%r4,4\n"
	"lgfi	%r5,5\n"
	"lgfi	%r6,6\n"
	"lgfi	%r7,7\n"
	"0:\n"
	"	diag	0,0,0x44\n"
	"	ahi	%r0,1\n"
	"	j	0b\n"
);

/* Test program manipulating memory */
extern char test_mem_asm[];
asm("test_mem_asm:\n"
	"xgr	%r0, %r0\n"

	"0:\n"
	"	ahi	%r0,1\n"
	"	st	%r1,0(%r5,%r6)\n"

	"	xgr	%r1, %r1\n"
	"	l	%r1,0(%r5,%r6)\n"
	"	ahi	%r0,1\n"
	"	diag	0,0,0x44\n"

	"	j	0b\n"
);

/* Test program manipulating storage keys */
extern char test_skey_asm[];
asm("test_skey_asm:\n"
	"xgr	%r0, %r0\n"

	"0:\n"
	"	ahi	%r0,1\n"
	"	st	%r1,0(%r5,%r6)\n"

	"	iske	%r1,%r6\n"
	"	ahi	%r0,1\n"
	"	diag	0,0,0x44\n"

	"	sske	%r1,%r6\n"
	"	iske	%r1,%r6\n"
	"	ahi	%r0,1\n"
	"	diag	0,0,0x44\n"

	"	rrbe	%r1,%r6\n"
	"	iske	%r1,%r6\n"
	"	ahi	%r0,1\n"
	"	diag	0,0,0x44\n"

	"	j	0b\n"
);

FIXTURE(uc_kvm)
{
	struct kvm_s390_sie_block *sie_block;
	struct kvm_run *run;
	uintptr_t base_gpa;
	uintptr_t code_gpa;
	uintptr_t base_hva;
	uintptr_t code_hva;
	int kvm_run_size;
	vm_paddr_t pgd;
	void *vm_mem;
	int vcpu_fd;
	int kvm_fd;
	int vm_fd;
};

/**
 * create VM with single vcpu, map kvm_run and SIE control block for easy access
 */
FIXTURE_SETUP(uc_kvm)
{
	struct kvm_s390_vm_cpu_processor info;
	int rc;

	require_ucontrol_admin();

	self->kvm_fd = open_kvm_dev_path_or_exit();
	self->vm_fd = ioctl(self->kvm_fd, KVM_CREATE_VM, KVM_VM_S390_UCONTROL);
	ASSERT_GE(self->vm_fd, 0);

	kvm_device_attr_get(self->vm_fd, KVM_S390_VM_CPU_MODEL,
			    KVM_S390_VM_CPU_PROCESSOR, &info);
	TH_LOG("create VM 0x%llx", info.cpuid);

	self->vcpu_fd = ioctl(self->vm_fd, KVM_CREATE_VCPU, 0);
	ASSERT_GE(self->vcpu_fd, 0);

	self->kvm_run_size = ioctl(self->kvm_fd, KVM_GET_VCPU_MMAP_SIZE, NULL);
	ASSERT_GE(self->kvm_run_size, sizeof(struct kvm_run))
		  TH_LOG(KVM_IOCTL_ERROR(KVM_GET_VCPU_MMAP_SIZE, self->kvm_run_size));
	self->run = (struct kvm_run *)mmap(NULL, self->kvm_run_size,
		    PROT_READ | PROT_WRITE, MAP_SHARED, self->vcpu_fd, 0);
	ASSERT_NE(self->run, MAP_FAILED);
	/**
	 * For virtual cpus that have been created with S390 user controlled
	 * virtual machines, the resulting vcpu fd can be memory mapped at page
	 * offset KVM_S390_SIE_PAGE_OFFSET in order to obtain a memory map of
	 * the virtual cpu's hardware control block.
	 */
	self->sie_block = (struct kvm_s390_sie_block *)mmap(NULL, PAGE_SIZE,
			  PROT_READ | PROT_WRITE, MAP_SHARED,
			  self->vcpu_fd, KVM_S390_SIE_PAGE_OFFSET << PAGE_SHIFT);
	ASSERT_NE(self->sie_block, MAP_FAILED);

	TH_LOG("VM created %p %p", self->run, self->sie_block);

	self->base_gpa = 0;
	self->code_gpa = self->base_gpa + (3 * SZ_1M);

	self->vm_mem = aligned_alloc(SZ_1M, VM_MEM_SIZE + VM_MEM_EXT_SIZE);
	ASSERT_NE(NULL, self->vm_mem) TH_LOG("malloc failed %u", errno);
	self->base_hva = (uintptr_t)self->vm_mem;
	self->code_hva = self->base_hva - self->base_gpa + self->code_gpa;
	struct kvm_s390_ucas_mapping map = {
		.user_addr = self->base_hva,
		.vcpu_addr = self->base_gpa,
		.length = VM_MEM_SIZE,
	};
	TH_LOG("ucas map %p %p 0x%llx",
	       (void *)map.user_addr, (void *)map.vcpu_addr, map.length);
	rc = ioctl(self->vcpu_fd, KVM_S390_UCAS_MAP, &map);
	ASSERT_EQ(0, rc) TH_LOG("ucas map result %d not expected, %s",
				rc, strerror(errno));

	TH_LOG("page in %p", (void *)self->base_gpa);
	rc = ioctl(self->vcpu_fd, KVM_S390_VCPU_FAULT, self->base_gpa);
	ASSERT_EQ(0, rc) TH_LOG("vcpu fault (%p) result %d not expected, %s",
				(void *)self->base_hva, rc, strerror(errno));

	self->sie_block->cpuflags &= ~CPUSTAT_STOPPED;
}

FIXTURE_TEARDOWN(uc_kvm)
{
	munmap(self->sie_block, PAGE_SIZE);
	munmap(self->run, self->kvm_run_size);
	close(self->vcpu_fd);
	close(self->vm_fd);
	close(self->kvm_fd);
	free(self->vm_mem);
}

TEST_F(uc_kvm, uc_sie_assertions)
{
	/* assert interception of Code 08 (Program Interruption) is set */
	EXPECT_EQ(0, self->sie_block->ecb & ECB_SPECI);
}

TEST_F(uc_kvm, uc_attr_mem_limit)
{
	u64 limit;
	struct kvm_device_attr attr = {
		.group = KVM_S390_VM_MEM_CTRL,
		.attr = KVM_S390_VM_MEM_LIMIT_SIZE,
		.addr = (unsigned long)&limit,
	};
	int rc;

	rc = ioctl(self->vm_fd, KVM_GET_DEVICE_ATTR, &attr);
	EXPECT_EQ(0, rc);
	EXPECT_EQ(~0UL, limit);

	/* assert set not supported */
	rc = ioctl(self->vm_fd, KVM_SET_DEVICE_ATTR, &attr);
	EXPECT_EQ(-1, rc);
	EXPECT_EQ(EINVAL, errno);
}

TEST_F(uc_kvm, uc_no_dirty_log)
{
	struct kvm_dirty_log dlog;
	int rc;

	rc = ioctl(self->vm_fd, KVM_GET_DIRTY_LOG, &dlog);
	EXPECT_EQ(-1, rc);
	EXPECT_EQ(EINVAL, errno);
}

/**
 * Assert HPAGE CAP cannot be enabled on UCONTROL VM
 */
TEST(uc_cap_hpage)
{
	int rc, kvm_fd, vm_fd, vcpu_fd;
	struct kvm_enable_cap cap = {
		.cap = KVM_CAP_S390_HPAGE_1M,
	};

	require_ucontrol_admin();

	kvm_fd = open_kvm_dev_path_or_exit();
	vm_fd = ioctl(kvm_fd, KVM_CREATE_VM, KVM_VM_S390_UCONTROL);
	ASSERT_GE(vm_fd, 0);

	/* assert hpages are not supported on ucontrol vm */
	rc = ioctl(vm_fd, KVM_CHECK_EXTENSION, KVM_CAP_S390_HPAGE_1M);
	EXPECT_EQ(0, rc);

	/* Test that KVM_CAP_S390_HPAGE_1M can't be enabled for a ucontrol vm */
	rc = ioctl(vm_fd, KVM_ENABLE_CAP, cap);
	EXPECT_EQ(-1, rc);
	EXPECT_EQ(EINVAL, errno);

	/* assert HPAGE CAP is rejected after vCPU creation */
	vcpu_fd = ioctl(vm_fd, KVM_CREATE_VCPU, 0);
	ASSERT_GE(vcpu_fd, 0);
	rc = ioctl(vm_fd, KVM_ENABLE_CAP, cap);
	EXPECT_EQ(-1, rc);
	EXPECT_EQ(EBUSY, errno);

	close(vcpu_fd);
	close(vm_fd);
	close(kvm_fd);
}

/* calculate host virtual addr from guest physical addr */
static void *gpa2hva(FIXTURE_DATA(uc_kvm) * self, u64 gpa)
{
	return (void *)(self->base_hva - self->base_gpa + gpa);
}

/* initialize segment and page tables for uc_kvm tests */
static void init_st_pt(FIXTURE_DATA(uc_kvm) * self)
{
	struct kvm_sync_regs *sync_regs = &self->run->s.regs;
	struct kvm_run *run = self->run;
	void *se_addr;
	int si, pi;
	u64 *phd;

	/* set PASCE addr */
	self->pgd = self->base_gpa + SZ_1M;
	phd = gpa2hva(self, self->pgd);
	memset(phd, 0xff, PAGES_PER_SEGMENT * PAGE_SIZE);

	for (si = 0; si < ((VM_MEM_SIZE + VM_MEM_EXT_SIZE) / SZ_1M); si++) {
		/* create ste */
		phd[si] = (self->pgd
			+ (PAGES_PER_SEGMENT * PAGE_SIZE
				* ((VM_MEM_SIZE + VM_MEM_EXT_SIZE) / SZ_1M))
			+ (PAGES_PER_SEGMENT * PAGE_SIZE * si)) & ~0x7fful;
		se_addr = gpa2hva(self, phd[si]);
		memset(se_addr, 0xff, PAGES_PER_SEGMENT * PAGE_SIZE);
		for (pi = 0; pi < (SZ_1M / PAGE_SIZE); pi++) {
			/* create pte */
			((u64 *)se_addr)[pi] = (self->base_gpa
				+ (si * SZ_1M) + (pi * PAGE_SIZE)) & ~0xffful;
		}
	}
	pr_debug("segment table entry %p (0x%lx) --> %p\n",
		 phd, phd[0], gpa2hva(self, (phd[0] & ~0x7fful)));
	print_hex_bytes("st", (u64)phd, 64);
	print_hex_bytes("pt", (u64)gpa2hva(self, phd[0]), 128);
	print_hex_bytes("pt+", (u64)
			gpa2hva(self, phd[0] + (PAGES_PER_SEGMENT * PAGE_SIZE
			* ((VM_MEM_SIZE + VM_MEM_EXT_SIZE) / SZ_1M)) - 0x64), 128);

	/* PASCE TT=00 for segment table */
	sync_regs->crs[1] = self->pgd | 0x3;
	run->kvm_dirty_regs |= KVM_SYNC_CRS;
}

static void uc_handle_exit_ucontrol(FIXTURE_DATA(uc_kvm) * self)
{
	struct kvm_run *run = self->run;

	TEST_ASSERT_EQ(KVM_EXIT_S390_UCONTROL, run->exit_reason);
	switch (run->s390_ucontrol.pgm_code) {
	case PGM_SEGMENT_TRANSLATION:
		pr_info("ucontrol pic segment translation 0x%llx\n",
			run->s390_ucontrol.trans_exc_code);
		/* map / make additional memory available */
		struct kvm_s390_ucas_mapping map2 = {
			.user_addr = (u64)gpa2hva(self, run->s390_ucontrol.trans_exc_code),
			.vcpu_addr = run->s390_ucontrol.trans_exc_code,
			.length = VM_MEM_EXT_SIZE,
		};
		pr_info("ucas map %p %p 0x%llx\n",
			(void *)map2.user_addr, (void *)map2.vcpu_addr, map2.length);
		TEST_ASSERT_EQ(0, ioctl(self->vcpu_fd, KVM_S390_UCAS_MAP, &map2));
		break;
	default:
		TEST_FAIL("UNEXPECTED PGM CODE %d", run->s390_ucontrol.pgm_code);
	}
}

/* verify SIEIC exit
 * * reset stop requests
 * * fail on codes not expected in the test cases
 */
static bool uc_handle_sieic(FIXTURE_DATA(uc_kvm) * self)
{
	struct kvm_s390_sie_block *sie_block = self->sie_block;
	struct kvm_run *run = self->run;

	/* check SIE interception code */
	pr_info("sieic: 0x%.2x 0x%.4x 0x%.4x\n",
		run->s390_sieic.icptcode,
		run->s390_sieic.ipa,
		run->s390_sieic.ipb);
	switch (run->s390_sieic.icptcode) {
	case ICPT_INST:
		/* end execution in caller on intercepted instruction */
		pr_info("sie instruction interception\n");
		return false;
	case ICPT_OPEREXC:
		/* operation exception */
		TEST_FAIL("sie exception on %.4x%.8x", sie_block->ipa, sie_block->ipb);
	default:
		TEST_FAIL("UNEXPECTED SIEIC CODE %d", run->s390_sieic.icptcode);
	}
	return true;
}

/* verify VM state on exit */
static bool uc_handle_exit(FIXTURE_DATA(uc_kvm) * self)
{
	struct kvm_run *run = self->run;

	switch (run->exit_reason) {
	case KVM_EXIT_S390_UCONTROL:
		/** check program interruption code
		 * handle page fault --> ucas map
		 */
		uc_handle_exit_ucontrol(self);
		break;
	case KVM_EXIT_S390_SIEIC:
		return uc_handle_sieic(self);
	default:
		pr_info("exit_reason %2d not handled\n", run->exit_reason);
	}
	return true;
}

/* run the VM until interrupted */
static int uc_run_once(FIXTURE_DATA(uc_kvm) * self)
{
	int rc;

	rc = ioctl(self->vcpu_fd, KVM_RUN, NULL);
	print_run(self->run, self->sie_block);
	print_regs(self->run);
	pr_debug("run %d / %d %s\n", rc, errno, strerror(errno));
	return rc;
}

static void uc_assert_diag44(FIXTURE_DATA(uc_kvm) * self)
{
	struct kvm_s390_sie_block *sie_block = self->sie_block;

	/* assert vm was interrupted by diag 0x0044 */
	TEST_ASSERT_EQ(KVM_EXIT_S390_SIEIC, self->run->exit_reason);
	TEST_ASSERT_EQ(ICPT_INST, sie_block->icptcode);
	TEST_ASSERT_EQ(0x8300, sie_block->ipa);
	TEST_ASSERT_EQ(0x440000, sie_block->ipb);
}

TEST_F(uc_kvm, uc_skey)
{
	u64 test_vaddr = self->base_gpa + VM_MEM_SIZE - (SZ_1M / 2);
	struct kvm_sync_regs *sync_regs = &self->run->s.regs;
	struct kvm_run *run = self->run;
	u8 skeyvalue = 0x34;

	init_st_pt(self);

	/* copy test_skey_asm to code_hva / code_gpa */
	TH_LOG("copy code %p to vm mapped memory %p / %p",
	       &test_skey_asm, (void *)self->code_hva, (void *)self->code_gpa);
	memcpy((void *)self->code_hva, &test_skey_asm, PAGE_SIZE);

	/* set register content for test_skey_asm to access not mapped memory */
	sync_regs->gprs[1] = skeyvalue;
	sync_regs->gprs[5] = self->base_gpa;
	sync_regs->gprs[6] = test_vaddr;
	run->kvm_dirty_regs |= KVM_SYNC_GPRS;

	run->kvm_dirty_regs |= KVM_SYNC_CRS;
	TH_LOG("set CR0 to 0x%llx", sync_regs->crs[0]);

	self->sie_block->ictl |= ICTL_OPEREXC | ICTL_PINT;
	self->sie_block->cpuflags &= ~CPUSTAT_KSS;
	/* DAT enabled + 64 bit mode */
	run->psw_mask = 0x0400000180000000ULL;
	run->psw_addr = self->code_gpa;

	ASSERT_EQ(0, uc_run_once(self));
	ASSERT_EQ(false, uc_handle_exit(self));
	ASSERT_EQ(2, sync_regs->gprs[0]);
	ASSERT_EQ(0x06, sync_regs->gprs[1]);
	uc_assert_diag44(self);

	sync_regs->gprs[1] = skeyvalue;
	run->kvm_dirty_regs |= KVM_SYNC_GPRS;
	ASSERT_EQ(0, uc_run_once(self));
	ASSERT_EQ(false, uc_handle_exit(self));
	ASSERT_EQ(3, sync_regs->gprs[0]);
	ASSERT_EQ(skeyvalue, sync_regs->gprs[1]);
	uc_assert_diag44(self);

	sync_regs->gprs[1] = skeyvalue;
	run->kvm_dirty_regs |= KVM_SYNC_GPRS;
	ASSERT_EQ(0, uc_run_once(self));
	ASSERT_EQ(false, uc_handle_exit(self));
	ASSERT_EQ(4, sync_regs->gprs[0]);
	ASSERT_EQ(skeyvalue & 0xfb, sync_regs->gprs[1]);
	uc_assert_diag44(self);
}

TEST_F(uc_kvm, uc_map_unmap)
{
	struct kvm_sync_regs *sync_regs = &self->run->s.regs;
	struct kvm_run *run = self->run;
	int rc;

	init_st_pt(self);

	/* copy test_mem_asm to code_hva / code_gpa */
	TH_LOG("copy code %p to vm mapped memory %p / %p",
	       &test_mem_asm, (void *)self->code_hva, (void *)self->code_gpa);
	memcpy((void *)self->code_hva, &test_mem_asm, PAGE_SIZE);

	/* DAT enabled + 64 bit mode */
	run->psw_mask = 0x0400000180000000ULL;
	run->psw_addr = self->code_gpa;

	/* set register content for test_mem_asm to access not mapped memory*/
	sync_regs->gprs[1] = 0x55;
	sync_regs->gprs[5] = self->base_gpa;
	sync_regs->gprs[6] = VM_MEM_SIZE;
	run->kvm_dirty_regs |= KVM_SYNC_GPRS;

	/* run and expect to fail witch ucontrol pic segment translation */
	ASSERT_EQ(0, uc_run_once(self));
	ASSERT_EQ(1, sync_regs->gprs[0]);
	ASSERT_EQ(KVM_EXIT_S390_UCONTROL, run->exit_reason);

	ASSERT_EQ(PGM_SEGMENT_TRANSLATION, run->s390_ucontrol.pgm_code);
	ASSERT_EQ(self->base_gpa + VM_MEM_SIZE, run->s390_ucontrol.trans_exc_code);
	/* map / make additional memory available */
	struct kvm_s390_ucas_mapping map2 = {
		.user_addr = (u64)gpa2hva(self, self->base_gpa + VM_MEM_SIZE),
		.vcpu_addr = self->base_gpa + VM_MEM_SIZE,
		.length = VM_MEM_EXT_SIZE,
	};
	TH_LOG("ucas map %p %p 0x%llx",
	       (void *)map2.user_addr, (void *)map2.vcpu_addr, map2.length);
	rc = ioctl(self->vcpu_fd, KVM_S390_UCAS_MAP, &map2);
	ASSERT_EQ(0, rc)
		TH_LOG("ucas map result %d not expected, %s", rc, strerror(errno));
	ASSERT_EQ(0, uc_run_once(self));
	ASSERT_EQ(false, uc_handle_exit(self));
	uc_assert_diag44(self);

	/* assert registers and memory are in expected state */
	ASSERT_EQ(2, sync_regs->gprs[0]);
	ASSERT_EQ(0x55, sync_regs->gprs[1]);
	ASSERT_EQ(0x55, *(u32 *)gpa2hva(self, self->base_gpa + VM_MEM_SIZE));

	/* unmap and run loop again */
	TH_LOG("ucas unmap %p %p 0x%llx",
	       (void *)map2.user_addr, (void *)map2.vcpu_addr, map2.length);
	rc = ioctl(self->vcpu_fd, KVM_S390_UCAS_UNMAP, &map2);
	ASSERT_EQ(0, rc)
		TH_LOG("ucas map result %d not expected, %s", rc, strerror(errno));
	ASSERT_EQ(0, uc_run_once(self));
	ASSERT_EQ(3, sync_regs->gprs[0]);
	ASSERT_EQ(KVM_EXIT_S390_UCONTROL, run->exit_reason);
	ASSERT_EQ(true, uc_handle_exit(self));
}

TEST_F(uc_kvm, uc_gprs)
{
	struct kvm_sync_regs *sync_regs = &self->run->s.regs;
	struct kvm_run *run = self->run;
	struct kvm_regs regs = {};

	/* Set registers to values that are different from the ones that we expect below */
	for (int i = 0; i < 8; i++)
		sync_regs->gprs[i] = 8;
	run->kvm_dirty_regs |= KVM_SYNC_GPRS;

	/* copy test_gprs_asm to code_hva / code_gpa */
	TH_LOG("copy code %p to vm mapped memory %p / %p",
	       &test_gprs_asm, (void *)self->code_hva, (void *)self->code_gpa);
	memcpy((void *)self->code_hva, &test_gprs_asm, PAGE_SIZE);

	/* DAT disabled + 64 bit mode */
	run->psw_mask = 0x0000000180000000ULL;
	run->psw_addr = self->code_gpa;

	/* run and expect interception of diag 44 */
	ASSERT_EQ(0, uc_run_once(self));
	ASSERT_EQ(false, uc_handle_exit(self));
	uc_assert_diag44(self);

	/* Retrieve and check guest register values */
	ASSERT_EQ(0, ioctl(self->vcpu_fd, KVM_GET_REGS, &regs));
	for (int i = 0; i < 8; i++) {
		ASSERT_EQ(i, regs.gprs[i]);
		ASSERT_EQ(i, sync_regs->gprs[i]);
	}

	/* run and expect interception of diag 44 again */
	ASSERT_EQ(0, uc_run_once(self));
	ASSERT_EQ(false, uc_handle_exit(self));
	uc_assert_diag44(self);

	/* check continued increment of register 0 value */
	ASSERT_EQ(0, ioctl(self->vcpu_fd, KVM_GET_REGS, &regs));
	ASSERT_EQ(1, regs.gprs[0]);
	ASSERT_EQ(1, sync_regs->gprs[0]);
}

TEST_HARNESS_MAIN
