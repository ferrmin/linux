// SPDX-License-Identifier: GPL-2.0
/*
 * CMA DebugFS Interface
 *
 * Copyright (c) 2015 Sasha Levin <sasha.levin@oracle.com>
 */


#include <linux/debugfs.h>
#include <linux/cma.h>
#include <linux/list.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mm_types.h>

#include "cma.h"

struct cma_mem {
	struct hlist_node node;
	struct page *p;
	unsigned long n;
};

static int cma_debugfs_get(void *data, u64 *val)
{
	unsigned long *p = data;

	*val = *p;

	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(cma_debugfs_fops, cma_debugfs_get, NULL, "%llu\n");

static int cma_used_get(void *data, u64 *val)
{
	struct cma *cma = data;

	spin_lock_irq(&cma->lock);
	*val = cma->count - cma->available_count;
	spin_unlock_irq(&cma->lock);

	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(cma_used_fops, cma_used_get, NULL, "%llu\n");

static int cma_maxchunk_get(void *data, u64 *val)
{
	struct cma *cma = data;
	struct cma_memrange *cmr;
	unsigned long maxchunk = 0;
	unsigned long start, end;
	unsigned long bitmap_maxno;
	int r;

	spin_lock_irq(&cma->lock);
	for (r = 0; r < cma->nranges; r++) {
		cmr = &cma->ranges[r];
		bitmap_maxno = cma_bitmap_maxno(cma, cmr);
		for_each_clear_bitrange(start, end, cmr->bitmap, bitmap_maxno)
			maxchunk = max(end - start, maxchunk);
	}
	spin_unlock_irq(&cma->lock);
	*val = (u64)maxchunk << cma->order_per_bit;

	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(cma_maxchunk_fops, cma_maxchunk_get, NULL, "%llu\n");

static void cma_add_to_cma_mem_list(struct cma *cma, struct cma_mem *mem)
{
	spin_lock(&cma->mem_head_lock);
	hlist_add_head(&mem->node, &cma->mem_head);
	spin_unlock(&cma->mem_head_lock);
}

static struct cma_mem *cma_get_entry_from_list(struct cma *cma)
{
	struct cma_mem *mem = NULL;

	spin_lock(&cma->mem_head_lock);
	if (!hlist_empty(&cma->mem_head)) {
		mem = hlist_entry(cma->mem_head.first, struct cma_mem, node);
		hlist_del_init(&mem->node);
	}
	spin_unlock(&cma->mem_head_lock);

	return mem;
}

static int cma_free_mem(struct cma *cma, int count)
{
	struct cma_mem *mem = NULL;

	while (count) {
		mem = cma_get_entry_from_list(cma);
		if (mem == NULL)
			return 0;

		if (mem->n <= count) {
			cma_release(cma, mem->p, mem->n);
			count -= mem->n;
			kfree(mem);
		} else if (cma->order_per_bit == 0) {
			cma_release(cma, mem->p, count);
			mem->p += count;
			mem->n -= count;
			count = 0;
			cma_add_to_cma_mem_list(cma, mem);
		} else {
			pr_debug("cma: cannot release partial block when order_per_bit != 0\n");
			cma_add_to_cma_mem_list(cma, mem);
			break;
		}
	}

	return 0;

}

static int cma_free_write(void *data, u64 val)
{
	int pages = val;
	struct cma *cma = data;

	return cma_free_mem(cma, pages);
}
DEFINE_DEBUGFS_ATTRIBUTE(cma_free_fops, NULL, cma_free_write, "%llu\n");

static int cma_alloc_mem(struct cma *cma, int count)
{
	struct cma_mem *mem;
	struct page *p;

	mem = kzalloc(sizeof(*mem), GFP_KERNEL);
	if (!mem)
		return -ENOMEM;

	p = cma_alloc(cma, count, 0, false);
	if (!p) {
		kfree(mem);
		return -ENOMEM;
	}

	mem->p = p;
	mem->n = count;

	cma_add_to_cma_mem_list(cma, mem);

	return 0;
}

static int cma_alloc_write(void *data, u64 val)
{
	int pages = val;
	struct cma *cma = data;

	return cma_alloc_mem(cma, pages);
}
DEFINE_DEBUGFS_ATTRIBUTE(cma_alloc_fops, NULL, cma_alloc_write, "%llu\n");

static void cma_debugfs_add_one(struct cma *cma, struct dentry *root_dentry)
{
	struct dentry *tmp, *dir, *rangedir;
	int r;
	char rdirname[12];
	struct cma_memrange *cmr;

	tmp = debugfs_create_dir(cma->name, root_dentry);

	debugfs_create_file("alloc", 0200, tmp, cma, &cma_alloc_fops);
	debugfs_create_file("free", 0200, tmp, cma, &cma_free_fops);
	debugfs_create_file("count", 0444, tmp, &cma->count, &cma_debugfs_fops);
	debugfs_create_file("order_per_bit", 0444, tmp,
			    &cma->order_per_bit, &cma_debugfs_fops);
	debugfs_create_file("used", 0444, tmp, cma, &cma_used_fops);
	debugfs_create_file("maxchunk", 0444, tmp, cma, &cma_maxchunk_fops);

	rangedir = debugfs_create_dir("ranges", tmp);
	for (r = 0; r < cma->nranges; r++) {
		cmr = &cma->ranges[r];
		snprintf(rdirname, sizeof(rdirname), "%d", r);
		dir = debugfs_create_dir(rdirname, rangedir);
		debugfs_create_file("base_pfn", 0444, dir,
			    &cmr->base_pfn, &cma_debugfs_fops);
		cmr->dfs_bitmap.array = (u32 *)cmr->bitmap;
		cmr->dfs_bitmap.n_elements =
			DIV_ROUND_UP(cma_bitmap_maxno(cma, cmr),
					BITS_PER_BYTE * sizeof(u32));
		debugfs_create_u32_array("bitmap", 0444, dir,
				&cmr->dfs_bitmap);
	}

	/*
	 * Backward compatible symlinks to range 0 for base_pfn and bitmap.
	 */
	debugfs_create_symlink("base_pfn", tmp, "ranges/0/base_pfn");
	debugfs_create_symlink("bitmap", tmp, "ranges/0/bitmap");
}

static int __init cma_debugfs_init(void)
{
	struct dentry *cma_debugfs_root;
	int i;

	cma_debugfs_root = debugfs_create_dir("cma", NULL);

	for (i = 0; i < cma_area_count; i++)
		cma_debugfs_add_one(&cma_areas[i], cma_debugfs_root);

	return 0;
}
late_initcall(cma_debugfs_init);
