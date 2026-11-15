// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * ZipContent: builds the directory tree of a ZIP archive. See zipContent.h.
 *
 * The build has four steps:
 *
 *   1. Read the central directory with zipr. Every path component becomes
 *      an element, appended in archive order, with the index of its parent
 *      in a separate array. Directories are found again through a hash
 *      table on (parent, name); files are not hashed, so the table stays
 *      small (405 thousand directories for 9.7 million files in a real
 *      archive). The directory part of the previous entry is remembered,
 *      because archives usually list a directory's files together.
 *   2. Group children: count the children of each directory, give every
 *      directory a range of indices, and move each element to its place
 *      (a counting sort by parent, applied in place).
 *   3. Sort each range by name.
 *   4. Drop duplicates: equal names next to each other after the sort.
 */
#ifndef __KERNEL__
#define _GNU_SOURCE		/* qsort_r() */
#endif

#include "zipContent.h"
#include "zipContentPort.h"
#include "zipr.h"

#define MAX_ELEMENTS	((size_t)(uint32_t)~0u)	/* indices are 32-bit */
#define MAX_NAMES	((size_t)(uint32_t)~0u)	/* offsets are 32-bit */
#define CENTRAL_MIN	46		/* smallest central directory record */
#define CHECK_STEP	4096		/* work between resched/signal checks */
#define TABLE_MIN	1024		/* initial directory hash table slots */

_Static_assert(sizeof(struct Element) == 16, "struct Element is 16 bytes");

struct Builder {
	struct Element *elements;
	size_t count;
	size_t elementCapacity;
	/* parent of each element during step 1, its new index in step 2 */
	uint32_t *parents;
	size_t parentCapacity;
	char *names;
	size_t namesSize;
	size_t namesCapacity;
	/* directories by (parent, name): element index, 0 = empty slot */
	uint32_t *table;
	size_t tableSize;		/* a power of two */
	size_t tableUsed;
	/* the directory part of the previous entry and its element */
	char *cachedPath;
	size_t cachedLength;
	uint32_t cachedIndex;
	bool cacheValid;
	uint64_t skipped;
	/* indices of elements removed as duplicates, step 4 */
	uint32_t *dropped;
	size_t droppedCount;
	size_t droppedCapacity;
};

/*
 * Makes room for @need items in *@array, keeping the first @used.
 * Grows by half to keep the number of copies low.
 */
static int growArray(void **array, size_t *capacity, size_t used,
		     size_t need, size_t itemSize, size_t limit)
{
	size_t newCapacity, maxItems;
	void *p;

	if (need <= *capacity)
		return 0;
	maxItems = ZIP_CONTENT_MAX_ALLOC / itemSize;
	if (maxItems > limit)
		maxItems = limit;
	if (need > maxItems)
		return -EFBIG;
	newCapacity = *capacity + *capacity / 2;
	if (newCapacity < need)
		newCapacity = need;
	if (newCapacity < 64)
		newCapacity = 64;
	if (newCapacity > maxItems)
		newCapacity = maxItems;
	p = zipContentAlloc(newCapacity * itemSize);
	if (!p)
		return -ENOMEM;
	if (used)
		memcpy(p, *array, used * itemSize);
	zipContentFree(*array);
	*array = p;
	*capacity = newCapacity;
	return 0;
}

/* Gives back unused capacity if it is noticeable. Failure is harmless. */
static void shrinkArray(void **array, size_t *capacity, size_t used,
			size_t itemSize)
{
	size_t newCapacity = used ? used : 1;
	void *p;

	if (*capacity - newCapacity <= *capacity / 16)
		return;
	p = zipContentAlloc(newCapacity * itemSize);
	if (!p)
		return;
	memcpy(p, *array, used * itemSize);
	zipContentFree(*array);
	*array = p;
	*capacity = newCapacity;
}

static bool isDot(const char *name, size_t length)
{
	return length == 1 && name[0] == '.';
}

static bool isDotDot(const char *name, size_t length)
{
	return length == 2 && name[0] == '.' && name[1] == '.';
}

/* FNV-1a over the parent index and the name, with a final mix. */
static uint32_t hashName(uint32_t parent, const char *name, size_t length)
{
	uint32_t hash = 2166136261u;
	size_t i;

	for (i = 0; i < 4; i++) {
		hash ^= (parent >> (8 * i)) & 0xff;
		hash *= 16777619u;
	}
	for (i = 0; i < length; i++) {
		hash ^= (uint8_t)name[i];
		hash *= 16777619u;
	}
	hash ^= hash >> 15;
	hash *= 0x2c1b3c6du;
	hash ^= hash >> 12;
	return hash;
}

static int growTable(struct Builder *b)
{
	size_t newSize = b->tableSize ? b->tableSize * 2 : TABLE_MIN;
	size_t mask = newSize - 1, i, slot;
	const struct Element *e;
	uint32_t *table;

	if (newSize > ZIP_CONTENT_MAX_ALLOC / sizeof(*table))
		return -EFBIG;
	table = zipContentZalloc(newSize * sizeof(*table));
	if (!table)
		return -ENOMEM;
	for (i = 0; i < b->tableSize; i++) {
		if (!b->table[i])
			continue;
		e = &b->elements[b->table[i]];
		slot = hashName(b->parents[b->table[i]], b->names + e->nameOffset,
				e->nameLength) & mask;
		while (table[slot])
			slot = (slot + 1) & mask;
		table[slot] = b->table[i];
	}
	zipContentFree(b->table);
	b->table = table;
	b->tableSize = newSize;
	return 0;
}

/* Appends an element whose name is copied to the pool. */
static int addElement(struct Builder *b, uint32_t parent, const char *name,
		      size_t length, enum ElementType type, uint64_t position,
		      uint32_t *index)
{
	struct Element *e;
	int err;

	err = growArray((void **)&b->elements, &b->elementCapacity, b->count,
			b->count + 1, sizeof(*b->elements), MAX_ELEMENTS);
	if (!err)
		err = growArray((void **)&b->parents, &b->parentCapacity,
				b->count, b->count + 1, sizeof(*b->parents),
				MAX_ELEMENTS);
	if (!err)
		err = growArray((void **)&b->names, &b->namesCapacity,
				b->namesSize, b->namesSize + length, 1,
				MAX_NAMES);
	if (err)
		return err;

	e = &b->elements[b->count];
	memset(e, 0, sizeof(*e));
	if (type == ElementFile)
		e->position = position;
	e->nameOffset = (uint32_t)b->namesSize;
	e->nameLength = (uint16_t)length;
	e->type = (uint8_t)type;
	memcpy(b->names + b->namesSize, name, length);
	b->namesSize += length;
	b->parents[b->count] = parent;
	*index = (uint32_t)b->count++;
	return 0;
}

/* Finds the directory @name in @parent, creating it if needed. */
static int directoryFor(struct Builder *b, uint32_t parent, const char *name,
			size_t length, uint32_t *index)
{
	const struct Element *e;
	size_t mask, slot;
	uint32_t found;
	int err;

	if ((b->tableUsed + 1) * 2 > b->tableSize) {
		err = growTable(b);
		if (err)
			return err;
	}
	mask = b->tableSize - 1;
	slot = hashName(parent, name, length) & mask;
	while ((found = b->table[slot])) {
		e = &b->elements[found];
		if (b->parents[found] == parent && e->nameLength == length &&
		    !memcmp(b->names + e->nameOffset, name, length)) {
			*index = found;
			return 0;
		}
		slot = (slot + 1) & mask;
	}
	err = addElement(b, parent, name, length, ElementDirectory, 0, index);
	if (err)
		return err;
	b->table[slot] = *index;
	b->tableUsed++;
	return 0;
}

/*
 * Walks (and creates) the directories of @path, @length bytes ending with
 * '/' or empty. Returns 1 if the path has a ".." component.
 */
static int walkDirectories(struct Builder *b, const char *path,
			   size_t length, uint32_t *directory)
{
	uint32_t node = ZIP_CONTENT_ROOT;
	size_t start, end;
	int err;

	/* check first: "a/../b" must not leave a directory "a" behind */
	for (start = 0; start < length; start = end + 1) {
		end = start;
		while (end < length && path[end] != '/')
			end++;
		if (isDotDot(path + start, end - start))
			return 1;
	}
	start = 0;
	while (start < length) {
		end = start;
		while (end < length && path[end] != '/')
			end++;
		if (end > start && !isDot(path + start, end - start)) {
			err = directoryFor(b, node, path + start,
					   end - start, &node);
			if (err)
				return err;
		}
		start = end + 1;
	}
	*directory = node;
	return 0;
}

static int addEntry(struct Builder *b, const struct ZiprEntry *entry)
{
	const char *name = entry->name;
	size_t length = entry->nameLen, start;
	uint32_t parent, index;
	int ret;

	if (memchr(name, 0, length))
		goto skip;
	while (length && name[length - 1] == '/')
		length--;
	start = length;
	while (start && name[start - 1] != '/')
		start--;
	/* name[0, start) is the directory part, name[start, length) the last */
	if (start == length || isDot(name + start, length - start) ||
	    isDotDot(name + start, length - start))
		goto skip;

	if (b->cacheValid && b->cachedLength == start &&
	    !memcmp(b->cachedPath, name, start)) {
		parent = b->cachedIndex;
	} else {
		ret = walkDirectories(b, name, start, &parent);
		if (ret < 0)
			return ret;
		if (ret)
			goto skip;
		memcpy(b->cachedPath, name, start);
		b->cachedLength = start;
		b->cachedIndex = parent;
		b->cacheValid = true;
	}

	if (entry->isDir)
		return directoryFor(b, parent, name + start, length - start,
				    &index);
	return addElement(b, parent, name + start, length - start, ElementFile,
			  entry->pos, &index);
skip:
	b->skipped++;
	return 0;
}

static int periodicCheck(size_t *work, size_t done)
{
	*work += done;
	if (*work < CHECK_STEP)
		return 0;
	*work = 0;
	zipContentCondResched();
	return zipContentInterrupted() ? -EINTR : 0;
}

/* Step 1: reads the central directory. */
static int readEntries(struct Builder *b, struct Zipr *zip)
{
	uint64_t entries = ziprCount(zip);
	uint64_t bound = ziprCentralDirectorySize(zip) / CENTRAL_MIN;
	struct ZiprIter *iterator;
	struct ZiprEntry entry;
	size_t guess, maxGuess, work = 0;
	uint32_t root;
	int ret;

	/*
	 * Preallocate for the recorded number of entries plus a guess for
	 * directories that have no entries. The count may be wrapped or
	 * wrong; the central directory size bounds it.
	 */
	if (entries > bound)
		entries = bound;
	if (entries > MAX_ELEMENTS / 2)
		entries = MAX_ELEMENTS / 2;
	guess = (size_t)(entries + entries / 16 + 1);
	maxGuess = ZIP_CONTENT_MAX_ALLOC / sizeof(*b->elements);
	if (guess > maxGuess)
		guess = maxGuess;
	ret = growArray((void **)&b->elements, &b->elementCapacity, 0, guess,
			sizeof(*b->elements), MAX_ELEMENTS);
	if (!ret)
		ret = growArray((void **)&b->parents, &b->parentCapacity, 0,
				guess, sizeof(*b->parents), MAX_ELEMENTS);
	if (!ret)
		ret = growArray((void **)&b->names, &b->namesCapacity, 0,
				guess < MAX_NAMES / 4 ? guess * 4 : MAX_NAMES,
				1, MAX_NAMES);
	if (!ret)
		ret = growTable(b);
	if (ret)
		return ret;
	b->cachedPath = zipContentAlloc(ZIPR_NAME_MAX);
	if (!b->cachedPath)
		return -ENOMEM;

	ret = addElement(b, 0, "", 0, ElementDirectory, 0, &root);
	if (ret)
		return ret;

	ret = ziprIterOpen(zip, &iterator);
	if (ret)
		return ret;
	while ((ret = ziprNext(iterator, &entry)) > 0) {
		ret = addEntry(b, &entry);
		if (!ret)
			ret = periodicCheck(&work, 1);
		if (ret)
			break;
	}
	ziprIterClose(iterator);
	return ret;
}

/* Step 2: moves every element to its place in the range of its parent. */
static int groupChildren(struct Builder *b)
{
	size_t n = b->count, i, work = 0;
	uint32_t *cursor, next = 1;	/* the root stays at 0 */
	struct Element node, displaced;
	uint8_t *done;
	uint32_t j;
	int err = 0;

	cursor = zipContentZalloc(n * sizeof(*cursor));
	if (!cursor)
		return -ENOMEM;
	for (i = 1; i < n; i++)
		cursor[b->parents[i]]++;
	for (i = 0; i < n; i++) {
		struct Element *e = &b->elements[i];

		if (e->type != ElementDirectory)
			continue;
		e->directory.first = next;
		e->directory.count = cursor[i];
		cursor[i] = next;
		next += e->directory.count;
	}
	b->parents[0] = ZIP_CONTENT_ROOT;
	for (i = 1; i < n; i++)
		b->parents[i] = cursor[b->parents[i]]++;
	zipContentFree(cursor);

	/* Apply the permutation in place: element i goes to parents[i]. */
	done = zipContentZalloc(n / 8 + 1);
	if (!done)
		return -ENOMEM;
	for (i = 0; i < n; i++) {
		if (done[i / 8] & (1u << (i % 8)))
			continue;
		node = b->elements[i];
		j = b->parents[i];
		while (j != i) {
			displaced = b->elements[j];
			b->elements[j] = node;
			done[j / 8] |= 1u << (j % 8);
			node = displaced;
			j = b->parents[j];
		}
		b->elements[i] = node;
		done[i / 8] |= 1u << (i % 8);
		err = periodicCheck(&work, 1);
		if (err)
			break;
	}
	zipContentFree(done);
	return err;
}

static int compareBytes(const char *a, size_t aLength, const char *b,
			size_t bLength)
{
	int r = memcmp(a, b, aLength < bLength ? aLength : bLength);

	if (r)
		return r;
	if (aLength != bLength)
		return aLength < bLength ? -1 : 1;
	return 0;
}

/*
 * Order of children: by name; for equal names a directory first, then
 * files from the latest in the central directory. The first of a run of
 * equal names is the one that is kept.
 */
static int compareElements(const void *pa, const void *pb,
			   ZIP_CONTENT_SORT_CONTEXT context)
{
	const struct Element *a = pa, *b = pb;
	const char *names = context;
	int r;

	r = compareBytes(names + a->nameOffset, a->nameLength,
			 names + b->nameOffset, b->nameLength);
	if (r)
		return r;
	if (a->type != b->type)
		return a->type == ElementDirectory ? -1 : 1;
	if (a->type == ElementFile && a->position != b->position)
		return a->position > b->position ? -1 : 1;
	return 0;
}

static int compareIndices(const void *pa, const void *pb,
			  ZIP_CONTENT_SORT_CONTEXT context)
{
	uint32_t a = *(const uint32_t *)pa, b = *(const uint32_t *)pb;

	(void)context;
	return a < b ? -1 : a > b;
}

/* Steps 3 and 4: sorts every range and records the duplicates. */
static int sortChildren(struct Builder *b)
{
	const struct Element *e, *x, *y;
	size_t i, k, work = 0;
	uint32_t first, count;
	int err;

	for (i = 0; i < b->count; i++) {
		e = &b->elements[i];
		if (e->type != ElementDirectory || e->directory.count < 2)
			continue;
		first = e->directory.first;
		count = e->directory.count;
		zipContentSort(&b->elements[first], count, sizeof(*e),
			       compareElements, b->names);
		for (k = first + 1; k < (size_t)first + count; k++) {
			x = &b->elements[k - 1];
			y = &b->elements[k];
			if (compareBytes(b->names + x->nameOffset, x->nameLength,
					 b->names + y->nameOffset,
					 y->nameLength))
				continue;
			err = growArray((void **)&b->dropped,
					&b->droppedCapacity, b->droppedCount,
					b->droppedCount + 1,
					sizeof(*b->dropped), MAX_ELEMENTS);
			if (err)
				return err;
			b->dropped[b->droppedCount++] = (uint32_t)k;
		}
		err = periodicCheck(&work, count);
		if (err)
			return err;
	}
	return 0;
}

/* Number of dropped elements with an index below @index. */
static size_t droppedBefore(const struct Builder *b, size_t index)
{
	size_t low = 0, high = b->droppedCount, middle;

	while (low < high) {
		middle = low + (high - low) / 2;
		if (b->dropped[middle] < index)
			low = middle + 1;
		else
			high = middle;
	}
	return low;
}

/* Removes the dropped elements and fixes the ranges of directories. */
static void compact(struct Builder *b)
{
	size_t read, write = 0, next = 0, before, inside;
	struct Element e;

	zipContentSort(b->dropped, b->droppedCount, sizeof(*b->dropped),
		       compareIndices, NULL);
	for (read = 0; read < b->count; read++) {
		if (next < b->droppedCount && b->dropped[next] == read) {
			next++;
			continue;
		}
		e = b->elements[read];
		if (e.type == ElementDirectory) {
			before = droppedBefore(b, e.directory.first);
			inside = droppedBefore(b, (size_t)e.directory.first +
						  e.directory.count) - before;
			e.directory.first -= (uint32_t)before;
			e.directory.count -= (uint32_t)inside;
		}
		b->elements[write++] = e;
	}
	b->count = write;
}

static void freeBuilder(struct Builder *b)
{
	zipContentFree(b->elements);
	zipContentFree(b->parents);
	zipContentFree(b->names);
	zipContentFree(b->table);
	zipContentFree(b->cachedPath);
	zipContentFree(b->dropped);
}

int zipContentBuild(struct ZipContent *content, struct Zipr *zip)
{
	struct Builder b;
	size_t i;
	int err;

	memset(content, 0, sizeof(*content));
	memset(&b, 0, sizeof(b));

	err = readEntries(&b, zip);
	if (err)
		goto fail;
	/* not needed any more: free before the next steps allocate */
	zipContentFree(b.table);
	zipContentFree(b.cachedPath);
	b.table = NULL;
	b.cachedPath = NULL;

	err = groupChildren(&b);
	if (err)
		goto fail;
	zipContentFree(b.parents);
	b.parents = NULL;

	err = sortChildren(&b);
	if (err)
		goto fail;
	if (b.droppedCount)
		compact(&b);

	shrinkArray((void **)&b.elements, &b.elementCapacity, b.count,
		    sizeof(*b.elements));
	shrinkArray((void **)&b.names, &b.namesCapacity, b.namesSize, 1);

	content->elements = b.elements;
	content->elementCount = (uint32_t)b.count;
	content->names = b.names;
	content->namesSize = (uint32_t)b.namesSize;
	content->skipped = b.skipped;
	content->duplicates = b.droppedCount;
	for (i = 0; i < b.count; i++) {
		if (b.elements[i].type == ElementDirectory)
			content->directoryCount++;
		else
			content->fileCount++;
	}
	zipContentFree(b.dropped);
	return 0;

fail:
	freeBuilder(&b);
	return err;
}

void zipContentDestroy(struct ZipContent *content)
{
	zipContentFree(content->elements);
	zipContentFree(content->names);
	memset(content, 0, sizeof(*content));
}

int zipContentLookup(const struct ZipContent *content,
		     const struct Element *directory,
		     const char *name, size_t length,
		     const struct Element **found)
{
	const struct Element *e;
	size_t low, high, middle;
	int r;

	if (directory->type != ElementDirectory)
		return -ENOTDIR;
	low = directory->directory.first;
	high = low + directory->directory.count;
	while (low < high) {
		middle = low + (high - low) / 2;
		e = &content->elements[middle];
		r = compareBytes(content->names + e->nameOffset, e->nameLength,
				 name, length);
		if (!r) {
			*found = e;
			return 0;
		}
		if (r < 0)
			low = middle + 1;
		else
			high = middle;
	}
	return -ENOENT;
}

int zipContentFind(const struct ZipContent *content, const char *path,
		   size_t length, const struct Element **found)
{
	const struct Element *node = zipContentRoot(content);
	size_t start = 0, end;
	int err;

	while (start < length) {
		end = start;
		while (end < length && path[end] != '/')
			end++;
		if (end > start && !isDot(path + start, end - start)) {
			err = zipContentLookup(content, node, path + start,
					       end - start, &node);
			if (err)
				return err;
		}
		start = end + 1;
	}
	if (length && path[length - 1] == '/' &&
	    node->type != ElementDirectory)
		return -ENOTDIR;
	*found = node;
	return 0;
}
