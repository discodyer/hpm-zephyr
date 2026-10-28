/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/ztest.h>
#include <drivers/adc/hpmicro_adc16.h>
#include <drivers/dac/hpmicro_dac.h>
#include <drivers/mux/hpmicro_trgm.h>

#define ADC_CHANNEL      13
#define ANALOG_TOLERANCE 160
static const struct device *const adc = DEVICE_DT_GET(DT_NODELABEL(adc0));
static const struct device *const dac = DEVICE_DT_GET(DT_NODELABEL(dac0));
static uint16_t samples[8];
static struct k_poll_signal sample_signal;
static struct adc_sequence sample_sequence;
static struct k_poll_signal external_signal;
static struct adc_sequence external_sequence;
static const struct device *const trgm = DEVICE_DT_GET(DT_NODELABEL(trgm0));
static uint32_t wave0[32] __nocache __aligned(32);
static uint32_t wave1[32] __nocache __aligned(32);

static void analog_before(void *fixture)
{
	const struct adc_channel_cfg adc_config = {
		.gain = ADC_GAIN_1,
		.reference = ADC_REF_INTERNAL,
		.acquisition_time = ADC_ACQ_TIME_DEFAULT,
		.channel_id = ADC_CHANNEL,
	};
	const struct adc_channel_cfg dac_feedback_config = {
		.gain = ADC_GAIN_1,
		.reference = ADC_REF_INTERNAL,
		.acquisition_time = ADC_ACQ_TIME_DEFAULT,
		.channel_id = 11,
	};
	const struct dac_channel_cfg dac_config = {.channel_id = 0, .resolution = 12};

	ARG_UNUSED(fixture);
	zassert_true(device_is_ready(adc));
	zassert_true(device_is_ready(dac));
	zassert_ok(adc_channel_setup(adc, &adc_config));
	zassert_ok(adc_channel_setup(adc, &dac_feedback_config));
	zassert_ok(dac_channel_setup(dac, &dac_config));
}

static void analog_after(void *fixture)
{
	const struct hpm_trgm_output_config disconnected = {0};

	ARG_UNUSED(fixture);
	(void)hpm_trgm_configure_output(trgm, HPM_TRGM_OUTPUT_ADC0_STRGI, &disconnected);
	(void)hpm_adc16_stop_preemption(adc);
	(void)hpm_adc16_set_trigger_source(adc, HPM_ADC16_TRIGGER_SOFTWARE);
	(void)hpm_dac_set_mode(dac, 0, HPM_DAC_MODE_DIRECT);
	(void)dac_write_value(dac, 0, 0);
}

static void check_voltage(uint16_t expected, bool asynchronous)
{
	sample_sequence = (struct adc_sequence){
		.channels = BIT(ADC_CHANNEL),
		.resolution = 12,
		.buffer_size = sizeof(samples[0]),
	};

	k_msleep(5);
	/* The current driver supports one sequence per request, not extra_samplings. */
	for (size_t i = 0; i < ARRAY_SIZE(samples); ++i) {
		sample_sequence.buffer = &samples[i];
		if (asynchronous) {
			struct k_poll_event event;
			unsigned int signaled;
			int result;

			k_poll_signal_init(&sample_signal);
			k_poll_event_init(&event, K_POLL_TYPE_SIGNAL, K_POLL_MODE_NOTIFY_ONLY,
					  &sample_signal);
			zassert_ok(adc_read_async(adc, &sample_sequence, &sample_signal));
			zassert_ok(k_poll(&event, 1, K_SECONDS(2)), "ADC sequence IRQ/DMA timeout");
			k_poll_signal_check(&sample_signal, &signaled, &result);
			zassert_true(signaled);
			zassert_ok(result, "ADC async sequence failed: %d", result);
		} else {
			int result = adc_read(adc, &sample_sequence);

			zassert_ok(result, "ADC sequence failed: %d", result);
		}
		zassert_within(samples[i], expected, ANALOG_TOLERANCE,
			       "DAC->ADC sample %u: got %u, expected %u; check PB08 -> PB05",
			       (unsigned int)i, samples[i], expected);
	}
}

ZTEST(hpm_analog_loop, test_dac_direct_adc_sync_async_sequence)
{
	const uint16_t values[] = {512, 1024, 2048, 3072, 3584};

	for (size_t i = 0; i < ARRAY_SIZE(values); ++i) {
		zassert_ok(dac_write_value(dac, 0, values[i]));
		check_voltage(values[i], i & 1);
	}
}

ZTEST(hpm_analog_loop, test_z_sequence_after_preemption_multichannel)
{
	struct adc_sequence sequence = {
		.channels = BIT(11) | BIT(ADC_CHANNEL),
		.resolution = 12,
		.buffer = samples,
		.buffer_size = 2U * sizeof(samples[0]),
	};
	const uint16_t expected = 2048;

	zassert_ok(dac_write_value(dac, 0, expected));
	zassert_ok(adc_read(adc, &sequence));
	zassert_within(samples[0], expected, ANALOG_TOLERANCE,
		       "ADC IN11 feedback sample %u, expected %u", samples[0], expected);
	zassert_within(samples[1], expected, ANALOG_TOLERANCE,
		       "ADC IN13 feedback sample %u, expected %u", samples[1], expected);
}

ZTEST(hpm_analog_loop, test_dac_step_endpoint)
{
	const struct hpm_dac_step_config config = {
		.direction = HPM_DAC_STEP_UP,
		.round_mode = HPM_DAC_STEP_ONESHOT,
		.start = 1024,
		.end = 2048,
		.step = 1,
	};
	uint32_t status;
	int64_t deadline;

	zassert_ok(hpm_dac_set_mode(dac, 0, HPM_DAC_MODE_STEP));
	zassert_ok(hpm_dac_configure_step(dac, 0, &config));
	zassert_ok(hpm_dac_clear_status(dac, BIT(4)));
	zassert_ok(hpm_dac_trigger_step(dac, 0));
	deadline = k_uptime_get() + 100;
	do {
		zassert_ok(hpm_dac_get_status(dac, &status));
		if ((status & BIT(4)) != 0U) {
			break;
		}
		k_msleep(1);
	} while (k_uptime_get() < deadline);
	zassert_true((status & BIT(4)) != 0U, "DAC step did not complete");
	zassert_ok(hpm_dac_clear_status(dac, BIT(4)));
	check_voltage(config.end, false);
}

ZTEST(hpm_analog_loop, test_dac_buffer_engine)
{
	struct hpm_dac_buffer_config config = {
		.data_mode = HPM_DAC_BUFFER_ONE_POINT,
		.burst = HPM_DAC_BUFFER_BURST_SINGLE,
		.buffer0 = wave0,
		.buffer0_len = ARRAY_SIZE(wave0),
		.buffer1 = wave1,
		.buffer1_len = ARRAY_SIZE(wave1),
	};

	for (size_t i = 0; i < ARRAY_SIZE(wave0); ++i) {
		wave0[i] = 1536;
		wave1[i] = 1536;
	}
	zassert_ok(hpm_dac_set_mode(dac, 0, HPM_DAC_MODE_BUFFER));
	zassert_ok(hpm_dac_configure_buffer(dac, &config));
	zassert_ok(hpm_dac_trigger_buffer(dac));
	check_voltage(1536, true);
}

ZTEST(hpm_analog_loop, test_adc_preemption_dma_result)
{
	const struct hpm_adc16_preemption_config config = {
		.trigger = 0,
		.channel_count = 1,
		.channels = {ADC_CHANNEL},
		.sample_cycle = 20,
	};
	struct hpm_adc16_preemption_result result;
	int ret;
	int64_t deadline;

	zassert_ok(dac_write_value(dac, 0, 2048));
	k_msleep(5);
	zassert_ok(hpm_adc16_configure_preemption(adc, &config));
	zassert_ok(hpm_adc16_trigger_preemption(adc));
	deadline = k_uptime_get() + 1000;
	do {
		ret = hpm_adc16_read_preemption(adc, &result, 1);
		if (ret != -EAGAIN) {
			break;
		}
		k_msleep(1);
	} while (k_uptime_get() < deadline);
	zassert_ok(ret, "ADC preemption failed: %d", ret);
	/* Preemption uses the ADC's native 16-bit conversion scale. */
	zassert_within(result.value, 32768, ANALOG_TOLERANCE * 16);
}

ZTEST(hpm_analog_loop, test_adc_hardware_trigger_via_trgm)
{
	struct hpm_trgm_output_config route = {.source = 0};
	struct k_poll_event event;
	unsigned int signaled;
	int result;

	zassert_ok(dac_write_value(dac, 0, 2048));
	k_msleep(5);
	zassert_ok(hpm_trgm_configure_output(trgm, HPM_TRGM_OUTPUT_ADC0_STRGI, &route));
	zassert_ok(hpm_adc16_set_trigger_source(adc, HPM_ADC16_TRIGGER_EXTERNAL));
	external_sequence = (struct adc_sequence){
		.channels = BIT(ADC_CHANNEL),
		.resolution = 12,
		.buffer = samples,
		.buffer_size = sizeof(samples[0]),
	};
	k_poll_signal_init(&external_signal);
	k_poll_event_init(&event, K_POLL_TYPE_SIGNAL, K_POLL_MODE_NOTIFY_ONLY, &external_signal);
	zassert_ok(adc_read_async(adc, &external_sequence, &external_signal));
	/* An armed ADC must wait for the trigger rather than finish immediately. */
	zassert_equal(k_poll(&event, 1, K_MSEC(5)), -EAGAIN);
	route.source = 1;
	zassert_ok(hpm_trgm_configure_output(trgm, HPM_TRGM_OUTPUT_ADC0_STRGI, &route));
	zassert_ok(k_poll(&event, 1, K_SECONDS(2)), "TRGM rising edge did not trigger ADC");
	k_poll_signal_check(&external_signal, &signaled, &result);
	zassert_true(signaled);
	zassert_ok(result);
	zassert_within(samples[0], 2048, ANALOG_TOLERANCE);
}

ZTEST_SUITE(hpm_analog_loop, NULL, NULL, analog_before, analog_after, NULL);
