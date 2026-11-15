/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Platform layer of zipr: memory, file I/O, CRC-32, sorting, scheduling
 * and raw inflate. Internal to zipr.c.
 *
 * Kernel (__KERNEL__):  kvmalloc, filp_open/kernel_read, crc32_le, sort(),
 *                       zlib_inflate (deflate support).
 * User space:           malloc, open/pread, slice-by-8 CRC-32, qsort(),
 *                       stored only. ZIPR_TEST_ZLIB enables deflate via the
 *                       system zlib; it exists only to test the kernel's
 *                       inflate code path in user space.
 */
#ifndef ZIPR_PORT_H
#define ZIPR_PORT_H

#ifdef __KERNEL__

#include <linux/crc32.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/limits.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/string.h>
#include <linux/zlib.h>

#define ZIPR_HAVE_DEFLATE 1

/* kvmalloc() refuses (and warns about) anything larger. */
#define ZIPR_MAX_ALLOC	((size_t)INT_MAX)

typedef struct file *ZiprOSFile;

static inline void ziprSort(void *base, size_t n, size_t size,
			     int (*cmp)(const void *, const void *))
{
	sort(base, n, size, cmp, NULL);
}

/* Long loops call this to let other tasks run. */
static inline void ziprCondResched(void)
{
	cond_resched();
}

/* Long loops stop when the task is being killed. */
static inline bool ziprInterrupted(void)
{
	return fatal_signal_pending(current);
}

static inline void *ziprAlloc(size_t n)
{
	return kvmalloc(n, GFP_KERNEL);
}

static inline void *ziprZalloc(size_t n)
{
	return kvzalloc(n, GFP_KERNEL);
}

static inline void ziprFree(void *p)
{
	kvfree(p);
}

static inline int ziprOSOpen(const char *path, ZiprOSFile *fp,
			       uint64_t *size)
{
	/*
	 * O_LARGEFILE: unlike open(2) on 64-bit, filp_open() does not add it,
	 * and generic_file_open() refuses files over 2 GiB with -EOVERFLOW.
	 */
	struct file *file = filp_open(path, O_RDONLY | O_LARGEFILE, 0);
	umode_t mode;

	if (IS_ERR(file))
		return PTR_ERR(file);
	mode = file_inode(file)->i_mode;
	if (!S_ISREG(mode)) {
		filp_close(file, NULL);
		return S_ISDIR(mode) ? -EISDIR : -EINVAL;
	}
	*size = i_size_read(file_inode(file));
	*fp = file;
	return 0;
}

/* Like pread(): may return fewer bytes, 0 at EOF, -errno on error. */
static inline ssize_t ziprOSPread(ZiprOSFile file, void *buf, size_t len,
				    uint64_t off)
{
	loff_t pos = off;

	return kernel_read(file, buf, len, &pos);
}

static inline void ziprOSClose(ZiprOSFile file)
{
	filp_close(file, NULL);
}

/* zlib-compatible CRC-32: start with 0, feed chunks, compare the result. */
static inline uint32_t ziprCRC32(uint32_t crc, const void *p, size_t n)
{
	return ~crc32_le(~crc, p, n);
}

static inline int ziprInflateInit(z_stream *zs)
{
	memset(zs, 0, sizeof(*zs));
	zs->workspace = kvmalloc(zlib_inflate_workspacesize(), GFP_KERNEL);
	if (!zs->workspace)
		return -ENOMEM;
	/* Negative window bits: raw deflate, no zlib header (as in ZIP). */
	if (zlib_inflateInit2(zs, -MAX_WBITS) != Z_OK) {
		kvfree(zs->workspace);
		return -ENOMEM;
	}
	return 0;
}

static inline int ziprInflate(z_stream *zs)
{
	return zlib_inflate(zs, Z_SYNC_FLUSH);
}

static inline void ziprInflateEnd(z_stream *zs)
{
	zlib_inflateEnd(zs);
	kvfree(zs->workspace);
}

#else /* !__KERNEL__ */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ziprCRC32.h"

#define ZIPR_MAX_ALLOC	((size_t)SIZE_MAX / 2)

typedef int ZiprOSFile;

static inline void ziprSort(void *base, size_t n, size_t size,
			     int (*cmp)(const void *, const void *))
{
	qsort(base, n, size, cmp);
}

static inline void ziprCondResched(void)
{
}

static inline bool ziprInterrupted(void)
{
	return false;
}

static inline void *ziprAlloc(size_t n)
{
	return malloc(n);
}

static inline void *ziprZalloc(size_t n)
{
	return calloc(1, n);
}

static inline void ziprFree(void *p)
{
	free(p);
}

static inline int ziprOSOpen(const char *path, ZiprOSFile *fp,
			       uint64_t *size)
{
	struct stat st;
	int fd, err;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	if (fstat(fd, &st) < 0) {
		err = -errno;
		close(fd);
		return err;
	}
	if (!S_ISREG(st.st_mode)) {
		close(fd);
		return S_ISDIR(st.st_mode) ? -EISDIR : -EINVAL;
	}
	*size = (uint64_t)st.st_size;
	*fp = fd;
	return 0;
}

static inline ssize_t ziprOSPread(ZiprOSFile fd, void *buf, size_t len,
				    uint64_t off)
{
	ssize_t n;

	do
		n = pread(fd, buf, len, (off_t)off);
	while (n < 0 && errno == EINTR);
	return n < 0 ? -errno : n;
}

static inline void ziprOSClose(ZiprOSFile fd)
{
	close(fd);
}

/* CRC-32 (IEEE 802.3, reflected), eight bytes at a time. */
static inline uint32_t ziprCRC32(uint32_t crc, const void *p, size_t n)
{
	const uint32_t (*t)[256] = ziprCRCTab;
	const uint8_t *b = p;
	uint32_t lo, hi;

	crc = ~crc;
	for (; n >= 8; n -= 8, b += 8) {
		lo = crc ^ ((uint32_t)b[0] | (uint32_t)b[1] << 8 |
			    (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24);
		hi = (uint32_t)b[4] | (uint32_t)b[5] << 8 |
		     (uint32_t)b[6] << 16 | (uint32_t)b[7] << 24;
		crc = t[7][lo & 0xff] ^ t[6][(lo >> 8) & 0xff] ^
		      t[5][(lo >> 16) & 0xff] ^ t[4][lo >> 24] ^
		      t[3][hi & 0xff] ^ t[2][(hi >> 8) & 0xff] ^
		      t[1][(hi >> 16) & 0xff] ^ t[0][hi >> 24];
	}
	while (n--)
		crc = t[0][(crc ^ *b++) & 0xff] ^ (crc >> 8);
	return ~crc;
}

#ifdef ZIPR_TEST_ZLIB
#include <zlib.h>

#define ZIPR_HAVE_DEFLATE 1

static inline int ziprInflateInit(z_stream *zs)
{
	memset(zs, 0, sizeof(*zs));
	return inflateInit2(zs, -MAX_WBITS) == Z_OK ? 0 : -ENOMEM;
}

static inline int ziprInflate(z_stream *zs)
{
	return inflate(zs, Z_SYNC_FLUSH);
}

static inline void ziprInflateEnd(z_stream *zs)
{
	inflateEnd(zs);
}
#endif /* ZIPR_TEST_ZLIB */

#endif /* ?__KERNEL__ */

#endif /* ZIPR_PORT_H */
