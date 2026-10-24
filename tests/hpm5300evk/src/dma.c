/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/drivers/dma.h>
#include <zephyr/ztest.h>

#if DT_NODE_HAS_STATUS(DT_NODELABEL(hdma), okay)
ZTEST(hpm_dma, test_linked_transfer_configuration)
{
	static uint8_t src[64] __aligned(4);
	static uint8_t dst[64] __aligned(4);
	struct dma_block_config blocks[2] = {
		{
			.source_address = (uint32_t)src,
			.dest_address = (uint32_t)dst,
			.block_size = 16U,
			.source_addr_adj = DMA_ADDR_ADJ_INCREMENT,
			.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT,
			.next_block = &blocks[1],
		},
		{
			.source_address = (uint32_t)src + 16U,
			.dest_address = (uint32_t)dst + 16U,
			.block_size = 16U,
			.source_addr_adj = DMA_ADDR_ADJ_INCREMENT,
			.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT,
		},
	};
	struct dma_config config = {
		.channel_direction = MEMORY_TO_MEMORY,
		.source_data_size = 1U,
		.dest_data_size = 1U,
		.head_block = &blocks[0],
		.block_count = ARRAY_SIZE(blocks),
	};
	uint32_t max_blocks;
	int channel;

	zassert_true(device_is_ready(DEVICE_DT_GET(DT_NODELABEL(hdma))));
	zassert_ok(dma_get_attribute(DEVICE_DT_GET(DT_NODELABEL(hdma)), DMA_ATTR_MAX_BLOCK_COUNT,
				     &max_blocks));
	zassert_equal(max_blocks, CONFIG_DMA_HPMICRO_MAX_BLOCKS);
	channel = dma_request_channel(DEVICE_DT_GET(DT_NODELABEL(hdma)), NULL);
	zassert_true(channel >= 0);
	zassert_ok(dma_config(DEVICE_DT_GET(DT_NODELABEL(hdma)), (uint32_t)channel, &config));
	dma_release_channel(DEVICE_DT_GET(DT_NODELABEL(hdma)), (uint32_t)channel);
}
#endif

ZTEST_SUITE(hpm_dma, NULL, NULL, NULL, NULL, NULL);
