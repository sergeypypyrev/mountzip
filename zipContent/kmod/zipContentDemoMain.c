// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * zipContentDemo: builds the tree of a ZIP archive, prints its size and
 * the build time in dmesg, the top-level names, and optionally looks a
 * path up.
 *
 *   insmod zipContentDemo.ko zip=/path/a.zip [find=dir/name.txt] [top=20]
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/err.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/string.h>

#include "../zipContent.h"
#include "zipr.h"

static char *zip;
module_param(zip, charp, 0444);
MODULE_PARM_DESC(zip, "Absolute path to a ZIP archive");

static char *find;
module_param(find, charp, 0444);
MODULE_PARM_DESC(find, "Path to look up");

static unsigned int top = 20;
module_param(top, uint, 0444);
MODULE_PARM_DESC(top, "How many top-level names to print (default 20)");

static void printTop(const struct ZipContent *content)
{
	const struct Element *root = zipContentRoot(content);
	const struct Element *child = zipContentChildren(content, root);
	uint32_t i;

	for (i = 0; i < root->directory.count && i < top; i++)
		pr_info("  %.*s%s\n", (int)child[i].nameLength,
			zipContentName(content, &child[i]),
			zipContentIsDirectory(&child[i]) ? "/" : "");
	if (root->directory.count > top)
		pr_info("  ... %u more\n", root->directory.count - top);
}

static void findPath(const struct ZipContent *content, struct Zipr *z)
{
	const struct Element *e;
	struct ZiprEntry entry;
	char *name;
	int err;

	err = zipContentFind(content, find, strlen(find), &e);
	if (err) {
		pr_info("find %s: %pe\n", find, ERR_PTR(err));
		return;
	}
	if (zipContentIsDirectory(e)) {
		pr_info("find %s: directory, %u children\n", find,
			e->directory.count);
		return;
	}
	name = kvmalloc(ZIPR_NAME_MAX, GFP_KERNEL);
	if (!name)
		return;
	err = ziprEntryAt(z, e->position, &entry, name, ZIPR_NAME_MAX);
	if (err)
		pr_info("find %s: %pe\n", find, ERR_PTR(err));
	else
		pr_info("find %s: file, %llu bytes, entry %.*s\n", find,
			entry.size, (int)min_t(size_t, entry.nameLen, 200),
			entry.name);
	kvfree(name);
}

static int __init zipContentDemoInit(void)
{
	struct ZipContent content;
	struct Zipr *z;
	ktime_t start;
	int err;

	if (!zip) {
		pr_err("zip= is required\n");
		return -EINVAL;
	}
	err = ziprOpen(zip, &z);
	if (err) {
		pr_err("%s: %pe\n", zip, ERR_PTR(err));
		return err;
	}
	start = ktime_get();
	err = zipContentBuild(&content, z);
	if (err) {
		pr_err("%s: %pe\n", zip, ERR_PTR(err));
		ziprClose(z);
		return err;
	}
	pr_info("%s: %u directories, %u files, %llu skipped, %llu duplicates\n",
		zip, content.directoryCount, content.fileCount,
		content.skipped, content.duplicates);
	pr_info("memory %zu KiB (elements %zu KiB, names %u KiB), built in %lld ms\n",
		((size_t)content.elementCount * sizeof(struct Element) +
		 content.namesSize) >> 10,
		((size_t)content.elementCount * sizeof(struct Element)) >> 10,
		content.namesSize >> 10, ktime_ms_delta(ktime_get(), start));
	printTop(&content);
	if (find)
		findPath(&content, z);

	zipContentDestroy(&content);
	ziprClose(z);
	return 0;
}

static void __exit zipContentDemoExit(void)
{
}

module_init(zipContentDemoInit);
module_exit(zipContentDemoExit);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("ZipContent demo: the directory tree of a ZIP archive");
