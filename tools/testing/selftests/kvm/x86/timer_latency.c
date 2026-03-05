// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Intel Corporation
 *
 * Measure timer interrupt latency between time set to the local timer and
 * interrupt arrival time.  Optionally print out max/min/avg of the latency.
 */

#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <signal.h>
#include <pthread.h>

#include "kvm_util.h"
#include "processor.h"
#include "apic.h"
#include "vmx.h"

#define L2_GUEST_STACK_SIZE 256
static unsigned long l2_guest_stack[L2_GUEST_STACK_SIZE];

#define LOCAL_TIMER_VECTOR	0xec

#define TEST_DURATION_DEFAULT_IN_SEC   10

/* Random number in ns, appropriate for timer interrupt */
#define DEFAULT_TIMER_INC_NS	10000

/* Twice 100Hz scheduler tick for nested virtualization. */
#define DEFAULT_ALLOWED_TIMER_LATENCY_NS	(20 * 1000 * 1000)

struct options {
	bool use_oneshot_timer;
	bool use_x2apic;
	bool use_poll;
	bool nested;

	uint64_t timer_inc_ns;
	uint64_t allowed_timer_latency_ns;

	bool print_result;
};

static struct options options = {
	.use_x2apic = true,
	.timer_inc_ns = DEFAULT_TIMER_INC_NS,
	.allowed_timer_latency_ns = DEFAULT_ALLOWED_TIMER_LATENCY_NS,
};

enum event_type {
	EVENT_TIMER_HANDLER,
	EVENT_HLT_WAKEUP,
	EVENT_MAX,
};

struct test_sample {
	uint64_t time_stamp;
	enum event_type etype;
	uint32_t latency;
};

struct test_latency_stat {
	uint64_t sum;
	uint64_t count;
	uint32_t min;
	uint32_t max;
};

struct test_shared_data {
	atomic_bool stop_test;
	atomic_bool terminated;
	uint64_t tsc_khz;
	uint64_t apic_bus_cycle_ns;
	uint64_t allowed_timer_latency_tsc;

	uint64_t timer_inc;

	uint64_t hlt_count;
	uint64_t timer_interrupt_set;
	uint64_t timer_interrupt_received;

	struct test_latency_stat latency_stat[EVENT_MAX];
};

#define GUEST_ASSERT_LATENCY(latency_tsc)				\
	__GUEST_ASSERT((latency_tsc) <= data->allowed_timer_latency_tsc, \
		       "too large timer latency %ld ns "		\
		       "(requires %ld ns) %ld khz tsc",			\
		       tsc_to_ns(data, latency_tsc),			\
		       options.allowed_timer_latency_ns,		\
		       data->tsc_khz)

static struct test_shared_data shared_data;

static u64 tsc_to_ns(struct test_shared_data *data, u64 tsc_delta)
{
	return tsc_delta * NSEC_PER_SEC / (data->tsc_khz * 1000);
}

static u64 ns_to_tsc(struct test_shared_data *data, u64 ns)
{
	return ns * (data->tsc_khz * 1000) / NSEC_PER_SEC;
}

static void latency_init(struct test_latency_stat *stat)
{
	stat->sum = 0;
	stat->count = 0;
	stat->min = -1;
	stat->max = 0;
}

static void shared_data_init(struct test_shared_data *data)
{
	int i;

	memset(data, 0, sizeof(*data));

	for (i = 0; i < ARRAY_SIZE(data->latency_stat); i++)
		latency_init(data->latency_stat + i);
}

static void stop_test(struct kvm_vm *vm, struct test_shared_data *data)
{
	atomic_store(&data->stop_test, true);
	sync_global_to_guest(vm, data->stop_test);
}

static void guest_apic_enable(void)
{
	if (options.use_x2apic)
		x2apic_enable();
	else
		xapic_enable();
}

static void guest_apic_write_reg(unsigned int reg, uint64_t val)
{
	if (options.use_x2apic)
		x2apic_write_reg(reg, val);
	else
		xapic_write_reg(reg, val);
}

static void record_sample(struct test_shared_data *data, enum event_type etype,
			 uint64_t ts, uint64_t latency)
{
	struct test_latency_stat *stat;

	stat = &data->latency_stat[etype];

	stat->count++;
	stat->sum += latency;

	if (stat->min > latency)
		stat->min = latency;
	if (stat->max < latency)
		stat->max = latency;

	if (etype == EVENT_TIMER_HANDLER &&
	    latency > data->allowed_timer_latency_tsc) {
		if (options.use_poll) {
			GUEST_PRINTF("latency is too high %ld ns (> %ld ns)\n",
				     tsc_to_ns(data, latency),
				     options.allowed_timer_latency_ns);
		} else
			GUEST_ASSERT_LATENCY(latency);
	}
}

static atomic_bool timer_interrupted;
static atomic_uint_fast64_t timer_tsc;

static inline bool tsc_before(u64 a, u64 b)
{
	return (s64)(a - b) < 0;
}

static void guest_timer_interrupt_handler(struct ex_regs *regs)
{
	uint64_t now = rdtsc();
	uint64_t timer_tsc__ = atomic_load(&timer_tsc);

	__GUEST_ASSERT(!atomic_load(&timer_interrupted),
		       "timer handler is called multiple times per timer");
	__GUEST_ASSERT(tsc_before(timer_tsc__, now),
		       "timer is fired before armed time timer_tsc 0x%lx now 0x%lx",
		       timer_tsc__, now);

	record_sample(&shared_data, EVENT_TIMER_HANDLER, now, now - timer_tsc__);

	shared_data.timer_interrupt_received++;
	atomic_store(&timer_interrupted, true);
	guest_apic_write_reg(APIC_EOI, 0);
}

static void __set_timer(struct test_shared_data *data,
			uint64_t next_tsc, uint64_t apic_inc)
{
	if (options.use_oneshot_timer)
		guest_apic_write_reg(APIC_TMICT, apic_inc);
	else
		wrmsr(MSR_IA32_TSC_DEADLINE, next_tsc);
}

static void set_timer(struct test_shared_data *data,
		      uint64_t next_tsc, uint64_t apic_inc)
{
	atomic_store(&timer_tsc, next_tsc);
	data->timer_interrupt_set++;
	__set_timer(data, next_tsc, apic_inc);
}

static u64 to_apic_bus_cycle(struct test_shared_data *data, u64 tsc_delta)
{
	u64 ret;

	if (!tsc_delta)
		return 0;

	ret = tsc_to_ns(data, tsc_delta) / data->apic_bus_cycle_ns;
	if (!ret)
		ret++;

	return ret;
}

static void hlt_loop(struct test_shared_data *data)
{
	uint64_t inc, now, prev_tsc, next_tsc;

	cli();
	guest_apic_enable();

	inc = data->timer_inc;

	/* DIVISOR = 1 for oneshot timer case */
	guest_apic_write_reg(APIC_TDCR, 0xb);
	guest_apic_write_reg(APIC_LVTT,
			     (options.use_oneshot_timer ?
			      APIC_LVT_TIMER_ONESHOT :
			      APIC_LVT_TIMER_TSCDEADLINE) |
			     LOCAL_TIMER_VECTOR);

	next_tsc = rdtsc() + inc;
	if (!next_tsc)
		next_tsc++;
	atomic_store(&timer_interrupted, false);
	set_timer(data, next_tsc, to_apic_bus_cycle(data, inc));

	while (!atomic_load(&data->stop_test)) {
		prev_tsc = rdtsc();

		if (options.use_poll) {
			sti();
			while (!atomic_load(&timer_interrupted) &&
			       rdtsc() < next_tsc + data->allowed_timer_latency_tsc)
				cpu_relax();
			cli();
		} else {
			/* "sti; hlt; cli" */
			safe_halt();
			cli();
		}

		now = rdtsc();

		record_sample(data, EVENT_HLT_WAKEUP, now, now - prev_tsc);
		data->hlt_count++;

		if (atomic_load(&timer_interrupted)) {
			while (next_tsc <= now)
				next_tsc += inc;
			if (!next_tsc)
				next_tsc++;

			atomic_store(&timer_interrupted, false);
			set_timer(data, next_tsc,
				  to_apic_bus_cycle(data, next_tsc - now));
		} else {
			uint64_t latency = now - next_tsc;

			GUEST_ASSERT_LATENCY(latency);
		}
	}

	/* Wait for the interrupt to arrive. */
	now = rdtsc();
	next_tsc = now + inc * 2;
	sti();
	while (now < next_tsc || !atomic_load(&timer_interrupted)) {
		cpu_relax();
		now = rdtsc();
	}
	cli();

	/* Stop timer explicitly just in case. */
	__set_timer(data, 0, 0);
}

static void guest_code(void)
{
	struct test_shared_data *data = &shared_data;

	hlt_loop(data);

	__GUEST_ASSERT(data->timer_interrupt_set == data->timer_interrupt_received,
		       "timer interrupt lost set %ld received %ld",
		       data->timer_interrupt_set, data->timer_interrupt_received);

	GUEST_DONE();
}

static void l1_guest_code(struct vmx_pages *vmx_pages)
{
	union vmx_ctrl_msr ctls_msr, ctls2_msr;
	uint64_t pin, ctls, ctls2, ctls3;

	GUEST_ASSERT(prepare_for_vmx_operation(vmx_pages));
	GUEST_ASSERT(load_vmcs(vmx_pages));
	prepare_vmcs(vmx_pages, guest_code,
		     &l2_guest_stack[L2_GUEST_STACK_SIZE]);

	/* Check prerequisites */
	GUEST_ASSERT(!rdmsr_safe(MSR_IA32_VMX_PROCBASED_CTLS, &ctls_msr.val));
	GUEST_ASSERT(ctls_msr.clr & CPU_BASED_ACTIVATE_SECONDARY_CONTROLS);
	GUEST_ASSERT(ctls_msr.clr & CPU_BASED_ACTIVATE_TERTIARY_CONTROLS);

	GUEST_ASSERT(!rdmsr_safe(MSR_IA32_VMX_PROCBASED_CTLS2, &ctls2_msr.val));
	GUEST_ASSERT(ctls2_msr.clr & SECONDARY_EXEC_VIRTUAL_INTR_DELIVERY);

	GUEST_ASSERT(!rdmsr_safe(MSR_IA32_VMX_PROCBASED_CTLS3, &ctls3));
	GUEST_ASSERT(ctls3 & TERTIARY_EXEC_GUEST_APIC_TIMER);

	/*
	 * SECONDARY_EXEC_VIRTUAL_INTR_DELIVERY requires
	 * PIN_BASED_EXT_INTR_MASK
	 */
	pin = vmreadz(PIN_BASED_VM_EXEC_CONTROL);
	pin |= PIN_BASED_EXT_INTR_MASK;
	GUEST_ASSERT(!vmwrite(PIN_BASED_VM_EXEC_CONTROL, pin));

	ctls = vmreadz(CPU_BASED_VM_EXEC_CONTROL);
	ctls |= CPU_BASED_USE_MSR_BITMAPS | CPU_BASED_TPR_SHADOW |
		CPU_BASED_ACTIVATE_SECONDARY_CONTROLS |
		CPU_BASED_ACTIVATE_TERTIARY_CONTROLS;
	GUEST_ASSERT(!vmwrite(CPU_BASED_VM_EXEC_CONTROL, ctls));

	/* guest apic timer requires virtual interrutp delivery */
	ctls2 = vmreadz(SECONDARY_VM_EXEC_CONTROL);
	ctls2 |= SECONDARY_EXEC_VIRTUALIZE_X2APIC_MODE |
		SECONDARY_EXEC_APIC_REGISTER_VIRT |
		SECONDARY_EXEC_VIRTUAL_INTR_DELIVERY;
	vmwrite(SECONDARY_VM_EXEC_CONTROL, ctls2);

	ctls3 = vmreadz(TERTIARY_VM_EXEC_CONTROL);
	ctls3 |= TERTIARY_EXEC_GUEST_APIC_TIMER;
	GUEST_ASSERT(!vmwrite(TERTIARY_VM_EXEC_CONTROL, ctls3));

	/*
	 * We don't emulate apic registers(including APIC_LVTT) for simplicity.
	 * Directly set vector for timer interrupt instead.
	 */
	GUEST_ASSERT(!vmwrite(GUEST_APIC_TIMER_VECTOR, LOCAL_TIMER_VECTOR));

	/* launch L2 */
	GUEST_ASSERT(!vmlaunch());
	GUEST_ASSERT_EQ(vmreadz(VM_EXIT_REASON), EXIT_REASON_VMCALL);

	GUEST_DONE();
}

static void __run_vcpu(struct kvm_vcpu *vcpu)
{
	struct ucall uc;

	for (;;) {
		vcpu_run(vcpu);

		TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);

		switch (get_ucall(vcpu, &uc)) {
		case UCALL_DONE:
			pr_info("vcpu id %d passed\n", vcpu->id);
			return;
		case UCALL_ABORT:
			REPORT_GUEST_ASSERT(uc);
			return;
		case UCALL_PRINTF:
			pr_info("%s", uc.buffer);
			continue;
		default:
			TEST_FAIL("Unexpected ucall cmd: %ld", uc.cmd);
			return;
		}

		return;
	}
}

static void *run_vcpu(void *args)
{
	struct kvm_vcpu *vcpu = args;

	__run_vcpu(vcpu);

	return NULL;
}

static void print_result_type(struct test_shared_data *data,
			      enum event_type etype, const char *event_name)
{
	struct test_latency_stat *stat = &data->latency_stat[etype];
	uint64_t avg = 0;

	if (stat->count)
		avg = stat->sum / stat->count;

	pr_info("%s latency (%ld samples)\tmin %ld avg %ld max %ld ns\n",
		event_name, stat->count,
		tsc_to_ns(data, stat->min), tsc_to_ns(data, avg),
		tsc_to_ns(data, stat->max));
}

static void print_result(struct test_shared_data *data)
{
	pr_info("guest timer: %s timer period %ld ns\n",
		options.use_oneshot_timer ?
		"APIC oneshot timer" : "tsc deadline",
		options.timer_inc_ns);

	pr_info("tsc_khz %ld apic_bus_cycle_ns %ld\n",
		data->tsc_khz, data->apic_bus_cycle_ns);

	pr_info("hlt %ld timer set %ld received %ld\n",
		data->hlt_count,
		data->timer_interrupt_set, data->timer_interrupt_received);

	print_result_type(data, EVENT_TIMER_HANDLER, "timer interrupt");
	print_result_type(data, EVENT_HLT_WAKEUP, "halt wakeup");
}

static void print_exit_stats(struct kvm_vcpu *vcpu)
{
	static const char * const stat_name[] = {
		"exits",
		"halt_exits",
		"irq_exits",
		"inject_tscdeadline"
	};
	uint64_t data;
	int i;

	for (i = 0; i < ARRAY_SIZE(stat_name); i++) {
		kvm_get_stat(&vcpu->stats, stat_name[i], &data, 1);
		pr_info("%s: %ld ", stat_name[i], data);
	}
	pr_info("\n");
}

static void setup_timer_freq(struct kvm_vm *vm,
			     struct test_shared_data *data)
{
	data->tsc_khz = __vm_ioctl(vm, KVM_GET_TSC_KHZ, NULL);
	TEST_ASSERT(data->tsc_khz > 0, "KVM_GET_TSC_KHZ failed..");

	data->apic_bus_cycle_ns = kvm_check_cap(KVM_CAP_X86_APIC_BUS_CYCLES_NS);
	if (options.use_oneshot_timer)
		data->timer_inc = options.timer_inc_ns * data->apic_bus_cycle_ns;
	else
		data->timer_inc = ns_to_tsc(data, options.timer_inc_ns);

	data->allowed_timer_latency_tsc =
		ns_to_tsc(data, options.allowed_timer_latency_ns);
}

static void clear_msr_bitmap(struct vmx_pages *vmx, int msr)
{
	clear_bit(msr, vmx->msr_hva);
	clear_bit(msr, vmx->msr_hva + 2048);
}

static void setup(struct kvm_vm **vm__, struct kvm_vcpu **vcpu__)
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;

	if (options.nested) {
		vm_vaddr_t vmx_pages_gva = 0;
		struct vmx_pages *vmx;

		vm = vm_create_with_one_vcpu(&vcpu, l1_guest_code);

		vmx = vcpu_alloc_vmx(vm, &vmx_pages_gva);
		memset(vmx->msr_hva, 0xff, 4096);

		/* Allow nested apic timer virtualization. */
		clear_msr_bitmap(vmx, MSR_IA32_TSC_DEADLINE);

		/*  Rely on x2apic virtualization. */
		clear_msr_bitmap(vmx, MSR_IA32_APICBASE);
		clear_msr_bitmap(vmx, APIC_BASE_MSR + (APIC_TDCR >> 4));
		clear_msr_bitmap(vmx, APIC_BASE_MSR + (APIC_LVTT >> 4));
		clear_msr_bitmap(vmx, APIC_BASE_MSR + (APIC_SPIV >> 4));
		clear_msr_bitmap(vmx, APIC_BASE_MSR + (APIC_EOI >> 4));

		vcpu_args_set(vcpu, 1, vmx_pages_gva);
	} else
		vm = vm_create_with_one_vcpu(&vcpu, guest_code);

	vm_install_exception_handler(vm, LOCAL_TIMER_VECTOR,
				     guest_timer_interrupt_handler);
	setup_timer_freq(vm, &shared_data);

	if (!options.use_oneshot_timer)
		vcpu_set_cpuid_feature(vcpu, X86_FEATURE_TSC_DEADLINE_TIMER);

	sync_global_to_guest(vm, options);
	sync_global_to_guest(vm, shared_data);

	*vm__ = vm;
	*vcpu__ = vcpu;
}

static void print_stats(struct kvm_vm *vm, struct kvm_vcpu *vcpu)
{
	if (options.print_result) {
		sync_global_from_guest(vm, *&shared_data);
		print_result(&shared_data);
		print_exit_stats(vcpu);
	}
}

static void sigterm_handler(int signum, siginfo_t *info, void *arg_)
{
	atomic_store(&shared_data.terminated, true);
}

static int run_test(unsigned int duration)
{
	struct kvm_vcpu *vcpu;
	struct sigaction sa;
	struct kvm_vm *vm;
	pthread_t thread;
	int r;

	shared_data_init(&shared_data);

	setup(&vm, &vcpu);

	sa = (struct sigaction) {
		.sa_sigaction = sigterm_handler,
	};
	sigemptyset(&sa.sa_mask);
	r = sigaction(SIGTERM, &sa, NULL);
	TEST_ASSERT(!r, "sigaction");

	r = pthread_create(&thread, NULL, run_vcpu, vcpu);
	TEST_ASSERT(!r, "pthread_create");

	while (duration > 0 && !atomic_load(&shared_data.terminated)) {
		duration = sleep(duration);
		TEST_ASSERT(duration >= 0, "sleep");
	}

	if (atomic_load(&shared_data.terminated)) {
		pr_info("terminated\n");
		print_stats(vm, vcpu);
		return -EINTR;
	}

	stop_test(vm, &shared_data);

	r = pthread_join(thread, NULL);
	TEST_ASSERT(!r, "pthread_join");

	print_stats(vm, vcpu);

	kvm_vm_free(vm);
	return 0;
}

static void help(const char *name)
{
	puts("");
	printf("usage: %s ", name);
	printf("[-h] [-l] [-d duration_in_sec] [-a allowed_timer_latency] ");
	printf("[-p period_in_ns] [-o] [-O] [-x] [-X]\n");
	puts("");
	printf("-h: Display this message.");
	printf("-l: use idle loop instead of hlt\n");
	printf("-d: specify test to run in second (default %d sec)\n",
	       TEST_DURATION_DEFAULT_IN_SEC);
	printf("-p: timer period in ns (default %d nsec)\n",
	       DEFAULT_TIMER_INC_NS);
	printf("-a: allowed timer latency in ns (default %d nsec)\n",
	       DEFAULT_ALLOWED_TIMER_LATENCY_NS);
	printf("-o: use APIC oneshot timer instead of TSC deadline timer\n");
	printf("-t: use TSC deadline timer instead of APIC oneshot timer (default)\n");
	printf("-P: print result stat\n");
	printf("-x: use xAPIC mode\n");
	printf("-X: use x2APIC mode (default)\n");
	printf("-n: Only measure nested VM (L2)\n");
	printf("-N: Don't measure nested VM (L2)\n");
	puts("");

	exit(EXIT_SUCCESS);
}

int main(int argc, char **argv)
{
	int opt;
	unsigned int duration = TEST_DURATION_DEFAULT_IN_SEC;
	bool nested_only = false;
	bool no_nest = false;

	while ((opt = getopt(argc, argv, "hld:p:a:otxXPnN")) != -1) {
		switch (opt) {
		case 'l':
			options.use_poll = true;
			break;

		case 'd':
			duration = atoi_non_negative("test duration in sec", optarg);
			break;
		case 'p':
			options.timer_inc_ns =
				atoi_non_negative("timer period in nsec", optarg);
			break;
		case 'a':
			options.allowed_timer_latency_ns =
				atoi_non_negative("allowed timer latency in nsec",
						  optarg);
			break;


		case 'x':
			options.use_x2apic = false;
			break;
		case 'X':
			options.use_x2apic = true;
			break;

		case 'o':
			options.use_oneshot_timer = true;
			break;
		case 't':
			options.use_oneshot_timer = false;
			break;

		case 'P':
			options.print_result = true;
			break;

		case 'n':
			nested_only = true;
			no_nest = false;
			break;
		case 'N':
			nested_only = false;
			no_nest = true;
			break;

		case 'h':
		default:
			help(argv[0]);
			break;
		}
	}

	TEST_REQUIRE(kvm_has_cap(KVM_CAP_GET_TSC_KHZ));
	TEST_REQUIRE(kvm_has_cap(KVM_CAP_VM_TSC_CONTROL));
	if (!options.use_oneshot_timer)
		TEST_REQUIRE(kvm_has_cap(KVM_CAP_TSC_DEADLINE_TIMER));
	TEST_REQUIRE(kvm_has_cap(KVM_CAP_X86_APIC_BUS_CYCLES_NS));
	if (options.use_x2apic)
		TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_X2APIC));

	if (!nested_only) {
		options.nested = false;
		run_test(duration);
	}

	if (!no_nest) {
		union vmx_ctrl_msr ctls;
		uint64_t ctls3;

		ctls.val = kvm_get_feature_msr(MSR_IA32_VMX_TRUE_PROCBASED_CTLS);
		TEST_REQUIRE(ctls.clr & CPU_BASED_ACTIVATE_TERTIARY_CONTROLS);

		ctls3 = kvm_get_feature_msr(MSR_IA32_VMX_PROCBASED_CTLS3);
		TEST_REQUIRE(ctls3 & TERTIARY_EXEC_GUEST_APIC_TIMER);

		/* L1 doesn't emulate HLT and memory-mapped APIC. */
		options.use_poll = true;
		options.use_oneshot_timer = false;

		options.nested = true;
		run_test(duration);
	}

	return 0;
}
