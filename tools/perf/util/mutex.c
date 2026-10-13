// SPDX-License-Identifier: GPL-2.0
#include "mutex.h"

#include "debug.h"
#include <linux/string.h>
#include <errno.h>

static void check_err(const char *fn, int err)
{
	char sbuf[STRERR_BUFSIZE];

	if (err == 0)
		return;

	pr_err("%s error: '%s'\n", fn, str_error_r(err, sbuf, sizeof(sbuf)));
}

#define CHECK_ERR(err) check_err(__func__, err)

static bool __mutex_init(struct mutex *mtx, bool pshared, bool recursive, bool robust)
{
	pthread_mutexattr_t attr;
	bool ok = false;
	int err;

	err = pthread_mutexattr_init(&attr);
	if (err)
		goto out;

#ifndef NDEBUG
	/* In normal builds enable error checking, such as recursive usage. */
	err = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
	if (err)
		goto out_destroy;
#endif
	if (recursive) {
		err = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
		if (err)
			goto out_destroy;
	}
	if (pshared) {
		err = pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
		if (err)
			goto out_destroy;
	}
	if (robust) {
#ifdef HAVE_PTHREAD_MUTEX_ROBUST
		err = pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
		if (err)
			goto out_unavailable;
#else
		goto out_unavailable;
#endif
	}

	err = pthread_mutex_init(&mtx->lock, &attr);
	if (!err)
		ok = true;
	else if (robust && pshared)
		/*
		 * glibc refuses a robust process shared mutex with ENOTSUP when
		 * set_robust_list(2) isn't usable, i.e. when the machine provides
		 * no robust mutexes for the process shared case: an unavailable
		 * API, not a bad request.
		 */
		err = 0;
out_destroy:
	pthread_mutexattr_destroy(&attr);
out:
	if (err)
		CHECK_ERR(err);

	return ok;

out_unavailable:
	pthread_mutexattr_destroy(&attr);
	return false;
}

void mutex_init(struct mutex *mtx)
{
	__mutex_init(mtx, /*pshared=*/false, /*recursive=*/false, /*robust=*/false);
}

void mutex_init_pshared(struct mutex *mtx)
{
	__mutex_init(mtx, /*pshared=*/true, /*recursive=*/false, /*robust=*/false);
}

void mutex_init_recursive(struct mutex *mtx)
{
	__mutex_init(mtx, /*pshared=*/false, /*recursive=*/true, /*robust=*/false);
}

bool mutex_init_pshared_robust(struct mutex *mtx)
{
	return __mutex_init(mtx, /*pshared=*/true, /*recursive=*/false, /*robust=*/true);
}

void mutex_destroy(struct mutex *mtx)
{
	CHECK_ERR(pthread_mutex_destroy(&mtx->lock));
}

void mutex_lock(struct mutex *mtx)
	NO_THREAD_SAFETY_ANALYSIS
{
#ifdef HAVE_PTHREAD_MUTEX_ROBUST
	int err = pthread_mutex_lock(&mtx->lock);

	/*
	 * A robust mutex whose owner died while holding it comes back as
	 * EOWNERDEAD, marked by the kernel: take it over.
	 */
	if (err == EOWNERDEAD) {
		pr_debug("mutex: taking over one left by a dead owner\n");
		CHECK_ERR(pthread_mutex_consistent(&mtx->lock));
	} else {
		CHECK_ERR(err);
	}
#else
	CHECK_ERR(pthread_mutex_lock(&mtx->lock));
#endif
}

void mutex_unlock(struct mutex *mtx)
	NO_THREAD_SAFETY_ANALYSIS
{
	CHECK_ERR(pthread_mutex_unlock(&mtx->lock));
}

bool mutex_trylock(struct mutex *mtx)
{
	int ret = pthread_mutex_trylock(&mtx->lock);

	if (ret == 0)
		return true; /* Lock acquired. */

	if (ret == EBUSY)
		return false; /* Lock busy. */

	/* Print error. */
	CHECK_ERR(ret);
	return false;
}

static void __cond_init(struct cond *cnd, bool pshared)
{
	pthread_condattr_t attr;

	CHECK_ERR(pthread_condattr_init(&attr));
	if (pshared)
		CHECK_ERR(pthread_condattr_setpshared(&attr, PTHREAD_PROCESS_SHARED));

	CHECK_ERR(pthread_cond_init(&cnd->cond, &attr));
	CHECK_ERR(pthread_condattr_destroy(&attr));
}

void cond_init(struct cond *cnd)
{
	__cond_init(cnd, /*pshared=*/false);
}

void cond_init_pshared(struct cond *cnd)
{
	__cond_init(cnd, /*pshared=*/true);
}

void cond_destroy(struct cond *cnd)
{
	CHECK_ERR(pthread_cond_destroy(&cnd->cond));
}

void cond_wait(struct cond *cnd, struct mutex *mtx)
{
	CHECK_ERR(pthread_cond_wait(&cnd->cond, &mtx->lock));
}

void cond_signal(struct cond *cnd)
{
	CHECK_ERR(pthread_cond_signal(&cnd->cond));
}

void cond_broadcast(struct cond *cnd)
{
	CHECK_ERR(pthread_cond_broadcast(&cnd->cond));
}
