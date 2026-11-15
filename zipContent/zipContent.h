/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * ZipContent: the directory tree of a ZIP archive in compact form, for a
 * file system over the archive. Builds in user space and in the Linux
 * kernel; the archive is read with zipr.
 *
 * The tree is two arrays:
 *
 *   elements  16 bytes per file or directory. elements[0] is the root.
 *             The children of a directory occupy consecutive elements,
 *             sorted by name, so a lookup is a binary search and readdir
 *             is a walk over a range.
 *   names     one pool with the last component of every path ("00" for
 *             "spot/ZRX/ETH/2022-12-31/00"), not NUL-terminated.
 *
 * Directories that the archive has no entry for ("a/" when there is only
 * "a/b.txt") are created. A file keeps its position in the central
 * directory, from which ziprEntryAt() reads its size, offset and full name.
 *
 * Paths are cleaned up while the tree is built:
 *   - empty and "." components are dropped: "/a//./b" is "a/b";
 *   - entries with a ".." component or a NUL byte in the name are skipped;
 *   - when several entries have the same path, a directory wins over
 *     files and a later file (in central directory order) over an earlier
 *     one. The hidden entries are counted in ZipContent.duplicates.
 *
 * Errors are negative errno values:
 *   -ENOENT   lookup: no such name
 *   -ENOTDIR  lookup: a path component is a file
 *   -EFBIG    more elements or name bytes than the 32-bit fields hold, or
 *             than one allocation may take (INT_MAX bytes in the kernel)
 *   -EINTR    the build was interrupted by a fatal signal (kernel)
 *   -ENOMEM   out of memory
 *   anything that zipr returns while reading the central directory
 *
 * After zipContentBuild() the content is only read: any number of threads
 * may use it at once. In the kernel zipContentBuild() sleeps.
 */
#ifndef ZIP_CONTENT_H
#define ZIP_CONTENT_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#endif

struct Zipr;

/* Index of the root directory in ZipContent.elements. */
#define ZIP_CONTENT_ROOT	0u

enum ElementType {
	ElementFile = 0,
	ElementDirectory = 1,
};

struct Element {
	union {
		/* ElementFile: ZiprEntry.pos, for ziprEntryAt() */
		uint64_t position;
		/* ElementDirectory: children are elements[first, first + count) */
		struct {
			uint32_t first;
			uint32_t count;
		} directory;
	};
	uint32_t nameOffset;	/* in ZipContent.names */
	uint16_t nameLength;
	uint8_t type;		/* enum ElementType */
};

struct ZipContent {
	struct Element *elements;
	uint32_t elementCount;		/* including the root */
	uint32_t directoryCount;	/* including the root */
	uint32_t fileCount;
	char *names;
	uint32_t namesSize;		/* bytes used in names */
	uint64_t skipped;		/* entries with unusable names */
	uint64_t duplicates;		/* entries hidden by another one */
};

/*
 * Reads the central directory of @zip and builds the tree in @content.
 * Memory: 16 bytes per element plus the names, and during the build about
 * 8 more bytes per element. On error @content is left empty.
 */
int zipContentBuild(struct ZipContent *content, struct Zipr *zip);

/* Frees the arrays; @content may be empty. */
void zipContentDestroy(struct ZipContent *content);

/*
 * Finds @name (@length bytes, a single component without '/') among the
 * children of @directory.
 */
int zipContentLookup(const struct ZipContent *content,
		     const struct Element *directory,
		     const char *name, size_t length,
		     const struct Element **found);

/*
 * Finds a path relative to the root. Empty and "." components are
 * ignored, so "", "/" and "." find the root. A trailing '/' requires a
 * directory (-ENOTDIR otherwise).
 */
int zipContentFind(const struct ZipContent *content, const char *path,
		   size_t length, const struct Element **found);

static inline const struct Element *
zipContentRoot(const struct ZipContent *content)
{
	return &content->elements[ZIP_CONTENT_ROOT];
}

static inline const struct Element *
zipContentElement(const struct ZipContent *content, uint32_t index)
{
	return &content->elements[index];
}

/* Index of @element, e.g. for an inode number. */
static inline uint32_t zipContentIndex(const struct ZipContent *content,
				       const struct Element *element)
{
	return (uint32_t)(element - content->elements);
}

static inline bool zipContentIsDirectory(const struct Element *element)
{
	return element->type == ElementDirectory;
}

/* The name of @element, element->nameLength bytes, no NUL at the end. */
static inline const char *zipContentName(const struct ZipContent *content,
					 const struct Element *element)
{
	return content->names + element->nameOffset;
}

/* The first child of @directory; there are directory.count of them. */
static inline const struct Element *
zipContentChildren(const struct ZipContent *content,
		   const struct Element *directory)
{
	return &content->elements[directory->directory.first];
}

#endif /* ZIP_CONTENT_H */
