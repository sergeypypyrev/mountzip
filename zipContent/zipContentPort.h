/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Platform layer of ZipContent: memory, sorting with a context argument,
 * and scheduling.
 *
 * Kernel (__KERNEL__):  kvmalloc, sort_r(), cond_resched().
 * User space:           malloc, qsort_r() (needs _GNU_SOURCE).
 *
 * Internal header: included by zipContent.c only.
 */
#ifndef ZIP_CONTENT_PORT_H
#define ZIP_CONTENT_PORT_H

#ifdef __KERNEL__

#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/string.h>

/* kvmalloc() refuses larger sizes */
#define ZIP_CONTENT_MAX_ALLOC	((size_t)INT_MAX)

/* The context argument of a comparison function. */
#define ZIP_CONTENT_SORT_CONTEXT	const void *

static inline void *zipContentAlloc(size_t size)
{
	return kvmalloc(size, GFP_KERNEL);
}

static inline void *zipContentZalloc(size_t size)
{
	return kvzalloc(size, GFP_KERNEL);
}

static inline void zipContentFree(void *p)
{
	kvfree(p);
}

static inline void zipContentSort(void *base, size_t count, size_t size,
				  int (*compare)(const void *, const void *,
						 const void *),
				  const void *context)
{
	sort_r(base, count, size, compare, NULL, context);
}

static inline void zipContentCondResched(void)
{
	cond_resched();
}

static inline bool zipContentInterrupted(void)
{
	return fatal_signal_pending(current);
}

#else /* user space */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ZIP_CONTENT_MAX_ALLOC	((size_t)SIZE_MAX / 2)

#define ZIP_CONTENT_SORT_CONTEXT	void *

static inline void *zipContentAlloc(size_t size)
{
	return malloc(size);
}

static inline void *zipContentZalloc(size_t size)
{
	return calloc(1, size);
}

static inline void zipContentFree(void *p)
{
	free(p);
}

static inline void zipContentSort(void *base, size_t count, size_t size,
				  int (*compare)(const void *, const void *,
						 void *),
				  void *context)
{
	qsort_r(base, count, size, compare, context);
}

static inline void zipContentCondResched(void)
{
}

static inline bool zipContentInterrupted(void)
{
	return false;
}

#endif /* __KERNEL__ */

#endif /* ZIP_CONTENT_PORT_H */
