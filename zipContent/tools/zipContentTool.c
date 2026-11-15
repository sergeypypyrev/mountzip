// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * zipContentTool: command-line front end for ZipContent.
 *
 *   zipContentTool l ARCHIVE           every path, depth first, sorted;
 *                                      directories end with '/'
 *   zipContentTool s ARCHIVE           statistics, memory and build time
 *   zipContentTool d ARCHIVE [DIR]     children of a directory
 *   zipContentTool f ARCHIVE PATH...   look paths up
 *
 * Exit status: 0 success, 1 error, 2 usage.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "zipContent.h"
#include "zipr.h"

#define PATH_BUFFER	(ZIPR_NAME_MAX + 2)
#define MAX_DEPTH	(ZIPR_NAME_MAX / 2 + 2)

struct Frame {
	uint32_t next;		/* next child to print */
	uint32_t end;
	size_t pathLength;	/* length of the directory's path with '/' */
};

static int fail(const char *what, int err)
{
	fprintf(stderr, "zipContentTool: %s: %s\n", what, strerror(-err));
	return 1;
}

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* Prints every path under the root, depth first. */
static int list(const struct ZipContent *content)
{
	const struct Element *root = zipContentRoot(content), *e;
	struct Frame *stack, *top;
	char *path;
	int depth = 0;

	stack = malloc(MAX_DEPTH * sizeof(*stack));
	path = malloc(PATH_BUFFER);
	if (!stack || !path) {
		free(stack);
		free(path);
		return fail("list", -ENOMEM);
	}
	stack[0].next = root->directory.first;
	stack[0].end = root->directory.first + root->directory.count;
	stack[0].pathLength = 0;
	while (depth >= 0) {
		top = &stack[depth];
		if (top->next == top->end) {
			depth--;
			continue;
		}
		e = zipContentElement(content, top->next++);
		memcpy(path + top->pathLength, zipContentName(content, e),
		       e->nameLength);
		if (!zipContentIsDirectory(e)) {
			fwrite(path, 1, top->pathLength + e->nameLength,
			       stdout);
			putchar('\n');
			continue;
		}
		path[top->pathLength + e->nameLength] = '/';
		fwrite(path, 1, top->pathLength + e->nameLength + 1, stdout);
		putchar('\n');
		stack[depth + 1].next = e->directory.first;
		stack[depth + 1].end = e->directory.first + e->directory.count;
		stack[depth + 1].pathLength = top->pathLength + e->nameLength + 1;
		depth++;
	}
	free(stack);
	free(path);
	return 0;
}

static void statistics(const struct ZipContent *content, double seconds)
{
	uint64_t elementBytes = (uint64_t)content->elementCount *
				sizeof(struct Element);

	printf("elements     %" PRIu32 "\n", content->elementCount);
	printf("directories  %" PRIu32 "\n", content->directoryCount);
	printf("files        %" PRIu32 "\n", content->fileCount);
	printf("skipped      %" PRIu64 "\n", content->skipped);
	printf("duplicates   %" PRIu64 "\n", content->duplicates);
	printf("elements     %.1f MB\n", (double)elementBytes / 1e6);
	printf("names        %.1f MB\n", (double)content->namesSize / 1e6);
	printf("total        %.1f MB\n",
	       (double)(elementBytes + content->namesSize) / 1e6);
	printf("build        %.2f s\n", seconds);
}

static void printChild(const struct ZipContent *content,
		       const struct Element *e)
{
	if (zipContentIsDirectory(e))
		printf("%.*s/\n", (int)e->nameLength,
		       zipContentName(content, e));
	else
		printf("%.*s\n", (int)e->nameLength,
		       zipContentName(content, e));
}

static int children(const struct ZipContent *content, const char *directory)
{
	const struct Element *e, *child;
	uint32_t i;
	int err;

	err = zipContentFind(content, directory, strlen(directory), &e);
	if (err)
		return fail(directory, err);
	if (!zipContentIsDirectory(e))
		return fail(directory, -ENOTDIR);
	child = zipContentChildren(content, e);
	for (i = 0; i < e->directory.count; i++)
		printChild(content, &child[i]);
	return 0;
}

/* Looks paths up; for files checks ziprEntryAt() against the element. */
static int find(const struct ZipContent *content, struct Zipr *zip,
		char **paths, int count)
{
	static char name[ZIPR_NAME_MAX];
	const struct Element *e;
	struct ZiprEntry entry;
	int i, err, ret = 0;

	for (i = 0; i < count; i++) {
		err = zipContentFind(content, paths[i], strlen(paths[i]), &e);
		if (err) {
			ret = fail(paths[i], err);
			continue;
		}
		if (zipContentIsDirectory(e)) {
			printf("directory %s: %" PRIu32 " children, index %"
			       PRIu32 "\n", paths[i], e->directory.count,
			       zipContentIndex(content, e));
			continue;
		}
		err = ziprEntryAt(zip, e->position, &entry, name, sizeof(name));
		if (!err && (entry.nameLen < e->nameLength ||
			     memcmp(entry.name + entry.nameLen - e->nameLength,
				    zipContentName(content, e),
				    e->nameLength)))
			err = -EINVAL;	/* the position is wrong */
		if (err) {
			ret = fail(paths[i], err);
			continue;
		}
		printf("file %s: %" PRIu64 " bytes, index %" PRIu32
		       ", entry %.*s\n", paths[i], entry.size,
		       zipContentIndex(content, e), (int)entry.nameLen,
		       entry.name);
	}
	return ret;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: zipContentTool l|s ARCHIVE\n"
		"       zipContentTool d ARCHIVE [DIR]\n"
		"       zipContentTool f ARCHIVE PATH...\n");
	exit(2);
}

int main(int argc, char **argv)
{
	struct ZipContent content;
	struct Zipr *zip;
	double start;
	int err, ret;
	char command;

	if (argc < 3 || strlen(argv[1]) != 1 || !strchr("lsdf", argv[1][0]))
		usage();
	command = argv[1][0];
	if ((strchr("ls", command) && argc != 3) ||
	    (command == 'd' && argc > 4) || (command == 'f' && argc < 4))
		usage();

	err = ziprOpen(argv[2], &zip);
	if (err)
		return fail(argv[2], err);
	start = now();
	err = zipContentBuild(&content, zip);
	if (err) {
		ziprClose(zip);
		return fail(argv[2], err);
	}

	switch (command) {
	case 'l':
		ret = list(&content);
		break;
	case 's':
		statistics(&content, now() - start);
		ret = 0;
		break;
	case 'd':
		ret = children(&content, argc == 4 ? argv[3] : "");
		break;
	default:
		ret = find(&content, zip, argv + 3, argc - 3);
		break;
	}
	if (fflush(stdout))
		ret = fail("stdout", -errno);
	zipContentDestroy(&content);
	ziprClose(zip);
	return ret;
}
