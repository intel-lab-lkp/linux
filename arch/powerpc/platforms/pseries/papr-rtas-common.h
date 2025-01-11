/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_POWERPC_PAPR_RTAS_COMMON_H
#define _ASM_POWERPC_PAPR_RTAS_COMMON_H

#include <linux/types.h>

/*
 * Internal "blob" APIs for accumulating RTAS call results into
 * an immutable buffer to be attached to a file descriptor.
 */
struct papr_rtas_blob {
	const char *data;
	size_t len;
};

/**
 * struct papr_sequence - State for managing a sequence of RTAS calls.
 * @error:  Shall be zero as long as the sequence has not encountered an error,
 *          -ve errno otherwise. Use papr_rtas_sequence_set_err() to update.
 * @params: Parameter block to pass to rtas_*() calls.
 * @begin: Work area allocation and initialize the needed parameter
 *         values passed to RTAS call
 * @end: Free the allocated work area
 * @work: Obtain data with RTAS call and invoke it until the sequence is
 *        completed.
 *
 */
struct papr_rtas_sequence {
	int error;
	void *params;
	void (*begin)(struct papr_rtas_sequence *seq, void *param);
	void (*end)(struct papr_rtas_sequence *seq);
	const char *(*work)(struct papr_rtas_sequence *seq, size_t *len);
};

extern bool papr_rtas_blob_has_data(const struct papr_rtas_blob *blob);
extern void papr_rtas_blob_free(const struct papr_rtas_blob *blob);
extern int papr_rtas_sequence_set_err(struct papr_rtas_sequence *seq,
		int err);
extern const struct papr_rtas_blob *papr_rtas_retrieve(struct papr_rtas_sequence *seq, void *param);
extern long papr_rtas_setup_file_interface(struct papr_rtas_sequence *seq,
		void *param, const struct file_operations *fops, char *name);

#endif /* _ASM_POWERPC_PAPR_RTAS_COMMON_H */

