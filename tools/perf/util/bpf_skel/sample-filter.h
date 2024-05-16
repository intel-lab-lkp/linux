#ifndef PERF_UTIL_BPF_SKEL_SAMPLE_FILTER_H
#define PERF_UTIL_BPF_SKEL_SAMPLE_FILTER_H

#define MAX_FILTERS  64

/* supported filter operations */
enum perf_bpf_filter_op {
	PBF_OP_EQ,
	PBF_OP_NEQ,
	PBF_OP_GT,
	PBF_OP_GE,
	PBF_OP_LT,
	PBF_OP_LE,
	PBF_OP_AND,
	PBF_OP_GROUP_BEGIN,
	PBF_OP_GROUP_END,
};

enum perf_bpf_filter_term {
	/* No term is in use. */
	PBF_TERM_NONE,
	/* Terms that correspond to PERF_SAMPLE_xx values. */
	PBF_TERM_IP,
	PBF_TERM_ID,
	PBF_TERM_TID,
	PBF_TERM_CPU,
	PBF_TERM_TIME,
	PBF_TERM_ADDR,
	PBF_TERM_PERIOD,
	PBF_TERM_TRANSACTION,
	PBF_TERM_WEIGHT,
	PBF_TERM_PHYS_ADDR,
	PBF_TERM_CODE_PAGE_SIZE,
	PBF_TERM_DATA_PAGE_SIZE,
	PBF_TERM_WEIGHT_STRUCT,
	PBF_TERM_DATA_SRC,
};

/* BPF map entry for filtering */
struct perf_bpf_filter_entry {
	enum perf_bpf_filter_op op;
	__u32 part; /* sub-sample type info when it has multiple values */
	enum perf_bpf_filter_term term;
	__u64 value;
};

#endif /* PERF_UTIL_BPF_SKEL_SAMPLE_FILTER_H */
