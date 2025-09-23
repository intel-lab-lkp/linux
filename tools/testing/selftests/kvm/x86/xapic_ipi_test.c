// SPDX-License-Identifier: GPL-2.0
/*
 * xapic_ipi_test
 *
 * Copyright (C) 2020, Google LLC.
 *
 * This work is licensed under the terms of the GNU GPL, version 2.
 *
 * Test that when the APIC is in xAPIC mode, a vCPU can send an IPI to wake
 * another vCPU that is halted when KVM's backing page for the APIC access
 * address has been moved by mm.
 *
 * The test starts two vCPUs: one that sends IPIs and one that continually
 * executes HLT. The sender checks that the halter has woken from the HLT and
 * has reentered HLT before sending the next IPI. While the vCPUs are running,
 * the host continually calls migrate_pages to move all of the process' pages
 * amongst the available numa nodes on the machine.
 *
 * Migration is a command line option. When used on non-numa machines will 
 * exit with error. Test is still usefull on non-numa for testing IPIs.
 */
#include <getopt.h>
#include <pthread.h>
#include <inttypes.h>
#include <string.h>
#include <time.h>

#include "kvm_util.h"
#include "numaif.h"
#include "processor.h"
#include "test_util.h"
#include "vmx.h"
#include "sev.h"

/* Default running time for the test */
#define DEFAULT_RUN_SECS 3

/* Default delay between migrate_pages calls (microseconds) */
#define DEFAULT_DELAY_USECS 500000

/*
 * Vector for IPI from sender vCPU to halting vCPU.
 * Value is arbitrary and was chosen for the alternating bit pattern. Any
 * value should work.
 */
#define IPI_VECTOR	 0xa5

/*
 * Incremented in the IPI handler. Provides evidence to the sender that the IPI
 * arrived at the destination
 */
static volatile uint64_t *ipis_rcvd;

static bool x2apic;

static void apic_enable(void)
{
	if (x2apic)
		x2apic_enable();
	else
		xapic_enable();
}

static uint32_t apic_read_reg(unsigned int reg)
{
	return x2apic ? x2apic_read_reg(reg) : xapic_read_reg(reg);
}

static void apic_write_reg(unsigned int reg, uint64_t val)
{
	if (x2apic)
		x2apic_write_reg(reg, val);
	else
		xapic_write_reg(reg, (uint32_t)val);
}

/* Data struct shared between host main thread and vCPUs */
struct test_data_page {
	uint32_t halter_apic_id;
	volatile uint64_t hlt_count;
	volatile uint64_t wake_count;
	uint64_t ipis_sent;
	uint64_t migrations_attempted;
	uint64_t migrations_completed;
	uint32_t icr;
	uint32_t icr2;
	uint32_t halter_tpr;
	uint32_t halter_ppr;

	/*
	 *  Record local version register as a cross-check that APIC access
	 *  worked. Value should match what KVM reports (APIC_VERSION in
	 *  arch/x86/kvm/lapic.c). If test is failing, check that values match
	 *  to determine whether APIC access exits are working.
	 */
	uint32_t halter_lvr;
	uint64_t ipis_rcvd;
};

struct thread_params {
	struct test_data_page *data;
	struct kvm_vcpu *vcpu;
	uint64_t *pipis_rcvd; /* host address of ipis_rcvd global */
};

void verify_apic_base_addr(void)
{
	uint64_t msr = rdmsr(MSR_IA32_APICBASE);
	uint64_t base = GET_APIC_BASE(msr);

	GUEST_ASSERT(base == APIC_DEFAULT_GPA);
}

static void halter_guest_code(struct test_data_page *data)
{
	verify_apic_base_addr();
	apic_enable();

	data->halter_apic_id = GET_APIC_ID_FIELD(apic_read_reg(APIC_ID));
	data->halter_lvr = apic_read_reg(APIC_LVR);

	/*
	 * Loop forever HLTing and recording halts & wakes. Disable interrupts
	 * each time around to minimize window between signaling the pending
	 * halt to the sender vCPU and executing the halt. No need to disable on
	 * first run as this vCPU executes first and the host waits for it to
	 * signal going into first halt before starting the sender vCPU. Record
	 * TPR and PPR for diagnostic purposes in case the test fails.
	 */
	for (;;) {
		data->halter_tpr = apic_read_reg(APIC_TASKPRI);
		data->halter_ppr = apic_read_reg(APIC_PROCPRI);
		data->hlt_count++;
		safe_halt();
		cli();
		data->wake_count++;
	}
}

/*
 * Runs on halter vCPU when IPI arrives. Write an arbitrary non-zero value to
 * enable diagnosing errant writes to the APIC access address backing page in
 * case of test failure.
 */
static void guest_ipi_handler(struct ex_regs *regs)
{
	(*ipis_rcvd)++;
	apic_write_reg(APIC_EOI, 77);
}

static void sender_guest_code(struct test_data_page *data)
{
	uint64_t last_wake_count;
	uint64_t last_hlt_count;
	uint64_t last_ipis_rcvd_count;
	uint32_t icr_val;
	uint32_t icr2_val;
	uint64_t tsc_start;

	verify_apic_base_addr();
	apic_enable();

	/*
	 * Init interrupt command register for sending IPIs
	 *
	 * Delivery mode=fixed, per SDM:
	 *   "Delivers the interrupt specified in the vector field to the target
	 *    processor."
	 *
	 * Destination mode=physical i.e. specify target by its local APIC
	 * ID. This vCPU assumes that the halter vCPU has already started and
	 * set data->halter_apic_id.
	 */
	icr_val = (APIC_DEST_PHYSICAL | APIC_DM_FIXED | IPI_VECTOR);
	icr2_val = SET_APIC_DEST_FIELD(data->halter_apic_id);
	data->icr = icr_val;
	data->icr2 = icr2_val;

	last_wake_count = data->wake_count;
	last_hlt_count = data->hlt_count;
	last_ipis_rcvd_count = *ipis_rcvd;
	for (;;) {
		/*
		 * Send IPI to halter vCPU.
		 * First IPI can be sent unconditionally because halter vCPU
		 * starts earlier.
		 */
		if (!x2apic) {
			apic_write_reg(APIC_ICR2, icr2_val);
			apic_write_reg(APIC_ICR, icr_val);
		} else {
			apic_write_reg(APIC_ICR, (uint64_t)icr2_val << 32 | icr_val);
		}
		data->ipis_sent++;

		/*
		 * Wait up to ~1 sec for halter to indicate that it has:
		 * 1. Received the IPI
		 * 2. Woken up from the halt
		 * 3. Gone back into halt
		 * Current CPUs typically run at 2.x Ghz which is ~2
		 * billion ticks per second.
		 */
		tsc_start = rdtsc();
		while (rdtsc() - tsc_start < 2000000000) {
			if ((*ipis_rcvd != last_ipis_rcvd_count) &&
			    (data->wake_count != last_wake_count) &&
			    (data->hlt_count != last_hlt_count))
				break;
		}

		GUEST_ASSERT((*ipis_rcvd != last_ipis_rcvd_count) &&
			     (data->wake_count != last_wake_count) &&
			     (data->hlt_count != last_hlt_count));

		last_wake_count = data->wake_count;
		last_hlt_count = data->hlt_count;
		last_ipis_rcvd_count = *ipis_rcvd;
	}
}

static void *vcpu_thread(void *arg)
{
	struct thread_params *params = (struct thread_params *)arg;
	struct kvm_vcpu *vcpu = params->vcpu;
	struct ucall uc;
	int old;
	int r;

	r = pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &old);
	TEST_ASSERT(r == 0,
		    "pthread_setcanceltype failed on vcpu_id=%u with errno=%d",
		    vcpu->id, r);

	fprintf(stderr, "vCPU thread running vCPU %u\n", vcpu->id);
	vcpu_run(vcpu);

	TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);

	if (get_ucall(vcpu, &uc) == UCALL_ABORT) {
		TEST_ASSERT(false,
			    "vCPU %u exited with error: %s.\n"
			    "Sending vCPU sent %lu IPIs to halting vCPU\n"
			    "Halting vCPU halted %lu times, woke %lu times, received %lu IPIs.\n"
			    "Halter TPR=%#x PPR=%#x LVR=%#x\n"
			    "Migrations attempted: %lu\n"
			    "Migrations completed: %lu",
			    vcpu->id, (const char *)uc.args[0],
			    params->data->ipis_sent, params->data->hlt_count,
			    params->data->wake_count,
			    *params->pipis_rcvd, params->data->halter_tpr,
			    params->data->halter_ppr, params->data->halter_lvr,
			    params->data->migrations_attempted,
			    params->data->migrations_completed);
	}

	return NULL;
}

static void cancel_join_vcpu_thread(pthread_t thread, struct kvm_vcpu *vcpu)
{
	void *retval;
	int r;

	r = pthread_cancel(thread);
	TEST_ASSERT(r == 0,
		    "pthread_cancel on vcpu_id=%d failed with errno=%d",
		    vcpu->id, r);

	r = pthread_join(thread, &retval);
	TEST_ASSERT(r == 0,
		    "pthread_join on vcpu_id=%d failed with errno=%d",
		    vcpu->id, r);
	TEST_ASSERT(retval == PTHREAD_CANCELED,
		    "expected retval=%p, got %p", PTHREAD_CANCELED,
		    retval);
}

void do_migrations(struct test_data_page *data, int run_secs, int delay_usecs,
		   uint64_t *pipis_rcvd)
{
	long pages_not_moved;
	unsigned long nodemask = 0;
	unsigned long nodemasks[sizeof(nodemask) * 8];
	int nodes = 0;
	time_t start_time, last_update, now;
	time_t interval_secs = 1;
	int i, r;
	int from, to;
	unsigned long bit;
	uint64_t hlt_count;
	uint64_t wake_count;
	uint64_t ipis_sent;

	fprintf(stderr, "Calling migrate_pages every %d microseconds\n",
		delay_usecs);

	/* Get set of first 64 numa nodes available */
	r = get_mempolicy(NULL, &nodemask, sizeof(nodemask) * 8,
			  0, MPOL_F_MEMS_ALLOWED);
	TEST_ASSERT(r == 0, "get_mempolicy failed errno=%d", errno);

	fprintf(stderr, "Numa nodes found amongst first %lu possible nodes "
		"(each 1-bit indicates node is present): %#lx\n",
		sizeof(nodemask) * 8, nodemask);

	/* Init array of masks containing a single-bit in each, one for each
	 * available node. migrate_pages called below requires specifying nodes
	 * as bit masks.
	 */
	for (i = 0, bit = 1; i < sizeof(nodemask) * 8; i++, bit <<= 1) {
		if (nodemask & bit) {
			nodemasks[nodes] = nodemask & bit;
			nodes++;
		}
	}

	TEST_ASSERT(nodes > 1,
		    "Did not find at least 2 numa nodes. Can't do migration");

	fprintf(stderr, "Migrating amongst %d nodes found\n", nodes);

	from = 0;
	to = 1;
	start_time = time(NULL);
	last_update = start_time;

	ipis_sent = data->ipis_sent;
	hlt_count = data->hlt_count;
	wake_count = data->wake_count;

	while ((int)(time(NULL) - start_time) < run_secs) {
		data->migrations_attempted++;

		/*
		 * migrate_pages with PID=0 will migrate all pages of this
		 * process between the nodes specified as bitmasks. The page
		 * backing the APIC access address belongs to this process
		 * because it is allocated by KVM in the context of the
		 * KVM_CREATE_VCPU ioctl. If that assumption ever changes this
		 * test may break or give a false positive signal.
		 */
		pages_not_moved = migrate_pages(0, sizeof(nodemasks[from]),
						&nodemasks[from],
						&nodemasks[to]);
		if (pages_not_moved < 0)
			fprintf(stderr,
				"migrate_pages failed, errno=%d\n", errno);
		else if (pages_not_moved > 0)
			fprintf(stderr,
				"migrate_pages could not move %ld pages\n",
				pages_not_moved);
		else
			data->migrations_completed++;

		from = to;
		to++;
		if (to == nodes)
			to = 0;

		now = time(NULL);
		if (((now - start_time) % interval_secs == 0) &&
		    (now != last_update)) {
			last_update = now;
			fprintf(stderr,
				"%lu seconds: Migrations attempted=%lu completed=%lu, "
				"IPIs sent=%lu received=%lu, HLTs=%lu wakes=%lu\n",
				now - start_time, data->migrations_attempted,
				data->migrations_completed,
				data->ipis_sent, *pipis_rcvd,
				data->hlt_count, data->wake_count);

			TEST_ASSERT(ipis_sent != data->ipis_sent &&
				    hlt_count != data->hlt_count &&
				    wake_count != data->wake_count,
				    "IPI, HLT and wake count have not increased "
				    "in the last %lu seconds. "
				    "HLTer is likely hung.", interval_secs);

			ipis_sent = data->ipis_sent;
			hlt_count = data->hlt_count;
			wake_count = data->wake_count;
		}
		usleep(delay_usecs);
	}
}

void get_cmdline_args(int argc, char *argv[], int *run_secs,
		      bool *migrate, int *delay_usecs, bool *x2apic, int *vm_type)
{
	for (;;) {
		int opt = getopt(argc, argv, "s:d:v:me:t:");

		if (opt == -1)
			break;
		switch (opt) {
		case 's':
			*run_secs = parse_size(optarg);
			break;
		case 'm':
			*migrate = true;
			break;
		case 'd':
			*delay_usecs = parse_size(optarg);
			break;
		case 'e':
			*x2apic = parse_size(optarg) == 1;
			break;
		case 't':
			*vm_type = parse_size(optarg);
			switch (*vm_type) {
			case KVM_X86_DEFAULT_VM:
				break;
			case KVM_X86_SEV_VM:
				TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_SEV));
				break;
			case KVM_X86_SEV_ES_VM:
				TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_SEV_ES));
				break;
			case KVM_X86_SNP_VM:
				TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_SNP));
				break;
			default:
				TEST_ASSERT(false, "Unsupported VM type :%d",
					    *vm_type);
			}
			break;
		default:
			TEST_ASSERT(false,
				    "Usage: -s <runtime seconds>. Default is %d seconds.\n"
				    "-m adds calls to migrate_pages while vCPUs are running."
				    " Default is no migrations.\n"
				    "-d <delay microseconds> - delay between migrate_pages() calls."
				    " Default is %d microseconds.\n"
				    "-e <apic mode> - APIC mode 0 - xapic , 1 - x2apic"
				    " Default is xAPIC.\n"
				    "-t <vm type>. Default is %d.\n"
				    "Supported values:\n"
				    "0 - default\n"
				    "2 - SEV\n"
				    "3 - SEV-ES\n"
				    "4 - SNP",
				    DEFAULT_RUN_SECS, DEFAULT_DELAY_USECS, KVM_X86_DEFAULT_VM);
		}
	}
}

static inline bool is_sev_vm_type(int type)
{
	return type == KVM_X86_SEV_VM ||
		type == KVM_X86_SEV_ES_VM ||
		type == KVM_X86_SNP_VM;
}

static inline uint64_t get_sev_policy(int vm_type)
{
	switch (vm_type) {
	case KVM_X86_SEV_VM:
		return SEV_POLICY_NO_DBG;
	case KVM_X86_SEV_ES_VM:
		return SEV_POLICY_ES;
	case KVM_X86_SNP_VM:
		return snp_default_policy();
	default:
		return 0;
	}
}

int main(int argc, char *argv[])
{
	int r;
	int wait_secs;
	const int max_halter_wait = 10;
	int run_secs = 0;
	int delay_usecs = 0;
	struct test_data_page *data;
	vm_vaddr_t test_data_page_vaddr;
	bool migrate = false;
	pthread_t threads[2];
	struct thread_params params[2];
	struct kvm_vm *vm;
	uint64_t *pipis_rcvd;
	int vm_type = KVM_X86_DEFAULT_VM;
	bool is_sev;

	get_cmdline_args(argc, argv, &run_secs, &migrate, &delay_usecs,
			 &x2apic, &vm_type);
	is_sev = is_sev_vm_type(vm_type);

	if (x2apic)
		migrate = 0;

	if (run_secs <= 0)
		run_secs = DEFAULT_RUN_SECS;
	if (delay_usecs <= 0)
		delay_usecs = DEFAULT_DELAY_USECS;

	if (is_sev)
		vm = vm_sev_create_with_one_vcpu(vm_type, halter_guest_code,
				&params[0].vcpu);
	else
		vm = vm_create_with_one_vcpu(&params[0].vcpu, halter_guest_code);

	vm_install_exception_handler(vm, IPI_VECTOR, guest_ipi_handler);
	if (is_sev_es_vm(vm))
		vm_install_exception_handler(vm, 29, sev_es_vc_handler);

	sync_global_to_guest(vm, x2apic);
	if (!x2apic)
		virt_pg_map(vm, APIC_DEFAULT_GPA, APIC_DEFAULT_GPA);

	params[1].vcpu = vm_vcpu_add(vm, 1, sender_guest_code);

	test_data_page_vaddr = vm_vaddr_alloc_page_shared(vm);
	data = addr_gva2hva(vm, test_data_page_vaddr);
	if (is_sev_snp_vm(vm))
		vm_mem_set_shared(vm, addr_hva2gpa(vm, data), getpagesize());
	memset(data, 0, sizeof(*data));
	params[0].data = data;
	params[1].data = data;

	vcpu_args_set(params[0].vcpu, 1, test_data_page_vaddr);
	vcpu_args_set(params[1].vcpu, 1, test_data_page_vaddr);

	ipis_rcvd = &((struct test_data_page *)test_data_page_vaddr)->ipis_rcvd;
	sync_global_to_guest(vm, ipis_rcvd);
	pipis_rcvd = (uint64_t *)addr_gva2hva(vm, (uint64_t)ipis_rcvd);
	params[0].pipis_rcvd = pipis_rcvd;
	params[1].pipis_rcvd = pipis_rcvd;

	if (is_sev)
		vm_sev_launch(vm, get_sev_policy(vm_type), NULL);

	/* Start halter vCPU thread and wait for it to execute first HLT. */
	r = pthread_create(&threads[0], NULL, vcpu_thread, &params[0]);
	TEST_ASSERT(r == 0,
		    "pthread_create halter failed errno=%d", errno);
	fprintf(stderr, "Halter vCPU thread started\n");

	wait_secs = 0;
	while ((wait_secs < max_halter_wait) && !data->hlt_count) {
		sleep(1);
		wait_secs++;
	}

	TEST_ASSERT(data->hlt_count,
		    "Halter vCPU did not execute first HLT within %d seconds",
		    max_halter_wait);

	fprintf(stderr,
		"Halter vCPU thread reported its APIC ID: %u after %d seconds.\n",
		data->halter_apic_id, wait_secs);

	r = pthread_create(&threads[1], NULL, vcpu_thread, &params[1]);
	TEST_ASSERT(r == 0, "pthread_create sender failed errno=%d", errno);

	fprintf(stderr,
		"IPI sender vCPU thread started. Letting vCPUs run for %d seconds.\n",
		run_secs);

	if (!migrate)
		sleep(run_secs);
	else
		do_migrations(data, run_secs, delay_usecs, pipis_rcvd);

	/*
	 * Cancel threads and wait for them to stop.
	 */
	cancel_join_vcpu_thread(threads[0], params[0].vcpu);
	cancel_join_vcpu_thread(threads[1], params[1].vcpu);

	/*
	 * If the host support Idle HLT, i.e. KVM *might* be using Idle HLT,
	 * then the number of HLT exits may be less than the number of HLTs
	 * that were executed, as Idle HLT elides the exit if the vCPU has an
	 * unmasked, pending IRQ (or NMI).
	 */
	if (this_cpu_has(X86_FEATURE_IDLE_HLT))
		TEST_ASSERT(data->hlt_count >= vcpu_get_stat(params[0].vcpu, halt_exits),
			    "HLT insns = %lu, HLT exits = %lu",
			    data->hlt_count, vcpu_get_stat(params[0].vcpu, halt_exits));
	else
		TEST_ASSERT_EQ(data->hlt_count, vcpu_get_stat(params[0].vcpu, halt_exits));

	fprintf(stderr,
		"Test successful after running for %d seconds.\n"
		"Sending vCPU sent %lu IPIs to halting vCPU\n"
		"Halting vCPU halted %lu times, woke %lu times, received %lu IPIs.\n"
		"Halter APIC ID=%#x\n"
		"Sender ICR value=%#x ICR2 value=%#x\n"
		"Halter TPR=%#x PPR=%#x LVR=%#x\n"
		"Migrations attempted: %lu\n"
		"Migrations completed: %lu\n",
		run_secs, data->ipis_sent,
		data->hlt_count, data->wake_count, *pipis_rcvd,
		data->halter_apic_id,
		data->icr, data->icr2,
		data->halter_tpr, data->halter_ppr, data->halter_lvr,
		data->migrations_attempted, data->migrations_completed);

	kvm_vm_free(vm);

	return 0;
}
