/* SPDX-License-Identifier: Apache-2.0 */

#include <string.h>
#include <zephyr/cache.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/ztest.h>

#define SCRATCH_NODE DT_NODELABEL(hw_scratch)
#define IMAGE_NODE   DT_CHOSEN(zephyr_code_partition)
BUILD_ASSERT(DT_REG_ADDR(SCRATCH_NODE) >= DT_REG_ADDR(IMAGE_NODE) + DT_REG_SIZE(IMAGE_NODE));
BUILD_ASSERT(DT_REG_SIZE(SCRATCH_NODE) == 4096);
BUILD_ASSERT((DT_REG_ADDR(SCRATCH_NODE) % 4096) == 0);
static uint8_t pattern[256] __aligned(32);
static uint8_t readback[256] __aligned(32);
static const struct flash_area *area;

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

	zassert_ok(flash_area_open(PARTITION_ID(hw_scratch), &area));
	zassert_true(device_is_ready(area->fa_dev));
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

ZTEST_SUITE(hpm_flash_scratch, NULL, NULL, NULL, flash_after, NULL);
