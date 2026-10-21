/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpmicro_flash

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/cache.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

#include <hpm_common.h>
#include <hpm_romapi.h>

#define HPM_FLASH_WRITE_CHUNK 128U

struct hpm_flash_config {
	XPI_Type *xpi;
	uint32_t mapped_base;
	uint32_t nor_cfg_hdr;
	uint32_t nor_cfg_opt0;
	uint32_t nor_cfg_opt1;
	uint32_t mapped_size;
	uint32_t write_block_size;
	struct flash_parameters parameters;
};

struct hpm_flash_data {
	xpi_nor_config_t nor_cfg;
	struct k_spinlock lock;
	uint32_t size;
	uint32_t page_size;
	uint32_t sector_size;
#ifdef CONFIG_FLASH_PAGE_LAYOUT
	struct flash_pages_layout layout;
#endif
};

static __ramfunc int hpm_flash_status_to_errno(hpm_stat_t status)
{
	switch (status) {
	case status_success:
		return 0;
	case status_invalid_argument:
		return -EINVAL;
	case status_timeout:
		return -ETIMEDOUT;
	default:
		return -EIO;
	}
}

static void hpm_flash_invalidate_cache(uint32_t addr, size_t size)
{
	size_t data_line_size = sys_cache_data_line_size_get();
	size_t instr_line_size = sys_cache_instr_line_size_get();
	uint32_t start;
	uint32_t end;

	if (size == 0U) {
		return;
	}

	if (data_line_size != 0U) {
		start = ROUND_DOWN(addr, data_line_size);
		end = ROUND_UP(addr + size, data_line_size);
		(void)sys_cache_data_invd_range((void *)(uintptr_t)start, end - start);
	}

	if (instr_line_size != 0U) {
		start = ROUND_DOWN(addr, instr_line_size);
		end = ROUND_UP(addr + size, instr_line_size);
		(void)sys_cache_instr_invd_range((void *)(uintptr_t)start, end - start);
	}
}

static int hpm_flash_range_is_valid(const struct hpm_flash_data *data, off_t offset, size_t size)
{
	if (offset < 0) {
		return false;
	}

	return ((uint64_t)offset + size) <= data->size;
}

static int hpm_flash_read(const struct device *dev, off_t offset, void *data, size_t size)
{
	const struct hpm_flash_config *config = dev->config;
	struct hpm_flash_data *dev_data = dev->data;
	uint32_t addr;

	if (size == 0U) {
		return 0;
	}

	if ((data == NULL) || !hpm_flash_range_is_valid(dev_data, offset, size)) {
		return -EINVAL;
	}

	addr = config->mapped_base + (uint32_t)offset;
	hpm_flash_invalidate_cache(addr, size);
	memcpy(data, (const void *)(uintptr_t)addr, size);

	return 0;
}

static __ramfunc int hpm_flash_write(const struct device *dev, off_t offset, const void *data,
				     size_t size)
{
	const struct hpm_flash_config *config = dev->config;
	struct hpm_flash_data *dev_data = dev->data;
	const uint8_t *src = data;
	uint32_t chunk[HPM_FLASH_WRITE_CHUNK / sizeof(uint32_t)];
	uint32_t addr;
	k_spinlock_key_t key;
	hpm_stat_t status;

	if (size == 0U) {
		return 0;
	}

	if ((data == NULL) || !hpm_flash_range_is_valid(dev_data, offset, size)) {
		return -EINVAL;
	}

	if (((uint32_t)offset % config->write_block_size) != 0U ||
	    (size % config->write_block_size) != 0U) {
		return -EINVAL;
	}

	addr = config->mapped_base + (uint32_t)offset;

	while (size > 0U) {
		size_t chunk_size = MIN(size, sizeof(chunk));

		if (dev_data->page_size != 0U) {
			chunk_size = MIN(chunk_size, dev_data->page_size - ((uint32_t)offset %
									    dev_data->page_size));
			chunk_size &= ~(config->write_block_size - 1U);
		}

		if (chunk_size == sizeof(chunk)) {
			memcpy(chunk, src, chunk_size);
		} else {
			memset(chunk, 0xff, sizeof(chunk));
			memcpy(chunk, src, chunk_size);
		}
		(void)sys_cache_data_flush_range(chunk, chunk_size);

		key = k_spin_lock(&dev_data->lock);
		status = rom_xpi_nor_program(config->xpi, xpi_xfer_channel_auto, &dev_data->nor_cfg,
					     (const uint32_t *)chunk, (uint32_t)offset, chunk_size);
		k_spin_unlock(&dev_data->lock, key);

		if (status != status_success) {
			return hpm_flash_status_to_errno(status);
		}

		hpm_flash_invalidate_cache(addr, chunk_size);
		src += chunk_size;
		offset += chunk_size;
		addr += chunk_size;
		size -= chunk_size;
	}

	return 0;
}

static __ramfunc int hpm_flash_erase(const struct device *dev, off_t offset, size_t size)
{
	const struct hpm_flash_config *config = dev->config;
	struct hpm_flash_data *dev_data = dev->data;
	uint32_t addr;

	if (size == 0U) {
		return 0;
	}

	if (!hpm_flash_range_is_valid(dev_data, offset, size) || (dev_data->sector_size == 0U) ||
	    (((uint32_t)offset % dev_data->sector_size) != 0U) ||
	    ((size % dev_data->sector_size) != 0U)) {
		return -EINVAL;
	}

	for (addr = (uint32_t)offset; addr < ((uint32_t)offset + size);
	     addr += dev_data->sector_size) {
		k_spinlock_key_t key = k_spin_lock(&dev_data->lock);
		hpm_stat_t status = rom_xpi_nor_erase_sector(config->xpi, xpi_xfer_channel_auto,
							     &dev_data->nor_cfg, addr);

		k_spin_unlock(&dev_data->lock, key);

		if (status != status_success) {
			return hpm_flash_status_to_errno(status);
		}
	}

	hpm_flash_invalidate_cache(config->mapped_base + (uint32_t)offset, size);

	return 0;
}

static int hpm_flash_get_size(const struct device *dev, uint64_t *size)
{
	const struct hpm_flash_data *data = dev->data;

	if (size == NULL) {
		return -EINVAL;
	}

	*size = data->size;

	return 0;
}

BUILD_ASSERT(DT_INST_PROP(0, write_block_size) == sizeof(uint32_t),
	     "HPM XPI ROM programming requires a 4-byte write block size");

static const struct flash_parameters *hpm_flash_get_parameters(const struct device *dev)
{
	const struct hpm_flash_config *config = dev->config;

	return &config->parameters;
}

#ifdef CONFIG_FLASH_PAGE_LAYOUT
static void hpm_flash_page_layout(const struct device *dev,
				  const struct flash_pages_layout **layout, size_t *layout_size)
{
	struct hpm_flash_data *data = dev->data;

	*layout = &data->layout;
	*layout_size = 1U;
}
#endif

static __ramfunc int hpm_flash_init(const struct device *dev)
{
	const struct hpm_flash_config *config = dev->config;
	struct hpm_flash_data *data = dev->data;
	xpi_nor_config_option_t option = {
		.header.U = config->nor_cfg_hdr,
		.option0.U = config->nor_cfg_opt0,
		.option1.U = config->nor_cfg_opt1,
	};
	uint32_t total_size;
	hpm_stat_t status;

	k_spinlock_key_t key = k_spin_lock(&data->lock);

	status = rom_xpi_nor_auto_config(config->xpi, &data->nor_cfg, &option);
	if (status != status_success) {
		k_spin_unlock(&data->lock, key);
		return hpm_flash_status_to_errno(status);
	}

	status = rom_xpi_nor_get_property(config->xpi, &data->nor_cfg, xpi_nor_property_total_size,
					  &total_size);
	if (status != status_success) {
		k_spin_unlock(&data->lock, key);
		return hpm_flash_status_to_errno(status);
	}

	status = rom_xpi_nor_get_property(config->xpi, &data->nor_cfg, xpi_nor_property_sector_size,
					  &data->sector_size);
	if (status != status_success) {
		k_spin_unlock(&data->lock, key);
		return hpm_flash_status_to_errno(status);
	}

	status = rom_xpi_nor_get_property(config->xpi, &data->nor_cfg, xpi_nor_property_page_size,
					  &data->page_size);
	if (status != status_success) {
		k_spin_unlock(&data->lock, key);
		return hpm_flash_status_to_errno(status);
	}

	if ((data->sector_size == 0U) || (total_size == 0U)) {
		k_spin_unlock(&data->lock, key);
		return -EINVAL;
	}

	data->size = MIN(total_size, config->mapped_size);

#ifdef CONFIG_FLASH_PAGE_LAYOUT
	data->layout.pages_count = data->size / data->sector_size;
	data->layout.pages_size = data->sector_size;
#endif

	k_spin_unlock(&data->lock, key);

	return 0;
}

static const struct flash_driver_api hpm_flash_api = {
	.read = hpm_flash_read,
	.write = hpm_flash_write,
	.erase = hpm_flash_erase,
	.get_parameters = hpm_flash_get_parameters,
	.get_size = hpm_flash_get_size,
#ifdef CONFIG_FLASH_PAGE_LAYOUT
	.page_layout = hpm_flash_page_layout,
#endif
};

#define HPM_FLASH_INIT(n)                                                                          \
	static const struct hpm_flash_config hpm_flash_config_##n = {                              \
		.xpi = (XPI_Type *)DT_REG_ADDR(DT_INST_PARENT(n)),                                 \
		.mapped_base = DT_INST_REG_ADDR(n),                                                \
		.nor_cfg_hdr = DT_INST_PROP(n, nor_cfg_opt_hdr),                                   \
		.nor_cfg_opt0 = DT_INST_PROP(n, nor_cfg_opt_opt0),                                 \
		.nor_cfg_opt1 = DT_INST_PROP(n, nor_cfg_opt_opt1),                                 \
		.write_block_size = DT_INST_PROP(n, write_block_size),                             \
		.parameters =                                                                      \
			{                                                                          \
				.write_block_size = DT_INST_PROP(n, write_block_size),             \
				.erase_value = 0xff,                                               \
			},                                                                         \
	};                                                                                         \
                                                                                                   \
	static struct hpm_flash_data hpm_flash_data_##n;                                           \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, hpm_flash_init, NULL, &hpm_flash_data_##n, &hpm_flash_config_##n, \
			      POST_KERNEL, CONFIG_FLASH_INIT_PRIORITY, &hpm_flash_api);

DT_INST_FOREACH_STATUS_OKAY(HPM_FLASH_INIT)
