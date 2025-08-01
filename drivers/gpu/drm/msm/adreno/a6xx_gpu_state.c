// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2018-2019 The Linux Foundation. All rights reserved. */

#include <linux/ascii85.h>
#include "msm_gem.h"
#include "a6xx_gpu.h"
#include "a6xx_gmu.h"
#include "a6xx_gpu_state.h"
#include "a6xx_gmu.xml.h"

static const unsigned int *gen7_0_0_external_core_regs[] __always_unused;
static const unsigned int *gen7_2_0_external_core_regs[] __always_unused;
static const unsigned int *gen7_9_0_external_core_regs[] __always_unused;
static struct gen7_sptp_cluster_registers gen7_9_0_sptp_clusters[] __always_unused;
static const u32 gen7_9_0_cx_debugbus_blocks[] __always_unused;

#include "adreno_gen7_0_0_snapshot.h"
#include "adreno_gen7_2_0_snapshot.h"
#include "adreno_gen7_9_0_snapshot.h"

struct a6xx_gpu_state_obj {
	const void *handle;
	u32 *data;
	u32 count;	/* optional, used when count potentially read from hw */
};

struct a6xx_gpu_state {
	struct msm_gpu_state base;

	struct a6xx_gpu_state_obj *gmu_registers;
	int nr_gmu_registers;

	struct a6xx_gpu_state_obj *registers;
	int nr_registers;

	struct a6xx_gpu_state_obj *shaders;
	int nr_shaders;

	struct a6xx_gpu_state_obj *clusters;
	int nr_clusters;

	struct a6xx_gpu_state_obj *dbgahb_clusters;
	int nr_dbgahb_clusters;

	struct a6xx_gpu_state_obj *indexed_regs;
	int nr_indexed_regs;

	struct a6xx_gpu_state_obj *debugbus;
	int nr_debugbus;

	struct a6xx_gpu_state_obj *vbif_debugbus;

	struct a6xx_gpu_state_obj *cx_debugbus;
	int nr_cx_debugbus;

	struct msm_gpu_state_bo *gmu_log;
	struct msm_gpu_state_bo *gmu_hfi;
	struct msm_gpu_state_bo *gmu_debug;

	s32 hfi_queue_history[2][HFI_HISTORY_SZ];

	struct list_head objs;

	bool gpu_initialized;
};

static inline int CRASHDUMP_WRITE(u64 *in, u32 reg, u32 val)
{
	in[0] = val;
	in[1] = (((u64) reg) << 44 | (1 << 21) | 1);

	return 2;
}

static inline int CRASHDUMP_READ(u64 *in, u32 reg, u32 dwords, u64 target)
{
	in[0] = target;
	in[1] = (((u64) reg) << 44 | dwords);

	return 2;
}

static inline int CRASHDUMP_FINI(u64 *in)
{
	in[0] = 0;
	in[1] = 0;

	return 2;
}

struct a6xx_crashdumper {
	void *ptr;
	struct drm_gem_object *bo;
	u64 iova;
};

struct a6xx_state_memobj {
	struct list_head node;
	unsigned long long data[];
};

static void *state_kcalloc(struct a6xx_gpu_state *a6xx_state, int nr, size_t objsize)
{
	struct a6xx_state_memobj *obj =
		kvzalloc((nr * objsize) + sizeof(*obj), GFP_KERNEL);

	if (!obj)
		return NULL;

	list_add_tail(&obj->node, &a6xx_state->objs);
	return &obj->data;
}

static void *state_kmemdup(struct a6xx_gpu_state *a6xx_state, void *src,
		size_t size)
{
	void *dst = state_kcalloc(a6xx_state, 1, size);

	if (dst)
		memcpy(dst, src, size);
	return dst;
}

/*
 * Allocate 1MB for the crashdumper scratch region - 8k for the script and
 * the rest for the data
 */
#define A6XX_CD_DATA_OFFSET 8192
#define A6XX_CD_DATA_SIZE  (SZ_1M - 8192)

static int a6xx_crashdumper_init(struct msm_gpu *gpu,
		struct a6xx_crashdumper *dumper)
{
	dumper->ptr = msm_gem_kernel_new(gpu->dev,
		SZ_1M, MSM_BO_WC, gpu->vm,
		&dumper->bo, &dumper->iova);

	if (!IS_ERR(dumper->ptr))
		msm_gem_object_set_name(dumper->bo, "crashdump");

	return PTR_ERR_OR_ZERO(dumper->ptr);
}

static int a6xx_crashdumper_run(struct msm_gpu *gpu,
		struct a6xx_crashdumper *dumper)
{
	struct adreno_gpu *adreno_gpu = to_adreno_gpu(gpu);
	struct a6xx_gpu *a6xx_gpu = to_a6xx_gpu(adreno_gpu);
	u32 val;
	int ret;

	if (IS_ERR_OR_NULL(dumper->ptr))
		return -EINVAL;

	if (!a6xx_gmu_sptprac_is_on(&a6xx_gpu->gmu))
		return -EINVAL;

	/* Make sure all pending memory writes are posted */
	wmb();

	gpu_write64(gpu, REG_A6XX_CP_CRASH_DUMP_SCRIPT_BASE, dumper->iova);

	gpu_write(gpu, REG_A6XX_CP_CRASH_DUMP_CNTL, 1);

	ret = gpu_poll_timeout(gpu, REG_A6XX_CP_CRASH_DUMP_STATUS, val,
		val & 0x02, 100, 10000);

	gpu_write(gpu, REG_A6XX_CP_CRASH_DUMP_CNTL, 0);

	return ret;
}

/* read a value from the GX debug bus */
static int debugbus_read(struct msm_gpu *gpu, u32 block, u32 offset,
		u32 *data)
{
	u32 reg = A6XX_DBGC_CFG_DBGBUS_SEL_D_PING_INDEX(offset) |
		A6XX_DBGC_CFG_DBGBUS_SEL_D_PING_BLK_SEL(block);

	gpu_write(gpu, REG_A6XX_DBGC_CFG_DBGBUS_SEL_A, reg);
	gpu_write(gpu, REG_A6XX_DBGC_CFG_DBGBUS_SEL_B, reg);
	gpu_write(gpu, REG_A6XX_DBGC_CFG_DBGBUS_SEL_C, reg);
	gpu_write(gpu, REG_A6XX_DBGC_CFG_DBGBUS_SEL_D, reg);

	/* Wait 1 us to make sure the data is flowing */
	udelay(1);

	data[0] = gpu_read(gpu, REG_A6XX_DBGC_CFG_DBGBUS_TRACE_BUF2);
	data[1] = gpu_read(gpu, REG_A6XX_DBGC_CFG_DBGBUS_TRACE_BUF1);

	return 2;
}

#define cxdbg_write(ptr, offset, val) \
	writel((val), (ptr) + ((offset) << 2))

#define cxdbg_read(ptr, offset) \
	readl((ptr) + ((offset) << 2))

/* read a value from the CX debug bus */
static int cx_debugbus_read(void __iomem *cxdbg, u32 block, u32 offset,
		u32 *data)
{
	u32 reg = A6XX_CX_DBGC_CFG_DBGBUS_SEL_A_PING_INDEX(offset) |
		A6XX_CX_DBGC_CFG_DBGBUS_SEL_A_PING_BLK_SEL(block);

	cxdbg_write(cxdbg, REG_A6XX_CX_DBGC_CFG_DBGBUS_SEL_A, reg);
	cxdbg_write(cxdbg, REG_A6XX_CX_DBGC_CFG_DBGBUS_SEL_B, reg);
	cxdbg_write(cxdbg, REG_A6XX_CX_DBGC_CFG_DBGBUS_SEL_C, reg);
	cxdbg_write(cxdbg, REG_A6XX_CX_DBGC_CFG_DBGBUS_SEL_D, reg);

	/* Wait 1 us to make sure the data is flowing */
	udelay(1);

	data[0] = cxdbg_read(cxdbg, REG_A6XX_CX_DBGC_CFG_DBGBUS_TRACE_BUF2);
	data[1] = cxdbg_read(cxdbg, REG_A6XX_CX_DBGC_CFG_DBGBUS_TRACE_BUF1);

	return 2;
}

/* Read a chunk of data from the VBIF debug bus */
static int vbif_debugbus_read(struct msm_gpu *gpu, u32 ctrl0, u32 ctrl1,
		u32 reg, int count, u32 *data)
{
	int i;

	gpu_write(gpu, ctrl0, reg);

	for (i = 0; i < count; i++) {
		gpu_write(gpu, ctrl1, i);
		data[i] = gpu_read(gpu, REG_A6XX_VBIF_TEST_BUS_OUT);
	}

	return count;
}

#define AXI_ARB_BLOCKS 2
#define XIN_AXI_BLOCKS 5
#define XIN_CORE_BLOCKS 4

#define VBIF_DEBUGBUS_BLOCK_SIZE \
	((16 * AXI_ARB_BLOCKS) + \
	 (18 * XIN_AXI_BLOCKS) + \
	 (12 * XIN_CORE_BLOCKS))

static void a6xx_get_vbif_debugbus_block(struct msm_gpu *gpu,
		struct a6xx_gpu_state *a6xx_state,
		struct a6xx_gpu_state_obj *obj)
{
	u32 clk, *ptr;
	int i;

	obj->data = state_kcalloc(a6xx_state, VBIF_DEBUGBUS_BLOCK_SIZE,
		sizeof(u32));
	if (!obj->data)
		return;

	obj->handle = NULL;

	/* Get the current clock setting */
	clk = gpu_read(gpu, REG_A6XX_VBIF_CLKON);

	/* Force on the bus so we can read it */
	gpu_write(gpu, REG_A6XX_VBIF_CLKON,
		clk | A6XX_VBIF_CLKON_FORCE_ON_TESTBUS);

	/* We will read from BUS2 first, so disable BUS1 */
	gpu_write(gpu, REG_A6XX_VBIF_TEST_BUS1_CTRL0, 0);

	/* Enable the VBIF bus for reading */
	gpu_write(gpu, REG_A6XX_VBIF_TEST_BUS_OUT_CTRL, 1);

	ptr = obj->data;

	for (i = 0; i < AXI_ARB_BLOCKS; i++)
		ptr += vbif_debugbus_read(gpu,
			REG_A6XX_VBIF_TEST_BUS2_CTRL0,
			REG_A6XX_VBIF_TEST_BUS2_CTRL1,
			1 << (i + 16), 16, ptr);

	for (i = 0; i < XIN_AXI_BLOCKS; i++)
		ptr += vbif_debugbus_read(gpu,
			REG_A6XX_VBIF_TEST_BUS2_CTRL0,
			REG_A6XX_VBIF_TEST_BUS2_CTRL1,
			1 << i, 18, ptr);

	/* Stop BUS2 so we can turn on BUS1 */
	gpu_write(gpu, REG_A6XX_VBIF_TEST_BUS2_CTRL0, 0);

	for (i = 0; i < XIN_CORE_BLOCKS; i++)
		ptr += vbif_debugbus_read(gpu,
			REG_A6XX_VBIF_TEST_BUS1_CTRL0,
			REG_A6XX_VBIF_TEST_BUS1_CTRL1,
			1 << i, 12, ptr);

	/* Restore the VBIF clock setting */
	gpu_write(gpu, REG_A6XX_VBIF_CLKON, clk);
}

static void a6xx_get_debugbus_block(struct msm_gpu *gpu,
		struct a6xx_gpu_state *a6xx_state,
		const struct a6xx_debugbus_block *block,
		struct a6xx_gpu_state_obj *obj)
{
	int i;
	u32 *ptr;

	obj->data = state_kcalloc(a6xx_state, block->count, sizeof(u64));
	if (!obj->data)
		return;

	obj->handle = block;

	for (ptr = obj->data, i = 0; i < block->count; i++)
		ptr += debugbus_read(gpu, block->id, i, ptr);
}

static void a6xx_get_cx_debugbus_block(void __iomem *cxdbg,
		struct a6xx_gpu_state *a6xx_state,
		const struct a6xx_debugbus_block *block,
		struct a6xx_gpu_state_obj *obj)
{
	int i;
	u32 *ptr;

	obj->data = state_kcalloc(a6xx_state, block->count, sizeof(u64));
	if (!obj->data)
		return;

	obj->handle = block;

	for (ptr = obj->data, i = 0; i < block->count; i++)
		ptr += cx_debugbus_read(cxdbg, block->id, i, ptr);
}

static void a6xx_get_debugbus_blocks(struct msm_gpu *gpu,
		struct a6xx_gpu_state *a6xx_state)
{
	int nr_debugbus_blocks = ARRAY_SIZE(a6xx_debugbus_blocks) +
		(a6xx_has_gbif(to_adreno_gpu(gpu)) ? 1 : 0);

	if (adreno_is_a650_family(to_adreno_gpu(gpu)))
		nr_debugbus_blocks += ARRAY_SIZE(a650_debugbus_blocks);

	a6xx_state->debugbus = state_kcalloc(a6xx_state, nr_debugbus_blocks,
			sizeof(*a6xx_state->debugbus));

	if (a6xx_state->debugbus) {
		int i;

		for (i = 0; i < ARRAY_SIZE(a6xx_debugbus_blocks); i++)
			a6xx_get_debugbus_block(gpu,
				a6xx_state,
				&a6xx_debugbus_blocks[i],
				&a6xx_state->debugbus[i]);

		a6xx_state->nr_debugbus = ARRAY_SIZE(a6xx_debugbus_blocks);

		/*
		 * GBIF has same debugbus as of other GPU blocks, fall back to
		 * default path if GPU uses GBIF, also GBIF uses exactly same
		 * ID as of VBIF.
		 */
		if (a6xx_has_gbif(to_adreno_gpu(gpu))) {
			a6xx_get_debugbus_block(gpu, a6xx_state,
				&a6xx_gbif_debugbus_block,
				&a6xx_state->debugbus[i]);

			a6xx_state->nr_debugbus += 1;
		}


		if (adreno_is_a650_family(to_adreno_gpu(gpu))) {
			for (i = 0; i < ARRAY_SIZE(a650_debugbus_blocks); i++)
				a6xx_get_debugbus_block(gpu,
					a6xx_state,
					&a650_debugbus_blocks[i],
					&a6xx_state->debugbus[i]);
		}
	}
}

static void a7xx_get_debugbus_blocks(struct msm_gpu *gpu,
		struct a6xx_gpu_state *a6xx_state)
{
	struct adreno_gpu *adreno_gpu = to_adreno_gpu(gpu);
	int debugbus_blocks_count, gbif_debugbus_blocks_count, total_debugbus_blocks;
	const u32 *debugbus_blocks, *gbif_debugbus_blocks;
	int i;

	if (adreno_gpu->info->family == ADRENO_7XX_GEN1) {
		debugbus_blocks = gen7_0_0_debugbus_blocks;
		debugbus_blocks_count = ARRAY_SIZE(gen7_0_0_debugbus_blocks);
		gbif_debugbus_blocks = a7xx_gbif_debugbus_blocks;
		gbif_debugbus_blocks_count = ARRAY_SIZE(a7xx_gbif_debugbus_blocks);
	} else if (adreno_gpu->info->family == ADRENO_7XX_GEN2) {
		debugbus_blocks = gen7_2_0_debugbus_blocks;
		debugbus_blocks_count = ARRAY_SIZE(gen7_2_0_debugbus_blocks);
		gbif_debugbus_blocks = a7xx_gbif_debugbus_blocks;
		gbif_debugbus_blocks_count = ARRAY_SIZE(a7xx_gbif_debugbus_blocks);
	} else {
		BUG_ON(adreno_gpu->info->family != ADRENO_7XX_GEN3);
		debugbus_blocks = gen7_9_0_debugbus_blocks;
		debugbus_blocks_count = ARRAY_SIZE(gen7_9_0_debugbus_blocks);
		gbif_debugbus_blocks = gen7_9_0_gbif_debugbus_blocks;
		gbif_debugbus_blocks_count = ARRAY_SIZE(gen7_9_0_gbif_debugbus_blocks);
	}

	total_debugbus_blocks = debugbus_blocks_count + gbif_debugbus_blocks_count;

	a6xx_state->debugbus = state_kcalloc(a6xx_state, total_debugbus_blocks,
			sizeof(*a6xx_state->debugbus));

	if (a6xx_state->debugbus) {
		for (i = 0; i < debugbus_blocks_count; i++) {
			a6xx_get_debugbus_block(gpu,
				a6xx_state, &a7xx_debugbus_blocks[debugbus_blocks[i]],
				&a6xx_state->debugbus[i]);
		}

		for (i = 0; i < gbif_debugbus_blocks_count; i++) {
			a6xx_get_debugbus_block(gpu,
				a6xx_state, &a7xx_debugbus_blocks[gbif_debugbus_blocks[i]],
				&a6xx_state->debugbus[i + debugbus_blocks_count]);
		}
	}

}

static void a6xx_get_debugbus(struct msm_gpu *gpu,
		struct a6xx_gpu_state *a6xx_state)
{
	struct adreno_gpu *adreno_gpu = to_adreno_gpu(gpu);
	struct resource *res;
	void __iomem *cxdbg = NULL;

	/* Set up the GX debug bus */

	gpu_write(gpu, REG_A6XX_DBGC_CFG_DBGBUS_CNTLT,
		A6XX_DBGC_CFG_DBGBUS_CNTLT_SEGT(0xf));

	gpu_write(gpu, REG_A6XX_DBGC_CFG_DBGBUS_CNTLM,
		A6XX_DBGC_CFG_DBGBUS_CNTLM_ENABLE(0xf));

	gpu_write(gpu, REG_A6XX_DBGC_CFG_DBGBUS_IVTL_0, 0);
	gpu_write(gpu, REG_A6XX_DBGC_CFG_DBGBUS_IVTL_1, 0);
	gpu_write(gpu, REG_A6XX_DBGC_CFG_DBGBUS_IVTL_2, 0);
	gpu_write(gpu, REG_A6XX_DBGC_CFG_DBGBUS_IVTL_3, 0);

	gpu_write(gpu, REG_A6XX_DBGC_CFG_DBGBUS_BYTEL_0, 0x76543210);
	gpu_write(gpu, REG_A6XX_DBGC_CFG_DBGBUS_BYTEL_1, 0xFEDCBA98);

	gpu_write(gpu, REG_A6XX_DBGC_CFG_DBGBUS_MASKL_0, 0);
	gpu_write(gpu, REG_A6XX_DBGC_CFG_DBGBUS_MASKL_1, 0);
	gpu_write(gpu, REG_A6XX_DBGC_CFG_DBGBUS_MASKL_2, 0);
	gpu_write(gpu, REG_A6XX_DBGC_CFG_DBGBUS_MASKL_3, 0);

	/* Set up the CX debug bus - it lives elsewhere in the system so do a
	 * temporary ioremap for the registers
	 */
	res = platform_get_resource_byname(gpu->pdev, IORESOURCE_MEM,
			"cx_dbgc");

	if (res)
		cxdbg = ioremap(res->start, resource_size(res));

	if (cxdbg) {
		cxdbg_write(cxdbg, REG_A6XX_CX_DBGC_CFG_DBGBUS_CNTLT,
			A6XX_DBGC_CFG_DBGBUS_CNTLT_SEGT(0xf));

		cxdbg_write(cxdbg, REG_A6XX_CX_DBGC_CFG_DBGBUS_CNTLM,
			A6XX_DBGC_CFG_DBGBUS_CNTLM_ENABLE(0xf));

		cxdbg_write(cxdbg, REG_A6XX_CX_DBGC_CFG_DBGBUS_IVTL_0, 0);
		cxdbg_write(cxdbg, REG_A6XX_CX_DBGC_CFG_DBGBUS_IVTL_1, 0);
		cxdbg_write(cxdbg, REG_A6XX_CX_DBGC_CFG_DBGBUS_IVTL_2, 0);
		cxdbg_write(cxdbg, REG_A6XX_CX_DBGC_CFG_DBGBUS_IVTL_3, 0);

		cxdbg_write(cxdbg, REG_A6XX_CX_DBGC_CFG_DBGBUS_BYTEL_0,
			0x76543210);
		cxdbg_write(cxdbg, REG_A6XX_CX_DBGC_CFG_DBGBUS_BYTEL_1,
			0xFEDCBA98);

		cxdbg_write(cxdbg, REG_A6XX_CX_DBGC_CFG_DBGBUS_MASKL_0, 0);
		cxdbg_write(cxdbg, REG_A6XX_CX_DBGC_CFG_DBGBUS_MASKL_1, 0);
		cxdbg_write(cxdbg, REG_A6XX_CX_DBGC_CFG_DBGBUS_MASKL_2, 0);
		cxdbg_write(cxdbg, REG_A6XX_CX_DBGC_CFG_DBGBUS_MASKL_3, 0);
	}

	if (adreno_is_a7xx(adreno_gpu)) {
		a7xx_get_debugbus_blocks(gpu, a6xx_state);
	} else {
		a6xx_get_debugbus_blocks(gpu, a6xx_state);
	}

	/*  Dump the VBIF debugbus on applicable targets */
	if (!a6xx_has_gbif(adreno_gpu)) {
		a6xx_state->vbif_debugbus =
			state_kcalloc(a6xx_state, 1,
					sizeof(*a6xx_state->vbif_debugbus));

		if (a6xx_state->vbif_debugbus)
			a6xx_get_vbif_debugbus_block(gpu, a6xx_state,
					a6xx_state->vbif_debugbus);
	}

	if (cxdbg) {
		unsigned nr_cx_debugbus_blocks;
		const struct a6xx_debugbus_block *cx_debugbus_blocks;

		if (adreno_is_a7xx(adreno_gpu)) {
			BUG_ON(adreno_gpu->info->family > ADRENO_7XX_GEN3);
			cx_debugbus_blocks = a7xx_cx_debugbus_blocks;
			nr_cx_debugbus_blocks = ARRAY_SIZE(a7xx_cx_debugbus_blocks);
		} else {
			cx_debugbus_blocks = a6xx_cx_debugbus_blocks;
			nr_cx_debugbus_blocks = ARRAY_SIZE(a6xx_cx_debugbus_blocks);
		}

		a6xx_state->cx_debugbus =
			state_kcalloc(a6xx_state,
			nr_cx_debugbus_blocks,
			sizeof(*a6xx_state->cx_debugbus));

		if (a6xx_state->cx_debugbus) {
			int i;

			for (i = 0; i < nr_cx_debugbus_blocks; i++)
				a6xx_get_cx_debugbus_block(cxdbg,
					a6xx_state,
					&cx_debugbus_blocks[i],
					&a6xx_state->cx_debugbus[i]);

			a6xx_state->nr_cx_debugbus =
				nr_cx_debugbus_blocks;
		}

		iounmap(cxdbg);
	}
}

#define RANGE(reg, a) ((reg)[(a) + 1] - (reg)[(a)] + 1)

/* Read a data cluster from behind the AHB aperture */
static void a6xx_get_dbgahb_cluster(struct msm_gpu *gpu,
		struct a6xx_gpu_state *a6xx_state,
		const struct a6xx_dbgahb_cluster *dbgahb,
		struct a6xx_gpu_state_obj *obj,
		struct a6xx_crashdumper *dumper)
{
	u64 *in = dumper->ptr;
	u64 out = dumper->iova + A6XX_CD_DATA_OFFSET;
	size_t datasize;
	int i, regcount = 0;

	for (i = 0; i < A6XX_NUM_CONTEXTS; i++) {
		int j;

		in += CRASHDUMP_WRITE(in, REG_A6XX_HLSQ_DBG_READ_SEL,
			(dbgahb->statetype + i * 2) << 8);

		for (j = 0; j < dbgahb->count; j += 2) {
			int count = RANGE(dbgahb->registers, j);
			u32 offset = REG_A6XX_HLSQ_DBG_AHB_READ_APERTURE +
				dbgahb->registers[j] - (dbgahb->base >> 2);

			in += CRASHDUMP_READ(in, offset, count, out);

			out += count * sizeof(u32);

			if (i == 0)
				regcount += count;
		}
	}

	CRASHDUMP_FINI(in);

	datasize = regcount * A6XX_NUM_CONTEXTS * sizeof(u32);

	if (WARN_ON(datasize > A6XX_CD_DATA_SIZE))
		return;

	if (a6xx_crashdumper_run(gpu, dumper))
		return;

	obj->handle = dbgahb;
	obj->data = state_kmemdup(a6xx_state, dumper->ptr + A6XX_CD_DATA_OFFSET,
		datasize);
}

static void a7xx_get_dbgahb_cluster(struct msm_gpu *gpu,
		struct a6xx_gpu_state *a6xx_state,
		const struct gen7_sptp_cluster_registers *dbgahb,
		struct a6xx_gpu_state_obj *obj,
		struct a6xx_crashdumper *dumper)
{
	u64 *in = dumper->ptr;
	u64 out = dumper->iova + A6XX_CD_DATA_OFFSET;
	size_t datasize;
	int i, regcount = 0;

	in += CRASHDUMP_WRITE(in, REG_A7XX_SP_READ_SEL,
		A7XX_SP_READ_SEL_LOCATION(dbgahb->location_id) |
		A7XX_SP_READ_SEL_PIPE(dbgahb->pipe_id) |
		A7XX_SP_READ_SEL_STATETYPE(dbgahb->statetype));

	for (i = 0; dbgahb->regs[i] != UINT_MAX; i += 2) {
		int count = RANGE(dbgahb->regs, i);
		u32 offset = REG_A7XX_SP_AHB_READ_APERTURE +
			dbgahb->regs[i] - dbgahb->regbase;

		in += CRASHDUMP_READ(in, offset, count, out);

		out += count * sizeof(u32);
		regcount += count;
	}

	CRASHDUMP_FINI(in);

	datasize = regcount * sizeof(u32);

	if (WARN_ON(datasize > A6XX_CD_DATA_SIZE))
		return;

	if (a6xx_crashdumper_run(gpu, dumper))
		return;

	obj->handle = dbgahb;
	obj->data = state_kmemdup(a6xx_state, dumper->ptr + A6XX_CD_DATA_OFFSET,
		datasize);
}

static void a6xx_get_dbgahb_clusters(struct msm_gpu *gpu,
		struct a6xx_gpu_state *a6xx_state,
		struct a6xx_crashdumper *dumper)
{
	int i;

	a6xx_state->dbgahb_clusters = state_kcalloc(a6xx_state,
		ARRAY_SIZE(a6xx_dbgahb_clusters),
		sizeof(*a6xx_state->dbgahb_clusters));

	if (!a6xx_state->dbgahb_clusters)
		return;

	a6xx_state->nr_dbgahb_clusters = ARRAY_SIZE(a6xx_dbgahb_clusters);

	for (i = 0; i < ARRAY_SIZE(a6xx_dbgahb_clusters); i++)
		a6xx_get_dbgahb_cluster(gpu, a6xx_state,
			&a6xx_dbgahb_clusters[i],
			&a6xx_state->dbgahb_clusters[i], dumper);
}

static void a7xx_get_dbgahb_clusters(struct msm_gpu *gpu,
		struct a6xx_gpu_state *a6xx_state,
		struct a6xx_crashdumper *dumper)
{
	struct adreno_gpu *adreno_gpu = to_adreno_gpu(gpu);
	int i;
	const struct gen7_sptp_cluster_registers *dbgahb_clusters;
	unsigned dbgahb_clusters_size;

	if (adreno_gpu->info->family == ADRENO_7XX_GEN1) {
		dbgahb_clusters = gen7_0_0_sptp_clusters;
		dbgahb_clusters_size = ARRAY_SIZE(gen7_0_0_sptp_clusters);
	} else if (adreno_gpu->info->family == ADRENO_7XX_GEN2) {
		dbgahb_clusters = gen7_2_0_sptp_clusters;
		dbgahb_clusters_size = ARRAY_SIZE(gen7_2_0_sptp_clusters);
	} else {
		BUG_ON(adreno_gpu->info->family != ADRENO_7XX_GEN3);
		dbgahb_clusters = gen7_9_0_sptp_clusters;
		dbgahb_clusters_size = ARRAY_SIZE(gen7_9_0_sptp_clusters);
	}

	a6xx_state->dbgahb_clusters = state_kcalloc(a6xx_state,
		dbgahb_clusters_size,
		sizeof(*a6xx_state->dbgahb_clusters));

	if (!a6xx_state->dbgahb_clusters)
		return;

	a6xx_state->nr_dbgahb_clusters = dbgahb_clusters_size;

	for (i = 0; i < dbgahb_clusters_size; i++)
		a7xx_get_dbgahb_cluster(gpu, a6xx_state,
			&dbgahb_clusters[i],
			&a6xx_state->dbgahb_clusters[i], dumper);
}

/* Read a data cluster from the CP aperture with the crashdumper */
static void a6xx_get_cluster(struct msm_gpu *gpu,
		struct a6xx_gpu_state *a6xx_state,
		const struct a6xx_cluster *cluster,
		struct a6xx_gpu_state_obj *obj,
		struct a6xx_crashdumper *dumper)
{
	struct adreno_gpu *adreno_gpu = to_adreno_gpu(gpu);
	u64 *in = dumper->ptr;
	u64 out = dumper->iova + A6XX_CD_DATA_OFFSET;
	size_t datasize;
	int i, regcount = 0;
	u32 id = cluster->id;

	/* Skip registers that are not present on older generation */
	if (!adreno_is_a660_family(adreno_gpu) &&
			cluster->registers == a660_fe_cluster)
		return;

	if (adreno_is_a650_family(adreno_gpu) &&
			cluster->registers == a6xx_ps_cluster)
		id = CLUSTER_VPC_PS;

	/* Some clusters need a selector register to be programmed too */
	if (cluster->sel_reg)
		in += CRASHDUMP_WRITE(in, cluster->sel_reg, cluster->sel_val);

	for (i = 0; i < A6XX_NUM_CONTEXTS; i++) {
		int j;

		in += CRASHDUMP_WRITE(in, REG_A6XX_CP_APERTURE_CNTL_CD,
			(id << 8) | (i << 4) | i);

		for (j = 0; j < cluster->count; j += 2) {
			int count = RANGE(cluster->registers, j);

			in += CRASHDUMP_READ(in, cluster->registers[j],
				count, out);

			out += count * sizeof(u32);

			if (i == 0)
				regcount += count;
		}
	}

	CRASHDUMP_FINI(in);

	datasize = regcount * A6XX_NUM_CONTEXTS * sizeof(u32);

	if (WARN_ON(datasize > A6XX_CD_DATA_SIZE))
		return;

	if (a6xx_crashdumper_run(gpu, dumper))
		return;

	obj->handle = cluster;
	obj->data = state_kmemdup(a6xx_state, dumper->ptr + A6XX_CD_DATA_OFFSET,
		datasize);
}

static void a7xx_get_cluster(struct msm_gpu *gpu,
		struct a6xx_gpu_state *a6xx_state,
		const struct gen7_cluster_registers *cluster,
		struct a6xx_gpu_state_obj *obj,
		struct a6xx_crashdumper *dumper)
{
	u64 *in = dumper->ptr;
	u64 out = dumper->iova + A6XX_CD_DATA_OFFSET;
	size_t datasize;
	int i, regcount = 0;

	/* Some clusters need a selector register to be programmed too */
	if (cluster->sel)
		in += CRASHDUMP_WRITE(in, cluster->sel->cd_reg, cluster->sel->val);

	in += CRASHDUMP_WRITE(in, REG_A7XX_CP_APERTURE_CNTL_CD,
		A7XX_CP_APERTURE_CNTL_CD_PIPE(cluster->pipe_id) |
		A7XX_CP_APERTURE_CNTL_CD_CLUSTER(cluster->cluster_id) |
		A7XX_CP_APERTURE_CNTL_CD_CONTEXT(cluster->context_id));

	for (i = 0; cluster->regs[i] != UINT_MAX; i += 2) {
		int count = RANGE(cluster->regs, i);

		in += CRASHDUMP_READ(in, cluster->regs[i],
			count, out);

		out += count * sizeof(u32);
		regcount += count;
	}

	CRASHDUMP_FINI(in);

	datasize = regcount * sizeof(u32);

	if (WARN_ON(datasize > A6XX_CD_DATA_SIZE))
		return;

	if (a6xx_crashdumper_run(gpu, dumper))
		return;

	obj->handle = cluster;
	obj->data = state_kmemdup(a6xx_state, dumper->ptr + A6XX_CD_DATA_OFFSET,
		datasize);
}

static void a6xx_get_clusters(struct msm_gpu *gpu,
		struct a6xx_gpu_state *a6xx_state,
		struct a6xx_crashdumper *dumper)
{
	int i;

	a6xx_state->clusters = state_kcalloc(a6xx_state,
		ARRAY_SIZE(a6xx_clusters), sizeof(*a6xx_state->clusters));

	if (!a6xx_state->clusters)
		return;

	a6xx_state->nr_clusters = ARRAY_SIZE(a6xx_clusters);

	for (i = 0; i < ARRAY_SIZE(a6xx_clusters); i++)
		a6xx_get_cluster(gpu, a6xx_state, &a6xx_clusters[i],
			&a6xx_state->clusters[i], dumper);
}

static void a7xx_get_clusters(struct msm_gpu *gpu,
		struct a6xx_gpu_state *a6xx_state,
		struct a6xx_crashdumper *dumper)
{
	struct adreno_gpu *adreno_gpu = to_adreno_gpu(gpu);
	int i;
	const struct gen7_cluster_registers *clusters;
	unsigned clusters_size;

	if (adreno_gpu->info->family == ADRENO_7XX_GEN1) {
		clusters = gen7_0_0_clusters;
		clusters_size = ARRAY_SIZE(gen7_0_0_clusters);
	} else if (adreno_gpu->info->family == ADRENO_7XX_GEN2) {
		clusters = gen7_2_0_clusters;
		clusters_size = ARRAY_SIZE(gen7_2_0_clusters);
	} else {
		BUG_ON(adreno_gpu->info->family != ADRENO_7XX_GEN3);
		clusters = gen7_9_0_clusters;
		clusters_size = ARRAY_SIZE(gen7_9_0_clusters);
	}

	a6xx_state->clusters = state_kcalloc(a6xx_state,
		clusters_size, sizeof(*a6xx_state->clusters));

	if (!a6xx_state->clusters)
		return;

	a6xx_state->nr_clusters = clusters_size;

	for (i = 0; i < clusters_size; i++)
		a7xx_get_cluster(gpu, a6xx_state, &clusters[i],
			&a6xx_state->clusters[i], dumper);
}

/* Read a shader / debug block from the HLSQ aperture with the crashdumper */
static void a6xx_get_shader_block(struct msm_gpu *gpu,
		struct a6xx_gpu_state *a6xx_state,
		const struct a6xx_shader_block *block,
		struct a6xx_gpu_state_obj *obj,
		struct a6xx_crashdumper *dumper)
{
	u64 *in = dumper->ptr;
	u64 out = dumper->iova + A6XX_CD_DATA_OFFSET;
	size_t datasize = block->size * A6XX_NUM_SHADER_BANKS * sizeof(u32);
	int i;

	if (WARN_ON(datasize > A6XX_CD_DATA_SIZE))
		return;

	for (i = 0; i < A6XX_NUM_SHADER_BANKS; i++) {
		in += CRASHDUMP_WRITE(in, REG_A6XX_HLSQ_DBG_READ_SEL,
			(block->type << 8) | i);

		in += CRASHDUMP_READ(in, REG_A6XX_HLSQ_DBG_AHB_READ_APERTURE,
			block->size, out);

		out += block->size * sizeof(u32);
	}

	CRASHDUMP_FINI(in);

	if (a6xx_crashdumper_run(gpu, dumper))
		return;

	obj->handle = block;
	obj->data = state_kmemdup(a6xx_state, dumper->ptr + A6XX_CD_DATA_OFFSET,
		datasize);
}

static void a7xx_get_shader_block(struct msm_gpu *gpu,
		struct a6xx_gpu_state *a6xx_state,
		const struct gen7_shader_block *block,
		struct a6xx_gpu_state_obj *obj,
		struct a6xx_crashdumper *dumper)
{
	struct adreno_gpu *adreno_gpu = to_adreno_gpu(gpu);
	u64 *in = dumper->ptr;
	u64 out = dumper->iova + A6XX_CD_DATA_OFFSET;
	size_t datasize = block->size * block->num_sps * block->num_usptps * sizeof(u32);
	int i, j;

	if (WARN_ON(datasize > A6XX_CD_DATA_SIZE))
		return;

	if (adreno_gpu->info->family == ADRENO_7XX_GEN1) {
		gpu_rmw(gpu, REG_A7XX_SP_DBG_CNTL, GENMASK(1, 0), 3);
	}

	for (i = 0; i < block->num_sps; i++) {
		for (j = 0; j < block->num_usptps; j++) {
			in += CRASHDUMP_WRITE(in, REG_A7XX_SP_READ_SEL,
				A7XX_SP_READ_SEL_LOCATION(block->location) |
				A7XX_SP_READ_SEL_PIPE(block->pipeid) |
				A7XX_SP_READ_SEL_STATETYPE(block->statetype) |
				A7XX_SP_READ_SEL_USPTP(j) |
				A7XX_SP_READ_SEL_SPTP(i));

			in += CRASHDUMP_READ(in, REG_A7XX_SP_AHB_READ_APERTURE,
				block->size, out);

			out += block->size * sizeof(u32);
		}
	}

	CRASHDUMP_FINI(in);

	if (a6xx_crashdumper_run(gpu, dumper))
		goto out;

	obj->handle = block;
	obj->data = state_kmemdup(a6xx_state, dumper->ptr + A6XX_CD_DATA_OFFSET,
		datasize);

out:
	if (adreno_gpu->info->family == ADRENO_7XX_GEN1) {
		gpu_rmw(gpu, REG_A7XX_SP_DBG_CNTL, GENMASK(1, 0), 0);
	}
}

static void a6xx_get_shaders(struct msm_gpu *gpu,
		struct a6xx_gpu_state *a6xx_state,
		struct a6xx_crashdumper *dumper)
{
	int i;

	a6xx_state->shaders = state_kcalloc(a6xx_state,
		ARRAY_SIZE(a6xx_shader_blocks), sizeof(*a6xx_state->shaders));

	if (!a6xx_state->shaders)
		return;

	a6xx_state->nr_shaders = ARRAY_SIZE(a6xx_shader_blocks);

	for (i = 0; i < ARRAY_SIZE(a6xx_shader_blocks); i++)
		a6xx_get_shader_block(gpu, a6xx_state, &a6xx_shader_blocks[i],
			&a6xx_state->shaders[i], dumper);
}

static void a7xx_get_shaders(struct msm_gpu *gpu,
		struct a6xx_gpu_state *a6xx_state,
		struct a6xx_crashdumper *dumper)
{
	struct adreno_gpu *adreno_gpu = to_adreno_gpu(gpu);
	const struct gen7_shader_block *shader_blocks;
	unsigned num_shader_blocks;
	int i;

	if (adreno_gpu->info->family == ADRENO_7XX_GEN1) {
		shader_blocks = gen7_0_0_shader_blocks;
		num_shader_blocks = ARRAY_SIZE(gen7_0_0_shader_blocks);
	} else if (adreno_gpu->info->family == ADRENO_7XX_GEN2) {
		shader_blocks = gen7_2_0_shader_blocks;
		num_shader_blocks = ARRAY_SIZE(gen7_2_0_shader_blocks);
	} else {
		BUG_ON(adreno_gpu->info->family != ADRENO_7XX_GEN3);
		shader_blocks = gen7_9_0_shader_blocks;
		num_shader_blocks = ARRAY_SIZE(gen7_9_0_shader_blocks);
	}

	a6xx_state->shaders = state_kcalloc(a6xx_state,
		num_shader_blocks, sizeof(*a6xx_state->shaders));

	if (!a6xx_state->shaders)
		return;

	a6xx_state->nr_shaders = num_shader_blocks;

	for (i = 0; i < num_shader_blocks; i++)
		a7xx_get_shader_block(gpu, a6xx_state, &shader_blocks[i],
			&a6xx_state->shaders[i], dumper);
}

/* Read registers from behind the HLSQ aperture with the crashdumper */
static void a6xx_get_crashdumper_hlsq_registers(struct msm_gpu *gpu,
		struct a6xx_gpu_state *a6xx_state,
		const struct a6xx_registers *regs,
		struct a6xx_gpu_state_obj *obj,
		struct a6xx_crashdumper *dumper)

{
	u64 *in = dumper->ptr;
	u64 out = dumper->iova + A6XX_CD_DATA_OFFSET;
	int i, regcount = 0;

	in += CRASHDUMP_WRITE(in, REG_A6XX_HLSQ_DBG_READ_SEL, regs->val1);

	for (i = 0; i < regs->count; i += 2) {
		u32 count = RANGE(regs->registers, i);
		u32 offset = REG_A6XX_HLSQ_DBG_AHB_READ_APERTURE +
			regs->registers[i] - (regs->val0 >> 2);

		in += CRASHDUMP_READ(in, offset, count, out);

		out += count * sizeof(u32);
		regcount += count;
	}

	CRASHDUMP_FINI(in);

	if (WARN_ON((regcount * sizeof(u32)) > A6XX_CD_DATA_SIZE))
		return;

	if (a6xx_crashdumper_run(gpu, dumper))
		return;

	obj->handle = regs;
	obj->data = state_kmemdup(a6xx_state, dumper->ptr + A6XX_CD_DATA_OFFSET,
		regcount * sizeof(u32));
}

/* Read a block of registers using the crashdumper */
static void a6xx_get_crashdumper_registers(struct msm_gpu *gpu,
		struct a6xx_gpu_state *a6xx_state,
		const struct a6xx_registers *regs,
		struct a6xx_gpu_state_obj *obj,
		struct a6xx_crashdumper *dumper)

{
	u64 *in = dumper->ptr;
	u64 out = dumper->iova + A6XX_CD_DATA_OFFSET;
	int i, regcount = 0;

	/* Skip unsupported registers on older generations */
	if (!adreno_is_a660_family(to_adreno_gpu(gpu)) &&
			(regs->registers == a660_registers))
		return;

	/* Some blocks might need to program a selector register first */
	if (regs->val0)
		in += CRASHDUMP_WRITE(in, regs->val0, regs->val1);

	for (i = 0; i < regs->count; i += 2) {
		u32 count = RANGE(regs->registers, i);

		in += CRASHDUMP_READ(in, regs->registers[i], count, out);

		out += count * sizeof(u32);
		regcount += count;
	}

	CRASHDUMP_FINI(in);

	if (WARN_ON((regcount * sizeof(u32)) > A6XX_CD_DATA_SIZE))
		return;

	if (a6xx_crashdumper_run(gpu, dumper))
		return;

	obj->handle = regs;
	obj->data = state_kmemdup(a6xx_state, dumper->ptr + A6XX_CD_DATA_OFFSET,
		regcount * sizeof(u32));
}

static void a7xx_get_crashdumper_registers(struct msm_gpu *gpu,
		struct a6xx_gpu_state *a6xx_state,
		const struct gen7_reg_list *regs,
		struct a6xx_gpu_state_obj *obj,
		struct a6xx_crashdumper *dumper)

{
	u64 *in = dumper->ptr;
	u64 out = dumper->iova + A6XX_CD_DATA_OFFSET;
	int i, regcount = 0;

	/* Some blocks might need to program a selector register first */
	if (regs->sel)
		in += CRASHDUMP_WRITE(in, regs->sel->cd_reg, regs->sel->val);

	for (i = 0; regs->regs[i] != UINT_MAX; i += 2) {
		u32 count = RANGE(regs->regs, i);

		in += CRASHDUMP_READ(in, regs->regs[i], count, out);

		out += count * sizeof(u32);
		regcount += count;
	}

	CRASHDUMP_FINI(in);

	if (WARN_ON((regcount * sizeof(u32)) > A6XX_CD_DATA_SIZE))
		return;

	if (a6xx_crashdumper_run(gpu, dumper))
		return;

	obj->handle = regs->regs;
	obj->data = state_kmemdup(a6xx_state, dumper->ptr + A6XX_CD_DATA_OFFSET,
		regcount * sizeof(u32));
}


/* Read a block of registers via AHB */
static void a6xx_get_ahb_gpu_registers(struct msm_gpu *gpu,
		struct a6xx_gpu_state *a6xx_state,
		const struct a6xx_registers *regs,
		struct a6xx_gpu_state_obj *obj)
{
	int i, regcount = 0, index = 0;

	/* Skip unsupported registers on older generations */
	if (!adreno_is_a660_family(to_adreno_gpu(gpu)) &&
			(regs->registers == a660_registers))
		return;

	for (i = 0; i < regs->count; i += 2)
		regcount += RANGE(regs->registers, i);

	obj->handle = (const void *) regs;
	obj->data = state_kcalloc(a6xx_state, regcount, sizeof(u32));
	if (!obj->data)
		return;

	for (i = 0; i < regs->count; i += 2) {
		u32 count = RANGE(regs->registers, i);
		int j;

		for (j = 0; j < count; j++)
			obj->data[index++] = gpu_read(gpu,
				regs->registers[i] + j);
	}
}

static void a7xx_get_ahb_gpu_registers(struct msm_gpu *gpu,
		struct a6xx_gpu_state *a6xx_state,
		const u32 *regs,
		struct a6xx_gpu_state_obj *obj)
{
	int i, regcount = 0, index = 0;

	for (i = 0; regs[i] != UINT_MAX; i += 2)
		regcount += RANGE(regs, i);

	obj->handle = (const void *) regs;
	obj->data = state_kcalloc(a6xx_state, regcount, sizeof(u32));
	if (!obj->data)
		return;

	for (i = 0; regs[i] != UINT_MAX; i += 2) {
		u32 count = RANGE(regs, i);
		int j;

		for (j = 0; j < count; j++)
			obj->data[index++] = gpu_read(gpu, regs[i] + j);
	}
}

static void a7xx_get_ahb_gpu_reglist(struct msm_gpu *gpu,
		struct a6xx_gpu_state *a6xx_state,
		const struct gen7_reg_list *regs,
		struct a6xx_gpu_state_obj *obj)
{
	if (regs->sel)
		gpu_write(gpu, regs->sel->host_reg, regs->sel->val);

	a7xx_get_ahb_gpu_registers(gpu, a6xx_state, regs->regs, obj);
}

/* Read a block of GMU registers */
static void _a6xx_get_gmu_registers(struct msm_gpu *gpu,
		struct a6xx_gpu_state *a6xx_state,
		const struct a6xx_registers *regs,
		struct a6xx_gpu_state_obj *obj,
		bool rscc)
{
	struct adreno_gpu *adreno_gpu = to_adreno_gpu(gpu);
	struct a6xx_gpu *a6xx_gpu = to_a6xx_gpu(adreno_gpu);
	struct a6xx_gmu *gmu = &a6xx_gpu->gmu;
	int i, regcount = 0, index = 0;

	for (i = 0; i < regs->count; i += 2)
		regcount += RANGE(regs->registers, i);

	obj->handle = (const void *) regs;
	obj->data = state_kcalloc(a6xx_state, regcount, sizeof(u32));
	if (!obj->data)
		return;

	for (i = 0; i < regs->count; i += 2) {
		u32 count = RANGE(regs->registers, i);
		int j;

		for (j = 0; j < count; j++) {
			u32 offset = regs->registers[i] + j;
			u32 val;

			if (rscc)
				val = gmu_read_rscc(gmu, offset);
			else
				val = gmu_read(gmu, offset);

			obj->data[index++] = val;
		}
	}
}

static void a6xx_get_gmu_registers(struct msm_gpu *gpu,
		struct a6xx_gpu_state *a6xx_state)
{
	struct adreno_gpu *adreno_gpu = to_adreno_gpu(gpu);
	struct a6xx_gpu *a6xx_gpu = to_a6xx_gpu(adreno_gpu);

	a6xx_state->gmu_registers = state_kcalloc(a6xx_state,
		4, sizeof(*a6xx_state->gmu_registers));

	if (!a6xx_state->gmu_registers)
		return;

	a6xx_state->nr_gmu_registers = 4;

	/* Get the CX GMU registers from AHB */
	_a6xx_get_gmu_registers(gpu, a6xx_state, &a6xx_gmu_reglist[0],
		&a6xx_state->gmu_registers[0], false);
	_a6xx_get_gmu_registers(gpu, a6xx_state, &a6xx_gmu_reglist[1],
		&a6xx_state->gmu_registers[1], true);

	if (adreno_is_a621(adreno_gpu) || adreno_is_a623(adreno_gpu))
		_a6xx_get_gmu_registers(gpu, a6xx_state, &a621_gpucc_reg,
			&a6xx_state->gmu_registers[2], false);
	else
		_a6xx_get_gmu_registers(gpu, a6xx_state, &a6xx_gpucc_reg,
			&a6xx_state->gmu_registers[2], false);

	if (!a6xx_gmu_gx_is_on(&a6xx_gpu->gmu))
		return;

	/* Set the fence to ALLOW mode so we can access the registers */
	gpu_write(gpu, REG_A6XX_GMU_AO_AHB_FENCE_CTRL, 0);

	_a6xx_get_gmu_registers(gpu, a6xx_state, &a6xx_gmu_reglist[2],
		&a6xx_state->gmu_registers[3], false);
}

static struct msm_gpu_state_bo *a6xx_snapshot_gmu_bo(
		struct a6xx_gpu_state *a6xx_state, struct a6xx_gmu_bo *bo)
{
	struct msm_gpu_state_bo *snapshot;

	if (!bo->size)
		return NULL;

	snapshot = state_kcalloc(a6xx_state, 1, sizeof(*snapshot));
	if (!snapshot)
		return NULL;

	snapshot->iova = bo->iova;
	snapshot->size = bo->size;
	snapshot->data = kvzalloc(snapshot->size, GFP_KERNEL);
	if (!snapshot->data)
		return NULL;

	memcpy(snapshot->data, bo->virt, bo->size);

	return snapshot;
}

static void a6xx_snapshot_gmu_hfi_history(struct msm_gpu *gpu,
					  struct a6xx_gpu_state *a6xx_state)
{
	struct adreno_gpu *adreno_gpu = to_adreno_gpu(gpu);
	struct a6xx_gpu *a6xx_gpu = to_a6xx_gpu(adreno_gpu);
	struct a6xx_gmu *gmu = &a6xx_gpu->gmu;
	unsigned i, j;

	BUILD_BUG_ON(ARRAY_SIZE(gmu->queues) != ARRAY_SIZE(a6xx_state->hfi_queue_history));

	for (i = 0; i < ARRAY_SIZE(gmu->queues); i++) {
		struct a6xx_hfi_queue *queue = &gmu->queues[i];
		for (j = 0; j < HFI_HISTORY_SZ; j++) {
			unsigned idx = (j + queue->history_idx) % HFI_HISTORY_SZ;
			a6xx_state->hfi_queue_history[i][j] = queue->history[idx];
		}
	}
}

#define A6XX_REGLIST_SIZE        1
#define A6XX_GBIF_REGLIST_SIZE   1
static void a6xx_get_registers(struct msm_gpu *gpu,
		struct a6xx_gpu_state *a6xx_state,
		struct a6xx_crashdumper *dumper)
{
	int i, count = A6XX_REGLIST_SIZE +
		ARRAY_SIZE(a6xx_reglist) +
		ARRAY_SIZE(a6xx_hlsq_reglist) + A6XX_GBIF_REGLIST_SIZE;
	int index = 0;
	struct adreno_gpu *adreno_gpu = to_adreno_gpu(gpu);

	a6xx_state->registers = state_kcalloc(a6xx_state,
		count, sizeof(*a6xx_state->registers));

	if (!a6xx_state->registers)
		return;

	a6xx_state->nr_registers = count;

	a6xx_get_ahb_gpu_registers(gpu,
		a6xx_state, &a6xx_ahb_reglist,
		&a6xx_state->registers[index++]);

	if (a6xx_has_gbif(adreno_gpu))
		a6xx_get_ahb_gpu_registers(gpu,
				a6xx_state, &a6xx_gbif_reglist,
				&a6xx_state->registers[index++]);
	else
		a6xx_get_ahb_gpu_registers(gpu,
				a6xx_state, &a6xx_vbif_reglist,
				&a6xx_state->registers[index++]);
	if (!dumper) {
		/*
		 * We can't use the crashdumper when the SMMU is stalled,
		 * because the GPU has no memory access until we resume
		 * translation (but we don't want to do that until after
		 * we have captured as much useful GPU state as possible).
		 * So instead collect registers via the CPU:
		 */
		for (i = 0; i < ARRAY_SIZE(a6xx_reglist); i++)
			a6xx_get_ahb_gpu_registers(gpu,
				a6xx_state, &a6xx_reglist[i],
				&a6xx_state->registers[index++]);
		return;
	}

	for (i = 0; i < ARRAY_SIZE(a6xx_reglist); i++)
		a6xx_get_crashdumper_registers(gpu,
			a6xx_state, &a6xx_reglist[i],
			&a6xx_state->registers[index++],
			dumper);

	for (i = 0; i < ARRAY_SIZE(a6xx_hlsq_reglist); i++)
		a6xx_get_crashdumper_hlsq_registers(gpu,
			a6xx_state, &a6xx_hlsq_reglist[i],
			&a6xx_state->registers[index++],
			dumper);
}

#define A7XX_PRE_CRASHDUMPER_SIZE    1
#define A7XX_POST_CRASHDUMPER_SIZE   1
static void a7xx_get_registers(struct msm_gpu *gpu,
		struct a6xx_gpu_state *a6xx_state,
		struct a6xx_crashdumper *dumper)
{
	struct adreno_gpu *adreno_gpu = to_adreno_gpu(gpu);
	int i, count;
	int index = 0;
	const u32 *pre_crashdumper_regs;
	const struct gen7_reg_list *reglist;

	if (adreno_gpu->info->family == ADRENO_7XX_GEN1) {
		reglist = gen7_0_0_reg_list;
		pre_crashdumper_regs = gen7_0_0_pre_crashdumper_gpu_registers;
	} else if (adreno_gpu->info->family == ADRENO_7XX_GEN2) {
		reglist = gen7_2_0_reg_list;
		pre_crashdumper_regs = gen7_0_0_pre_crashdumper_gpu_registers;
	} else {
		BUG_ON(adreno_gpu->info->family != ADRENO_7XX_GEN3);
		reglist = gen7_9_0_reg_list;
		pre_crashdumper_regs = gen7_9_0_pre_crashdumper_gpu_registers;
	}

	count = A7XX_PRE_CRASHDUMPER_SIZE + A7XX_POST_CRASHDUMPER_SIZE;

	/* The downstream reglist contains registers in other memory regions
	 * (cx_misc/cx_mem and cx_dbgc) and we need to plumb through their
	 * offsets and map them to read them on the CPU. For now only read the
	 * first region which is the main one.
	 */
	if (dumper) {
		for (i = 0; reglist[i].regs; i++)
			count++;
	} else {
		count++;
	}

	a6xx_state->registers = state_kcalloc(a6xx_state,
		count, sizeof(*a6xx_state->registers));

	if (!a6xx_state->registers)
		return;

	a6xx_state->nr_registers = count;

	a7xx_get_ahb_gpu_registers(gpu, a6xx_state, pre_crashdumper_regs,
		&a6xx_state->registers[index++]);

	if (!dumper) {
		a7xx_get_ahb_gpu_reglist(gpu,
			a6xx_state, &reglist[0],
			&a6xx_state->registers[index++]);
		return;
	}

	for (i = 0; reglist[i].regs; i++)
		a7xx_get_crashdumper_registers(gpu,
			a6xx_state, &reglist[i],
			&a6xx_state->registers[index++],
			dumper);
}

static void a7xx_get_post_crashdumper_registers(struct msm_gpu *gpu,
		struct a6xx_gpu_state *a6xx_state)
{
	struct adreno_gpu *adreno_gpu = to_adreno_gpu(gpu);
	const u32 *regs;

	BUG_ON(adreno_gpu->info->family > ADRENO_7XX_GEN3);
	regs = gen7_0_0_post_crashdumper_registers;

	a7xx_get_ahb_gpu_registers(gpu,
		a6xx_state, regs,
		&a6xx_state->registers[a6xx_state->nr_registers - 1]);
}

static u32 a6xx_get_cp_roq_size(struct msm_gpu *gpu)
{
	/* The value at [16:31] is in 4dword units. Convert it to dwords */
	return gpu_read(gpu, REG_A6XX_CP_ROQ_THRESHOLDS_2) >> 14;
}

static u32 a7xx_get_cp_roq_size(struct msm_gpu *gpu)
{
	/*
	 * The value at CP_ROQ_THRESHOLDS_2[20:31] is in 4dword units.
	 * That register however is not directly accessible from APSS on A7xx.
	 * Program the SQE_UCODE_DBG_ADDR with offset=0x70d3 and read the value.
	 */
	gpu_write(gpu, REG_A6XX_CP_SQE_UCODE_DBG_ADDR, 0x70d3);

	return 4 * (gpu_read(gpu, REG_A6XX_CP_SQE_UCODE_DBG_DATA) >> 20);
}

/* Read a block of data from an indexed register pair */
static void a6xx_get_indexed_regs(struct msm_gpu *gpu,
		struct a6xx_gpu_state *a6xx_state,
		const struct a6xx_indexed_registers *indexed,
		struct a6xx_gpu_state_obj *obj)
{
	u32 count = indexed->count;
	int i;

	obj->handle = (const void *) indexed;
	if (indexed->count_fn)
		count = indexed->count_fn(gpu);

	obj->data = state_kcalloc(a6xx_state, count, sizeof(u32));
	obj->count = count;
	if (!obj->data)
		return;

	/* All the indexed banks start at address 0 */
	gpu_write(gpu, indexed->addr, 0);

	/* Read the data - each read increments the internal address by 1 */
	for (i = 0; i < count; i++)
		obj->data[i] = gpu_read(gpu, indexed->data);
}

static void a6xx_get_indexed_registers(struct msm_gpu *gpu,
		struct a6xx_gpu_state *a6xx_state)
{
	u32 mempool_size;
	int count = ARRAY_SIZE(a6xx_indexed_reglist) + 1;
	int i;

	a6xx_state->indexed_regs = state_kcalloc(a6xx_state, count,
		sizeof(*a6xx_state->indexed_regs));
	if (!a6xx_state->indexed_regs)
		return;

	for (i = 0; i < ARRAY_SIZE(a6xx_indexed_reglist); i++)
		a6xx_get_indexed_regs(gpu, a6xx_state, &a6xx_indexed_reglist[i],
			&a6xx_state->indexed_regs[i]);

	if (adreno_is_a650_family(to_adreno_gpu(gpu))) {
		u32 val;

		val = gpu_read(gpu, REG_A6XX_CP_CHICKEN_DBG);
		gpu_write(gpu, REG_A6XX_CP_CHICKEN_DBG, val | 4);

		/* Get the contents of the CP mempool */
		a6xx_get_indexed_regs(gpu, a6xx_state, &a6xx_cp_mempool_indexed,
			&a6xx_state->indexed_regs[i]);

		gpu_write(gpu, REG_A6XX_CP_CHICKEN_DBG, val);
		a6xx_state->nr_indexed_regs = count;
		return;
	}

	/* Set the CP mempool size to 0 to stabilize it while dumping */
	mempool_size = gpu_read(gpu, REG_A6XX_CP_MEM_POOL_SIZE);
	gpu_write(gpu, REG_A6XX_CP_MEM_POOL_SIZE, 0);

	/* Get the contents of the CP mempool */
	a6xx_get_indexed_regs(gpu, a6xx_state, &a6xx_cp_mempool_indexed,
		&a6xx_state->indexed_regs[i]);

	/*
	 * Offset 0x2000 in the mempool is the size - copy the saved size over
	 * so the data is consistent
	 */
	a6xx_state->indexed_regs[i].data[0x2000] = mempool_size;

	/* Restore the size in the hardware */
	gpu_write(gpu, REG_A6XX_CP_MEM_POOL_SIZE, mempool_size);

	a6xx_state->nr_indexed_regs = count;
}

static void a7xx_get_indexed_registers(struct msm_gpu *gpu,
		struct a6xx_gpu_state *a6xx_state)
{
	struct adreno_gpu *adreno_gpu = to_adreno_gpu(gpu);
	const struct a6xx_indexed_registers *indexed_regs;
	int i, indexed_count, mempool_count;

	if (adreno_gpu->info->family <= ADRENO_7XX_GEN2) {
		indexed_regs = a7xx_indexed_reglist;
		indexed_count = ARRAY_SIZE(a7xx_indexed_reglist);
	} else {
		BUG_ON(adreno_gpu->info->family != ADRENO_7XX_GEN3);
		indexed_regs = gen7_9_0_cp_indexed_reg_list;
		indexed_count = ARRAY_SIZE(gen7_9_0_cp_indexed_reg_list);
	}

	mempool_count = ARRAY_SIZE(a7xx_cp_bv_mempool_indexed);

	a6xx_state->indexed_regs = state_kcalloc(a6xx_state,
					indexed_count + mempool_count,
					sizeof(*a6xx_state->indexed_regs));
	if (!a6xx_state->indexed_regs)
		return;

	a6xx_state->nr_indexed_regs = indexed_count + mempool_count;

	/* First read the common regs */
	for (i = 0; i < indexed_count; i++)
		a6xx_get_indexed_regs(gpu, a6xx_state, &indexed_regs[i],
			&a6xx_state->indexed_regs[i]);

	gpu_rmw(gpu, REG_A6XX_CP_CHICKEN_DBG, 0, BIT(2));
	gpu_rmw(gpu, REG_A7XX_CP_BV_CHICKEN_DBG, 0, BIT(2));

	/* Get the contents of the CP_BV mempool */
	for (i = 0; i < mempool_count; i++)
		a6xx_get_indexed_regs(gpu, a6xx_state, &a7xx_cp_bv_mempool_indexed[i],
			&a6xx_state->indexed_regs[indexed_count + i]);

	gpu_rmw(gpu, REG_A6XX_CP_CHICKEN_DBG, BIT(2), 0);
	gpu_rmw(gpu, REG_A7XX_CP_BV_CHICKEN_DBG, BIT(2), 0);
	return;
}

struct msm_gpu_state *a6xx_gpu_state_get(struct msm_gpu *gpu)
{
	struct a6xx_crashdumper _dumper = { 0 }, *dumper = NULL;
	struct adreno_gpu *adreno_gpu = to_adreno_gpu(gpu);
	struct a6xx_gpu *a6xx_gpu = to_a6xx_gpu(adreno_gpu);
	struct a6xx_gpu_state *a6xx_state = kzalloc(sizeof(*a6xx_state),
		GFP_KERNEL);
	bool stalled = !!(gpu_read(gpu, REG_A6XX_RBBM_STATUS3) &
			A6XX_RBBM_STATUS3_SMMU_STALLED_ON_FAULT);

	if (!a6xx_state)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&a6xx_state->objs);

	/* Get the generic state from the adreno core */
	adreno_gpu_state_get(gpu, &a6xx_state->base);

	if (!adreno_has_gmu_wrapper(adreno_gpu)) {
		a6xx_get_gmu_registers(gpu, a6xx_state);

		a6xx_state->gmu_log = a6xx_snapshot_gmu_bo(a6xx_state, &a6xx_gpu->gmu.log);
		a6xx_state->gmu_hfi = a6xx_snapshot_gmu_bo(a6xx_state, &a6xx_gpu->gmu.hfi);
		a6xx_state->gmu_debug = a6xx_snapshot_gmu_bo(a6xx_state, &a6xx_gpu->gmu.debug);

		a6xx_snapshot_gmu_hfi_history(gpu, a6xx_state);
	}

	/* If GX isn't on the rest of the data isn't going to be accessible */
	if (!adreno_has_gmu_wrapper(adreno_gpu) && !a6xx_gmu_gx_is_on(&a6xx_gpu->gmu))
		return &a6xx_state->base;

	/* Get the banks of indexed registers */
	if (adreno_is_a7xx(adreno_gpu))
		a7xx_get_indexed_registers(gpu, a6xx_state);
	else
		a6xx_get_indexed_registers(gpu, a6xx_state);

	/*
	 * Try to initialize the crashdumper, if we are not dumping state
	 * with the SMMU stalled.  The crashdumper needs memory access to
	 * write out GPU state, so we need to skip this when the SMMU is
	 * stalled in response to an iova fault
	 */
	if (!stalled && !gpu->needs_hw_init &&
	    !a6xx_crashdumper_init(gpu, &_dumper)) {
		dumper = &_dumper;
	}

	if (adreno_is_a7xx(adreno_gpu)) {
		a7xx_get_registers(gpu, a6xx_state, dumper);

		if (dumper) {
			a7xx_get_shaders(gpu, a6xx_state, dumper);
			a7xx_get_clusters(gpu, a6xx_state, dumper);
			a7xx_get_dbgahb_clusters(gpu, a6xx_state, dumper);

			msm_gem_kernel_put(dumper->bo, gpu->vm);
		}

		a7xx_get_post_crashdumper_registers(gpu, a6xx_state);
	} else {
		a6xx_get_registers(gpu, a6xx_state, dumper);

		if (dumper) {
			a6xx_get_shaders(gpu, a6xx_state, dumper);
			a6xx_get_clusters(gpu, a6xx_state, dumper);
			a6xx_get_dbgahb_clusters(gpu, a6xx_state, dumper);

			msm_gem_kernel_put(dumper->bo, gpu->vm);
		}
	}

	if (snapshot_debugbus)
		a6xx_get_debugbus(gpu, a6xx_state);

	a6xx_state->gpu_initialized = !gpu->needs_hw_init;

	return  &a6xx_state->base;
}

static void a6xx_gpu_state_destroy(struct kref *kref)
{
	struct a6xx_state_memobj *obj, *tmp;
	struct msm_gpu_state *state = container_of(kref,
			struct msm_gpu_state, ref);
	struct a6xx_gpu_state *a6xx_state = container_of(state,
			struct a6xx_gpu_state, base);

	if (a6xx_state->gmu_log)
		kvfree(a6xx_state->gmu_log->data);

	if (a6xx_state->gmu_hfi)
		kvfree(a6xx_state->gmu_hfi->data);

	if (a6xx_state->gmu_debug)
		kvfree(a6xx_state->gmu_debug->data);

	list_for_each_entry_safe(obj, tmp, &a6xx_state->objs, node) {
		list_del(&obj->node);
		kvfree(obj);
	}

	adreno_gpu_state_destroy(state);
	kfree(a6xx_state);
}

int a6xx_gpu_state_put(struct msm_gpu_state *state)
{
	if (IS_ERR_OR_NULL(state))
		return 1;

	return kref_put(&state->ref, a6xx_gpu_state_destroy);
}

static void a6xx_show_registers(const u32 *registers, u32 *data, size_t count,
		struct drm_printer *p)
{
	int i, index = 0;

	if (!data)
		return;

	for (i = 0; i < count; i += 2) {
		u32 count = RANGE(registers, i);
		u32 offset = registers[i];
		int j;

		for (j = 0; j < count; index++, offset++, j++) {
			if (data[index] == 0xdeafbead)
				continue;

			drm_printf(p, "  - { offset: 0x%06x, value: 0x%08x }\n",
				offset << 2, data[index]);
		}
	}
}

static void a7xx_show_registers_indented(const u32 *registers, u32 *data,
		struct drm_printer *p, unsigned indent)
{
	int i, index = 0;

	for (i = 0; registers[i] != UINT_MAX; i += 2) {
		u32 count = RANGE(registers, i);
		u32 offset = registers[i];
		int j;

		for (j = 0; j < count; index++, offset++, j++) {
			int k;

			if (data[index] == 0xdeafbead)
				continue;

			for (k = 0; k < indent; k++)
				drm_printf(p, "  ");
			drm_printf(p, "- { offset: 0x%06x, value: 0x%08x }\n",
				offset << 2, data[index]);
		}
	}
}

static void a7xx_show_registers(const u32 *registers, u32 *data, struct drm_printer *p)
{
	a7xx_show_registers_indented(registers, data, p, 1);
}

static void print_ascii85(struct drm_printer *p, size_t len, u32 *data)
{
	char out[ASCII85_BUFSZ];
	long i, l, datalen = 0;

	for (i = 0; i < len >> 2; i++) {
		if (data[i])
			datalen = (i + 1) << 2;
	}

	if (datalen == 0)
		return;

	drm_puts(p, "    data: !!ascii85 |\n");
	drm_puts(p, "      ");


	l = ascii85_encode_len(datalen);

	for (i = 0; i < l; i++)
		drm_puts(p, ascii85_encode(data[i], out));

	drm_puts(p, "\n");
}

static void print_name(struct drm_printer *p, const char *fmt, const char *name)
{
	drm_puts(p, fmt);
	drm_puts(p, name);
	drm_puts(p, "\n");
}

static void a6xx_show_shader(struct a6xx_gpu_state_obj *obj,
		struct drm_printer *p)
{
	const struct a6xx_shader_block *block = obj->handle;
	int i;

	if (!obj->handle)
		return;

	print_name(p, "  - type: ", block->name);

	for (i = 0; i < A6XX_NUM_SHADER_BANKS; i++) {
		drm_printf(p, "    - bank: %d\n", i);
		drm_printf(p, "      size: %d\n", block->size);

		if (!obj->data)
			continue;

		print_ascii85(p, block->size << 2,
			obj->data + (block->size * i));
	}
}

static void a7xx_show_shader(struct a6xx_gpu_state_obj *obj,
		struct drm_printer *p)
{
	const struct gen7_shader_block *block = obj->handle;
	int i, j;
	u32 *data = obj->data;

	if (!obj->handle)
		return;

	print_name(p, "  - type: ", a7xx_statetype_names[block->statetype]);
	print_name(p, "    - pipe: ", a7xx_pipe_names[block->pipeid]);

	for (i = 0; i < block->num_sps; i++) {
		drm_printf(p, "      - sp: %d\n", i);

		for (j = 0; j < block->num_usptps; j++) {
			drm_printf(p, "        - usptp: %d\n", j);
			drm_printf(p, "          size: %d\n", block->size);

			if (!obj->data)
				continue;

			print_ascii85(p, block->size << 2, data);

			data += block->size;
		}
	}
}

static void a6xx_show_cluster_data(const u32 *registers, int size, u32 *data,
		struct drm_printer *p)
{
	int ctx, index = 0;

	for (ctx = 0; ctx < A6XX_NUM_CONTEXTS; ctx++) {
		int j;

		drm_printf(p, "    - context: %d\n", ctx);

		for (j = 0; j < size; j += 2) {
			u32 count = RANGE(registers, j);
			u32 offset = registers[j];
			int k;

			for (k = 0; k < count; index++, offset++, k++) {
				if (data[index] == 0xdeafbead)
					continue;

				drm_printf(p, "      - { offset: 0x%06x, value: 0x%08x }\n",
					offset << 2, data[index]);
			}
		}
	}
}

static void a6xx_show_dbgahb_cluster(struct a6xx_gpu_state_obj *obj,
		struct drm_printer *p)
{
	const struct a6xx_dbgahb_cluster *dbgahb = obj->handle;

	if (dbgahb) {
		print_name(p, "  - cluster-name: ", dbgahb->name);
		a6xx_show_cluster_data(dbgahb->registers, dbgahb->count,
			obj->data, p);
	}
}

static void a6xx_show_cluster(struct a6xx_gpu_state_obj *obj,
		struct drm_printer *p)
{
	const struct a6xx_cluster *cluster = obj->handle;

	if (cluster) {
		print_name(p, "  - cluster-name: ", cluster->name);
		a6xx_show_cluster_data(cluster->registers, cluster->count,
			obj->data, p);
	}
}

static void a7xx_show_dbgahb_cluster(struct a6xx_gpu_state_obj *obj,
		struct drm_printer *p)
{
	const struct gen7_sptp_cluster_registers *dbgahb = obj->handle;

	if (dbgahb) {
		print_name(p, "  - pipe: ", a7xx_pipe_names[dbgahb->pipe_id]);
		print_name(p, "    - cluster-name: ", a7xx_cluster_names[dbgahb->cluster_id]);
		drm_printf(p, "      - context: %d\n", dbgahb->context_id);
		a7xx_show_registers_indented(dbgahb->regs, obj->data, p, 4);
	}
}

static void a7xx_show_cluster(struct a6xx_gpu_state_obj *obj,
		struct drm_printer *p)
{
	const struct gen7_cluster_registers *cluster = obj->handle;

	if (cluster) {
		int context = (cluster->context_id == STATE_FORCE_CTXT_1) ? 1 : 0;

		print_name(p, "  - pipe: ", a7xx_pipe_names[cluster->pipe_id]);
		print_name(p, "    - cluster-name: ", a7xx_cluster_names[cluster->cluster_id]);
		drm_printf(p, "      - context: %d\n", context);
		a7xx_show_registers_indented(cluster->regs, obj->data, p, 4);
	}
}

static void a6xx_show_indexed_regs(struct a6xx_gpu_state_obj *obj,
		struct drm_printer *p)
{
	const struct a6xx_indexed_registers *indexed = obj->handle;

	if (!indexed)
		return;

	print_name(p, "  - regs-name: ", indexed->name);
	drm_printf(p, "    dwords: %d\n", obj->count);

	print_ascii85(p, obj->count << 2, obj->data);
}

static void a6xx_show_debugbus_block(const struct a6xx_debugbus_block *block,
		u32 *data, struct drm_printer *p)
{
	if (block) {
		print_name(p, "  - debugbus-block: ", block->name);

		/*
		 * count for regular debugbus data is in quadwords,
		 * but print the size in dwords for consistency
		 */
		drm_printf(p, "    count: %d\n", block->count << 1);

		print_ascii85(p, block->count << 3, data);
	}
}

static void a6xx_show_debugbus(struct a6xx_gpu_state *a6xx_state,
		struct drm_printer *p)
{
	int i;

	for (i = 0; i < a6xx_state->nr_debugbus; i++) {
		struct a6xx_gpu_state_obj *obj = &a6xx_state->debugbus[i];

		a6xx_show_debugbus_block(obj->handle, obj->data, p);
	}

	if (a6xx_state->vbif_debugbus) {
		struct a6xx_gpu_state_obj *obj = a6xx_state->vbif_debugbus;

		drm_puts(p, "  - debugbus-block: A6XX_DBGBUS_VBIF\n");
		drm_printf(p, "    count: %d\n", VBIF_DEBUGBUS_BLOCK_SIZE);

		/* vbif debugbus data is in dwords.  Confusing, huh? */
		print_ascii85(p, VBIF_DEBUGBUS_BLOCK_SIZE << 2, obj->data);
	}

	for (i = 0; i < a6xx_state->nr_cx_debugbus; i++) {
		struct a6xx_gpu_state_obj *obj = &a6xx_state->cx_debugbus[i];

		a6xx_show_debugbus_block(obj->handle, obj->data, p);
	}
}

void a6xx_show(struct msm_gpu *gpu, struct msm_gpu_state *state,
		struct drm_printer *p)
{
	struct adreno_gpu *adreno_gpu = to_adreno_gpu(gpu);
	struct a6xx_gpu_state *a6xx_state = container_of(state,
			struct a6xx_gpu_state, base);
	int i;

	if (IS_ERR_OR_NULL(state))
		return;

	drm_printf(p, "gpu-initialized: %d\n", a6xx_state->gpu_initialized);

	adreno_show(gpu, state, p);

	drm_puts(p, "gmu-log:\n");
	if (a6xx_state->gmu_log) {
		struct msm_gpu_state_bo *gmu_log = a6xx_state->gmu_log;

		drm_printf(p, "    iova: 0x%016llx\n", gmu_log->iova);
		drm_printf(p, "    size: %zu\n", gmu_log->size);
		adreno_show_object(p, &gmu_log->data, gmu_log->size,
				&gmu_log->encoded);
	}

	drm_puts(p, "gmu-hfi:\n");
	if (a6xx_state->gmu_hfi) {
		struct msm_gpu_state_bo *gmu_hfi = a6xx_state->gmu_hfi;
		unsigned i, j;

		drm_printf(p, "    iova: 0x%016llx\n", gmu_hfi->iova);
		drm_printf(p, "    size: %zu\n", gmu_hfi->size);
		for (i = 0; i < ARRAY_SIZE(a6xx_state->hfi_queue_history); i++) {
			drm_printf(p, "    queue-history[%u]:", i);
			for (j = 0; j < HFI_HISTORY_SZ; j++) {
				drm_printf(p, " %d", a6xx_state->hfi_queue_history[i][j]);
			}
			drm_printf(p, "\n");
		}
		adreno_show_object(p, &gmu_hfi->data, gmu_hfi->size,
				&gmu_hfi->encoded);
	}

	drm_puts(p, "gmu-debug:\n");
	if (a6xx_state->gmu_debug) {
		struct msm_gpu_state_bo *gmu_debug = a6xx_state->gmu_debug;

		drm_printf(p, "    iova: 0x%016llx\n", gmu_debug->iova);
		drm_printf(p, "    size: %zu\n", gmu_debug->size);
		adreno_show_object(p, &gmu_debug->data, gmu_debug->size,
				&gmu_debug->encoded);
	}

	drm_puts(p, "registers:\n");
	for (i = 0; i < a6xx_state->nr_registers; i++) {
		struct a6xx_gpu_state_obj *obj = &a6xx_state->registers[i];

		if (!obj->handle)
			continue;

		if (adreno_is_a7xx(adreno_gpu)) {
			a7xx_show_registers(obj->handle, obj->data, p);
		} else {
			const struct a6xx_registers *regs = obj->handle;

			a6xx_show_registers(regs->registers, obj->data, regs->count, p);
		}
	}

	drm_puts(p, "registers-gmu:\n");
	for (i = 0; i < a6xx_state->nr_gmu_registers; i++) {
		struct a6xx_gpu_state_obj *obj = &a6xx_state->gmu_registers[i];
		const struct a6xx_registers *regs = obj->handle;

		if (!obj->handle)
			continue;

		a6xx_show_registers(regs->registers, obj->data, regs->count, p);
	}

	drm_puts(p, "indexed-registers:\n");
	for (i = 0; i < a6xx_state->nr_indexed_regs; i++)
		a6xx_show_indexed_regs(&a6xx_state->indexed_regs[i], p);

	drm_puts(p, "shader-blocks:\n");
	for (i = 0; i < a6xx_state->nr_shaders; i++) {
		if (adreno_is_a7xx(adreno_gpu))
			a7xx_show_shader(&a6xx_state->shaders[i], p);
		else
			a6xx_show_shader(&a6xx_state->shaders[i], p);
	}

	drm_puts(p, "clusters:\n");
	for (i = 0; i < a6xx_state->nr_clusters; i++) {
		if (adreno_is_a7xx(adreno_gpu))
			a7xx_show_cluster(&a6xx_state->clusters[i], p);
		else
			a6xx_show_cluster(&a6xx_state->clusters[i], p);
	}

	for (i = 0; i < a6xx_state->nr_dbgahb_clusters; i++) {
		if (adreno_is_a7xx(adreno_gpu))
			a7xx_show_dbgahb_cluster(&a6xx_state->dbgahb_clusters[i], p);
		else
			a6xx_show_dbgahb_cluster(&a6xx_state->dbgahb_clusters[i], p);
	}

	drm_puts(p, "debugbus:\n");
	a6xx_show_debugbus(a6xx_state, p);
}
