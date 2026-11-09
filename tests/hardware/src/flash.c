/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * Copyright (c) 2026 Cody Gu <gujiaqi@iscas.ac.cn>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>
#include <zephyr/cache.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/ztest.h>

#define SCRATCH_NODE DT_NODELABEL(hw_scratch)
#define IMAGE_NODE   DT_CHOSEN(zephyr_code_partition)
#define JEDEC_ID_LEN 3U
BUILD_ASSERT(DT_REG_ADDR(SCRATCH_NODE) >= DT_REG_ADDR(IMAGE_NODE) + DT_REG_SIZE(IMAGE_NODE));
BUILD_ASSERT(DT_REG_SIZE(SCRATCH_NODE) == 4096);
BUILD_ASSERT((DT_REG_ADDR(SCRATCH_NODE) % 4096) == 0);
static uint8_t pattern[256] __aligned(32);
static uint8_t readback[256] __aligned(32);
static const struct flash_area *area;

static const struct device *flash_device(void)
{
	zassert_ok(flash_area_open(PARTITION_ID(hw_scratch), &area));
	zassert_true(device_is_ready(area->fa_dev));

	return area->fa_dev;
}

static void flash_after(void *fixture)
{
	ARG_UNUSED(fixture);
	if (area != NULL) {
		flash_area_close(area);
		area = NULL;
	}
}

ZTEST(hpm_flash_scratch, test_erase_program_read_and_xip_cache)
{
	const volatile uint8_t *mapped;

	(void)flash_device();
	mapped = (const volatile uint8_t *)(DT_REG_ADDR(DT_NODELABEL(flash0)) + area->fa_off);
	printk("NOR scratch offset 0x%lx only\n", (unsigned long)area->fa_off);
	zassert_ok(flash_area_erase(area, 0, area->fa_size));
	/* Check the entire reserved sector, rather than just the programmed page. */
	for (off_t offset = 0; offset < area->fa_size; offset += sizeof(readback)) {
		zassert_ok(flash_area_read(area, offset, readback, sizeof(readback)));
		for (size_t i = 0; i < sizeof(readback); ++i) {
			zassert_equal(readback[i], 0xff, "Erase failed at %ld", (long)offset + i);
		}
	}
	for (size_t i = 0; i < sizeof(pattern); ++i) {
		pattern[i] = (i * 37) ^ 0xa5;
		zassert_equal(mapped[i], 0xff);
	}
	zassert_ok(flash_area_write(area, 0, pattern, sizeof(pattern)));
	/* Observe the primed XPI mapping before flash_read() can invalidate it. */
	for (size_t i = 0; i < sizeof(pattern); ++i) {
		zassert_equal(mapped[i], pattern[i], "XPI mapped cache is stale at %u",
			      (unsigned int)i);
	}
	zassert_ok(flash_area_read(area, 0, readback, sizeof(readback)));
	zassert_mem_equal(pattern, readback, sizeof(pattern));
	zassert_ok(flash_area_erase(area, 0, area->fa_size));
	for (size_t i = 0; i < sizeof(pattern); ++i) {
		zassert_equal(mapped[i], 0xff, "XPI mapped cache is stale after erase");
	}
	zassert_ok(flash_area_read(area, 0, readback, sizeof(readback)));
	for (size_t i = 0; i < sizeof(readback); ++i) {
		zassert_equal(readback[i], 0xff, "Second erase failed");
	}
}

ZTEST(hpm_flash_scratch, test_jedec_id)
{
	static const uint8_t all_ones[JEDEC_ID_LEN] = {0xff, 0xff, 0xff};
	static const uint8_t all_zero[JEDEC_ID_LEN];
	const struct device *flash = flash_device();
	uint8_t id[JEDEC_ID_LEN];
	uint8_t id_repeat[JEDEC_ID_LEN];

	zassert_ok(flash_read_jedec_id(flash, id));
	zassert_ok(flash_read_jedec_id(flash, id_repeat));
	printk("JEDEC ID: %02x %02x %02x\n", id[0], id[1], id[2]);
	zassert_mem_equal(id, id_repeat, sizeof(id));
	zassert_not_equal(memcmp(id, all_zero, sizeof(id)), 0, "JEDEC ID is all zeroes");
	zassert_not_equal(memcmp(id, all_ones, sizeof(id)), 0, "JEDEC ID is all ones");
	zassert_equal(flash_read_jedec_id(flash, NULL), -EINVAL);
}

ZTEST(hpm_flash_scratch, test_sfdp_header_and_bfp)
{
	const struct device *flash = flash_device();
	uint8_t header[16];
	uint8_t bfp[64];
	uint16_t bfp_id;
	uint32_t bfp_addr;
	size_t bfp_len;
	uint8_t byte;

	zassert_ok(flash_sfdp_read(flash, 0, header, sizeof(header)));
	zassert_mem_equal(header, "SFDP", 4);

	bfp_id = (uint16_t)(header[8] | (header[15] << 8));
	bfp_addr =
		(uint32_t)header[12] | ((uint32_t)header[13] << 8) | ((uint32_t)header[14] << 16);
	printk("SFDP v%u.%u, %u parameter header(s), BFP %04x at 0x%x, %u DW\n", header[5],
	       header[4], header[6] + 1U, bfp_id, bfp_addr, header[11]);
	zassert_equal(bfp_id, 0xff00, "First SFDP parameter is not BFP");
	zassert_true(header[11] > 0U, "BFP length is zero");
	bfp_len = (size_t)header[11] * sizeof(uint32_t);
	zassert_true(bfp_len > 32U, "BFP does not exercise multi-block SFDP reads");
	zassert_true(bfp_len <= sizeof(bfp), "BFP is larger than the test buffer");
	zassert_ok(flash_sfdp_read(flash, bfp_addr, bfp, bfp_len));

	zassert_equal(flash_sfdp_read(flash, -1, &byte, sizeof(byte)), -EINVAL);
	zassert_equal(flash_sfdp_read(flash, 0, NULL, sizeof(byte)), -EINVAL);
	zassert_equal(flash_sfdp_read(flash, 0x1000000, &byte, sizeof(byte)), -EINVAL);
	zassert_ok(flash_sfdp_read(flash, 0, NULL, 0));
}

ZTEST(hpm_flash_scratch, test_jedec_sfdp_repeat_and_program)
{
	const struct device *flash = flash_device();
	const uint32_t marker_first = 0x5aa5c33c;
	const uint32_t marker_second = 0xc33c5aa5;
	uint8_t id_first[JEDEC_ID_LEN];
	uint8_t id_second[JEDEC_ID_LEN];
	uint8_t sfdp_first[16];
	uint8_t sfdp_second[16];
	uint32_t readback;

	zassert_ok(flash_read_jedec_id(flash, id_first));
	zassert_ok(flash_sfdp_read(flash, 0, sfdp_first, sizeof(sfdp_first)));

	zassert_ok(flash_area_erase(area, 0, area->fa_size));
	zassert_ok(flash_area_write(area, 0, &marker_first, sizeof(marker_first)));
	zassert_ok(flash_area_read(area, 0, &readback, sizeof(readback)));
	zassert_equal(readback, marker_first);

	zassert_ok(flash_read_jedec_id(flash, id_second));
	zassert_ok(flash_sfdp_read(flash, 0, sfdp_second, sizeof(sfdp_second)));
	zassert_mem_equal(id_first, id_second, sizeof(id_first));
	zassert_mem_equal(sfdp_first, sfdp_second, sizeof(sfdp_first));

	zassert_ok(flash_area_erase(area, 0, area->fa_size));
	zassert_ok(flash_area_write(area, 0, &marker_second, sizeof(marker_second)));
	zassert_ok(flash_area_read(area, 0, &readback, sizeof(readback)));
	zassert_equal(readback, marker_second);
	zassert_ok(flash_area_erase(area, 0, area->fa_size));
}

ZTEST_SUITE(hpm_flash_scratch, NULL, NULL, NULL, flash_after, NULL);
