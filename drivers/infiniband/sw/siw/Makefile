obj-$(CONFIG_RDMA_SIW) += siw.o

siw-y := \
	siw_cm.o \
	siw_cq.o \
	siw_main.o \
	siw_mem.o \
	siw_qp.o \
	siw_qp_tx.o \
	siw_qp_rx.o \
	siw_verbs.o

siw-$(CONFIG_SIW_MPA_RX_KUNIT_TEST) += siw_mpa_rx_kunit.o
