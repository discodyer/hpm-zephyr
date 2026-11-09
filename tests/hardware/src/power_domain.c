/*
 * Copyright (c) 2026 The HPM Zephyr Module Contributors
 * Copyright (c) 2026 Cody Gu <gujiaqi@iscas.ac.cn>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/crc.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/ztest.h>

#include <dt-bindings/power/hpmicro-hpm5300-power.h>
#include <hpm_sysctl_drv.h>

#define DT_DRV_COMPAT hpmicro_power_domain_test_consumer

#define HPM_PD_TEST_WAIT_STEP_US 10U

#define HPM_PD_TEST_DOMAIN_NODE     DT_NODELABEL(power_domain_test)
#define HPM_PD_TEST_CONSUMER_A_NODE DT_NODELABEL(power_domain_test_consumer_a)
#define HPM_PD_TEST_CONSUMER_B_NODE DT_NODELABEL(power_domain_test_consumer_b)

#define HPM_PD_TEST_DOMAIN     DEVICE_DT_GET(HPM_PD_TEST_DOMAIN_NODE)
#define HPM_PD_TEST_CONSUMER_A DEVICE_DT_GET(HPM_PD_TEST_CONSUMER_A_NODE)
#define HPM_PD_TEST_CONSUMER_B DEVICE_DT_GET(HPM_PD_TEST_CONSUMER_B_NODE)

#define HPM_PD_TEST_SYSCTL                                                                         \
	((SYSCTL_Type *)DT_REG_ADDR(DT_PHANDLE(HPM_PD_TEST_DOMAIN_NODE, hpmicro_controller)))
#define HPM_PD_TEST_RESOURCE                                                                       \
	((sysctl_resource_t)DT_PROP(HPM_PD_TEST_DOMAIN_NODE, hpmicro_resource_id))

struct hpm_pd_test_consumer_data {
	uint32_t turn_on_count;
	uint32_t turn_off_count;
	uint32_t resume_count;
	uint32_t suspend_count;
};

static int hpm_pd_test_consumer_pm_action(const struct device *dev, enum pm_device_action action)
{
	struct hpm_pd_test_consumer_data *data = dev->data;

	switch (action) {
	case PM_DEVICE_ACTION_TURN_ON:
		zassert_equal(
			sysctl_resource_target_get_mode(HPM_PD_TEST_SYSCTL, HPM_PD_TEST_RESOURCE),
			sysctl_resource_mode_force_on,
			"TURN_ON notification arrived before domain power-on");
		data->turn_on_count++;
		return 0;
	case PM_DEVICE_ACTION_TURN_OFF:
		zassert_equal(
			sysctl_resource_target_get_mode(HPM_PD_TEST_SYSCTL, HPM_PD_TEST_RESOURCE),
			sysctl_resource_mode_force_on,
			"TURN_OFF notification arrived after domain power-off");
		data->turn_off_count++;
		return 0;
	case PM_DEVICE_ACTION_RESUME:
		data->resume_count++;
		return 0;
	case PM_DEVICE_ACTION_SUSPEND:
		data->suspend_count++;
		return 0;
	default:
		return -ENOTSUP;
	}
}

static int hpm_pd_test_consumer_init(const struct device *dev)
{
	return pm_device_driver_init(dev, hpm_pd_test_consumer_pm_action);
}

#define HPM_PD_TEST_CONSUMER_INIT(n)                                                               \
	static struct hpm_pd_test_consumer_data hpm_pd_test_consumer_data_##n;                     \
                                                                                                   \
	PM_DEVICE_DT_INST_DEFINE(n, hpm_pd_test_consumer_pm_action);                               \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, hpm_pd_test_consumer_init, PM_DEVICE_DT_INST_GET(n),              \
			      &hpm_pd_test_consumer_data_##n, NULL, POST_KERNEL,                   \
			      CONFIG_POWER_DOMAIN_HPMICRO_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(HPM_PD_TEST_CONSUMER_INIT)

static int hpm_pd_test_wait_ready(void)
{
	uint32_t waited_us = 0U;

	while (sysctl_resource_target_is_busy(HPM_PD_TEST_SYSCTL, HPM_PD_TEST_RESOURCE)) {
		if (waited_us >= CONFIG_POWER_DOMAIN_HPMICRO_TIMEOUT_US) {
			return -ETIMEDOUT;
		}

		k_busy_wait(HPM_PD_TEST_WAIT_STEP_US);
		waited_us += HPM_PD_TEST_WAIT_STEP_US;
	}

	return 0;
}

static void hpm_pd_test_set_mode(sysctl_resource_mode_t mode)
{
	sysctl_resource_target_set_mode(HPM_PD_TEST_SYSCTL, HPM_PD_TEST_RESOURCE, mode);
	zassert_ok(hpm_pd_test_wait_ready(), "SYSCTL resource transition timed out");
	zassert_equal(sysctl_resource_target_get_mode(HPM_PD_TEST_SYSCTL, HPM_PD_TEST_RESOURCE),
		      mode);
}

static struct hpm_pd_test_consumer_data *hpm_pd_test_data(const struct device *dev)
{
	return dev->data;
}

static void hpm_pd_test_crc_known_answer(void)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(crc0));
	const uint8_t input[] = "123456789";
	struct crc_ctx ctx = {
		.type = CRC32_IEEE,
		.seed = 0xffffffff,
		.polynomial = 0x04c11db7,
		.reversed = CRC_FLAG_REVERSE_INPUT | CRC_FLAG_REVERSE_OUTPUT,
	};

	zassert_true(device_is_ready(dev));
	zassert_ok(crc_begin(dev, &ctx));
	zassert_ok(crc_update(dev, &ctx, input, 4));
	zassert_ok(crc_update(dev, &ctx, input + 4, 5));
	zassert_ok(crc_finish(dev, &ctx));
	zassert_equal(ctx.result, 0xcbf43926, "CRC32/ISO-HDLC vector mismatch");
}

ZTEST(hpm_power_domain, test_runtime_reference_count_and_notifications)
{
	struct hpm_pd_test_consumer_data *data_a = hpm_pd_test_data(HPM_PD_TEST_CONSUMER_A);
	struct hpm_pd_test_consumer_data *data_b = hpm_pd_test_data(HPM_PD_TEST_CONSUMER_B);
	enum pm_device_state state;

	zassert_true(device_is_ready(HPM_PD_TEST_DOMAIN));
	zassert_true(device_is_ready(HPM_PD_TEST_CONSUMER_A));
	zassert_true(device_is_ready(HPM_PD_TEST_CONSUMER_B));
	zassert_true(pm_device_runtime_is_enabled(HPM_PD_TEST_DOMAIN));
	zassert_true(pm_device_runtime_is_enabled(HPM_PD_TEST_CONSUMER_A));
	zassert_true(pm_device_runtime_is_enabled(HPM_PD_TEST_CONSUMER_B));

	/* Remove any override left by a previous run before checking startup state. */
	hpm_pd_test_set_mode(sysctl_resource_mode_auto);
	zassert_ok(pm_device_state_get(HPM_PD_TEST_DOMAIN, &state));
	zassert_equal(state, PM_DEVICE_STATE_SUSPENDED);
	zassert_ok(pm_device_state_get(HPM_PD_TEST_CONSUMER_A, &state));
	zassert_equal(state, PM_DEVICE_STATE_OFF);
	zassert_ok(pm_device_state_get(HPM_PD_TEST_CONSUMER_B, &state));
	zassert_equal(state, PM_DEVICE_STATE_OFF);
	zassert_false(pm_device_is_powered(HPM_PD_TEST_CONSUMER_A));
	zassert_false(pm_device_is_powered(HPM_PD_TEST_CONSUMER_B));

	zassert_ok(pm_device_runtime_get(HPM_PD_TEST_CONSUMER_A));
	zassert_equal(pm_device_runtime_usage(HPM_PD_TEST_DOMAIN), 1U);
	zassert_equal(pm_device_runtime_usage(HPM_PD_TEST_CONSUMER_A), 1U);
	zassert_equal(sysctl_resource_target_get_mode(HPM_PD_TEST_SYSCTL, HPM_PD_TEST_RESOURCE),
		      sysctl_resource_mode_force_on);
	zassert_ok(pm_device_state_get(HPM_PD_TEST_DOMAIN, &state));
	zassert_equal(state, PM_DEVICE_STATE_ACTIVE);
	zassert_ok(pm_device_state_get(HPM_PD_TEST_CONSUMER_A, &state));
	zassert_equal(state, PM_DEVICE_STATE_ACTIVE);
	zassert_ok(pm_device_state_get(HPM_PD_TEST_CONSUMER_B, &state));
	zassert_equal(state, PM_DEVICE_STATE_SUSPENDED);
	zassert_true(pm_device_is_powered(HPM_PD_TEST_CONSUMER_A));
	zassert_true(pm_device_is_powered(HPM_PD_TEST_CONSUMER_B));
	hpm_pd_test_crc_known_answer();

	zassert_ok(pm_device_runtime_get(HPM_PD_TEST_CONSUMER_B));
	zassert_equal(pm_device_runtime_usage(HPM_PD_TEST_DOMAIN), 2U);
	zassert_equal(pm_device_runtime_usage(HPM_PD_TEST_CONSUMER_B), 1U);
	zassert_ok(pm_device_state_get(HPM_PD_TEST_CONSUMER_B, &state));
	zassert_equal(state, PM_DEVICE_STATE_ACTIVE);

	zassert_ok(pm_device_runtime_put(HPM_PD_TEST_CONSUMER_A));
	zassert_equal(pm_device_runtime_usage(HPM_PD_TEST_DOMAIN), 1U);
	zassert_equal(pm_device_runtime_usage(HPM_PD_TEST_CONSUMER_A), 0U);
	zassert_ok(pm_device_state_get(HPM_PD_TEST_DOMAIN, &state));
	zassert_equal(state, PM_DEVICE_STATE_ACTIVE);
	zassert_ok(pm_device_state_get(HPM_PD_TEST_CONSUMER_A, &state));
	zassert_equal(state, PM_DEVICE_STATE_SUSPENDED);
	zassert_ok(pm_device_state_get(HPM_PD_TEST_CONSUMER_B, &state));
	zassert_equal(state, PM_DEVICE_STATE_ACTIVE);
	zassert_equal(sysctl_resource_target_get_mode(HPM_PD_TEST_SYSCTL, HPM_PD_TEST_RESOURCE),
		      sysctl_resource_mode_force_on);

	zassert_ok(pm_device_runtime_put(HPM_PD_TEST_CONSUMER_B));
	zassert_equal(pm_device_runtime_usage(HPM_PD_TEST_DOMAIN), 0U);
	zassert_equal(pm_device_runtime_usage(HPM_PD_TEST_CONSUMER_B), 0U);
	zassert_equal(sysctl_resource_target_get_mode(HPM_PD_TEST_SYSCTL, HPM_PD_TEST_RESOURCE),
		      sysctl_resource_mode_force_off);
	zassert_ok(pm_device_state_get(HPM_PD_TEST_DOMAIN, &state));
	zassert_equal(state, PM_DEVICE_STATE_SUSPENDED);
	zassert_ok(pm_device_state_get(HPM_PD_TEST_CONSUMER_A, &state));
	zassert_equal(state, PM_DEVICE_STATE_OFF);
	zassert_ok(pm_device_state_get(HPM_PD_TEST_CONSUMER_B, &state));
	zassert_equal(state, PM_DEVICE_STATE_OFF);
	zassert_false(pm_device_is_powered(HPM_PD_TEST_CONSUMER_A));
	zassert_false(pm_device_is_powered(HPM_PD_TEST_CONSUMER_B));

	zassert_equal(data_a->turn_on_count, 1U);
	zassert_equal(data_a->turn_off_count, 1U);
	zassert_equal(data_a->resume_count, 1U);
	zassert_equal(data_a->suspend_count, 1U);
	zassert_equal(data_b->turn_on_count, 1U);
	zassert_equal(data_b->turn_off_count, 1U);
	zassert_equal(data_b->resume_count, 1U);
	zassert_equal(data_b->suspend_count, 1U);

	/* A second cycle proves that a force-off resource can be recovered. */
	zassert_ok(pm_device_runtime_get(HPM_PD_TEST_CONSUMER_A));
	zassert_equal(sysctl_resource_target_get_mode(HPM_PD_TEST_SYSCTL, HPM_PD_TEST_RESOURCE),
		      sysctl_resource_mode_force_on);
	hpm_pd_test_crc_known_answer();
	zassert_ok(pm_device_runtime_put(HPM_PD_TEST_CONSUMER_A));
	zassert_equal(sysctl_resource_target_get_mode(HPM_PD_TEST_SYSCTL, HPM_PD_TEST_RESOURCE),
		      sysctl_resource_mode_force_off);
	zassert_equal(data_a->turn_on_count, 2U);
	zassert_equal(data_a->turn_off_count, 2U);
	zassert_equal(data_a->resume_count, 2U);
	zassert_equal(data_a->suspend_count, 2U);
	zassert_equal(data_b->turn_on_count, 2U);
	zassert_equal(data_b->turn_off_count, 2U);

	/* Leave the resource under normal automatic clock/resource management. */
	hpm_pd_test_set_mode(sysctl_resource_mode_auto);
}

ZTEST_SUITE(hpm_power_domain, NULL, NULL, NULL, NULL, NULL);
