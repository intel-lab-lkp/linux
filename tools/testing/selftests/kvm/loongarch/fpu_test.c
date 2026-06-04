// SPDX-License-Identifier: GPL-2.0

#include <stdio.h>
#include <string.h>
#include "kvm_util.h"
#include "processor.h"
#include "loongarch/processor.h"

struct kvm_fpureg vector = {{1, 2, 3, 4 }};

static void guest_code(void)
{
	unsigned long val;
	struct kvm_fpureg *fp = &vector;

	val = csr_read(LOONGARCH_CSR_EUEN);
	csr_write(val | 7, LOONGARCH_CSR_EUEN);

	__asm__ __volatile__("fld.d $f0, %0, 0\n" : : "r"(fp) :);
	GUEST_SYNC(0);

	__asm__ __volatile__("vld $vr0, %0, 0\n" : : "r"(fp) : );
	GUEST_SYNC(1);

	__asm__ __volatile__("xvld $xr0, %0, 0\n" : : "r"(fp) :);
	GUEST_SYNC(2);

	__asm__ __volatile__("fst.d $f0, %0, 0\n" : : "r"(fp) : "memory");
	GUEST_SYNC(3);

	__asm__ __volatile__("vst $vr0, %0, 0\n" : : "r"(fp) : "memory");
	GUEST_SYNC(4);

	__asm__ __volatile__("xvst $xr0, %0, 0\n" : : "r"(fp) : "memory");
	GUEST_SYNC(5);

	GUEST_DONE();
}

static void run_vcpu(struct kvm_vcpu *vcpu)
{
	struct ucall uc;
	int cont;

	cont = 1;
	while (cont) {
		vcpu_run(vcpu);

		switch (get_ucall(vcpu, &uc)) {
		case UCALL_PRINTF:
			printf("%s", (const char *)uc.buffer);
			break;
		case UCALL_DONE:
			printf("FPU test PASSED\n");
		case UCALL_SYNC:
			cont = 0;
			break;
		case UCALL_ABORT:
			REPORT_GUEST_ASSERT(uc);
		default:
			TEST_ASSERT(false, "Unexpected exit: %s",
				exit_reason_str(vcpu->run->exit_reason));
		}
	}
}

static bool __vm_has_feature(struct kvm_vm *vm, int feature)
{
	int ret;

	ret = __kvm_has_device_attr(vm->fd, KVM_LOONGARCH_VM_FEAT_CTRL, feature);
	return !ret;
}


int main(void)
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	struct kvm_fpu fpu;
	struct kvm_fpureg *fp = &vector;

	vm = vm_create_with_one_vcpu(&vcpu, guest_code);
	__TEST_REQUIRE(__vm_has_feature(vm, KVM_LOONGARCH_VM_FEAT_LSX),
			"LSX not available, skipping test\n");
	__TEST_REQUIRE(__vm_has_feature(vm, KVM_LOONGARCH_VM_FEAT_LASX),
			"LASX not available, skipping test\n");

	run_vcpu(vcpu);
	vcpu_fpu_get(vcpu, &fpu);
	sync_global_from_guest(vm, *fp);
	TEST_ASSERT(!memcmp(fpu.fpr, fp, 8), "Wanted 0x%llx from f0, got 0x%llx",
			fp->val64[0], fpu.fpr[0].val64[0]);

	run_vcpu(vcpu);
	vcpu_fpu_get(vcpu, &fpu);
	TEST_ASSERT(!memcmp(fpu.fpr, fp, 16), "Wanted 0x%llx %llx from vr0, got 0x%llx %llx",
			fp->val64[0], fp->val64[1],
			fpu.fpr[0].val64[0], fpu.fpr[0].val64[1]);

	run_vcpu(vcpu);
	vcpu_fpu_get(vcpu, &fpu);
	TEST_ASSERT(!memcmp(fpu.fpr, fp, 32),
			"Wanted 0x%llx %llx %llx %llx from xr0, got 0x%llx %llx %llx %llx",
			fp->val64[0], fp->val64[1], fp->val64[2], fp->val64[3],
			fpu.fpr[0].val64[0], fpu.fpr[0].val64[1],
			fpu.fpr[0].val64[2], fpu.fpr[0].val64[3]);

	fpu.fpr[0].val64[0] += 0x10;
	vcpu_fpu_set(vcpu, &fpu);
	run_vcpu(vcpu);
	vcpu_fpu_get(vcpu, &fpu);
	sync_global_from_guest(vm, *fp);
	TEST_ASSERT(!memcmp(fpu.fpr, fp, 8), "Wanted 0x%llx from f0, got 0x%llx",
			fp->val64[0], fpu.fpr[0].val64[0]);

	fpu.fpr[0].val64[0] += 0x10;
	fpu.fpr[0].val64[1] += 0x10;
	vcpu_fpu_set(vcpu, &fpu);
	run_vcpu(vcpu);
	vcpu_fpu_get(vcpu, &fpu);
	sync_global_from_guest(vm, *fp);
	TEST_ASSERT(!memcmp(fpu.fpr, fp, 16), "Wanted 0x%llx %llx from vr0, got 0x%llx %llx",
			fp->val64[0], fp->val64[1],
			fpu.fpr[0].val64[0], fpu.fpr[0].val64[1]);

	fpu.fpr[0].val64[0] += 0x10;
	fpu.fpr[0].val64[1] += 0x10;
	fpu.fpr[0].val64[2] += 0x10;
	fpu.fpr[0].val64[3] += 0x10;
	vcpu_fpu_set(vcpu, &fpu);
	run_vcpu(vcpu);
	vcpu_fpu_get(vcpu, &fpu);
	sync_global_from_guest(vm, *fp);
	TEST_ASSERT(!memcmp(fpu.fpr, fp, 32),
			"Wanted 0x%llx %llx %llx %llx from xr0, got 0x%llx %llx %llx %llx",
			fp->val64[0], fp->val64[1], fp->val64[2], fp->val64[3],
			fpu.fpr[0].val64[0], fpu.fpr[0].val64[1],
			fpu.fpr[0].val64[2], fpu.fpr[0].val64[3]);

	run_vcpu(vcpu);
	kvm_vm_free(vm);
	return 0;
}
