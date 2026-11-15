// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * zipr: read-only ZIP archives. See zipr.h for the API.
 *
 * The parts of a ZIP file this code uses:
 *
 *   [prefix] {local header, data}... central directory
 *            [Zip64 EOCD record, Zip64 EOCD locator] EOCD [comment]
 *
 * ziprOpen() reads only the EOCD and the Zip64 records. The central
 * directory, which for millions of entries is gigabytes long, is never held
 * in memory: iterators stream it through a fixed buffer and check each
 * entry as they decode it, and ziprEntryAt() reads single entries. The
 * optional index keeps a 32-bit name hash and the directory position of
 * every entry, sorted by hash.
 *
 * Every offset and size taken from the archive is checked against the file
 * before it is used.
 */
#ifndef __KERNEL__
#define _POSIX_C_SOURCE 200809L	/* pread(), O_CLOEXEC */
#endif

#include "zipr.h"
#include "ziprPort.h"

#define SIG_LOCAL	0x04034b50	/* PK\3\4 */
#define SIG_CENTRAL	0x02014b50	/* PK\1\2 */
#define SIG_EOCD	0x06054b50	/* PK\5\6 */
#define SIG_EOCD64	0x06064b50	/* PK\6\6 */
#define SIG_LOC64	0x07064b50	/* PK\6\7 */

#define LOCAL_SIZE	30
#define CENTRAL_SIZE	46
#define EOCD_SIZE	22
#define EOCD64_SIZE	56
#define LOC64_SIZE	20
#define MAX_COMMENT	0xffff

#define EXTRA_ZIP64	0x0001
#define FLAG_ENCRYPTED	0x0001

/* Iterator buffer: holds the largest entry, 46 + 3 * 65535 bytes. */
#define ITER_BUF	(256u << 10)
#define ENTRY_STACK	256		/* ziprEntryAt() reads this much at once */
#define INDEX_STEP	4096		/* entries between resched/signal checks */
#define INBUF_SIZE	(16u << 10)	/* compressed input chunk for inflate */
#define MAX_READ	0x7ffff000u	/* largest read, as MAX_RW_COUNT */

/* Index slot: 12 bytes, so 10 million entries take 120 MB. */
struct ZiprIx {
	uint32_t hash;
	uint32_t posLo;
	uint32_t posHi;
};

struct Zipr {
	ZiprOSFile file;
	uint64_t fileSize;
	uint64_t base;		/* bytes before the archive (SFX stub etc.) */
	uint64_t cdirOffset;	/* absolute offset of the central directory */
	uint64_t cdirSize;
	uint64_t count;		/* entries, as recorded or as counted */
	bool zip64;
	struct ZiprIx *ix;	/* sorted by hash, then position */
	size_t ixCount;
};

struct ZiprIter {
	struct Zipr *z;
	uint8_t *buf;		/* ITER_BUF bytes */
	size_t start;		/* unparsed bytes are buf[start..end) */
	size_t end;
	uint64_t pos;		/* directory position of buf[start] */
	uint64_t index;
	int err;		/* sticky error */
	bool done;
};

struct ZiprFile {
	struct Zipr *z;
	uint64_t pos;		/* absolute offset of the next compressed byte */
	uint64_t cleft;		/* compressed bytes not read yet */
	uint64_t uleft;		/* uncompressed bytes not returned yet */
	uint32_t crc;		/* CRC of the data returned so far */
	uint32_t expectCRC;
	uint16_t method;
	int err;		/* sticky error */
	bool finished;		/* end of data checked */
#ifdef ZIPR_HAVE_DEFLATE
	bool zsEnd;		/* the deflate stream has ended */
	z_stream zs;
	uint8_t *inbuf;
#endif
};

/* Fields from the EOCD, overridden by the Zip64 EOCD record if present. */
struct EOCDInfo {
	uint64_t total;		/* entries in the central directory */
	uint64_t diskEntries;	/* entries on this disk */
	uint64_t cdSize;
	uint64_t cdOff;	/* as stored, i.e. without the prefix */
	uint64_t cdirEnd;	/* absolute offset where the directory ends */
	uint32_t disk;
	uint32_t cdDisk;
	bool zip64;
};

static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
	       (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t le64(const uint8_t *p)
{
	return le32(p) | (uint64_t)le32(p + 4) << 32;
}

static uint64_t minU64(uint64_t a, uint64_t b)
{
	return a < b ? a : b;
}

/* Reads exactly @len bytes at @off. */
static int readExact(ZiprOSFile file, void *buf, size_t len, uint64_t off)
{
	uint8_t *p = buf;
	ssize_t n;

	while (len) {
		n = ziprOSPread(file, p, len, off);
		if (n < 0)
			return (int)n;
		if (n == 0)
			return -EIO;	/* the file shrank under us */
		p += n;
		len -= (size_t)n;
		off += (uint64_t)n;
	}
	return 0;
}

/*
 * Finds the EOCD in the last 22 + 65535 bytes. A comment may contain the
 * signature too, so a record whose comment ends exactly at the end of the
 * file wins over one that merely fits.
 */
static int findEOCD(struct Zipr *z, uint8_t *eocd, uint64_t *eocdOff)
{
	size_t tail, i, end;
	uint8_t *buf;
	int err;

	if (z->fileSize < EOCD_SIZE)
		return -EINVAL;
	tail = (size_t)minU64(z->fileSize, EOCD_SIZE + MAX_COMMENT);
	buf = ziprAlloc(tail);
	if (!buf)
		return -ENOMEM;
	err = readExact(z->file, buf, tail, z->fileSize - tail);
	if (err)
		goto out;

	err = -EINVAL;
	for (i = tail - EOCD_SIZE + 1; i-- > 0;) {
		if (le32(buf + i) != SIG_EOCD)
			continue;
		end = i + EOCD_SIZE + le16(buf + i + 20);
		if (end > tail)
			continue;
		if (err || end == tail) {
			memcpy(eocd, buf + i, EOCD_SIZE);
			*eocdOff = z->fileSize - tail + i;
			err = 0;
		}
		if (end == tail)
			break;
	}
out:
	ziprFree(buf);
	return err;
}

/* Replaces @info with the Zip64 EOCD record if the archive has one. */
static int readZip64EOCD(struct Zipr *z, uint64_t eocdOff,
			   struct EOCDInfo *info)
{
	uint8_t loc[LOC64_SIZE], rec[EOCD64_SIZE];
	uint64_t locOff, recOff;
	int err;

	if (eocdOff < LOC64_SIZE)
		return 0;
	locOff = eocdOff - LOC64_SIZE;
	err = readExact(z->file, loc, LOC64_SIZE, locOff);
	if (err)
		return err;
	if (le32(loc) != SIG_LOC64)
		return 0;		/* not a Zip64 archive */
	if (le32(loc + 4) != 0 || le32(loc + 16) > 1)
		return -EOPNOTSUPP;	/* multi-disk */

	/* The record lies before the locator. With a prefix it is not where
	 * the locator says, which is not supported. */
	recOff = le64(loc + 8);
	if (locOff < EOCD64_SIZE || recOff > locOff - EOCD64_SIZE)
		return -EINVAL;
	err = readExact(z->file, rec, EOCD64_SIZE, recOff);
	if (err)
		return err;
	if (le32(rec) != SIG_EOCD64)
		return -EINVAL;

	info->disk = le32(rec + 16);
	info->cdDisk = le32(rec + 20);
	info->diskEntries = le64(rec + 24);
	info->total = le64(rec + 32);
	info->cdSize = le64(rec + 40);
	info->cdOff = le64(rec + 48);
	info->cdirEnd = recOff;
	info->zip64 = true;
	return 0;
}

int ziprOpen(const char *path, struct Zipr **zp)
{
	uint8_t eocd[EOCD_SIZE];
	struct EOCDInfo info = { 0 };
	uint64_t eocdOff, cdirStart;
	struct Zipr *z;
	int err;

	z = ziprZalloc(sizeof(*z));
	if (!z)
		return -ENOMEM;
	err = ziprOSOpen(path, &z->file, &z->fileSize);
	if (err) {
		ziprFree(z);
		return err;
	}

	err = findEOCD(z, eocd, &eocdOff);
	if (err)
		goto fail;
	info.disk = le16(eocd + 4);
	info.cdDisk = le16(eocd + 6);
	info.diskEntries = le16(eocd + 8);
	info.total = le16(eocd + 10);
	info.cdSize = le32(eocd + 12);
	info.cdOff = le32(eocd + 16);
	info.cdirEnd = eocdOff;
	err = readZip64EOCD(z, eocdOff, &info);
	if (err)
		goto fail;

	err = -EOPNOTSUPP;
	if (info.disk != 0 || info.cdDisk != 0 ||
	    info.diskEntries != info.total)
		goto fail;

	/* Where the directory really is tells the size of any prefix. */
	err = -EINVAL;
	if (info.cdSize > info.cdirEnd)
		goto fail;
	cdirStart = info.cdirEnd - info.cdSize;
	if (info.cdOff > cdirStart)
		goto fail;
	z->base = cdirStart - info.cdOff;
	z->cdirOffset = cdirStart;
	z->cdirSize = info.cdSize;
	z->count = info.total;
	z->zip64 = info.zip64;

	*zp = z;
	return 0;

fail:
	ziprClose(z);
	return err;
}

void ziprClose(struct Zipr *z)
{
	if (!z)
		return;
	ziprOSClose(z->file);
	ziprFree(z->ix);
	ziprFree(z);
}

uint64_t ziprCount(const struct Zipr *z)
{
	return z->count;
}

uint64_t ziprCentralDirectorySize(const struct Zipr *z)
{
	return z->cdirSize;
}

/* Applies the Zip64 extra field to the 32-bit fields that overflowed. */
static int parseZip64Extra(const uint8_t *x, size_t xlen,
			     struct ZiprEntry *e)
{
	uint16_t id, sz;
	const uint8_t *d;

	while (xlen >= 4) {
		id = le16(x);
		sz = le16(x + 2);
		if (sz > xlen - 4)
			return -EINVAL;
		if (id == EXTRA_ZIP64) {
			/* Only overflowed fields are present, in this order. */
			d = x + 4;
			if (e->size == 0xffffffff) {
				if (sz < 8)
					return -EINVAL;
				e->size = le64(d);
				d += 8;
				sz -= 8;
			}
			if (e->csize == 0xffffffff) {
				if (sz < 8)
					return -EINVAL;
				e->csize = le64(d);
				d += 8;
				sz -= 8;
			}
			if (e->offset == 0xffffffff) {
				if (sz < 8)
					return -EINVAL;
				e->offset = le64(d);
			}
			return 0;
		}
		x += 4 + sz;
		xlen -= 4 + (size_t)sz;
	}
	return 0;
}

/*
 * Decodes the central directory entry at @p. The fixed part, the name and
 * the extra field must be among the @avail bytes at @p; the whole entry,
 * comment included, must fit in the @room bytes left in the directory.
 * Sets *@len to the size of the whole entry.
 */
static int parseEntry(const struct Zipr *z, const uint8_t *p, size_t avail,
		       uint64_t room, struct ZiprEntry *e, size_t *len)
{
	size_t nlen, xlen, clen;
	uint64_t cdRel;
	int err;

	if (avail < CENTRAL_SIZE || le32(p) != SIG_CENTRAL)
		return -EINVAL;
	nlen = le16(p + 28);
	xlen = le16(p + 30);
	clen = le16(p + 32);
	*len = CENTRAL_SIZE + nlen + xlen + clen;
	if (*len > room || CENTRAL_SIZE + nlen + xlen > avail)
		return -EINVAL;

	e->encrypted = le16(p + 8) & FLAG_ENCRYPTED;
	e->method = le16(p + 10);
	e->crc32 = le32(p + 16);
	e->csize = le32(p + 20);
	e->size = le32(p + 24);
	e->offset = le32(p + 42);
	e->name = (const char *)p + CENTRAL_SIZE;
	e->nameLen = nlen;
	e->isDir = nlen && e->name[nlen - 1] == '/';

	err = parseZip64Extra(p + CENTRAL_SIZE + nlen, xlen, e);
	if (err)
		return err;

	/* The local header must lie before the central directory. */
	cdRel = z->cdirOffset - z->base;
	if (cdRel < LOCAL_SIZE || e->offset > cdRel - LOCAL_SIZE)
		return -EINVAL;
	e->offset += z->base;
	return 0;
}

/* Does the number of entries seen match the one recorded? */
static bool countOk(const struct Zipr *z, uint64_t n)
{
	/* Without Zip64 the 16-bit count wraps; unzip accepts that too. */
	return n == z->count || (!z->zip64 && (n & 0xffff) == z->count);
}

int ziprIterOpen(struct Zipr *z, struct ZiprIter **itp)
{
	struct ZiprIter *it;

	it = ziprZalloc(sizeof(*it));
	if (!it)
		return -ENOMEM;
	it->buf = ziprAlloc(ITER_BUF);
	if (!it->buf) {
		ziprFree(it);
		return -ENOMEM;
	}
	it->z = z;
	*itp = it;
	return 0;
}

void ziprIterClose(struct ZiprIter *it)
{
	if (!it)
		return;
	ziprFree(it->buf);
	ziprFree(it);
}

/* Makes at least @need bytes available, unless the directory ends first. */
static int iterFill(struct ZiprIter *it, size_t need)
{
	const struct Zipr *z = it->z;
	size_t have = it->end - it->start;
	uint64_t next = it->pos + have;
	size_t n;
	int err;

	if (have >= need)
		return 0;
	memmove(it->buf, it->buf + it->start, have);
	it->start = 0;
	it->end = have;
	n = (size_t)minU64(ITER_BUF - have, z->cdirSize - next);
	if (!n)
		return 0;
	err = readExact(z->file, it->buf + have, n, z->cdirOffset + next);
	if (err)
		return err;
	it->end += n;
	return 0;
}

int ziprNext(struct ZiprIter *it, struct ZiprEntry *e)
{
	const uint8_t *p;
	size_t avail, len;
	int err;

	if (it->err)
		return it->err;
	if (it->done)
		return 0;

	err = iterFill(it, CENTRAL_SIZE);
	if (err)
		return it->err = err;
	p = it->buf + it->start;
	avail = it->end - it->start;
	if (avail < 4 || le32(p) != SIG_CENTRAL) {
		/* No more entries. Data may follow them inside the directory
		 * (a digital signature, for one), so only the count tells
		 * whether the directory is complete. */
		it->done = true;
		if (!countOk(it->z, it->index))
			return it->err = -EINVAL;
		return 0;
	}
	if (avail < CENTRAL_SIZE)
		return it->err = -EINVAL;

	len = CENTRAL_SIZE + le16(p + 28) + le16(p + 30) + le16(p + 32);
	err = iterFill(it, len);
	if (err)
		return it->err = err;
	err = parseEntry(it->z, it->buf + it->start, it->end - it->start,
			  it->z->cdirSize - it->pos, e, &len);
	if (err)
		return it->err = err;

	e->pos = it->pos;
	e->index = it->index++;
	it->start += len;
	it->pos += len;
	return 1;
}

/*
 * Reads the entry at directory position @pos. With @want, an entry with a
 * different name gives -ENOENT and e->name is set to @want; otherwise the
 * name is copied to @buf. Without @want and with @buf == NULL the name is
 * not copied: e->name is NULL and e->nameLen is 0.
 */
static int entryAt(struct Zipr *z, uint64_t pos, struct ZiprEntry *e,
		    const char *want, size_t wantLen, char *buf, size_t size)
{
	uint8_t small[ENTRY_STACK], *p = small;
	size_t chunk, nlen, need, len;
	uint64_t off;
	int err;

	if (pos > z->cdirSize || z->cdirSize - pos < CENTRAL_SIZE)
		return -EINVAL;
	off = z->cdirOffset + pos;
	chunk = (size_t)minU64(sizeof(small), z->cdirSize - pos);
	err = readExact(z->file, small, chunk, off);
	if (err)
		return err;
	if (le32(small) != SIG_CENTRAL)
		return -EINVAL;

	nlen = le16(small + 28);
	if (want ? nlen != wantLen : (buf && nlen > size))
		return want ? -ENOENT : -ENAMETOOLONG;

	/* The fixed part, name and extra field; the comment is not needed. */
	need = CENTRAL_SIZE + nlen + le16(small + 30);
	if (need > chunk) {
		if (need > z->cdirSize - pos)
			return -EINVAL;
		p = ziprAlloc(need);
		if (!p)
			return -ENOMEM;
		memcpy(p, small, chunk);
		err = readExact(z->file, p + chunk, need - chunk, off + chunk);
		if (err)
			goto out;
	}

	err = -ENOENT;
	if (want && memcmp(p + CENTRAL_SIZE, want, nlen) != 0)
		goto out;
	err = parseEntry(z, p, need, z->cdirSize - pos, e, &len);
	if (err)
		goto out;
	if (want) {
		e->name = want;
	} else if (buf) {
		memcpy(buf, p + CENTRAL_SIZE, nlen);
		e->name = buf;
	} else {
		e->name = NULL;
		e->nameLen = 0;
	}
	e->pos = pos;
	e->index = 0;
out:
	if (p != small)
		ziprFree(p);
	return err;
}

int ziprEntryAt(struct Zipr *z, uint64_t pos, struct ZiprEntry *e,
		  char *buf, size_t size)
{
	return entryAt(z, pos, e, NULL, 0, buf, size);
}

/* FNV-1a, 32 bits. */
static uint32_t nameHash(const char *name, size_t len)
{
	const uint8_t *p = (const uint8_t *)name;
	uint32_t h = 2166136261u;

	while (len--) {
		h ^= *p++;
		h *= 16777619u;
	}
	return h;
}

static int ixCmp(const void *a, const void *b)
{
	const struct ZiprIx *x = a, *y = b;

	if (x->hash != y->hash)
		return x->hash < y->hash ? -1 : 1;
	if (x->posHi != y->posHi)
		return x->posHi < y->posHi ? -1 : 1;
	if (x->posLo != y->posLo)
		return x->posLo < y->posLo ? -1 : 1;
	return 0;
}

int ziprIndex(struct Zipr *z)
{
	struct ZiprEntry e;
	struct ZiprIter *it;
	struct ZiprIx *ix;
	uint64_t cap;
	size_t n = 0;
	int ret;

	if (z->ix)
		return 0;

	/* Every entry takes at least CENTRAL_SIZE bytes. A Zip64 archive
	 * records the exact count; otherwise it may have wrapped. */
	cap = z->cdirSize / CENTRAL_SIZE;
	if (z->zip64) {
		if (z->count > cap)
			return -EINVAL;
		cap = z->count;
	}
	if (cap > ZIPR_MAX_ALLOC / sizeof(*ix))
		return -EFBIG;
	ix = ziprAlloc(cap ? (size_t)cap * sizeof(*ix) : 1);
	if (!ix)
		return -ENOMEM;

	ret = ziprIterOpen(z, &it);
	if (ret)
		goto fail;
	while ((ret = ziprNext(it, &e)) > 0) {
		if (n == cap) {
			ret = -EINVAL;	/* more entries than recorded */
			break;
		}
		ix[n].hash = nameHash(e.name, e.nameLen);
		ix[n].posLo = (uint32_t)e.pos;
		ix[n].posHi = (uint32_t)(e.pos >> 32);
		if (++n % INDEX_STEP == 0) {
			ziprCondResched();
			if (ziprInterrupted()) {
				ret = -EINTR;
				break;
			}
		}
	}
	ziprIterClose(it);
	if (ret < 0)
		goto fail;

	ziprSort(ix, n, sizeof(*ix), ixCmp);
	z->ix = ix;
	z->ixCount = n;
	z->count = n;
	return 0;

fail:
	ziprFree(ix);
	return ret;
}

static int findIndexed(struct Zipr *z, const char *name, size_t nameLen,
			struct ZiprEntry *e)
{
	uint32_t h = nameHash(name, nameLen);
	size_t lo = 0, hi = z->ixCount, mid;
	uint64_t pos;
	int err;

	while (lo < hi) {
		mid = lo + (hi - lo) / 2;
		if (z->ix[mid].hash < h)
			lo = mid + 1;
		else
			hi = mid;
	}
	/* Equal hashes are rare; each candidate costs one read. */
	for (; lo < z->ixCount && z->ix[lo].hash == h; lo++) {
		pos = (uint64_t)z->ix[lo].posHi << 32 | z->ix[lo].posLo;
		err = entryAt(z, pos, e, name, nameLen, NULL, 0);
		if (err != -ENOENT)
			return err;
	}
	return -ENOENT;
}

int ziprFind(struct Zipr *z, const char *name, size_t nameLen,
	      struct ZiprEntry *e)
{
	struct ZiprIter *it;
	int ret;

	if (z->ix)
		return findIndexed(z, name, nameLen, e);

	ret = ziprIterOpen(z, &it);
	if (ret)
		return ret;
	while ((ret = ziprNext(it, e)) > 0) {
		if (e->nameLen == nameLen &&
		    memcmp(e->name, name, nameLen) == 0) {
			e->name = name;	/* the iterator's copy goes away */
			break;
		}
	}
	ziprIterClose(it);
	return ret > 0 ? 0 : ret < 0 ? ret : -ENOENT;
}

int ziprFopen(struct Zipr *z, const struct ZiprEntry *e,
	       struct ZiprFile **fp)
{
	uint8_t lh[LOCAL_SIZE];
	struct ZiprFile *f;
	uint64_t data;
	int err;

	if (e->encrypted)
		return -EOPNOTSUPP;
	switch (e->method) {
	case ZIPR_STORED:
		if (e->csize != e->size)
			return -EINVAL;
		break;
#ifdef ZIPR_HAVE_DEFLATE
	case ZIPR_DEFLATED:
		break;
#endif
	default:
		return -EOPNOTSUPP;
	}

	/* The local header has its own name and extra lengths; the data
	 * follows them and must end before the central directory. */
	err = readExact(z->file, lh, LOCAL_SIZE, e->offset);
	if (err)
		return err;
	if (le32(lh) != SIG_LOCAL)
		return -EINVAL;
	data = e->offset + LOCAL_SIZE + le16(lh + 26) + le16(lh + 28);
	if (data > z->cdirOffset || e->csize > z->cdirOffset - data)
		return -EINVAL;

	f = ziprZalloc(sizeof(*f));
	if (!f)
		return -ENOMEM;
	f->z = z;
	f->pos = data;
	f->cleft = e->csize;
	f->uleft = e->size;
	f->expectCRC = e->crc32;
	f->method = e->method;

#ifdef ZIPR_HAVE_DEFLATE
	if (f->method == ZIPR_DEFLATED) {
		f->inbuf = ziprAlloc(INBUF_SIZE);
		err = -ENOMEM;
		if (!f->inbuf)
			goto fail;
		err = ziprInflateInit(&f->zs);
		if (err)
			goto fail;
	}
#endif
	*fp = f;
	return 0;

#ifdef ZIPR_HAVE_DEFLATE
fail:
	ziprFree(f->inbuf);
	ziprFree(f);
	return err;
#endif
}

void ziprFclose(struct ZiprFile *f)
{
	if (!f)
		return;
#ifdef ZIPR_HAVE_DEFLATE
	if (f->method == ZIPR_DEFLATED) {
		ziprInflateEnd(&f->zs);
		ziprFree(f->inbuf);
	}
#endif
	ziprFree(f);
}

static ssize_t readStored(struct ZiprFile *f, uint8_t *buf, size_t len)
{
	int err = readExact(f->z->file, buf, len, f->pos);

	if (err)
		return err;
	f->pos += len;
	f->cleft -= len;
	return (ssize_t)len;
}

#ifdef ZIPR_HAVE_DEFLATE
/* Refills the input buffer once inflate has consumed it. */
static int refill(struct ZiprFile *f)
{
	size_t n;
	int err;

	if (f->zs.avail_in || !f->cleft)
		return 0;
	n = (size_t)minU64(f->cleft, INBUF_SIZE);
	err = readExact(f->z->file, f->inbuf, n, f->pos);
	if (err)
		return err;
	f->pos += n;
	f->cleft -= n;
	f->zs.next_in = f->inbuf;
	f->zs.avail_in = (unsigned int)n;
	return 0;
}

/* Inflates into @buf until it is full or the stream ends. */
static ssize_t inflateSome(struct ZiprFile *f, uint8_t *buf, size_t len)
{
	int err;

	f->zs.next_out = buf;
	f->zs.avail_out = (unsigned int)len;	/* len <= MAX_READ */
	while (f->zs.avail_out && !f->zsEnd) {
		err = refill(f);
		if (err)
			return err;
		switch (ziprInflate(&f->zs)) {
		case Z_OK:
			break;
		case Z_STREAM_END:
			f->zsEnd = true;
			break;
		case Z_MEM_ERROR:
			return -ENOMEM;
		default:	/* Z_DATA_ERROR, Z_NEED_DICT, Z_BUF_ERROR */
			return -EBADMSG;	/* Z_BUF_ERROR: input ran out */
		}
	}
	return (ssize_t)(len - f->zs.avail_out);
}

static ssize_t readDeflated(struct ZiprFile *f, uint8_t *buf, size_t len)
{
	ssize_t n = inflateSome(f, buf, len);

	if (n >= 0 && (size_t)n < len)
		return -EBADMSG;	/* shorter than the declared size */
	return n;
}
#endif /* ZIPR_HAVE_DEFLATE */

/* Called once all declared bytes are returned: the data must end here. */
static int finish(struct ZiprFile *f)
{
#ifdef ZIPR_HAVE_DEFLATE
	uint8_t extra;
	ssize_t n;

	if (f->method == ZIPR_DEFLATED && !f->zsEnd) {
		n = inflateSome(f, &extra, 1);
		if (n < 0)
			return (int)n;
		if (n > 0 || !f->zsEnd)
			return -EBADMSG;	/* longer than declared */
	}
#endif
	f->finished = true;
	return f->crc == f->expectCRC ? 0 : -EBADMSG;
}

ssize_t ziprFread(struct ZiprFile *f, void *buf, size_t len)
{
	ssize_t n = 0;
	int err;

	if (f->err)
		return f->err;
	if (len > MAX_READ)
		len = MAX_READ;
	if (len > f->uleft)
		len = (size_t)f->uleft;

	if (len) {
#ifdef ZIPR_HAVE_DEFLATE
		if (f->method == ZIPR_DEFLATED)
			n = readDeflated(f, buf, len);
		else
#endif
			n = readStored(f, buf, len);
		if (n < 0)
			return f->err = (int)n;
		f->crc = ziprCRC32(f->crc, buf, (size_t)n);
		f->uleft -= (uint64_t)n;
	}

	if (!f->uleft && !f->finished) {
		err = finish(f);
		if (err)
			return f->err = err;
	}
	return n;
}
