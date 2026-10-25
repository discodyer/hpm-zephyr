/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_otp

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/otp.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <drivers/otp/hpmicro_otp.h>

#include <hpm_common.h>

LOG_MODULE_REGISTER(otp_hpmicro, CONFIG_OTP_HPMICRO_LOG_LEVEL);

/*
 * These names alias the upstream SDK entry points. hpm_otp_drv.h cannot be
 * included here because its otp_program() declaration conflicts with the
 * Zephyr OTP syscall of the same name.
 */
uint32_t hpm_sdk_otp_read_from_shadow(uint32_t addr) __asm__("otp_read_from_shadow");
hpm_stat_t hpm_sdk_otp_program(uint32_t addr, const uint32_t *src,
			       uint32_t num_of_words) __asm__("otp_program");
void hpm_sdk_otp_init(void) __asm__("otp_init");
hpm_stat_t hpm_sdk_otp_reload(uint32_t region) __asm__("otp_reload");
hpm_stat_t hpm_sdk_otp_lock_otp(uint32_t addr, uint32_t option) __asm__("otp_lock_otp");
hpm_stat_t hpm_sdk_otp_lock_shadow(uint32_t addr, uint32_t option) __asm__("otp_lock_shadow");
hpm_stat_t
hpm_sdk_otp_set_configurable_region(uint32_t start,
				    uint32_t words) __asm__("otp_set_configurable_region");

#define HPM_OTP_WORDS 128U
#define HPM_OTP_BYTES (HPM_OTP_WORDS * sizeof(uint32_t))

struct hpm_otp_config {
	size_t size;
};

struct hpm_otp_data {
	struct k_mutex lock;
};

static bool hpm_otp_range_is_valid(const struct hpm_otp_config *config, off_t offset, size_t len)
{
	size_t start;

	if (offset < 0) {
		return false;
	}

	start = (size_t)offset;

	return start <= config->size && len <= config->size - start;
}

static int hpm_otp_read(const struct device *dev, off_t offset, void *buffer, size_t len)
{
	const struct hpm_otp_config *config = dev->config;
	struct hpm_otp_data *data = dev->data;
	uint8_t *out = buffer;
	size_t skip;
	size_t remaining;
	uint32_t word_index;
	uint32_t word;

	if ((buffer == NULL) && (len != 0U)) {
		return -EINVAL;
	}

	if (!hpm_otp_range_is_valid(config, offset, len)) {
		return -EINVAL;
	}

	if (len == 0U) {
		return 0;
	}

	word_index = (uint32_t)offset / sizeof(uint32_t);
	skip = (size_t)offset % sizeof(uint32_t);
	remaining = len;

	k_mutex_lock(&data->lock, K_FOREVER);
	while (remaining != 0U) {
		size_t chunk;

		word = hpm_sdk_otp_read_from_shadow(word_index);
		chunk = MIN(remaining, sizeof(word) - skip);
		memcpy(out, (const uint8_t *)&word + skip, chunk);
		out += chunk;
		remaining -= chunk;
		word_index++;
		skip = 0U;
	}
	k_mutex_unlock(&data->lock);

	return 0;
}

#if defined(CONFIG_OTP_PROGRAM)
static int hpm_otp_program(const struct device *dev, off_t offset, const void *buffer, size_t len)
{
	const struct hpm_otp_config *config = dev->config;
	struct hpm_otp_data *data = dev->data;
	const uint8_t *in = buffer;
	uint32_t word_index;
	hpm_stat_t status = status_success;

	if ((buffer == NULL) && (len != 0U)) {
		return -EINVAL;
	}

	if (!IS_ALIGNED(offset, sizeof(uint32_t)) || !IS_ALIGNED(len, sizeof(uint32_t)) ||
	    !hpm_otp_range_is_valid(config, offset, len)) {
		return -EINVAL;
	}

	word_index = (uint32_t)offset / sizeof(uint32_t);

	k_mutex_lock(&data->lock, K_FOREVER);
	/*
	 * The HPM SDK only permits one word per call, so split a multi-word
	 * request into individual programming operations.
	 */
	for (size_t index = 0U; index < len / sizeof(uint32_t); index++) {
		uint32_t word;

		memcpy(&word, in + index * sizeof(uint32_t), sizeof(word));
		status = hpm_sdk_otp_program(word_index + index, &word, 1U);
		if (status != status_success) {
			break;
		}
	}
	k_mutex_unlock(&data->lock);

	return status == status_success ? 0 : -EIO;
}
#endif

static int hpm_otp_status_to_errno(hpm_stat_t status)
{
	if (status == status_success) {
		return 0;
	}

	if (status == status_invalid_argument) {
		return -EINVAL;
	}

	return -EIO;
}

static int hpm_otp_reload_impl(const struct device *dev, enum hpm_otp_region region)
{
	struct hpm_otp_data *data = dev->data;
	hpm_stat_t status;

	if (region > HPM_OTP_REGION_3) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	status = hpm_sdk_otp_reload(BIT(region));
	k_mutex_unlock(&data->lock);

	return hpm_otp_status_to_errno(status);
}

#if defined(CONFIG_OTP_HPMICRO_ALLOW_LOCK)
static int hpm_otp_lock_impl(const struct device *dev, uint32_t word_index,
			     enum hpm_otp_lock_option option)
{
	struct hpm_otp_data *data = dev->data;
	hpm_stat_t status;

	if ((word_index >= HPM_OTP_WORD_COUNT) || (option > HPM_OTP_DISABLE_ACCESS)) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	status = hpm_sdk_otp_lock_otp(word_index, option);
	k_mutex_unlock(&data->lock);

	return hpm_otp_status_to_errno(status);
}

static int hpm_otp_lock_shadow_impl(const struct device *dev, uint32_t word_index,
				    enum hpm_otp_lock_option option)
{
	struct hpm_otp_data *data = dev->data;
	hpm_stat_t status;

	if ((word_index >= HPM_OTP_WORD_COUNT) || (option > HPM_OTP_DISABLE_ACCESS)) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	status = hpm_sdk_otp_lock_shadow(word_index, option);
	k_mutex_unlock(&data->lock);

	return hpm_otp_status_to_errno(status);
}
#endif

#if defined(CONFIG_OTP_HPMICRO_ALLOW_REGION_CONFIG)
static int hpm_otp_set_configurable_region_impl(const struct device *dev, uint32_t start,
						uint32_t word_count)
{
	struct hpm_otp_data *data = dev->data;
	hpm_stat_t status;

	if ((word_count == 0U) || (start >= HPM_OTP_WORD_COUNT) ||
	    (word_count > HPM_OTP_WORD_COUNT - start)) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	status = hpm_sdk_otp_set_configurable_region(start, word_count);
	k_mutex_unlock(&data->lock);

	return hpm_otp_status_to_errno(status);
}
#endif

static DEVICE_API(hpm_otp, hpm_otp_api) = {
	.otp_api =
		{
			.read = hpm_otp_read,
#if defined(CONFIG_OTP_PROGRAM)
			.program = hpm_otp_program,
#endif
		},
	.reload = hpm_otp_reload_impl,
#if defined(CONFIG_OTP_HPMICRO_ALLOW_LOCK)
	.lock_otp = hpm_otp_lock_impl,
	.lock_shadow = hpm_otp_lock_shadow_impl,
#endif
#if defined(CONFIG_OTP_HPMICRO_ALLOW_REGION_CONFIG)
	.set_configurable_region = hpm_otp_set_configurable_region_impl,
#endif
};

static int hpm_otp_init(const struct device *dev)
{
	struct hpm_otp_data *data = dev->data;

	hpm_sdk_otp_init();
	k_mutex_init(&data->lock);

	return 0;
}

#define HPM_OTP_INIT(n)                                                                            \
	BUILD_ASSERT(CONFIG_OTP_HPMICRO_SIZE <= HPM_OTP_BYTES,                                     \
		     "OTP_HPMICRO_SIZE exceeds the hardware shadow array");                        \
	BUILD_ASSERT(IS_ALIGNED(CONFIG_OTP_HPMICRO_SIZE, sizeof(uint32_t)),                        \
		     "OTP_HPMICRO_SIZE must be word aligned");                                     \
                                                                                                   \
	static struct hpm_otp_data hpm_otp_data_##n;                                               \
                                                                                                   \
	static const struct hpm_otp_config hpm_otp_config_##n = {                                  \
		.size = CONFIG_OTP_HPMICRO_SIZE,                                                   \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, hpm_otp_init, NULL, &hpm_otp_data_##n, &hpm_otp_config_##n,       \
			      POST_KERNEL, CONFIG_OTP_INIT_PRIORITY, &hpm_otp_api);

DT_INST_FOREACH_STATUS_OKAY(HPM_OTP_INIT)
