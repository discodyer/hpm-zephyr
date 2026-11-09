/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * Copyright (c) 2026 Cody Gu <gujiaqi@iscas.ac.cn>
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_xpi

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

#define HPM_FLASH_WRITE_CHUNK       128U
#define HPM_FLASH_CMD_READ_ID       0x9fU
#define HPM_FLASH_CMD_READ_SFDP     0x5aU
#define HPM_FLASH_READ_ID_LEN       3U
#define HPM_FLASH_SFDP_DUMMY_CYCLES 8U
#define HPM_FLASH_SFDP_IO_SIZE      32U
#define HPM_FLASH_SFDP_ADDR_BITS    24U
#define HPM_FLASH_SFDP_SIZE         (1U << HPM_FLASH_SFDP_ADDR_BITS)
#define HPM_FLASH_XPI_SEQ_IDX       xpi_std_nor_seq_idx_page_program

#define HPM_FLASH_NODE(n) DT_INST_CHILD_BY_UNIT_ADDR_INT(n, 2147483648)

#if defined(CONFIG_XIP)
#define HPM_FLASH_RAMFUNC __noinline Z_GENERIC_DOT_SECTION(itcm)
#else
#define HPM_FLASH_RAMFUNC
#endif

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

static HPM_FLASH_RAMFUNC int hpm_flash_status_to_errno(hpm_stat_t status)
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

static void hpm_flash_invalidate_data_cache(uint32_t addr, size_t size)
{
	size_t data_line_size = sys_cache_data_line_size_get();
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
}

static void hpm_flash_invalidate_instruction_cache(void)
{
	/* VA CCTL operations can fault on the XIP flash mapping. */
	(void)sys_cache_instr_invd_all();
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
	hpm_flash_invalidate_data_cache(addr, size);
	memcpy(data, (const void *)(uintptr_t)addr, size);

	return 0;
}

static HPM_FLASH_RAMFUNC int hpm_flash_write(const struct device *dev, off_t offset,
					     const void *data, size_t size)
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

		hpm_flash_invalidate_data_cache(addr, chunk_size);
		hpm_flash_invalidate_instruction_cache();
		src += chunk_size;
		offset += chunk_size;
		addr += chunk_size;
		size -= chunk_size;
	}

	return 0;
}

static HPM_FLASH_RAMFUNC int hpm_flash_erase(const struct device *dev, off_t offset, size_t size)
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

	hpm_flash_invalidate_data_cache(config->mapped_base + (uint32_t)offset, size);
	hpm_flash_invalidate_instruction_cache();

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

#if defined(CONFIG_FLASH_JESD216_API)
static hpm_stat_t hpm_flash_xpi_read(const struct device *dev, uint8_t command, bool addressed,
				     uint32_t address, void *data, size_t size)
{
	const struct hpm_flash_config *config = dev->config;
	struct hpm_flash_data *dev_data = dev->data;
	xpi_instr_seq_t sequence = {0};
	xpi_instr_seq_t saved;
	xpi_xfer_ctx_t transfer = {
		.addr = address,
		.channel = xpi_xfer_channel_auto,
		.cmd_type = xpi_apb_xfer_type_read,
		.seq_idx = HPM_FLASH_XPI_SEQ_IDX,
		.seq_num = 1U,
		.buf = data,
		.xfer_size = size,
	};
	uint16_t *phase = (uint16_t *)sequence.entry;
	size_t phase_index = 0U;
	hpm_stat_t restore_status;
	hpm_stat_t status;
	k_spinlock_key_t key;

	phase[phase_index++] = SUB_INSTR(XPI_PHASE_CMD_SDR, XPI_1PAD, command);
	if (addressed) {
		phase[phase_index++] =
			SUB_INSTR(XPI_PHASE_RADDR_SDR, XPI_1PAD, HPM_FLASH_SFDP_ADDR_BITS);
		phase[phase_index++] =
			SUB_INSTR(XPI_PHASE_DUMMY_SDR, XPI_1PAD, HPM_FLASH_SFDP_DUMMY_CYCLES);
	}
	phase[phase_index] = SUB_INSTR(XPI_PHASE_READ_SDR, XPI_1PAD, 4U);

	key = k_spin_lock(&dev_data->lock);
	saved = dev_data->nor_cfg.instr_set[HPM_FLASH_XPI_SEQ_IDX];
	status = ROM_API_TABLE_ROOT->xpi_driver_if->update_instr_table(config->xpi, sequence.entry,
								       HPM_FLASH_XPI_SEQ_IDX, 1U);
	if (status == status_success) {
		status = ROM_API_TABLE_ROOT->xpi_driver_if->transfer_blocking(config->xpi,
									      &transfer);
	}
	restore_status = ROM_API_TABLE_ROOT->xpi_driver_if->update_instr_table(
		config->xpi, saved.entry, HPM_FLASH_XPI_SEQ_IDX, 1U);
	fencei();
	k_spin_unlock(&dev_data->lock, key);

	if (status != status_success) {
		return status;
	}

	return restore_status;
}

static int hpm_flash_read_jedec_id(const struct device *dev, uint8_t *id)
{
	uint32_t id_data = 0U;
	hpm_stat_t status;

	if (id == NULL) {
		return -EINVAL;
	}

	status = hpm_flash_xpi_read(dev, HPM_FLASH_CMD_READ_ID, false, 0U, &id_data,
				    HPM_FLASH_READ_ID_LEN);
	if (status != status_success) {
		return hpm_flash_status_to_errno(status);
	}

	memcpy(id, &id_data, HPM_FLASH_READ_ID_LEN);

	return 0;
}

static int hpm_flash_sfdp_read(const struct device *dev, off_t offset, void *data, size_t size)
{
	uint32_t buffer[HPM_FLASH_SFDP_IO_SIZE / sizeof(uint32_t)];
	uint8_t *destination = data;
	uint64_t end;

	if (size == 0U) {
		return 0;
	}

	end = (uint64_t)offset + size;
	if ((data == NULL) || (offset < 0) || (end > HPM_FLASH_SFDP_SIZE)) {
		return -EINVAL;
	}

	while (size > 0U) {
		size_t chunk_size = MIN(size, sizeof(buffer));
		hpm_stat_t status;

		status = hpm_flash_xpi_read(dev, HPM_FLASH_CMD_READ_SFDP, true, (uint32_t)offset,
					    buffer, chunk_size);
		if (status != status_success) {
			return hpm_flash_status_to_errno(status);
		}

		memcpy(destination, buffer, chunk_size);
		destination += chunk_size;
		offset += chunk_size;
		size -= chunk_size;
	}

	return 0;
}
#endif /* CONFIG_FLASH_JESD216_API */

BUILD_ASSERT(DT_PROP(HPM_FLASH_NODE(0), write_block_size) == sizeof(uint32_t),
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

static HPM_FLASH_RAMFUNC int hpm_flash_init(const struct device *dev)
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

static DEVICE_API(flash, hpm_flash_api) = {
	.read = hpm_flash_read,
	.write = hpm_flash_write,
	.erase = hpm_flash_erase,
	.get_parameters = hpm_flash_get_parameters,
	.get_size = hpm_flash_get_size,
#ifdef CONFIG_FLASH_PAGE_LAYOUT
	.page_layout = hpm_flash_page_layout,
#endif
#if defined(CONFIG_FLASH_JESD216_API)
	.sfdp_read = hpm_flash_sfdp_read,
	.read_jedec_id = hpm_flash_read_jedec_id,
#endif
};

#define HPM_FLASH_INIT(n)                                                                          \
	static const struct hpm_flash_config hpm_flash_config_##n = {                              \
		.xpi = (XPI_Type *)DT_INST_REG_ADDR(n),                                            \
		.mapped_base = DT_REG_ADDR(HPM_FLASH_NODE(n)),                                     \
		.mapped_size = DT_REG_SIZE(HPM_FLASH_NODE(n)),                                     \
		.nor_cfg_hdr = DT_PROP(HPM_FLASH_NODE(n), nor_cfg_opt_hdr),                        \
		.nor_cfg_opt0 = DT_PROP(HPM_FLASH_NODE(n), nor_cfg_opt_opt0),                      \
		.nor_cfg_opt1 = DT_PROP(HPM_FLASH_NODE(n), nor_cfg_opt_opt1),                      \
		.write_block_size = DT_PROP(HPM_FLASH_NODE(n), write_block_size),                  \
		.parameters =                                                                      \
			{                                                                          \
				.write_block_size = DT_PROP(HPM_FLASH_NODE(n), write_block_size),  \
				.erase_value = 0xff,                                               \
			},                                                                         \
	};                                                                                         \
                                                                                                   \
	static struct hpm_flash_data hpm_flash_data_##n;                                           \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, hpm_flash_init, NULL, &hpm_flash_data_##n, &hpm_flash_config_##n, \
			      POST_KERNEL, CONFIG_FLASH_INIT_PRIORITY, &hpm_flash_api);

DT_INST_FOREACH_STATUS_OKAY(HPM_FLASH_INIT)
