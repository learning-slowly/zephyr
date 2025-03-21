/*
 * Copyright (c) 2018 Kokoon Technology Limited
 * Copyright (c) 2019 Song Qiang <songqiang1304521@gmail.com>
 * Copyright (c) 2019 Endre Karlson
 * Copyright (c) 2020 Teslabs Engineering S.L.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT st_stm32_adc

#include <errno.h>

#include <drivers/adc.h>
#include <device.h>
#include <kernel.h>
#include <init.h>
#include <soc.h>
#include <stm32_ll_adc.h>

#define ADC_CONTEXT_USES_KERNEL_TIMER
#include "adc_context.h"

#define LOG_LEVEL CONFIG_ADC_LOG_LEVEL
#include <logging/log.h>
LOG_MODULE_REGISTER(adc_stm32);

#include <drivers/clock_control/stm32_clock_control.h>
#include <pinmux/pinmux_stm32.h>

#if defined(CONFIG_SOC_SERIES_STM32F3X)
#if defined(ADC1_V2_5)
#define STM32F3X_ADC_V2_5
#elif defined(ADC5_V1_1)
#define STM32F3X_ADC_V1_1
#endif
#endif

#if !defined(CONFIG_SOC_SERIES_STM32F0X) && \
	!defined(CONFIG_SOC_SERIES_STM32G0X) && \
	!defined(CONFIG_SOC_SERIES_STM32L0X) && \
	!defined(CONFIG_SOC_SERIES_STM32WLX)
#define RANK(n)		LL_ADC_REG_RANK_##n
static const uint32_t table_rank[] = {
	RANK(1),
	RANK(2),
	RANK(3),
	RANK(4),
	RANK(5),
	RANK(6),
	RANK(7),
	RANK(8),
	RANK(9),
	RANK(10),
	RANK(11),
	RANK(12),
	RANK(13),
	RANK(14),
	RANK(15),
	RANK(16),
};

#define SEQ_LEN(n)	LL_ADC_REG_SEQ_SCAN_ENABLE_##n##RANKS
static const uint32_t table_seq_len[] = {
	LL_ADC_REG_SEQ_SCAN_DISABLE,
	SEQ_LEN(2),
	SEQ_LEN(3),
	SEQ_LEN(4),
	SEQ_LEN(5),
	SEQ_LEN(6),
	SEQ_LEN(7),
	SEQ_LEN(8),
	SEQ_LEN(9),
	SEQ_LEN(10),
	SEQ_LEN(11),
	SEQ_LEN(12),
	SEQ_LEN(13),
	SEQ_LEN(14),
	SEQ_LEN(15),
	SEQ_LEN(16),
};
#endif

#define RES(n)		LL_ADC_RESOLUTION_##n##B
static const uint32_t table_resolution[] = {
#if defined(CONFIG_SOC_SERIES_STM32F1X) || \
	defined(STM32F3X_ADC_V2_5)
	RES(12),
#elif !defined(CONFIG_SOC_SERIES_STM32H7X)
	RES(6),
	RES(8),
	RES(10),
	RES(12),
#else
	RES(8),
	RES(10),
	RES(12),
	RES(14),
	RES(16),
#endif
};

#define SMP_TIME(x, y)		LL_ADC_SAMPLINGTIME_##x##CYCLE##y

/*
 * Conversion time in ADC cycles. Many values should have been 0.5 less,
 * but the adc api system currently does not support describing 'half cycles'.
 * So all half cycles are counted as one.
 */
#if defined(CONFIG_SOC_SERIES_STM32F0X) || defined(CONFIG_SOC_SERIES_STM32F1X)
static const uint16_t acq_time_tbl[8] = {2, 8, 14, 29, 42, 56, 72, 240};
static const uint32_t table_samp_time[] = {
	SMP_TIME(1,   _5),
	SMP_TIME(7,   S_5),
	SMP_TIME(13,  S_5),
	SMP_TIME(28,  S_5),
	SMP_TIME(41,  S_5),
	SMP_TIME(55,  S_5),
	SMP_TIME(71,  S_5),
	SMP_TIME(239, S_5),
};
#elif defined(CONFIG_SOC_SERIES_STM32F2X) || \
	defined(CONFIG_SOC_SERIES_STM32F4X) || \
	defined(CONFIG_SOC_SERIES_STM32F7X)
static const uint16_t acq_time_tbl[8] = {3, 15, 28, 56, 84, 112, 144, 480};
static const uint32_t table_samp_time[] = {
	SMP_TIME(3,   S),
	SMP_TIME(15,  S),
	SMP_TIME(28,  S),
	SMP_TIME(56,  S),
	SMP_TIME(84,  S),
	SMP_TIME(112, S),
	SMP_TIME(144, S),
	SMP_TIME(480, S),
};
#elif defined(CONFIG_SOC_SERIES_STM32F3X)
#ifdef ADC5_V1_1
static const uint16_t acq_time_tbl[8] = {2, 3, 5, 8, 20, 62, 182, 602};
static const uint32_t table_samp_time[] = {
	SMP_TIME(1,   _5),
	SMP_TIME(2,   S_5),
	SMP_TIME(4,   S_5),
	SMP_TIME(7,   S_5),
	SMP_TIME(19,  S_5),
	SMP_TIME(61,  S_5),
	SMP_TIME(181, S_5),
	SMP_TIME(601, S_5),
};
#else
static const uint16_t acq_time_tbl[8] = {2, 8, 14, 29, 42, 56, 72, 240};
static const uint32_t table_samp_time[] = {
	SMP_TIME(1,   _5),
	SMP_TIME(7,   S_5),
	SMP_TIME(13,  S_5),
	SMP_TIME(28,  S_5),
	SMP_TIME(41,  S_5),
	SMP_TIME(55,  S_5),
	SMP_TIME(71,  S_5),
	SMP_TIME(239, S_5),
};
#endif /* ADC5_V1_1 */
#elif defined(CONFIG_SOC_SERIES_STM32L0X) || \
	defined(CONFIG_SOC_SERIES_STM32G0X) || \
	defined(CONFIG_SOC_SERIES_STM32WLX)
static const uint16_t acq_time_tbl[8] = {2, 4, 8, 13, 20, 40, 80, 161};
static const uint32_t table_samp_time[] = {
	SMP_TIME(1,   _5),
	SMP_TIME(3,   S_5),
	SMP_TIME(7,   S_5),
	SMP_TIME(12,  S_5),
	SMP_TIME(19,  S_5),
	SMP_TIME(39,  S_5),
	SMP_TIME(79,  S_5),
	SMP_TIME(160, S_5),
};
#elif defined(CONFIG_SOC_SERIES_STM32L4X) || \
	defined(CONFIG_SOC_SERIES_STM32L5X) || \
	defined(CONFIG_SOC_SERIES_STM32WBX) || \
	defined(CONFIG_SOC_SERIES_STM32G4X)
static const uint16_t acq_time_tbl[8] = {3, 7, 13, 25, 48, 93, 248, 641};
static const uint32_t table_samp_time[] = {
	SMP_TIME(2,   S_5),
	SMP_TIME(6,   S_5),
	SMP_TIME(12,  S_5),
	SMP_TIME(24,  S_5),
	SMP_TIME(47,  S_5),
	SMP_TIME(92,  S_5),
	SMP_TIME(247, S_5),
	SMP_TIME(640, S_5),
};
#elif defined(CONFIG_SOC_SERIES_STM32L1X)
static const uint16_t acq_time_tbl[8] = {5, 10, 17, 25, 49, 97, 193, 385};
static const uint32_t table_samp_time[] = {
	SMP_TIME(4,   S),
	SMP_TIME(9,   S),
	SMP_TIME(16,  S),
	SMP_TIME(24,  S),
	SMP_TIME(48,  S),
	SMP_TIME(96,  S),
	SMP_TIME(192, S),
	SMP_TIME(384, S),
};
#elif defined(CONFIG_SOC_SERIES_STM32H7X)
static const uint16_t acq_time_tbl[8] = {2, 3, 9, 17, 33, 65, 388, 811};
static const uint32_t table_samp_time[] = {
	SMP_TIME(1,   _5),
	SMP_TIME(2,   S_5),
	SMP_TIME(8,   S_5),
	SMP_TIME(16,  S_5),
	SMP_TIME(32,  S_5),
	SMP_TIME(64,  S_5),
	SMP_TIME(387, S_5),
	SMP_TIME(810, S_5),
};
#endif

/* Bugfix for STM32G4 HAL */
#if !defined(ADC_CHANNEL_TEMPSENSOR)
#define ADC_CHANNEL_TEMPSENSOR ADC_CHANNEL_TEMPSENSOR_ADC1
#endif

/* External channels (maximum). */
#define STM32_CHANNEL_COUNT		20

struct adc_stm32_data {
	struct adc_context ctx;
	const struct device *dev;
	uint16_t *buffer;
	uint16_t *repeat_buffer;

	uint8_t resolution;
	uint8_t channel_count;
#if defined(CONFIG_SOC_SERIES_STM32F0X) || \
	defined(CONFIG_SOC_SERIES_STM32G0X) || \
	defined(CONFIG_SOC_SERIES_STM32L0X)
	int8_t acq_time_index;
#endif
};

struct adc_stm32_cfg {
	ADC_TypeDef *base;
	void (*irq_cfg_func)(void);
	struct stm32_pclken pclken;
	const struct soc_gpio_pinctrl *pinctrl;
	size_t pinctrl_len;
};

static int check_buffer_size(const struct adc_sequence *sequence,
			     uint8_t active_channels)
{
	size_t needed_buffer_size;

	needed_buffer_size = active_channels * sizeof(uint16_t);

	if (sequence->options) {
		needed_buffer_size *= (1 + sequence->options->extra_samplings);
	}

	if (sequence->buffer_size < needed_buffer_size) {
		LOG_ERR("Provided buffer is too small (%u/%u)",
				sequence->buffer_size, needed_buffer_size);
		return -ENOMEM;
	}

	return 0;
}

static void adc_stm32_start_conversion(const struct device *dev)
{
	const struct adc_stm32_cfg *config = dev->config;
	ADC_TypeDef *adc = (ADC_TypeDef *)config->base;

	LOG_DBG("Starting conversion");

#if defined(CONFIG_SOC_SERIES_STM32F0X) || \
	defined(CONFIG_SOC_SERIES_STM32F3X) || \
	defined(CONFIG_SOC_SERIES_STM32L0X) || \
	defined(CONFIG_SOC_SERIES_STM32L4X) || \
	defined(CONFIG_SOC_SERIES_STM32L5X) || \
	defined(CONFIG_SOC_SERIES_STM32WBX) || \
	defined(CONFIG_SOC_SERIES_STM32G0X) || \
	defined(CONFIG_SOC_SERIES_STM32G4X) || \
	defined(CONFIG_SOC_SERIES_STM32H7X) || \
	defined(CONFIG_SOC_SERIES_STM32WLX)
	LL_ADC_REG_StartConversion(adc);
#else
	LL_ADC_REG_StartConversionSWStart(adc);
#endif
}

#if !defined(CONFIG_SOC_SERIES_STM32F2X) && \
	!defined(CONFIG_SOC_SERIES_STM32F4X) && \
	!defined(CONFIG_SOC_SERIES_STM32F7X) && \
	!defined(CONFIG_SOC_SERIES_STM32F1X) && \
	!defined(STM32F3X_ADC_V2_5) && \
	!defined(CONFIG_SOC_SERIES_STM32L1X)
static void adc_stm32_calib(const struct device *dev)
{
	const struct adc_stm32_cfg *config =
		(const struct adc_stm32_cfg *)dev->config;
	ADC_TypeDef *adc = config->base;

#if defined(STM32F3X_ADC_V1_1) || \
	defined(CONFIG_SOC_SERIES_STM32L4X) || \
	defined(CONFIG_SOC_SERIES_STM32L5X) || \
	defined(CONFIG_SOC_SERIES_STM32WBX) || \
	defined(CONFIG_SOC_SERIES_STM32G4X)
	LL_ADC_StartCalibration(adc, LL_ADC_SINGLE_ENDED);
#elif defined(CONFIG_SOC_SERIES_STM32F0X) || \
	defined(CONFIG_SOC_SERIES_STM32G0X) || \
	defined(CONFIG_SOC_SERIES_STM32L0X) || \
	defined(CONFIG_SOC_SERIES_STM32WLX)
	LL_ADC_StartCalibration(adc);
#elif defined(CONFIG_SOC_SERIES_STM32H7X)
	LL_ADC_StartCalibration(adc, LL_ADC_CALIB_OFFSET, LL_ADC_SINGLE_ENDED);
#endif
	while (LL_ADC_IsCalibrationOnGoing(adc)) {
	}
}
#endif

static int start_read(const struct device *dev,
		      const struct adc_sequence *sequence)
{
	const struct adc_stm32_cfg *config = dev->config;
	struct adc_stm32_data *data = dev->data;
	ADC_TypeDef *adc = (ADC_TypeDef *)config->base;
	uint8_t resolution;
	int err;

	switch (sequence->resolution) {
#if defined(CONFIG_SOC_SERIES_STM32F1X) || \
	defined(STM32F3X_ADC_V2_5)
	case 12:
		resolution = table_resolution[0];
		break;
#elif !defined(CONFIG_SOC_SERIES_STM32H7X)
	case 6:
		resolution = table_resolution[0];
		break;
	case 8:
		resolution = table_resolution[1];
		break;
	case 10:
		resolution = table_resolution[2];
		break;
	case 12:
		resolution = table_resolution[3];
		break;
#else
	case 8:
		resolution = table_resolution[0];
		break;
	case 10:
		resolution = table_resolution[1];
		break;
	case 12:
		resolution = table_resolution[2];
		break;
	case 14:
		resolution = table_resolution[3];
		break;
	case 16:
		resolution = table_resolution[4];
		break;
#endif
	default:
		LOG_ERR("Invalid resolution");
		return -EINVAL;
	}

	uint32_t channels = sequence->channels;
	uint8_t index = find_lsb_set(channels) - 1;

	if (channels > BIT(index)) {
		LOG_ERR("Only single channel supported");
		return -ENOTSUP;
	}

	data->buffer = sequence->buffer;

	uint32_t channel = __LL_ADC_DECIMAL_NB_TO_CHANNEL(index);
#if defined(CONFIG_SOC_SERIES_STM32H7X)
	/*
	 * Each channel in the sequence must be previously enabled in PCSEL.
	 * This register controls the analog switch integrated in the IO level.
	 * NOTE: There is no LL API to control this register yet.
	 */
	adc->PCSEL |= channels & ADC_PCSEL_PCSEL_Msk;
#endif

#if defined(CONFIG_SOC_SERIES_STM32F0X) || \
	defined(CONFIG_SOC_SERIES_STM32L0X)
	LL_ADC_REG_SetSequencerChannels(adc, channel);
#elif defined(CONFIG_SOC_SERIES_STM32G0X) || \
	defined(CONFIG_SOC_SERIES_STM32WLX)
	/* STM32G0 in "not fully configurable" sequencer mode */
	LL_ADC_REG_SetSequencerChannels(adc, channel);
	while (LL_ADC_IsActiveFlag_CCRDY(adc) == 0) {
	}
#else
	LL_ADC_REG_SetSequencerRanks(adc, table_rank[0], channel);
	LL_ADC_REG_SetSequencerLength(adc, table_seq_len[0]);
#endif
	data->channel_count = 1;

	err = check_buffer_size(sequence, data->channel_count);
	if (err) {
		return err;
	}

#if defined(CONFIG_SOC_SERIES_STM32G0X)
	/*
	 * Errata: Writing ADC_CFGR1 register while ADEN bit is set
	 * resets RES[1:0] bitfield. We need to disable and enable adc.
	 */
	if (LL_ADC_IsEnabled(adc) == 1UL) {
		LL_ADC_Disable(adc);
	}
	while (LL_ADC_IsEnabled(adc) == 1UL) {
	}
	LL_ADC_SetResolution(adc, resolution);
	LL_ADC_Enable(adc);
	while (LL_ADC_IsActiveFlag_ADRDY(adc) != 1UL) {
	}
#elif !defined(CONFIG_SOC_SERIES_STM32F1X) && \
	!defined(STM32F3X_ADC_V2_5)
	LL_ADC_SetResolution(adc, resolution);
#endif

#if defined(CONFIG_SOC_SERIES_STM32G0X) || \
	defined(CONFIG_SOC_SERIES_STM32G4X) || \
	defined(CONFIG_SOC_SERIES_STM32H7X) || \
	defined(CONFIG_SOC_SERIES_STM32L0X) || \
	defined(CONFIG_SOC_SERIES_STM32L4X) || \
	defined(CONFIG_SOC_SERIES_STM32WBX) || \
	defined(CONFIG_SOC_SERIES_STM32WLX)
	switch (sequence->oversampling) {
	case 0:
		LL_ADC_SetOverSamplingScope(adc, LL_ADC_OVS_DISABLE);
		break;
	case 1:
		LL_ADC_SetOverSamplingScope(adc, LL_ADC_OVS_GRP_REGULAR_CONTINUED);
		LL_ADC_ConfigOverSamplingRatioShift(adc, LL_ADC_OVS_RATIO_2,
						    LL_ADC_OVS_SHIFT_RIGHT_1);
		break;
	case 2:
		LL_ADC_SetOverSamplingScope(adc, LL_ADC_OVS_GRP_REGULAR_CONTINUED);
		LL_ADC_ConfigOverSamplingRatioShift(adc, LL_ADC_OVS_RATIO_4,
						    LL_ADC_OVS_SHIFT_RIGHT_2);
		break;
	case 3:
		LL_ADC_SetOverSamplingScope(adc, LL_ADC_OVS_GRP_REGULAR_CONTINUED);
		LL_ADC_ConfigOverSamplingRatioShift(adc, LL_ADC_OVS_RATIO_8,
						    LL_ADC_OVS_SHIFT_RIGHT_3);
		break;
	case 4:
		LL_ADC_SetOverSamplingScope(adc, LL_ADC_OVS_GRP_REGULAR_CONTINUED);
		LL_ADC_ConfigOverSamplingRatioShift(adc, LL_ADC_OVS_RATIO_16,
						    LL_ADC_OVS_SHIFT_RIGHT_4);
		break;
	case 5:
		LL_ADC_SetOverSamplingScope(adc, LL_ADC_OVS_GRP_REGULAR_CONTINUED);
		LL_ADC_ConfigOverSamplingRatioShift(adc, LL_ADC_OVS_RATIO_32,
						    LL_ADC_OVS_SHIFT_RIGHT_5);
		break;
	case 6:
		LL_ADC_SetOverSamplingScope(adc, LL_ADC_OVS_GRP_REGULAR_CONTINUED);
		LL_ADC_ConfigOverSamplingRatioShift(adc, LL_ADC_OVS_RATIO_64,
						    LL_ADC_OVS_SHIFT_RIGHT_6);
		break;
	case 7:
		LL_ADC_SetOverSamplingScope(adc, LL_ADC_OVS_GRP_REGULAR_CONTINUED);
		LL_ADC_ConfigOverSamplingRatioShift(adc, LL_ADC_OVS_RATIO_128,
						    LL_ADC_OVS_SHIFT_RIGHT_7);
		break;
	case 8:
		LL_ADC_SetOverSamplingScope(adc, LL_ADC_OVS_GRP_REGULAR_CONTINUED);
		LL_ADC_ConfigOverSamplingRatioShift(adc, LL_ADC_OVS_RATIO_256,
						    LL_ADC_OVS_SHIFT_RIGHT_8);
		break;
	default:
		LOG_ERR("Invalid oversampling");
		return -EINVAL;
	}
#else
	if (sequence->oversampling) {
		LOG_ERR("Oversampling not supported");
		return -ENOTSUP;
	}
#endif

	if (sequence->calibrate) {
#if !defined(CONFIG_SOC_SERIES_STM32F2X) && \
	!defined(CONFIG_SOC_SERIES_STM32F4X) && \
	!defined(CONFIG_SOC_SERIES_STM32F7X) && \
	!defined(CONFIG_SOC_SERIES_STM32F1X) && \
	!defined(STM32F3X_ADC_V2_5) && \
	!defined(CONFIG_SOC_SERIES_STM32L1X)
		adc_stm32_calib(dev);
#else
		LOG_ERR("Calibration not supported");
		return -ENOTSUP;
#endif
	}

#if defined(CONFIG_SOC_SERIES_STM32F0X) || \
	defined(STM32F3X_ADC_V1_1) || \
	defined(CONFIG_SOC_SERIES_STM32L0X) || \
	defined(CONFIG_SOC_SERIES_STM32L4X) || \
	defined(CONFIG_SOC_SERIES_STM32L5X) || \
	defined(CONFIG_SOC_SERIES_STM32WBX) || \
	defined(CONFIG_SOC_SERIES_STM32G0X) || \
	defined(CONFIG_SOC_SERIES_STM32G4X) || \
	defined(CONFIG_SOC_SERIES_STM32H7X) || \
	defined(CONFIG_SOC_SERIES_STM32WLX)
	LL_ADC_EnableIT_EOC(adc);
#elif defined(CONFIG_SOC_SERIES_STM32F1X)
	LL_ADC_EnableIT_EOS(adc);
#elif defined(STM32F3X_ADC_V2_5)
	LL_ADC_Enable(adc);
	LL_ADC_EnableIT_EOS(adc);
#else
	LL_ADC_EnableIT_EOCS(adc);
#endif

	adc_context_start_read(&data->ctx, sequence);

	return adc_context_wait_for_completion(&data->ctx);
}

static void adc_context_start_sampling(struct adc_context *ctx)
{
	struct adc_stm32_data *data =
		CONTAINER_OF(ctx, struct adc_stm32_data, ctx);

	data->repeat_buffer = data->buffer;

	adc_stm32_start_conversion(data->dev);
}

static void adc_context_update_buffer_pointer(struct adc_context *ctx,
					      bool repeat_sampling)
{
	struct adc_stm32_data *data =
		CONTAINER_OF(ctx, struct adc_stm32_data, ctx);

	if (repeat_sampling) {
		data->buffer = data->repeat_buffer;
	}
}

static void adc_stm32_isr(const struct device *dev)
{
	struct adc_stm32_data *data = (struct adc_stm32_data *)dev->data;
	const struct adc_stm32_cfg *config =
		(const struct adc_stm32_cfg *)dev->config;
	ADC_TypeDef *adc = config->base;

	*data->buffer++ = LL_ADC_REG_ReadConversionData32(adc);

	adc_context_on_sampling_done(&data->ctx, dev);

	LOG_DBG("ISR triggered.");
}

static int adc_stm32_read(const struct device *dev,
			  const struct adc_sequence *sequence)
{
	struct adc_stm32_data *data = dev->data;
	int error;

	adc_context_lock(&data->ctx, false, NULL);
	error = start_read(dev, sequence);
	adc_context_release(&data->ctx, error);

	return error;
}

#ifdef CONFIG_ADC_ASYNC
static int adc_stm32_read_async(const struct device *dev,
				 const struct adc_sequence *sequence,
				 struct k_poll_signal *async)
{
	struct adc_stm32_data *data = dev->data;
	int error;

	adc_context_lock(&data->ctx, true, async);
	error = start_read(dev, sequence);
	adc_context_release(&data->ctx, error);

	return error;
}
#endif

static int adc_stm32_check_acq_time(uint16_t acq_time)
{
	if (acq_time == ADC_ACQ_TIME_MAX) {
		return ARRAY_SIZE(acq_time_tbl) - 1;
	}

	for (int i = 0; i < 8; i++) {
		if (acq_time == ADC_ACQ_TIME(ADC_ACQ_TIME_TICKS,
					     acq_time_tbl[i])) {
			return i;
		}
	}

	if (acq_time == ADC_ACQ_TIME_DEFAULT) {
		return 0;
	}

	LOG_ERR("Conversion time not supportted.");
	return -EINVAL;
}

/*
 * Enable internal channel source
 */
static void adc_stm32_set_common_path(const struct device *dev, uint32_t PathInternal)
{
	const struct adc_stm32_cfg *config =
		(const struct adc_stm32_cfg *)dev->config;
	ADC_TypeDef *adc = (ADC_TypeDef *)config->base;
	(void) adc; /* Avoid 'unused variable' warning for some families */

	/* Do not remove existing paths */
	PathInternal |= LL_ADC_GetCommonPathInternalCh(__LL_ADC_COMMON_INSTANCE(adc));
	LL_ADC_SetCommonPathInternalCh(__LL_ADC_COMMON_INSTANCE(adc), PathInternal);
}

static void adc_stm32_setup_speed(const struct device *dev, uint8_t id,
				  uint8_t acq_time_index)
{
	const struct adc_stm32_cfg *config =
		(const struct adc_stm32_cfg *)dev->config;
	ADC_TypeDef *adc = config->base;

#if defined(CONFIG_SOC_SERIES_STM32F0X) || defined(CONFIG_SOC_SERIES_STM32L0X)
	LL_ADC_SetSamplingTimeCommonChannels(adc,
		table_samp_time[acq_time_index]);
#elif defined(CONFIG_SOC_SERIES_STM32G0X)
	LL_ADC_SetSamplingTimeCommonChannels(adc, LL_ADC_SAMPLINGTIME_COMMON_1,
		table_samp_time[acq_time_index]);
#else
	LL_ADC_SetChannelSamplingTime(adc,
		__LL_ADC_DECIMAL_NB_TO_CHANNEL(id),
		table_samp_time[acq_time_index]);
#endif
}

static int adc_stm32_channel_setup(const struct device *dev,
				   const struct adc_channel_cfg *channel_cfg)
{
#if defined(CONFIG_SOC_SERIES_STM32F0X) || \
	defined(CONFIG_SOC_SERIES_STM32G0X) || \
	 defined(CONFIG_SOC_SERIES_STM32L0X)
	struct adc_stm32_data *data = dev->data;
#endif
	int acq_time_index;

	if (channel_cfg->channel_id >= STM32_CHANNEL_COUNT) {
		LOG_ERR("Channel %d is not valid", channel_cfg->channel_id);
		return -EINVAL;
	}

	acq_time_index = adc_stm32_check_acq_time(
				channel_cfg->acquisition_time);
	if (acq_time_index < 0) {
		return acq_time_index;
	}
#if defined(CONFIG_SOC_SERIES_STM32F0X) || \
	defined(CONFIG_SOC_SERIES_STM32G0X) || \
	defined(CONFIG_SOC_SERIES_STM32L0X)
	if (data->acq_time_index == -1) {
		data->acq_time_index = acq_time_index;
	} else {
		/* All channels of F0/L0 must have identical acquisition time.*/
		if (acq_time_index != data->acq_time_index) {
			return -EINVAL;
		}
	}
#endif

	if (channel_cfg->differential) {
		LOG_ERR("Differential channels are not supported");
		return -EINVAL;
	}

	if (channel_cfg->gain != ADC_GAIN_1) {
		LOG_ERR("Invalid channel gain");
		return -EINVAL;
	}

	if (channel_cfg->reference != ADC_REF_INTERNAL) {
		LOG_ERR("Invalid channel reference");
		return -EINVAL;
	}

	if (__LL_ADC_CHANNEL_TO_DECIMAL_NB(ADC_CHANNEL_TEMPSENSOR) == channel_cfg->channel_id) {
		adc_stm32_set_common_path(dev, LL_ADC_PATH_INTERNAL_TEMPSENSOR);
	} else if (__LL_ADC_CHANNEL_TO_DECIMAL_NB(ADC_CHANNEL_VREFINT) == channel_cfg->channel_id) {
		adc_stm32_set_common_path(dev, LL_ADC_PATH_INTERNAL_VREFINT);
	}

	adc_stm32_setup_speed(dev, channel_cfg->channel_id,
				  acq_time_index);

	LOG_DBG("Channel setup succeeded!");

	return 0;
}

static int adc_stm32_init(const struct device *dev)
{
	struct adc_stm32_data *data = dev->data;
    /*
    LS: adc_stm32_cfg는 아래 962번 라인에서 확인가능
    */
	const struct adc_stm32_cfg *config = dev->config;
    /* LS:
        STM32_CLOCK_CONTROL_NODE -> DT_NODELABEL(rcc)
        -> DT_CAT(DT_N_NODELABEL_, rcc) -> DT_N_NODELABEL_rcc
        -> DT_N_S_soc_S_rcc_40021000 (defined in devicetree_unfixed.h)

        DEVICE_DT_GET(DT_N_S_soc_S_rcc_40021000) -> &__device_dts_ord_10
        (device_extern.h) extern const struct device DEVICE_DT_NAME_GET(DT_N_S_soc_S_rcc_40021000);
        DEVICE_DT_NAME_GET은 구조체를 가져오고, DEVICE_DT_GET는 그 포인터를 받아와 device *clk에 대입.
        (drivers/clock_control/clock_stm32_ll_common.c)에 __device_dts_ord_10 실제 정의가 있음.
    */
	const struct device *clk = DEVICE_DT_GET(STM32_CLOCK_CONTROL_NODE);
    /* LS: config->base -> DT_INST_REG_ADDR(index)

        DT_INST_REG_ADDR(13) -> DT_INST_REG_ADDR_BY_IDX(13, 0)
        -> DT_REG_ADDR_BY_IDX(DT_DRV_INST(13), 0) ->
        -> DT_CAT(node_id, _REG_IDX_0_VAL_ADDRESS)
        -> DT_N_S_soc_S_adc_40012400_REG_IDX_0_VAL_ADDRESS
        -> 1073816576 == 0x40012400 (defined in in devicetree_unfixed.h)
    */
	ADC_TypeDef *adc = (ADC_TypeDef *)config->base;
	int err;

	LOG_DBG("Initializing....");

    /* LS: data->dev에 dev를 대입. adc_stm32_data를 void *타입의 opaque pointer로 전달하기 위함 */
	data->dev = dev;
#if defined(CONFIG_SOC_SERIES_STM32F0X) || \
	defined(CONFIG_SOC_SERIES_STM32G0X) || \
	defined(CONFIG_SOC_SERIES_STM32L0X)
	/*
	 * All conversion time for all channels on one ADC instance for F0 and
	 * L0 series chips has to be the same. For STM32G0 currently only one
	 * of the two available common channel conversion times is used.
	 * This additional variable is for checking if the conversion time
	 * selection of all channels on one ADC instance is the same.
	 */
	data->acq_time_index = -1;
#endif

	if (clock_control_on(clk,
        /* LS: .config->pclken = {
            .enr = DT_INST_CLOCKS_CELL(index, bits),
            .bus = DT_INST_CLOCKS_CELL(index, bus),
            },
            config->pclken.enr -> DT_N_S_soc_S_adc_40012400_P_clocks_IDX_0_VAL_bits -> 512
            config->pclken.bus -> DT_N_S_soc_S_adc_40012400_P_clocks_IDX_0_VAL_bus -> 3
        */
		(clock_control_subsys_t *) &config->pclken) != 0) {
		return -EIO;
	}

	/* Configure dt provided device signals when available */
    /* LS: config->pinctrl => adc_pins_13
    -> ST_STM32_DT_INST_PINCTRL(index, 0)
    -> {{13, }}
    */
	err = stm32_dt_pinctrl_configure(config->pinctrl,
					 config->pinctrl_len,
					 (uint32_t)config->base);
	if (err < 0) {
		LOG_ERR("ADC pinctrl setup failed (%d)", err);
		return err;
	}

#if defined(CONFIG_SOC_SERIES_STM32L4X) || \
	defined(CONFIG_SOC_SERIES_STM32L5X) || \
	defined(CONFIG_SOC_SERIES_STM32WBX) || \
	defined(CONFIG_SOC_SERIES_STM32G4X) || \
	defined(CONFIG_SOC_SERIES_STM32H7X)
	/*
	 * L4, WB, G4 and H7 series STM32 needs to be awaken from deep sleep
	 * mode, and restore its calibration parameters if there are some
	 * previously stored calibration parameters.
	 */

	LL_ADC_DisableDeepPowerDown(adc);
#endif
	/*
	 * F3, L4, WB, G0 and G4 ADC modules need some time
	 * to be stabilized before performing any enable or calibration actions.
	 */
#if defined(STM32F3X_ADC_V1_1) || \
	defined(CONFIG_SOC_SERIES_STM32L4X) || \
	defined(CONFIG_SOC_SERIES_STM32L5X) || \
	defined(CONFIG_SOC_SERIES_STM32WBX) || \
	defined(CONFIG_SOC_SERIES_STM32G0X) || \
	defined(CONFIG_SOC_SERIES_STM32G4X) || \
	defined(CONFIG_SOC_SERIES_STM32H7X) || \
	defined(CONFIG_SOC_SERIES_STM32WLX)
	LL_ADC_EnableInternalRegulator(adc);
	k_busy_wait(LL_ADC_DELAY_INTERNAL_REGUL_STAB_US);
#endif

#if defined(CONFIG_SOC_SERIES_STM32F0X) || \
	defined(CONFIG_SOC_SERIES_STM32L0X) || \
	defined(CONFIG_SOC_SERIES_STM32WLX)
	LL_ADC_SetClock(adc, LL_ADC_CLOCK_SYNC_PCLK_DIV4);
#elif defined(STM32F3X_ADC_V1_1) || \
	defined(CONFIG_SOC_SERIES_STM32L4X) || \
	defined(CONFIG_SOC_SERIES_STM32L5X) || \
	defined(CONFIG_SOC_SERIES_STM32WBX) || \
	defined(CONFIG_SOC_SERIES_STM32G0X) || \
	defined(CONFIG_SOC_SERIES_STM32G4X) || \
	defined(CONFIG_SOC_SERIES_STM32H7X)
	LL_ADC_SetCommonClock(__LL_ADC_COMMON_INSTANCE(adc),
			      LL_ADC_CLOCK_SYNC_PCLK_DIV4);
#elif defined(CONFIG_SOC_SERIES_STM32L1X)
	LL_ADC_SetCommonClock(__LL_ADC_COMMON_INSTANCE(adc),
			LL_ADC_CLOCK_ASYNC_DIV4);
#endif

#if !defined(CONFIG_SOC_SERIES_STM32F2X) && \
	!defined(CONFIG_SOC_SERIES_STM32F4X) && \
	!defined(CONFIG_SOC_SERIES_STM32F7X) && \
	!defined(CONFIG_SOC_SERIES_STM32F1X) && \
	!defined(STM32F3X_ADC_V2_5) && \
	!defined(CONFIG_SOC_SERIES_STM32L1X)
	/*
	 * Calibration of F1 and F3 (ADC1_V2_5) series has to be started
	 * after ADC Module is enabled.
	 */
	adc_stm32_calib(dev);
#endif

#if defined(CONFIG_SOC_SERIES_STM32F0X) || \
	defined(CONFIG_SOC_SERIES_STM32L0X) || \
	defined(CONFIG_SOC_SERIES_STM32L4X) || \
	defined(CONFIG_SOC_SERIES_STM32L5X) || \
	defined(CONFIG_SOC_SERIES_STM32WBX) || \
	defined(CONFIG_SOC_SERIES_STM32G0X) || \
	defined(CONFIG_SOC_SERIES_STM32G4X) || \
	defined(CONFIG_SOC_SERIES_STM32H7X) || \
	defined(CONFIG_SOC_SERIES_STM32WLX)
	if (LL_ADC_IsActiveFlag_ADRDY(adc)) {
		LL_ADC_ClearFlag_ADRDY(adc);
	}

	/*
	 * These STM32 series has one internal voltage reference source
	 * to be enabled.
	 */
	LL_ADC_SetCommonPathInternalCh(__LL_ADC_COMMON_INSTANCE(adc),
				       LL_ADC_PATH_INTERNAL_VREFINT);
#endif

#if defined(CONFIG_SOC_SERIES_STM32F0X) || \
	defined(STM32F3X_ADC_V1_1) || \
	defined(CONFIG_SOC_SERIES_STM32L0X) || \
	defined(CONFIG_SOC_SERIES_STM32L4X) || \
	defined(CONFIG_SOC_SERIES_STM32L5X) || \
	defined(CONFIG_SOC_SERIES_STM32WBX) || \
	defined(CONFIG_SOC_SERIES_STM32G0X) || \
	defined(CONFIG_SOC_SERIES_STM32G4X) || \
	defined(CONFIG_SOC_SERIES_STM32H7X) || \
	defined(CONFIG_SOC_SERIES_STM32WLX)
	/*
	 * ADC modules on these series have to wait for some cycles to be
	 * enabled.
	 */
	uint32_t adc_rate, wait_cycles;

	if (clock_control_get_rate(clk,
		(clock_control_subsys_t *) &config->pclken, &adc_rate) < 0) {
		LOG_ERR("ADC clock rate get error.");
	}

	wait_cycles = SystemCoreClock / adc_rate *
		      LL_ADC_DELAY_CALIB_ENABLE_ADC_CYCLES;

	for (int i = wait_cycles; i >= 0; i--) {
	}
#endif

	LL_ADC_Enable(adc);

#if defined(CONFIG_SOC_SERIES_STM32L4X) || \
	defined(CONFIG_SOC_SERIES_STM32L5X) || \
	defined(CONFIG_SOC_SERIES_STM32WBX) || \
	defined(CONFIG_SOC_SERIES_STM32G0X) || \
	defined(CONFIG_SOC_SERIES_STM32G4X) || \
	defined(CONFIG_SOC_SERIES_STM32H7X) || \
	defined(CONFIG_SOC_SERIES_STM32WLX)
	/*
	 * Enabling ADC modules in L4, WB, G0 and G4 series may fail if they are
	 * still not stabilized, this will wait for a short time to ensure ADC
	 * modules are properly enabled.
	 */
	uint32_t countTimeout = 0;

	while (LL_ADC_IsActiveFlag_ADRDY(adc) == 0) {
		if (LL_ADC_IsEnabled(adc) == 0UL) {
			LL_ADC_Enable(adc);
			countTimeout++;
			if (countTimeout == 10) {
				return -ETIMEDOUT;
			}
		}
	}
#endif

	config->irq_cfg_func();

#if defined(CONFIG_SOC_SERIES_STM32F1X) || \
	defined(STM32F3X_ADC_V2_5)
	/*
	 * Calibration of F1 and F3 (ADC1_V2_5) must starts after two cycles
	 * after ADON is set.
	 */
	LL_ADC_StartCalibration(adc);
	LL_ADC_REG_SetTriggerSource(adc, LL_ADC_REG_TRIG_SOFTWARE);
#endif

#ifdef CONFIG_SOC_SERIES_STM32H7X
	/*
	 * To ensure linearity the factory calibration values
	 * should be loaded on initialization.
	 */
	uint32_t channel_offset = 0U;
	uint32_t linear_calib_buffer = 0U;

	if (adc == ADC1) {
		channel_offset = 0UL;
	} else if (adc == ADC2) {
		channel_offset = 8UL;
	} else   /*Case ADC3*/ {
		channel_offset = 16UL;
	}
	/* Read factory calibration factors */
	for (uint32_t count = 0UL; count < ADC_LINEAR_CALIB_REG_COUNT; count++) {
		linear_calib_buffer = *(uint32_t *)(
			ADC_LINEAR_CALIB_REG_1_ADDR + channel_offset + count
		);
		LL_ADC_SetCalibrationLinearFactor(
			adc, LL_ADC_CALIB_LINEARITY_WORD1 << count,
			linear_calib_buffer
		);
	}
#endif
	adc_context_unlock_unconditionally(&data->ctx);

	return 0;
}

static const struct adc_driver_api api_stm32_driver_api = {
	.channel_setup = adc_stm32_channel_setup,
	.read = adc_stm32_read,
#ifdef CONFIG_ADC_ASYNC
	.read_async = adc_stm32_read_async,
#endif
};

/* LS: 20210919 - START */
/* DEVICE_DT_INST_DEFINE 전처리 부터 시작 */
#define STM32_ADC_INIT(index)						\
									\
static void adc_stm32_cfg_func_##index(void);				\
									\
/* LS: ST_STM32_DT_INST_PINCTRL(13, 0)
    COND_CODE_1로 DT_N_S_soc_S_adc_40012400_P_pinctrl_0_EXISTS를 체크하고
    UTIL_LISTIFY를 통해 {
		ST_STM32_DT_INST_PINMUX(13, 0, 0),
		ST_STM32_DT_INST_PINCFG(13, 0, 0),
	} 로 initialzer를 만듬.
*/ \
static const struct soc_gpio_pinctrl adc_pins_##index[] =		\
	ST_STM32_DT_INST_PINCTRL(index, 0);				\
									\
static const struct adc_stm32_cfg adc_stm32_cfg_##index = {		\
	.base = (ADC_TypeDef *)DT_INST_REG_ADDR(index),			\
	.irq_cfg_func = adc_stm32_cfg_func_##index,			\
	.pclken = {							\
		.enr = DT_INST_CLOCKS_CELL(index, bits),		\
		.bus = DT_INST_CLOCKS_CELL(index, bus),			\
	},								\
	.pinctrl = adc_pins_##index,					\
	.pinctrl_len = ARRAY_SIZE(adc_pins_##index),			\
};									\
static struct adc_stm32_data adc_stm32_data_##index = {			\
	ADC_CONTEXT_INIT_TIMER(adc_stm32_data_##index, ctx),		\
	ADC_CONTEXT_INIT_LOCK(adc_stm32_data_##index, ctx),		\
	ADC_CONTEXT_INIT_SYNC(adc_stm32_data_##index, ctx),		\
};									\
									\
DEVICE_DT_INST_DEFINE(index,						\
		    &adc_stm32_init, NULL,				\
		    &adc_stm32_data_##index, &adc_stm32_cfg_##index,	\
		    POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,	\
		    &api_stm32_driver_api);				\
									\
static void adc_stm32_cfg_func_##index(void)				\
{									\
    /*
     * LS:
     *
     * ADC1 의 인터럽트 핸들러를 등록한다. 
     *  
     * irq number : 12
     * irq priority : 0
     * isr func : adc_stm32_isr
     * isr_param_p :&(__device_dts_ord_13)
     * flags_p : 0
     *
     */
	IRQ_CONNECT(DT_INST_IRQN(index),				\
		    DT_INST_IRQ(index, priority),			\
		    adc_stm32_isr, DEVICE_DT_INST_GET(index), 0);	\
	irq_enable(DT_INST_IRQN(index));				\
}

DT_INST_FOREACH_STATUS_OKAY(STM32_ADC_INIT)
