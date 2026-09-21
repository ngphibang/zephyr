/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Tensor decode element.
 * @ingroup mpipe_ai
 */

#ifndef ZEPHYR_INCLUDE_MPIPE_AI_MPIPE_AI_DECODE_H_
#define ZEPHYR_INCLUDE_MPIPE_AI_MPIPE_AI_DECODE_H_

/**
 * @addtogroup mpipe_ai
 * @{
 */

#include <zephyr/mpipe/mpipe_buffer.h>
#include <zephyr/mpipe/mpipe_transform.h>
#include <zephyr/mpipe/ai/mpipe_ai.h>
#include <zephyr/mpipe/ai/mpipe_ai_backend.h>
#include <zephyr/mpipe/ai/mpipe_ai_property.h>
#include <zephyr/mpipe/ai/mpipe_ai_results.h>

/**
 * @brief Tensor decode element
 *
 * A transform turning the output tensors of an inference into a results
 * stream: it dequantizes and ranks class scores (classification) or filters
 * boxes by score (detection). Each produced buffer carries one
 * struct mpipe_ai_results and negotiates as MPIPE_MEDIA_AI_RESULTS; an app_sink
 * typically terminates the branch and hands the results to the application.
 */
struct mpipe_ai_decode {
	/** Base transform element (must be first) */
	struct mpipe_transform transform;
	/** Kind of results to produce, an @ref mpipe_ai_results_type value */
	uint8_t decode_type;
	/** Detection score threshold */
	float threshold;
	/** Sequence counter across inferences */
	uint32_t seq;
	/** Internal output buffer pool */
	struct mpipe_buffer_pool out_pool;
};

/**
 * @brief Initialize a tensor decode element
 *
 * @param decode Pointer to the element to initialize.
 * @param id Unique element identifier.
 *
 * @return 0 on success, negative errno otherwise.
 */
int mpipe_ai_decode_init(struct mpipe_ai_decode *decode, uint8_t id);

/** @} */

#endif /* ZEPHYR_INCLUDE_MPIPE_AI_MPIPE_AI_DECODE_H_ */
