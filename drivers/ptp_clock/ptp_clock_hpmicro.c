/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT hpmicro_hpm_ptpc

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/ptp_clock.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <hpm_ptpc_drv.h>

LOG_MODULE_REGISTER(ptp_clock_hpmicro, CONFIG_PTP_CLOCK_HPMICRO_LOG_LEVEL);

struct hpm_ptpc_config {
	PTPC_Type *base;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	uint8_t index;
};

struct hpm_ptpc_data {
	struct k_mutex lock;
	uint32_t nominal_addend;
};

static int hpm_ptpc_status_to_errno(hpm_stat_t status)
{
	switch (status) {
	case status_success:
		return 0;
	case status_invalid_argument:
		return -EINVAL;
	default:
		return -EIO;
	}
}

static void hpm_ptpc_read_time(const struct hpm_ptpc_config *config, uint32_t *seconds,
			       uint32_t *nanoseconds)
{
	uint32_t sec_before;
	uint32_t sec_after;
	uint32_t nsec;

	do {
		sec_before = ptpc_get_timestamp_second(config->base, config->index);
		nsec = ptpc_get_timestamp_ns(config->base, config->index);
		sec_after = ptpc_get_timestamp_second(config->base, config->index);
	} while (sec_before != sec_after);

	*seconds = sec_after;
	*nanoseconds = nsec;
}

static int hpm_ptpc_set(const struct device *dev, struct net_ptp_time *tm)
{
	const struct hpm_ptpc_config *config = dev->config;
	struct hpm_ptpc_data *data = dev->data;
	hpm_stat_t status;

	if ((tm == NULL) || (tm->second > UINT32_MAX) || (tm->nanosecond >= NSEC_PER_SEC)) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	ptpc_disable_timer(config->base, config->index);
	status = ptpc_init_timer_with_initial(config->base, config->index, (uint32_t)tm->second,
					      tm->nanosecond, ptpc_counting_increment);
	if (status == status_success) {
		ptpc_enable_timer(config->base, config->index);
	}
	k_mutex_unlock(&data->lock);

	return hpm_ptpc_status_to_errno(status);
}

static int hpm_ptpc_get(const struct device *dev, struct net_ptp_time *tm)
{
	const struct hpm_ptpc_config *config = dev->config;
	struct hpm_ptpc_data *data = dev->data;
	uint32_t seconds;
	uint32_t nanoseconds;

	if (tm == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	hpm_ptpc_read_time(config, &seconds, &nanoseconds);
	k_mutex_unlock(&data->lock);

	tm->second = seconds;
	tm->nanosecond = nanoseconds;

	return 0;
}

static int hpm_ptpc_adjust(const struct device *dev, int increment)
{
	const struct hpm_ptpc_config *config = dev->config;
	struct hpm_ptpc_data *data = dev->data;
	ptpc_counting_mode mode;
	uint64_t magnitude;
	uint32_t seconds;
	uint32_t nanoseconds;
	hpm_stat_t status;

	if (increment == 0) {
		return 0;
	}

	if (increment < 0) {
		mode = ptpc_counting_decrement;
		magnitude = (uint64_t)(-(int64_t)increment);
	} else {
		mode = ptpc_counting_increment;
		magnitude = (uint64_t)increment;
	}

	seconds = (uint32_t)(magnitude / NSEC_PER_SEC);
	nanoseconds = (uint32_t)(magnitude % NSEC_PER_SEC);

	k_mutex_lock(&data->lock, K_FOREVER);
	status = ptpc_update_timer(config->base, config->index, seconds, nanoseconds, mode);
	k_mutex_unlock(&data->lock);

	return hpm_ptpc_status_to_errno(status);
}

static int hpm_ptpc_rate_adjust(const struct device *dev, double ratio)
{
	const struct hpm_ptpc_config *config = dev->config;
	struct hpm_ptpc_data *data = dev->data;
	double adjusted;
	uint32_t addend;

	if ((ratio <= 0.0) || (ratio > (double)UINT32_MAX / data->nominal_addend)) {
		return -ERANGE;
	}

	adjusted = (double)data->nominal_addend * ratio;
	addend = (uint32_t)(adjusted + 0.5);

	if (addend == 0U) {
		return -ERANGE;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	config->base->PTPC[config->index].ADDEND = addend;
	k_mutex_unlock(&data->lock);

	return 0;
}

static DEVICE_API(ptp_clock, hpm_ptpc_api) = {
	.set = hpm_ptpc_set,
	.get = hpm_ptpc_get,
	.adjust = hpm_ptpc_adjust,
	.rate_adjust = hpm_ptpc_rate_adjust,
};

static int hpm_ptpc_init(const struct device *dev)
{
	const struct hpm_ptpc_config *config = dev->config;
	struct hpm_ptpc_data *data = dev->data;
	ptpc_config_t ptpc_config;
	uint32_t clock_rate;
	uint32_t counter_incr;
	uint64_t denominator;
	uint64_t nominal_addend;
	hpm_stat_t status;
	int ret;

	if (!device_is_ready(config->clock_dev)) {
		return -ENODEV;
	}

	ret = clock_control_on(config->clock_dev, config->clock_subsys);
	if (ret < 0) {
		return ret;
	}

	ret = clock_control_get_rate(config->clock_dev, config->clock_subsys, &clock_rate);
	if ((ret < 0) || (clock_rate == 0U)) {
		return ret < 0 ? ret : -EINVAL;
	}

	ptpc_get_default_config(config->base, &ptpc_config);
	ptpc_config.capture_trigger = ptpc_capture_trigger_none;
	ptpc_config.capture_keep = false;
	ptpc_config.ns_rollover_mode = ptpc_ns_counter_rollover_digital;
	ptpc_config.src_frequency = clock_rate;
	/*
	 * The SDK field selects CTRL0.FINE_COARSE_SEL directly despite its name.
	 * Set it so ADDEND controls the fractional nanosecond accumulation.
	 */
	ptpc_config.coarse_increment = true;

	/*
	 * Use two nominal increments per clock. The fractional accumulator then
	 * runs at half rate, leaving headroom for both faster and slower tuning.
	 */
	counter_incr = 2U * DIV_ROUND_UP(NSEC_PER_SEC, clock_rate);
	if ((counter_incr == 0U) || (counter_incr > UINT8_MAX)) {
		return -ERANGE;
	}

	denominator = (uint64_t)clock_rate * counter_incr;
	nominal_addend = (((uint64_t)NSEC_PER_SEC << 32) + denominator / 2U) / denominator;
	if ((nominal_addend == 0U) || (nominal_addend > UINT32_MAX)) {
		return -ERANGE;
	}

	status = ptpc_init(config->base, config->index, &ptpc_config);
	if (status != status_success) {
		return hpm_ptpc_status_to_errno(status);
	}

	/* ptpc_init() derives SS_INCR for one increment per clock; override it. */
	ptpc_disable_timer(config->base, config->index);
	ptpc_set_ns_counter_step(config->base, config->index, counter_incr);
	config->base->PTPC[config->index].ADDEND = (uint32_t)nominal_addend;
	ptpc_init_timer(config->base, config->index);
	ptpc_enable_timer(config->base, config->index);
	data->nominal_addend = (uint32_t)nominal_addend;
	k_mutex_init(&data->lock);

	return 0;
}

#define HPM_PTPC_INIT(n)                                                                           \
	BUILD_ASSERT(DT_INST_REG_ADDR(n) < 2U, "invalid PTPC counter index");                      \
                                                                                                   \
	static struct hpm_ptpc_data hpm_ptpc_data_##n;                                             \
                                                                                                   \
	static const struct hpm_ptpc_config hpm_ptpc_config_##n = {                                \
		.base = (PTPC_Type *)DT_REG_ADDR(DT_INST_PARENT(n)),                               \
		.clock_dev = DEVICE_DT_GET(DT_CLOCKS_CTLR(DT_INST_PARENT(n))),                     \
		.clock_subsys = (clock_control_subsys_t)DT_CLOCKS_CELL(DT_INST_PARENT(n), name),   \
		.index = DT_INST_REG_ADDR(n),                                                      \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, hpm_ptpc_init, NULL, &hpm_ptpc_data_##n, &hpm_ptpc_config_##n,    \
			      POST_KERNEL, CONFIG_PTP_CLOCK_INIT_PRIORITY, &hpm_ptpc_api);

DT_INST_FOREACH_STATUS_OKAY(HPM_PTPC_INIT)
