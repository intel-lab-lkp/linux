{
    "Incorrect atomic_exchg verification",
    .insns = {
		BPF_ST_MEM(BPF_DW, BPF_REG_10, -8, 0x110),
		BPF_MOV64_REG(BPF_REG_2, BPF_REG_10),
		BPF_ATOMIC_OP(BPF_DW, BPF_XCHG, BPF_REG_2, BPF_REG_2, -8),
        BPF_MOV64_IMM(BPF_REG_0, 1),
        BPF_EXIT_INSN(),
    },
    .result = ACCEPT,
},

