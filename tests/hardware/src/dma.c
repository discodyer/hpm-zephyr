/* SPDX-License-Identifier: Apache-2.0 */

#include <string.h>
#include <zephyr/cache.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/ztest.h>

#define BLOCK_LEN 256
static const struct device *const dma = DEVICE_DT_GET(DT_NODELABEL(hdma));
static uint8_t source[BLOCK_LEN * 3] __aligned(32);
static uint8_t destination[BLOCK_LEN * 3 + 64] __aligned(32);
static int channel = -1;
static int irq_status;
static atomic_t completions;
static K_SEM_DEFINE(done, 0, 1);

static void dma_done(const struct device *dev, void *data, uint32_t chan, int status)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(chan);
	irq_status = status;
	atomic_inc(&completions);
	k_sem_give(data);
}

static void dma_after(void *fixture)
{
	ARG_UNUSED(fixture);
	if (channel >= 0) {
		(void)dma_stop(dma, channel);
		dma_release_channel(dma, channel);
		channel = -1;
	}
	k_sem_reset(&done);
}

ZTEST(hpm_dma_transfer, test_three_linked_blocks_and_guards)
{
	struct dma_block_config blocks[3] = {0};
	struct dma_config config = {
		.channel_direction = MEMORY_TO_MEMORY,
		.source_data_size = 1,
		.dest_data_size = 1,
		.source_burst_length = 1,
		.dest_burst_length = 1,
		.block_count = ARRAY_SIZE(blocks),
		.head_block = blocks,
		.dma_callback = dma_done,
		.user_data = &done,
	};

	zassert_true(device_is_ready(dma));
	for (size_t i = 0; i < sizeof(source); ++i) {
		source[i] = (i * 37 + (i >> 8)) & 0xff;
	}
	memset(destination, 0xa5, sizeof(destination));
	for (size_t i = 0; i < ARRAY_SIZE(blocks); ++i) {
		blocks[i].source_address = (uintptr_t)&source[i * BLOCK_LEN];
		blocks[i].dest_address = (uintptr_t)&destination[32 + i * BLOCK_LEN];
		blocks[i].block_size = BLOCK_LEN;
		blocks[i].source_addr_adj = DMA_ADDR_ADJ_INCREMENT;
		blocks[i].dest_addr_adj = DMA_ADDR_ADJ_INCREMENT;
		blocks[i].next_block = i + 1 < ARRAY_SIZE(blocks) ? &blocks[i + 1] : NULL;
	}
	(void)sys_cache_data_flush_range(source, sizeof(source));
	(void)sys_cache_data_flush_range(destination, sizeof(destination));
	channel = dma_request_channel(dma, NULL);
	zassert_true(channel >= 0);
	zassert_ok(dma_config(dma, channel, &config));
	zassert_ok(dma_start(dma, channel));
	zassert_ok(k_sem_take(&done, K_SECONDS(2)), "DMA completion IRQ timeout");
	zassert_equal(irq_status, DMA_STATUS_COMPLETE);
	(void)sys_cache_data_invd_range(destination, sizeof(destination));
	zassert_mem_equal(source, &destination[32], sizeof(source));
	for (size_t i = 0; i < 32; ++i) {
		zassert_equal(destination[i], 0xa5, "DMA wrote before destination");
		zassert_equal(destination[sizeof(destination) - 1 - i], 0xa5,
			      "DMA wrote beyond destination");
	}
}

ZTEST(hpm_dma_transfer, test_cyclic_suspend_resume)
{
	struct dma_block_config block = {
		.source_address = (uintptr_t)source,
		.dest_address = (uintptr_t)&destination[32],
		.block_size = sizeof(source),
		.source_addr_adj = DMA_ADDR_ADJ_INCREMENT,
		.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT,
	};
	struct dma_config config = {
		.channel_direction = MEMORY_TO_MEMORY,
		.source_data_size = 1,
		.dest_data_size = 1,
		.source_burst_length = 1,
		.dest_burst_length = 1,
		.block_count = 1,
		.head_block = &block,
		.cyclic = true,
		.dma_callback = dma_done,
		.user_data = &done,
	};
	struct dma_status status;
	atomic_val_t suspended_count;

	zassert_true(device_is_ready(dma));
	memset(source, 0x3c, sizeof(source));
	(void)sys_cache_data_flush_range(source, sizeof(source));
	atomic_clear(&completions);
	channel = dma_request_channel(dma, NULL);
	zassert_true(channel >= 0);
	zassert_ok(dma_config(dma, channel, &config));
	zassert_ok(dma_start(dma, channel));
	zassert_ok(k_sem_take(&done, K_SECONDS(1)));
	zassert_ok(dma_suspend(dma, channel));
	/* Let a completion already pending at suspend drain before counting. */
	k_msleep(1);
	suspended_count = atomic_get(&completions);
	k_msleep(2);
	zassert_equal(atomic_get(&completions), suspended_count,
		      "Cyclic DMA continues interrupting while suspended");
	zassert_ok(dma_get_status(dma, channel, &status));
	zassert_false(status.busy);
	k_sem_reset(&done);
	zassert_ok(dma_resume(dma, channel));
	zassert_ok(k_sem_take(&done, K_SECONDS(1)), "DMA did not restart after resume");
	zassert_ok(dma_stop(dma, channel));
	zassert_equal(irq_status, DMA_STATUS_COMPLETE);
	(void)sys_cache_data_invd_range(destination, sizeof(destination));
	zassert_mem_equal(source, &destination[32], sizeof(source));
}

ZTEST_SUITE(hpm_dma_transfer, NULL, NULL, NULL, dma_after, NULL);
