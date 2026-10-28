/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/ztest.h>
#include <drivers/gpiom/hpmicro_gpiom.h>
#include <drivers/motor/hpmicro_mmc.h>
#include <drivers/motor/hpmicro_plb.h>
#include <drivers/motor/hpmicro_qeo.h>
#include <drivers/motor/hpmicro_synt.h>
#include <drivers/mux/hpmicro_trgm.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/sys/printk.h>

#include <hpm_soc.h>
#include <hpm_trgm_drv.h>

static const struct device *const synt = DEVICE_DT_GET(DT_NODELABEL(synt));
static const struct device *const plb = DEVICE_DT_GET(DT_NODELABEL(plb));
static const struct device *const qeo = DEVICE_DT_GET(DT_NODELABEL(qeo0));
static const struct device *const mmc = DEVICE_DT_GET(DT_NODELABEL(mmc0));
static const struct device *const trgm = DEVICE_DT_GET(DT_NODELABEL(trgm0));

static void motor_after(void *fixture)
{
	ARG_UNUSED(fixture);
	(void)hpm_mmc_enable_predictor(mmc, 0, false);
	(void)hpm_mmc_enable(mmc, false);
	(void)hpm_mmc_reset(mmc);
	(void)hpm_synt_counter_enable(synt, false);
	(void)hpm_synt_timestamp_enable(synt, false);
	(void)hpm_plb_set_type_b_all_slices(plb, 0, HPM_PLB_SLICE_KEEP);
	(void)hpm_qeo_set_position_source(qeo, HPM_QEO_POSITION_HARDWARE);
	const struct hpm_trgm_output_config route_off = {.source = HPM_TRGM0_INPUT_SRC_VSS};

	for (uint8_t i = 0; i < 4; ++i) {
		(void)hpm_trgm_configure_output(trgm, HPM_TRGM0_OUTPUT_SRC_PLB_IN_16 + i,
						&route_off);
	}
}

ZTEST(hpm_motor_internal, test_synt_count_and_freeze)
{
	uint32_t first;
	uint32_t second;
	uint32_t running_delta;
	uint32_t frozen;

	zassert_true(device_is_ready(synt));
	zassert_ok(hpm_synt_set_reload(synt, UINT32_MAX));
	zassert_ok(hpm_synt_counter_reset(synt));
	zassert_ok(hpm_synt_counter_enable(synt, true));
	zassert_ok(hpm_synt_get_counter(synt, &first));
	k_msleep(2);
	zassert_ok(hpm_synt_get_counter(synt, &second));
	zassert_true(second > first, "SYNT counter is not advancing");
	running_delta = second - first;
	zassert_ok(hpm_synt_counter_enable(synt, false));
	zassert_ok(hpm_synt_get_counter(synt, &first));
	k_msleep(2);
	zassert_ok(hpm_synt_get_counter(synt, &second));
	zassert_equal(second, first, "Disabled SYNT counter still advances");
	frozen = second;
	zassert_ok(hpm_synt_counter_enable(synt, true));
	k_msleep(2);
	zassert_ok(hpm_synt_get_counter(synt, &second));
	printk("SYNT count: initial delta %u, frozen %u, resumed delta %u\n", running_delta, frozen,
	       second - frozen);
	zassert_true(second > frozen, "Re-enabled SYNT counter did not resume");
}

static void plb_assert_comparator(bool expected, const char *phase)
{
	int observed;

	k_busy_wait(10);
	/* TRGM input monitoring exposes the live PLB comparator, unlike SW_INJECT. */
	observed = trgm_input_get_signal_status(HPM_TRGM0, HPM_TRGM0_INPUT_SRC_PLB_OUT16);
	printk("PLB slice 15 %s: CMP0 equality %d (expected %u)\n", phase, observed, expected);
	zassert_equal(observed, expected, "PLB %s comparator mismatch", phase);
}

ZTEST(hpm_motor_internal, test_plb_hardware_load_and_hold)
{
	const struct hpm_trgm_output_config route_high = {.source = HPM_TRGM0_INPUT_SRC_VDD};
	uint32_t value = 0xdeadbeef;

	zassert_true(device_is_ready(plb));
	zassert_true(device_is_ready(trgm));
	/* TYPE_B0 selects slice 15 when all four internal inputs PLB_IN16..19 are high. */
	for (uint8_t i = 0; i < 4; ++i) {
		zassert_ok(hpm_trgm_configure_output(trgm, HPM_TRGM0_OUTPUT_SRC_PLB_IN_16 + i,
						     &route_high));
	}
	zassert_ok(hpm_plb_set_type_b_all_slices(plb, 0, HPM_PLB_SLICE_KEEP));
	zassert_ok(hpm_plb_set_type_b_comparator(plb, 0, 0, 0x5361abcd, HPM_PLB_COMPARATOR_EQUAL));
	zassert_ok(hpm_plb_set_type_b_counter(plb, 0, 0));
	plb_assert_comparator(false, "injected zero");
	zassert_equal(hpm_plb_get_type_b_counter(plb, 0, &value), -ENOTSUP);
	zassert_equal(value, 0xdeadbeef, "Unsupported PLB counter read changed output");
	zassert_ok(hpm_plb_set_type_b_all_slices(plb, 0, HPM_PLB_SLICE_LOAD_CMP0));
	plb_assert_comparator(true, "initial CMP0 load");
	zassert_ok(hpm_plb_set_type_b_all_slices(plb, 0, HPM_PLB_SLICE_KEEP));
	zassert_ok(hpm_plb_set_type_b_comparator(plb, 0, 0, 0x1234dcba, HPM_PLB_COMPARATOR_EQUAL));
	plb_assert_comparator(false, "KEEP after CMP0 change");
	zassert_ok(hpm_plb_set_type_b_comparator(plb, 0, 0, 0x5361abcd, HPM_PLB_COMPARATOR_EQUAL));
	plb_assert_comparator(true, "KEEP retained original value");
	zassert_ok(hpm_plb_set_type_b_comparator(plb, 0, 0, 0x1234dcba, HPM_PLB_COMPARATOR_EQUAL));
	zassert_ok(hpm_plb_set_type_b_all_slices(plb, 0, HPM_PLB_SLICE_LOAD_CMP0));
	plb_assert_comparator(true, "resumed CMP0 load");
}

ZTEST(hpm_motor_internal, test_qeo_cosine_quadrants)
{
	static const struct {
		const char *name;
		bool enable_amplitude;
		double amplitude;
		double mid_point_shift;
		uint16_t expected[4];
	} cases[] = {
		{
			.name = "default",
			.expected = {UINT16_MAX, 32768, 0, 32768},
		},
		{
			.name = "half amplitude",
			.enable_amplitude = true,
			.amplitude = 0.5,
			.expected = {49152, 32768, 16384, 32768},
		},
		{
			.name = "half amplitude, midpoint +0.25",
			.enable_amplitude = true,
			.amplitude = 0.5,
			.mid_point_shift = 0.25,
			.expected = {57344, 40960, 24576, 40960},
		},
		{
			.name = "half amplitude, midpoint -0.25",
			.enable_amplitude = true,
			.amplitude = 0.5,
			.mid_point_shift = -0.25,
			.expected = {40960, 24576, 8192, 24576},
		},
	};
	struct hpm_qeo_wave_config config = {.lines = 1, .type = HPM_QEO_WAVE_COSINE};

	zassert_true(device_is_ready(qeo));
	zassert_ok(hpm_qeo_set_position_source(qeo, HPM_QEO_POSITION_SOFTWARE));
	for (size_t i = 0; i < ARRAY_SIZE(cases); ++i) {
		for (uint8_t channel = 0; channel < HPM_QEO_WAVE_CHANNEL_COUNT; ++channel) {
			config.channel[channel].enable_amplitude = cases[i].enable_amplitude;
			config.channel[channel].amplitude = cases[i].amplitude;
			config.channel[channel].mid_point_shift = cases[i].mid_point_shift;
			config.channel[channel].limits.max_limit0 = UINT16_MAX;
			config.channel[channel].limits.max_limit1 = UINT16_MAX;
		}
		zassert_ok(hpm_qeo_configure_wave(qeo, &config));
		/* Debug output is (amplitude * cos(theta) + midpoint + 1) * 32768. */
		for (unsigned int quadrant = 0; quadrant < 4; ++quadrant) {
			uint16_t values[HPM_QEO_WAVE_CHANNEL_COUNT];
			bool finished = false;
			int64_t deadline = k_uptime_get() + 100;

			zassert_ok(hpm_qeo_inject_position(qeo, (uint32_t)quadrant << 30));
			do {
				zassert_ok(hpm_qeo_calculation_finished(qeo, &finished));
				if (finished) {
					break;
				}
				k_busy_wait(10);
			} while (k_uptime_get() < deadline);
			zassert_true(finished, "QEO calculation timeout");
			for (uint8_t channel = 0; channel < HPM_QEO_WAVE_CHANNEL_COUNT; ++channel) {
				zassert_ok(hpm_qeo_get_wave_output(qeo, channel, &values[channel]));
			}
			printk("QEO %s %u degrees: channels %u/%u/%u (expected %u +/- 256)\n",
			       cases[i].name, quadrant * 90, values[0], values[1], values[2],
			       cases[i].expected[quadrant]);
			for (uint8_t channel = 0; channel < HPM_QEO_WAVE_CHANNEL_COUNT; ++channel) {
				zassert_within(values[channel], cases[i].expected[quadrant], 256,
					       "QEO %s %u degrees channel %u: %u", cases[i].name,
					       quadrant * 90, channel, values[channel]);
			}
		}
	}
}

ZTEST(hpm_motor_internal, test_mmc_open_loop_predictor_moves)
{
	const struct hpm_mmc_predictor_mode mode = {
		.open_loop = true,
		.time_mode = HPM_MMC_PREDICTOR_CONTINUOUS,
	};
	const struct hpm_mmc_position_config position = {
		.speed = 1.0,
		.update_mask = HPM_MMC_POSITION_UPDATE_SPEED,
		.trigger = HPM_MMC_POSITION_TRIGGER_TIMESTAMP,
	};
	struct hpm_mmc_position_result first;
	struct hpm_mmc_position_result second;
	const struct device *clock = DEVICE_DT_GET(DT_CLOCKS_CTLR(DT_NODELABEL(mmc0)));
	clock_control_subsys_t clock_subsys =
		(clock_control_subsys_t)DT_CLOCKS_CELL(DT_NODELABEL(mmc0), name);
	uint32_t clock_hz;
	uint32_t first_timestamp;
	uint32_t second_timestamp;
	uint32_t delta;
	uint32_t timestamp_delta;
	uint32_t expected_delta;
	uint32_t tolerance;

	zassert_true(device_is_ready(mmc));
	zassert_true(device_is_ready(synt));
	zassert_ok(clock_control_get_rate(clock, clock_subsys, &clock_hz));
	zassert_not_equal(clock_hz, 0);
	zassert_ok(hpm_synt_timestamp_enable(synt, false));
	zassert_ok(hpm_synt_timestamp_reset(synt));
	zassert_ok(hpm_mmc_reset(mmc));
	zassert_ok(hpm_mmc_configure_predictor_mode(mmc, 0, &mode));
	zassert_ok(hpm_mmc_configure_predictor_position(mmc, 0, &position));
	zassert_ok(hpm_mmc_enable_predictor(mmc, 0, true));
	zassert_ok(hpm_mmc_enable(mmc, true));
	/* Start the timestamp only after the timestamp-zero update is armed. */
	zassert_ok(hpm_synt_timestamp_enable(synt, true));
	k_msleep(2);
	zassert_ok(hpm_synt_get_timestamp(synt, &first_timestamp));
	zassert_ok(hpm_mmc_get_predictor_result(mmc, 0, &first));
	k_msleep(5);
	zassert_ok(hpm_synt_get_timestamp(synt, &second_timestamp));
	zassert_ok(hpm_mmc_get_predictor_result(mmc, 0, &second));
	delta = second.position - first.position;
	timestamp_delta = second_timestamp - first_timestamp;
	/* Position uses one unsigned 32-bit turn; the configured speed is one turn/s. */
	expected_delta = ((uint64_t)timestamp_delta << 32) / clock_hz;
	tolerance = MAX(expected_delta / 50U, 10000U);
	printk("MMC 1 turn/s: position delta %u, SYNT delta %u ticks at %u Hz, expected %u "
	       "+/- %u; result time %u -> %u, speed Q19 %d -> %d\n",
	       delta, timestamp_delta, clock_hz, expected_delta, tolerance, first.time, second.time,
	       (int32_t)(first.speed * (1U << 19)), (int32_t)(second.speed * (1U << 19)));
	zassert_equal(first.speed, position.speed);
	zassert_equal(second.speed, position.speed);
	zassert_within(delta, expected_delta, tolerance, "MMC position does not match speed/time");
	zassert_true(delta > 0 && delta < 0x10000000, "MMC predictor stopped or wrong direction");

	/* MOD_EN pauses continuous prediction; BR_EN alone does not stop this mode. */
	zassert_ok(hpm_mmc_enable(mmc, false));
	k_msleep(1);
	zassert_ok(hpm_mmc_get_predictor_result(mmc, 0, &first));
	k_msleep(2);
	zassert_ok(hpm_mmc_get_predictor_result(mmc, 0, &second));
	zassert_equal(second.position, first.position, "Disabled MMC module still advances");
	zassert_equal(second.revolution, first.revolution);
	zassert_ok(hpm_mmc_enable(mmc, true));
	k_msleep(2);
	zassert_ok(hpm_mmc_get_predictor_result(mmc, 0, &second));
	delta = second.position - first.position;
	printk("MMC module disabled position 0x%08x, module resumed delta %u\n", first.position,
	       delta);
	zassert_true(delta > 0 && delta < 0x10000000, "MMC module did not resume prediction");
}

ZTEST(hpm_motor_internal, test_gpiom_visibility_restore)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(gpiom0));
	bool saved;
	bool observed = false;
	bool restored_value = false;
	int changed;
	int read;
	int restored;
	int restore_read;

	zassert_true(device_is_ready(dev));
	zassert_ok(hpm_gpiom_get_visibility(dev, HPM_GPIOM_PORT_GPIOA, 24,
					    HPM_GPIOM_CONTROLLER_FGPIO0, &saved));
	changed = hpm_gpiom_set_visibility(dev, HPM_GPIOM_PORT_GPIOA, 24,
					   HPM_GPIOM_CONTROLLER_FGPIO0, !saved);
	read = hpm_gpiom_get_visibility(dev, HPM_GPIOM_PORT_GPIOA, 24, HPM_GPIOM_CONTROLLER_FGPIO0,
					&observed);
	restored = hpm_gpiom_set_visibility(dev, HPM_GPIOM_PORT_GPIOA, 24,
					    HPM_GPIOM_CONTROLLER_FGPIO0, saved);
	restore_read = hpm_gpiom_get_visibility(dev, HPM_GPIOM_PORT_GPIOA, 24,
						HPM_GPIOM_CONTROLLER_FGPIO0, &restored_value);
	zassert_ok(changed);
	zassert_ok(read);
	zassert_ok(restored);
	zassert_ok(restore_read);
	zassert_equal(observed, !saved);
	zassert_equal(restored_value, saved);
	printk("GPIOM PA24 FGPIO0 visibility: %u -> %u -> %u\n", saved, observed, restored_value);
}

ZTEST_SUITE(hpm_motor_internal, NULL, NULL, NULL, motor_after, NULL);
