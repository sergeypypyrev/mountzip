// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * ziprTool: command-line front end for zipr.
 *
 *   ziprTool [-i] [-b SIZE] l ARCHIVE         list entries
 *   ziprTool [-i] [-b SIZE] n ARCHIVE         count entries by iterating
 *   ziprTool [-i] [-b SIZE] t ARCHIVE         read every file, check its CRC
 *   ziprTool [-i] [-b SIZE] x ARCHIVE NAME    write NAME to stdout
 *   ziprTool [-i] [-b SIZE] f ARCHIVE NAME... look names up
 *
 * -i builds the name index first, so x and f do a binary search instead of
 * a scan. -b sets the read buffer size (default 64 KiB); small values make
 * each file be read in many ziprFread() calls.
 *
 * Exit status: 0 success, 1 error, 2 usage.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../zipr.h"

static size_t bufsize = 64 << 10;

static const char *methodName(uint16_t method)
{
	switch (method) {
	case ZIPR_STORED:
		return "stored";
	case ZIPR_DEFLATED:
		return "deflate";
	case 9:
		return "deflate64";
	case 12:
		return "bzip2";
	case 14:
		return "lzma";
	case 93:
		return "zstd";
	default:
		return "other";
	}
}

static int fail(const char *what, int err)
{
	fprintf(stderr, "ziprTool: %s: %s\n", what, strerror(-err));
	return 1;
}

static void printEntry(const struct ZiprEntry *e)
{
	printf("%12" PRIu64 " %12" PRIu64 "  %-9s %08" PRIx32 "  %.*s%s\n",
	       e->size, e->csize, methodName(e->method), e->crc32,
	       (int)e->nameLen, e->name, e->encrypted ? "  (encrypted)" : "");
}

/* Iterates over all entries, printing them if @print. */
static int walk(struct Zipr *z, const char *path, int print)
{
	struct ZiprEntry e;
	struct ZiprIter *it;
	uint64_t n = 0;
	int ret;

	ret = ziprIterOpen(z, &it);
	if (ret)
		return fail(path, ret);
	if (print)
		printf("%12s %12s  %-9s %-8s  %s\n",
		       "size", "csize", "method", "crc32", "name");
	while ((ret = ziprNext(it, &e)) > 0) {
		if (print)
			printEntry(&e);
		n++;
	}
	ziprIterClose(it);
	if (ret < 0)
		return fail(path, ret);
	printf("%" PRIu64 " entries\n", n);
	return 0;
}

/* Reads one member, writing it to @out unless it is NULL. */
static int readEntry(struct Zipr *z, const struct ZiprEntry *e, FILE *out)
{
	struct ZiprFile *f;
	ssize_t n;
	char *buf;
	int err;

	err = ziprFopen(z, e, &f);
	if (err)
		return err;
	buf = malloc(bufsize);
	if (!buf) {
		ziprFclose(f);
		return -ENOMEM;
	}
	while ((n = ziprFread(f, buf, bufsize)) > 0) {
		if (out && fwrite(buf, 1, (size_t)n, out) != (size_t)n) {
			n = -EIO;
			break;
		}
	}
	free(buf);
	ziprFclose(f);
	return n < 0 ? (int)n : 0;
}

static int test(struct Zipr *z, const char *path)
{
	struct ZiprEntry e;
	struct ZiprIter *it;
	int ret, err, failed = 0;

	ret = ziprIterOpen(z, &it);
	if (ret)
		return fail(path, ret);
	while ((ret = ziprNext(it, &e)) > 0) {
		if (e.isDir)
			continue;
		err = readEntry(z, &e, NULL);
		printf("%-4s %.*s%s%s\n", err ? "FAIL" : "ok",
		       (int)e.nameLen, e.name,
		       err ? ": " : "", err ? strerror(-err) : "");
		if (err)
			failed++;
	}
	ziprIterClose(it);
	if (ret < 0)
		return fail(path, ret);
	return failed ? 1 : 0;
}

static int extract(struct Zipr *z, const char *name)
{
	struct ZiprEntry e;
	int err;

	err = ziprFind(z, name, strlen(name), &e);
	if (!err)
		err = readEntry(z, &e, stdout);
	if (!err && fflush(stdout))
		err = -errno;
	return err ? fail(name, err) : 0;
}

/* Looks names up and checks that ziprEntryAt() finds the same entry. */
static int find(struct Zipr *z, char **names, int count)
{
	static char buf[ZIPR_NAME_MAX];
	struct ZiprEntry e, again;
	int i, err, ret = 0;

	for (i = 0; i < count; i++) {
		err = ziprFind(z, names[i], strlen(names[i]), &e);
		if (!err)
			err = ziprEntryAt(z, e.pos, &again, buf, sizeof(buf));
		if (!err && (again.nameLen != e.nameLen ||
			     memcmp(again.name, e.name, e.nameLen) ||
			     again.offset != e.offset || again.size != e.size))
			err = -EINVAL;
		if (err) {
			ret = fail(names[i], err);
			continue;
		}
		printEntry(&e);
	}
	return ret;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: ziprTool [-i] [-b SIZE] l|n|t ARCHIVE\n"
		"       ziprTool [-i] [-b SIZE] x ARCHIVE NAME\n"
		"       ziprTool [-i] [-b SIZE] f ARCHIVE NAME...\n");
	exit(2);
}

int main(int argc, char **argv)
{
	int opt, err, ret, index = 0;
	const char *path;
	struct Zipr *z;
	char cmd;

	while ((opt = getopt(argc, argv, "b:i")) != -1) {
		switch (opt) {
		case 'b':
			bufsize = strtoul(optarg, NULL, 0);
			if (!bufsize)
				usage();
			break;
		case 'i':
			index = 1;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;
	if (argc < 2 || strlen(argv[0]) != 1 || !strchr("lntxf", argv[0][0]))
		usage();
	cmd = argv[0][0];
	path = argv[1];
	if ((cmd == 'x' && argc != 3) || (cmd == 'f' && argc < 3) ||
	    (strchr("lnt", cmd) && argc != 2))
		usage();

	err = ziprOpen(path, &z);
	if (err)
		return fail(path, err);
	if (index) {
		err = ziprIndex(z);
		if (err) {
			ziprClose(z);
			return fail(path, err);
		}
	}

	switch (cmd) {
	case 'l':
	case 'n':
		ret = walk(z, path, cmd == 'l');
		break;
	case 't':
		ret = test(z, path);
		break;
	case 'x':
		ret = extract(z, argv[2]);
		break;
	default:
		ret = find(z, argv + 2, argc - 2);
		break;
	}
	ziprClose(z);
	return ret;
}
