// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * ziprDemo: lists the first entries of a ZIP archive in dmesg, reading
 * each of those files and checking its CRC, and optionally looks a name
 * up through the index.
 *
 *   insmod ziprDemo.ko zip=/path/a.zip [list=20] [find=dir/name.txt]
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/err.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "../zipr.h"

#define BUF_SIZE	(64 << 10)

static char *zip;
module_param(zip, charp, 0444);
MODULE_PARM_DESC(zip, "Absolute path to a ZIP archive");

static unsigned int list = 20;
module_param(list, uint, 0444);
MODULE_PARM_DESC(list, "How many entries to list and check (default 20)");

static char *find;
module_param(find, charp, 0444);
MODULE_PARM_DESC(find, "Name to look up through the index");

static int readEntry(struct Zipr *z, const struct ZiprEntry *e, void *buf)
{
	struct ZiprFile *f;
	ssize_t n;
	int err;

	err = ziprFopen(z, e, &f);
	if (err)
		return err;
	while ((n = ziprFread(f, buf, BUF_SIZE)) > 0)
		cond_resched();
	ziprFclose(f);
	return n < 0 ? (int)n : 0;
}

static void listEntries(struct Zipr *z, void *buf)
{
	struct ZiprEntry e;
	struct ZiprIter *it;
	unsigned int shown = 0;
	int ret, err;

	ret = ziprIterOpen(z, &it);
	if (ret) {
		pr_err("iterator: %pe\n", ERR_PTR(ret));
		return;
	}
	while (shown < list && (ret = ziprNext(it, &e)) > 0) {
		shown++;
		if (e.isDir) {
			pr_info("%14s  %.*s\n", "<dir>", (int)e.nameLen,
				e.name);
			continue;
		}
		err = readEntry(z, &e, buf);
		if (err)
			pr_info("%14llu  %.*s: %pe\n", e.size, (int)e.nameLen,
				e.name, ERR_PTR(err));
		else
			pr_info("%14llu  %.*s: ok\n", e.size, (int)e.nameLen,
				e.name);
	}
	if (ret < 0)
		pr_err("central directory: %pe\n", ERR_PTR(ret));
	ziprIterClose(it);
}

static void findEntry(struct Zipr *z, void *buf)
{
	struct ZiprEntry e;
	ktime_t t;
	int err;

	t = ktime_get();
	err = ziprIndex(z);
	if (err) {
		pr_err("index: %pe\n", ERR_PTR(err));
		return;
	}
	pr_info("index of %llu entries built in %lld ms\n", ziprCount(z),
		ktime_ms_delta(ktime_get(), t));

	err = ziprFind(z, find, strlen(find), &e);
	if (!err)
		err = readEntry(z, &e, buf);
	if (err)
		pr_info("find %s: %pe\n", find, ERR_PTR(err));
	else
		pr_info("find %s: %llu bytes at %llu, ok\n", find, e.size,
			e.offset);
}

static int __init ziprDemoInit(void)
{
	struct Zipr *z;
	void *buf;
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
	buf = kvmalloc(BUF_SIZE, GFP_KERNEL);
	if (!buf) {
		ziprClose(z);
		return -ENOMEM;
	}

	pr_info("%s: %llu entries recorded\n", zip, ziprCount(z));
	listEntries(z, buf);
	if (find)
		findEntry(z, buf);

	kvfree(buf);
	ziprClose(z);
	return 0;
}

static void __exit ziprDemoExit(void)
{
}

module_init(ziprDemoInit);
module_exit(ziprDemoExit);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("zipr demo: list, check and look up entries of a ZIP archive");
