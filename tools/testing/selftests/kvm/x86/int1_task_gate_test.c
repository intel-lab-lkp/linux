// SPDX-License-Identifier: GPL-2.0-only
/*
 * Test ICEBP (INT1) with a 32-bit task gate for #DB delivery.
 *
 * On SVM, when ICEBP causes #DB and #DB is delivered via a task gate,
 * the saved RIP points at the ICEBP instruction rather than after it.
 * This is a hardware quirk that the hypervisor must work around by
 * intercepting ICEBP and advancing RIP before the task switch.
 *
 * This test sets up a 32-bit protected mode guest with:
 *   - A #DB IDT entry configured as a task gate (type 5)
 *   - A TSS for the #DB handler task
 *   - Guest code that executes ICEBP (0xF1)
 *
 * The #DB handler task records the EIP from the back-link TSS and
 * writes it to a known memory location. The test verifies that EIP
 * points after the ICEBP instruction, not at it.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/kvm.h>

#define KVM_DEV	"/dev/kvm"
#define PAGE_SIZE 4096

/* Guest physical memory layout (all below 1MB for simplicity) */
#define GDT_BASE	0x1000
#define IDT_BASE	0x2000
#define MAIN_TSS_BASE	0x3000
#define DB_TSS_BASE	0x4000
#define CODE_BASE	0x5000
#define RESULT_BASE	0x6000
#define MAIN_STACK	0x8000	/* top of main task stack */
#define DB_STACK	0x9000	/* top of #DB task stack */

/* GDT selectors */
#define SEL_CODE32	0x08
#define SEL_DATA32	0x10
#define SEL_MAIN_TSS	0x18
#define SEL_DB_TSS	0x20

/* 32-bit GDT entry */
struct gdt_entry {
	u16 limit_lo;
	u16 base_lo;
	u8  base_mid;
	u8  access;
	u8  limit_hi_flags;
	u8  base_hi;
} __attribute__((packed));

/* 32-bit IDT task gate entry */
struct idt_task_gate {
	u16 reserved0;
	u16 tss_selector;
	u8  reserved1;
	u8  type_attr;	/* P=1, DPL=0, type=0x5 (task gate) */
	u16 reserved2;
} __attribute__((packed));

/* 32-bit TSS */
struct tss32 {
	u16 back_link, __blh;
	u32 esp0;
	u16 ss0, __ss0h;
	u32 esp1;
	u16 ss1, __ss1h;
	u32 esp2;
	u16 ss2, __ss2h;
	u32 cr3;
	u32 eip;
	u32 eflags;
	u32 eax, ecx, edx, ebx;
	u32 esp, ebp, esi, edi;
	u16 es, __esh;
	u16 cs, __csh;
	u16 ss, __ssh;
	u16 ds, __dsh;
	u16 fs, __fsh;
	u16 gs, __gsh;
	u16 ldt, __ldth;
	u16 trace;
	u16 iomap_base;
} __attribute__((packed));

static void set_gdt_entry(struct gdt_entry *e, u32 base, u32 limit,
			  u8 access, u8 flags)
{
	e->limit_lo = limit & 0xffff;
	e->base_lo = base & 0xffff;
	e->base_mid = (base >> 16) & 0xff;
	e->access = access;
	e->limit_hi_flags = ((limit >> 16) & 0xf) | (flags << 4);
	e->base_hi = (base >> 24) & 0xff;
}

/*
 * Guest code (32-bit protected mode, assembled as bytes).
 *
 * The main task executes ICEBP. The #DB task gate switches to the
 * DB handler TSS. The DB handler reads the back-link TSS to get the
 * saved EIP, writes it to RESULT_BASE, then halts.
 */

/* Main task code: just execute ICEBP then halt */
static const u8 main_code[] = {
	0xf1,			/* ICEBP (INT1) */
	0xf4,			/* HLT - should not reach here if task gate works */
};

/*
 * #DB handler task code:
 *   - Read back_link from our TSS (at DB_TSS_BASE offset 0) to get main TSS selector
 *   - The main TSS (at MAIN_TSS_BASE) has saved EIP at offset 0x20
 *   - Read that EIP and store at RESULT_BASE
 *   - Store 0xCAFE at RESULT_BASE+4 as "handler ran" marker
 *   - HLT
 */
static const u8 db_handler_code[] = {
	/* mov eax, [MAIN_TSS_BASE + 0x20]  -- saved EIP in main TSS */
	0xa1, (MAIN_TSS_BASE + 0x20) & 0xff, ((MAIN_TSS_BASE + 0x20) >> 8) & 0xff, 0x00, 0x00,
	/* mov [RESULT_BASE], eax */
	0xa3, RESULT_BASE & 0xff, (RESULT_BASE >> 8) & 0xff, 0x00, 0x00,
	/* mov dword [RESULT_BASE+4], 0xCAFE */
	0xc7, 0x05, (RESULT_BASE + 4) & 0xff, ((RESULT_BASE + 4) >> 8) & 0xff, 0x00, 0x00,
	0xfe, 0xca, 0x00, 0x00,
	/* hlt */
	0xf4,
};

#define DB_HANDLER_OFFSET 0x100	/* offset within CODE_BASE page */

static int kvm_ioctl(int fd, unsigned long cmd, void *arg)
{
	int ret = ioctl(fd, cmd, arg);
	if (ret < 0) {
		perror("KVM ioctl");
		exit(1);
	}
	return ret;
}

int main(void)
{
	int kvm_fd, vm_fd, vcpu_fd;
	struct kvm_userspace_memory_region region;
	struct kvm_sregs sregs;
	struct kvm_regs regs;
	struct kvm_run *run;
	void *mem;
	size_t mmap_size;
	struct gdt_entry *gdt;
	struct idt_task_gate *idt;
	struct tss32 *main_tss, *db_tss;
	u32 *result;
	u32 expected_eip;

	kvm_fd = open(KVM_DEV, O_RDWR);
	if (kvm_fd < 0) { perror("open /dev/kvm"); return 1; }

	vm_fd = kvm_ioctl(kvm_fd, KVM_CREATE_VM, 0);
	vcpu_fd = kvm_ioctl(vm_fd, KVM_CREATE_VCPU, 0);

	/* Allocate guest memory — single 1MB region */
	mem = mmap(NULL, 0x100000, PROT_READ | PROT_WRITE,
		   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (mem == MAP_FAILED) { perror("mmap"); return 1; }
	memset(mem, 0, 0x100000);

	region = (struct kvm_userspace_memory_region){
		.slot = 0,
		.guest_phys_addr = 0,
		.memory_size = 0x100000,
		.userspace_addr = (u64)mem,
	};
	kvm_ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &region);

	/* Set up GDT */
	gdt = (struct gdt_entry *)(mem + GDT_BASE);
	/* Null descriptor */
	memset(&gdt[0], 0, sizeof(gdt[0]));
	/* Code32: base=0, limit=0xfffff, 32-bit, present, code r/x */
	set_gdt_entry(&gdt[1], 0, 0xfffff, 0x9a, 0xc);
	/* Data32: base=0, limit=0xfffff, 32-bit, present, data r/w */
	set_gdt_entry(&gdt[2], 0, 0xfffff, 0x92, 0xc);
	/* Main TSS: base=MAIN_TSS_BASE, limit=sizeof(tss32)-1, present, TSS32 available */
	set_gdt_entry(&gdt[3], MAIN_TSS_BASE, sizeof(struct tss32) - 1, 0x89, 0x0);
	/* DB TSS: base=DB_TSS_BASE, limit=sizeof(tss32)-1, present, TSS32 available */
	set_gdt_entry(&gdt[4], DB_TSS_BASE, sizeof(struct tss32) - 1, 0x89, 0x0);

	/* Set up IDT — task gate for #DB (vector 1) */
	idt = (struct idt_task_gate *)(mem + IDT_BASE);
	idt[1].tss_selector = SEL_DB_TSS;
	idt[1].type_attr = 0x85;  /* P=1, DPL=0, type=5 (32-bit task gate) */

	/* Set up main TSS */
	main_tss = (struct tss32 *)(mem + MAIN_TSS_BASE);
	main_tss->cr3 = 0;  /* no paging */
	main_tss->eip = CODE_BASE;
	main_tss->eflags = 0x02;
	main_tss->esp = MAIN_STACK;
	main_tss->cs = SEL_CODE32;
	main_tss->ds = SEL_DATA32;
	main_tss->es = SEL_DATA32;
	main_tss->ss = SEL_DATA32;
	main_tss->fs = SEL_DATA32;
	main_tss->gs = SEL_DATA32;

	/* Set up #DB handler TSS */
	db_tss = (struct tss32 *)(mem + DB_TSS_BASE);
	db_tss->cr3 = 0;
	db_tss->eip = CODE_BASE + DB_HANDLER_OFFSET;
	db_tss->eflags = 0x02;
	db_tss->esp = DB_STACK;
	db_tss->cs = SEL_CODE32;
	db_tss->ds = SEL_DATA32;
	db_tss->es = SEL_DATA32;
	db_tss->ss = SEL_DATA32;
	db_tss->fs = SEL_DATA32;
	db_tss->gs = SEL_DATA32;

	/* Copy guest code */
	memcpy(mem + CODE_BASE, main_code, sizeof(main_code));
	memcpy(mem + CODE_BASE + DB_HANDLER_OFFSET, db_handler_code, sizeof(db_handler_code));

	/* Expected EIP: after the 0xF1 byte */
	expected_eip = CODE_BASE + 1;

	/* Set up vCPU registers for 32-bit protected mode (no paging) */
	kvm_ioctl(vcpu_fd, KVM_GET_SREGS, &sregs);
	sregs.cr0 = 0x11;  /* PE + ET, no PG */
	sregs.cr3 = 0;
	sregs.cr4 = 0;

	sregs.gdt.base = GDT_BASE;
	sregs.gdt.limit = 5 * 8 - 1;
	sregs.idt.base = IDT_BASE;
	sregs.idt.limit = 256 * 8 - 1;

	sregs.cs.base = 0; sregs.cs.limit = 0xfffff; sregs.cs.selector = SEL_CODE32;
	sregs.cs.type = 0xb; sregs.cs.present = 1; sregs.cs.db = 1; sregs.cs.s = 1; sregs.cs.g = 1;
	sregs.ds.base = 0; sregs.ds.limit = 0xfffff; sregs.ds.selector = SEL_DATA32;
	sregs.ds.type = 3; sregs.ds.present = 1; sregs.ds.db = 1; sregs.ds.s = 1; sregs.ds.g = 1;
	sregs.es = sregs.ss = sregs.fs = sregs.gs = sregs.ds;

	sregs.tr.base = MAIN_TSS_BASE; sregs.tr.limit = sizeof(struct tss32) - 1;
	sregs.tr.selector = SEL_MAIN_TSS; sregs.tr.type = 0xb; /* 32-bit TSS busy */
	sregs.tr.present = 1;

	kvm_ioctl(vcpu_fd, KVM_SET_SREGS, &sregs);

	memset(&regs, 0, sizeof(regs));
	regs.rip = CODE_BASE;
	regs.rsp = MAIN_STACK;
	regs.rflags = 0x02;
	kvm_ioctl(vcpu_fd, KVM_SET_REGS, &regs);

	/* Run the guest */
	mmap_size = kvm_ioctl(kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
	run = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu_fd, 0);

	for (int i = 0; i < 100; i++) {
		kvm_ioctl(vcpu_fd, KVM_RUN, 0);

		if (run->exit_reason == KVM_EXIT_HLT)
			break;

		if (run->exit_reason != KVM_EXIT_INTERNAL_ERROR &&
		    run->exit_reason != KVM_EXIT_DEBUG) {
			fprintf(stderr, "Unexpected exit reason: %d\n", run->exit_reason);
			return 1;
		}
	}

	/* Check results */
	result = (u32 *)(mem + RESULT_BASE);
	if (result[1] != 0xCAFE) {
		fprintf(stderr, "FAIL: #DB handler did not run (marker=0x%x)\n", result[1]);
		return 1;
	}

	printf("Saved EIP: 0x%x, expected: 0x%x\n", result[0], expected_eip);
	if (result[0] == expected_eip) {
		printf("PASS: EIP points after ICEBP\n");
		return 0;
	} else if (result[0] == expected_eip - 1) {
		printf("FAIL: EIP points AT ICEBP (known SVM bug with task gates)\n");
		return 1;
	} else {
		printf("FAIL: EIP is unexpected\n");
		return 1;
	}
}
