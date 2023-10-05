/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/bio.h>
#include <linux/fs.h>
#include <linux/ioprio.h>

static inline enum rw_hint bio_get_data_lifetime(struct bio *bio)
{
	/* +1 to map 0 onto WRITE_LIFE_NONE. */
	return IOPRIO_PRIO_LIFETIME(bio->bi_ioprio) + 1;
}

static inline void bio_set_data_lifetime(struct bio *bio, enum rw_hint lifetime)
{
	/* -1 to map WRITE_LIFE_NONE onto 0. */
	if (lifetime != 0)
		lifetime--;
	WARN_ON_ONCE(lifetime & ~IOPRIO_LIFETIME_MASK);
	bio->bi_ioprio &= ~(IOPRIO_LIFETIME_MASK << IOPRIO_LIFETIME_SHIFT);
	bio->bi_ioprio |= lifetime << IOPRIO_LIFETIME_SHIFT;
}
