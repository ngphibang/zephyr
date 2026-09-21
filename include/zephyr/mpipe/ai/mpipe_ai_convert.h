/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Video to tensor converter element.
 * @ingroup mpipe_ai
 */

#ifndef ZEPHYR_INCLUDE_MPIPE_AI_MPIPE_AI_CONVERT_H_
#define ZEPHYR_INCLUDE_MPIPE_AI_MPIPE_AI_CONVERT_H_

/**
 * @addtogroup mpipe_ai
 * @{
 */

#include <zephyr/mpipe/mpipe_buffer.h>
#include <zephyr/mpipe/mpipe_transform.h>
#include <zephyr/mpipe/ai/mpipe_ai.h>
#include <zephyr/mpipe/ai/mpipe_ai_property.h>

struct mpipe_ai_convert;

/**
 * @brief One conversion the element implements
 */
struct mpipe_ai_convert_desc {
	/** Input pixel format */
	uint32_t pixfmt;
	/** Output tensor channels */
	uint8_t channels;
	/** Output tensor data type, an @ref mpipe_ai_tensor_type value */
	uint8_t dtype;
	/**
	 * @brief Convert one frame into one tensor
	 * @param conv The element, holding the negotiated geometry
	 * @param in Input frame
	 * @param out Output tensor buffer
	 * @return 0 on success, negative errno on failure
	 */
	int (*fn)(const struct mpipe_ai_convert *conv, const struct net_buf *in,
		  struct net_buf *out);
};

/**
 * @brief Video to tensor converter element
 *
 * Converts a video frame into a model input tensor in a single pass:
 * nearest-neighbor scaling, color conversion and quantization together. The
 * element always scales, so the video geometry and the tensor geometry
 * negotiate independently: the camera or display fixes one side, the model
 * fixes the other, and no separate resize element is needed.
 */
struct mpipe_ai_convert {
	/** Base transform element (must be first) */
	struct mpipe_transform transform;
	/** Input frame width */
	uint16_t in_w;
	/** Input frame height */
	uint16_t in_h;
	/** Output tensor width */
	uint16_t out_w;
	/** Output tensor height */
	uint16_t out_h;
	/** Input pixel format */
	uint32_t in_pixfmt;
	/** Output tensor data type, an @ref mpipe_ai_tensor_type value */
	uint8_t out_dtype;
	/** Output tensor channels */
	uint8_t out_channels;
	/** Quantization zero point applied to each sample */
	int32_t zero_point;
	/** True once MPIPE_PROP_AI_CONVERT_ZERO_POINT overrode the default */
	bool zero_point_set;
	/** The conversion picked once both sides are negotiated */
	const struct mpipe_ai_convert_desc *desc;
	/** Internal output buffer pool */
	struct mpipe_buffer_pool out_pool;
};

/**
 * @brief Initialize a video to tensor converter element
 *
 * @param conv Pointer to the element to initialize.
 * @param id Unique element identifier.
 *
 * @return 0 on success, negative errno otherwise.
 */
int mpipe_ai_convert_init(struct mpipe_ai_convert *conv, uint8_t id);

/** @} */

#endif /* ZEPHYR_INCLUDE_MPIPE_AI_MPIPE_AI_CONVERT_H_ */
