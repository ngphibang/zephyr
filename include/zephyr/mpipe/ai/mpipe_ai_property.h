/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Property identifiers of the AI/ML plugin.
 * @ingroup mpipe_ai
 */

#ifndef ZEPHYR_INCLUDE_MPIPE_AI_MPIPE_AI_PROPERTY_H_
#define ZEPHYR_INCLUDE_MPIPE_AI_MPIPE_AI_PROPERTY_H_

/**
 * @addtogroup mpipe_ai
 * @{
 */

#include <zephyr/sys/util_macro.h>

#include <zephyr/mpipe/mpipe_sink.h>
#include <zephyr/mpipe/mpipe_src.h>
#include <zephyr/mpipe/mpipe_transform.h>

/**
 * @brief AI element property identifiers
 *
 * The plugin has transform elements and a sink element, so the enumeration
 * starts past the last identifier of every base class to avoid conflicts.
 */
enum mpipe_prop_ai {
	/** Model blob, a const struct mpipe_ai_model pointer, copied */
	MPIPE_PROP_AI_MODEL = MAX((int)MPIPE_PROP_SINK_LAST,
				  MAX((int)MPIPE_PROP_SRC_LAST, (int)MPIPE_PROP_TRANSFORM_LAST)),
	/** Backend working memory, a const struct mpipe_ai_arena pointer, copied */
	MPIPE_PROP_AI_ARENA,
	/**
	 * Quantization zero point of the converter, an int32_t pointer.
	 * Unset, samples are stored as raw bytes and an int8 tensor simply
	 * reinterprets them - the TensorFlow Lite Micro tooling convention
	 * the stock quantized vision models are calibrated for. Set it only
	 * for a model documented with an affine shift (the sample value is
	 * added and the result clamped to the tensor type's range).
	 */
	MPIPE_PROP_AI_CONVERT_ZERO_POINT,
	/** Kind of results to decode, a uint8_t pointer, an mpipe_ai_results_type value */
	MPIPE_PROP_AI_DECODE_TYPE,
	/** Detection score threshold, a float pointer */
	MPIPE_PROP_AI_DECODE_THRESHOLD,
};

/** @} */

#endif /* ZEPHYR_INCLUDE_MPIPE_AI_MPIPE_AI_PROPERTY_H_ */
