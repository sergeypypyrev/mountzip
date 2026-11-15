/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * zipr: a small read-only ZIP library for user space and the Linux kernel.
 *
 * The same sources build in both worlds; kbuild defines __KERNEL__, which
 * selects the kernel backend (filp_open/kernel_read, kvmalloc, zlib_inflate).
 *
 * Supported compression methods:
 *   user space: stored (0)
 *   kernel:     stored (0) and deflate (8)
 *
 * Archive features: ZIP64 (archives and members larger than 4 GiB, more
 * than 65535 entries), data prepended to the archive (e.g. SFX stubs),
 * archive comments. Not supported: multi-disk archives, encryption, ZIP64
 * combined with prepended data.
 *
 * Large archives. Memory use does not depend on the size of the archive or
 * the number of entries: ziprOpen() reads only the end records, and
 * iteration streams the central directory through a 256 KiB buffer. The
 * only structure that grows with the archive is the optional name index
 * (ziprIndex(), 12 bytes per entry: 120 MB for 10 million entries), which
 * makes ziprFind() a binary search instead of a scan.
 *
 * Errors are reported as negative errno values:
 *   -EINVAL       not a ZIP archive, or its structure is corrupted
 *   -EBADMSG      CRC mismatch or corrupted compressed data
 *   -EOPNOTSUPP   unsupported compression method, encryption, multi-disk
 *   -ENOENT       ziprFind(): no such entry
 *   -ENAMETOOLONG ziprEntryAt(): name buffer too small
 *   -EFBIG        the index would exceed ZIPR_MAX_ALLOC
 *   -EINTR        ziprIndex() interrupted by a fatal signal (kernel)
 *   -ENOMEM       out of memory
 *   anything else is an I/O error passed through from the OS
 *
 * Thread safety: an open archive is only read, so several threads may each
 * use their own iterator and their own open files concurrently. ziprIndex()
 * must not run concurrently with anything else on the same archive. A single
 * iterator or open file must not be used by two threads at once.
 *
 * In the kernel all functions may sleep: call them in process context only.
 */
#ifndef ZIPR_H
#define ZIPR_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>		/* ssize_t */
#endif

#define ZIPR_STORED	0
#define ZIPR_DEFLATED	8

/* Longest possible entry name. */
#define ZIPR_NAME_MAX	0xffff

struct Zipr;			/* an open archive */
struct ZiprIter;		/* a cursor over the central directory */
struct ZiprFile;		/* an open file inside an archive */

/*
 * Description of one archive member. Everything ziprFopen() needs is in
 * here, so a caller may keep these fields (without the name) and open the
 * member later.
 */
struct ZiprEntry {
	const char *name;	/* NOT NUL-terminated; lifetime: see below */
	size_t nameLen;
	uint64_t size;		/* uncompressed size */
	uint64_t csize;		/* compressed size */
	uint64_t offset;	/* absolute offset of the local header */
	uint64_t pos;		/* position in the central directory, for
				 * ziprEntryAt() */
	uint64_t index;		/* ordinal number, from 0 (iteration only) */
	uint32_t crc32;
	uint16_t method;	/* ZIPR_STORED, ZIPR_DEFLATED, ... */
	bool isDir;		/* name ends with '/' */
	bool encrypted;
};

/*
 * Opens the archive at @path. Reads only the end-of-central-directory
 * records, so it takes the same time for any archive size. Entries are
 * checked when they are read. In the kernel, @path is resolved with the
 * credentials of the caller.
 */
int ziprOpen(const char *path, struct Zipr **zp);

/* Closes the archive. Iterators and files must be closed first. */
void ziprClose(struct Zipr *z);

/*
 * Number of entries as recorded in the archive. Exact for ZIP64 archives
 * and after ziprIndex(); a non-ZIP64 archive with more than 65535 entries
 * records the number modulo 65536.
 */
uint64_t ziprCount(const struct Zipr *z);

/*
 * Size of the central directory in bytes. Every entry takes at least 46
 * bytes of it, which bounds the number of entries even when ziprCount()
 * is wrapped or the archive lies about it.
 */
uint64_t ziprCentralDirectorySize(const struct Zipr *z);

/* Iteration over the central directory, in archive order. */
int ziprIterOpen(struct Zipr *z, struct ZiprIter **itp);

/*
 * Fills @e with the next entry. Returns 1, 0 after the last entry, or
 * -errno if the directory is corrupted (errors are sticky).
 * e->name points into the iterator and is valid until the next call.
 */
int ziprNext(struct ZiprIter *it, struct ZiprEntry *e);

void ziprIterClose(struct ZiprIter *it);

/*
 * Reads the entry at central directory position @pos (e->pos of an entry
 * seen earlier) and copies its name to @buf, which has @size bytes.
 * e->name points to @buf. e->index is not set.
 * If @buf is NULL, the name is not copied and no size check is made:
 * e->name is NULL and e->nameLen is 0 (@size is ignored).
 */
int ziprEntryAt(struct Zipr *z, uint64_t pos, struct ZiprEntry *e,
		  char *buf, size_t size);

/*
 * Builds an in-memory index of entry names (12 bytes per entry), which
 * makes ziprFind() fast. Reads the whole central directory once.
 * Optional; calling it again does nothing.
 */
int ziprIndex(struct Zipr *z);

/*
 * Finds an entry by its exact name: 0 or -ENOENT. With an index this is a
 * binary search plus one read; without, a scan of the central directory.
 * e->name points to @name.
 */
int ziprFind(struct Zipr *z, const char *name, size_t nameLen,
	      struct ZiprEntry *e);

/* Opens the member described by @e. */
int ziprFopen(struct Zipr *z, const struct ZiprEntry *e,
	       struct ZiprFile **fp);

/*
 * Reads up to @len bytes of uncompressed data into @buf, which must be a
 * kernel buffer in the kernel. Returns the number of bytes read; fewer than
 * @len only at the end of the file, 0 at the end. Call repeatedly to read a
 * whole file.
 *
 * The CRC is checked when the last byte is produced: the call that would
 * return it fails with -EBADMSG instead. Errors are sticky.
 */
ssize_t ziprFread(struct ZiprFile *f, void *buf, size_t len);

void ziprFclose(struct ZiprFile *f);

#endif /* ZIPR_H */
